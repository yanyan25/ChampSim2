/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ooo_cpu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include <fmt/ranges.h>

#include "cache.h"
#include "champsim.h"
#include "champsim_assert.h"
#include "deadlock.h"
#include "event_listeners.h"
#include "instruction.h"
#include "util/span.h"

long O3_CPU::operate()
{
  long progress{0};
  progress += retire_rob();                    // retire
  progress += complete_inflight_instruction(); // finalize execution
  progress += execute_instruction();           // execute instructions
  progress += schedule_instruction();          // schedule instructions
  progress += handle_memory_return();          // finalize memory transactions
  progress += operate_lsq();                   // execute memory transactions

  progress += dispatch_instruction(); // dispatch
  progress += decode_instruction();   // decode
  progress += promote_to_decode();

  progress += fetch_instruction(); // fetch
  progress += check_dib();
  initialize_instruction();

  return progress;
}

void O3_CPU::initialize()
{
  // BRANCH PREDICTOR & BTB
  impl_initialize_branch_predictor();
  impl_initialize_btb();
}

void O3_CPU::begin_phase()
{
  ctx().begin_phase_instr = ctx().num_retired;
  ctx().begin_phase_time = current_time;

  // Record where the next phase begins
  stats_type stats;
  stats.name = "CPU " + std::to_string(cpu);
  stats.begin_instrs = ctx().num_retired;
  stats.begin_cycles = ctx().begin_phase_time.time_since_epoch() / clock_period;
  ctx().sim_stats = stats;
}

void O3_CPU::end_phase(unsigned finished_cpu)
{
  // Record where the phase ended (overwrite if this is later)
  ctx().sim_stats.end_instrs = ctx().num_retired;
  ctx().sim_stats.end_cycles = current_time.time_since_epoch() / clock_period;

  if (finished_cpu == this->cpu) {
    ctx().finish_phase_instr = ctx().num_retired;
    ctx().finish_phase_time = current_time;

    ctx().roi_stats = ctx().sim_stats;
  }
}

void O3_CPU::initialize_instruction()
{
  champsim::bandwidth instrs_to_read_this_cycle{
      std::min(FETCH_WIDTH, champsim::bandwidth::maximum_type{static_cast<long>(IFETCH_BUFFER_SIZE - std::size(ctx().IFETCH_BUFFER))})};

  bool stop_fetch = false;
  while (current_time >= ctx().fetch_resume_time && instrs_to_read_this_cycle.has_remaining() && !stop_fetch && !std::empty(ctx().input_queue)) {
    instrs_to_read_this_cycle.consume();

    stop_fetch = do_init_instruction(ctx().input_queue.front());

    // Add to IFETCH_BUFFER
    ctx().IFETCH_BUFFER.push_back(std::move(ctx().input_queue.front()));
    ctx().input_queue.pop_front();

    ctx().IFETCH_BUFFER.back().ready_time = current_time;
  }
}

namespace
{
void do_stack_pointer_folding(ooo_model_instr& arch_instr)
{
  // The exact, true value of the stack pointer for any given instruction can usually be determined immediately after the instruction is decoded without
  // waiting for the stack pointer's dependency chain to be resolved.
  bool writes_sp = (std::count(std::begin(arch_instr.destination_registers), std::end(arch_instr.destination_registers), champsim::REG_STACK_POINTER) > 0);
  if (writes_sp) {
    // Avoid creating register dependencies on the stack pointer for calls, returns, pushes, and pops, but not for variable-sized changes in the
    // stack pointer position. reads_other indicates that the stack pointer is being changed by a variable amount, which can't be determined before
    // execution.
    bool reads_other =
        (std::count_if(std::begin(arch_instr.source_registers), std::end(arch_instr.source_registers),
                       [](auto r) { return r != champsim::REG_STACK_POINTER && r != champsim::REG_FLAGS && r != champsim::REG_INSTRUCTION_POINTER; })
         > 0);
    if ((arch_instr.is_branch) || !(std::empty(arch_instr.destination_memory) && std::empty(arch_instr.source_memory)) || (!reads_other)) {
      auto nonsp_end = std::remove(std::begin(arch_instr.destination_registers), std::end(arch_instr.destination_registers), champsim::REG_STACK_POINTER);
      arch_instr.destination_registers.erase(nonsp_end, std::end(arch_instr.destination_registers));
    }
  }
}
} // namespace

bool O3_CPU::do_predict_branch(ooo_model_instr& arch_instr)
{
  bool stop_fetch = false;

  // handle branch prediction for all instructions as at this point we do not know if the instruction is a branch
  ctx().sim_stats.total_branch_types.increment(arch_instr.branch);
  auto [predicted_branch_target, always_taken] = impl_btb_prediction(arch_instr.ip, arch_instr.branch);
  arch_instr.branch_prediction = impl_predict_branch(arch_instr.ip, predicted_branch_target, always_taken, arch_instr.branch) || always_taken;
  if (!arch_instr.branch_prediction) {
    predicted_branch_target = champsim::address{};
  }

  if (arch_instr.is_branch) {
    if constexpr (champsim::debug_print) {
      fmt::print("[BRANCH] instr_id: {} ip: {} taken: {}\n", arch_instr.instr_id, arch_instr.ip, arch_instr.branch_taken);
    }

    // call code prefetcher every time the branch predictor is used
    l1i->impl_prefetcher_branch_operate(arch_instr.ip, arch_instr.branch, predicted_branch_target);

    if (predicted_branch_target != arch_instr.branch_target
        || (((arch_instr.branch == BRANCH_CONDITIONAL) || (arch_instr.branch == BRANCH_OTHER))
            && arch_instr.branch_taken != arch_instr.branch_prediction)) { // conditional branches are re-evaluated at decode when the target is computed
      ctx().sim_stats.total_rob_occupancy_at_branch_mispredict += std::size(ctx().ROB);
      ctx().sim_stats.branch_type_misses.increment(arch_instr.branch);
      if (!warmup) {
        ctx().fetch_resume_time = champsim::chrono::clock::time_point::max();
        stop_fetch = true;
        arch_instr.branch_mispredicted = true;

        // Only the first freeze starts the clock: a second misprediction can be
        // detected while fetch is already frozen, and the lost cycles overlap.
        if (!ctx().fetch_stalled_on_mispredict) {
          ctx().fetch_stalled_on_mispredict = true;
          ctx().fetch_stall_begin = current_time;
        }
      }
    } else {
      stop_fetch = arch_instr.branch_taken; // if correctly predicted taken, then we can't fetch anymore instructions this cycle
    }

    impl_update_btb(arch_instr.ip, arch_instr.branch_target, arch_instr.branch_taken, arch_instr.branch);
    impl_last_branch_result(arch_instr.ip, arch_instr.branch_target, arch_instr.branch_taken, arch_instr.branch);
  }

  return stop_fetch;
}

bool O3_CPU::do_init_instruction(ooo_model_instr& arch_instr)
{
  // fast warmup eliminates register dependencies between instructions branch predictor, cache contents, and prefetchers are still warmed up
  if (warmup) {
    arch_instr.source_registers.clear();
    arch_instr.destination_registers.clear();
  }

  ::do_stack_pointer_folding(arch_instr);
  return do_predict_branch(arch_instr);
}

long O3_CPU::check_dib()
{
  // scan through IFETCH_BUFFER to find instructions that hit in the decoded instruction buffer
  auto begin = std::find_if(std::begin(ctx().IFETCH_BUFFER), std::end(ctx().IFETCH_BUFFER), [](const ooo_model_instr& x) { return !x.dib_checked; });
  auto [window_begin, window_end] = champsim::get_span(begin, std::end(ctx().IFETCH_BUFFER), champsim::bandwidth{FETCH_WIDTH});
  std::for_each(window_begin, window_end, [this](auto& ifetch_entry) { this->do_check_dib(ifetch_entry); });
  return std::distance(window_begin, window_end);
}

void O3_CPU::do_check_dib(ooo_model_instr& instr)
{
  // Check DIB to see if we recently fetched this line
  auto dib_result = ctx().DIB.check_hit(instr.ip);

  // Every instruction is checked exactly once -- `dib_checked` below gates
  // re-entry -- so this is the only site that charges a DIB lookup, and the
  // hit/miss counts sum to one lookup per fetched instruction. The fill is at
  // decode (`do_dib_update`), not here, so a whole cold fetch group misses.
  if (dib_result) {
    ctx().sim_stats.dib_hits++;

    // The cache line is in the L0, so we can mark this as complete
    instr.fetch_completed = true;

    // Also mark it as decoded
    instr.decoded = true;

    // It can be acted on immediately
    instr.ready_time = current_time;
  } else {
    ctx().sim_stats.dib_misses++;
  }

  instr.dib_checked = true;

  if constexpr (champsim::debug_print) {
    fmt::print("[DIB] {} instr_id: {} ip: {} hit: {} cycle: {}\n", __func__, instr.instr_id, instr.ip, dib_result.has_value(),
               current_time.time_since_epoch() / clock_period);
  }
}

long O3_CPU::fetch_instruction()
{
  long progress{0};

  // Fetch a single cache line
  auto fetch_ready = [](const ooo_model_instr& x) {
    return x.dib_checked && !x.fetch_issued;
  };

  // Find the chunk of instructions in the block
  auto no_match_ip = [](const auto& lhs, const auto& rhs) {
    return champsim::block_number{lhs.ip} != champsim::block_number{rhs.ip};
  };

  auto l1i_req_begin = std::find_if(std::begin(ctx().IFETCH_BUFFER), std::end(ctx().IFETCH_BUFFER), fetch_ready);
  for (champsim::bandwidth l1i_bw{L1I_BANDWIDTH}; l1i_bw.has_remaining() && l1i_req_begin != std::end(ctx().IFETCH_BUFFER); l1i_bw.consume()) {
    auto l1i_req_end = std::adjacent_find(l1i_req_begin, std::end(ctx().IFETCH_BUFFER), no_match_ip);
    if (l1i_req_end != std::end(ctx().IFETCH_BUFFER)) {
      l1i_req_end = std::next(l1i_req_end); // adjacent_find returns the first of the non-equal elements
    }

    // Issue to L1I
    auto success = do_fetch_instruction(l1i_req_begin, l1i_req_end);
    if (success) {
      std::for_each(l1i_req_begin, l1i_req_end, [](auto& x) { x.fetch_issued = true; });
      ++progress;
    }

    l1i_req_begin = std::find_if(l1i_req_end, std::end(ctx().IFETCH_BUFFER), fetch_ready);
  }

  return progress;
}

bool O3_CPU::do_fetch_instruction(std::deque<ooo_model_instr>::iterator begin, std::deque<ooo_model_instr>::iterator end)
{
  CacheBus::request_type fetch_packet;
  fetch_packet.v_address = begin->ip;
  fetch_packet.instr_id = begin->instr_id;
  fetch_packet.ip = begin->ip;
  fetch_packet.asid[0] = begin->asid[0];
  fetch_packet.asid[1] = begin->asid[1];

  std::transform(begin, end, std::back_inserter(fetch_packet.instr_depend_on_me), [](const auto& instr) { return instr.instr_id; });

  if constexpr (champsim::debug_print) {
    fmt::print("[IFETCH] {} instr_id: {} ip: {} dependents: {} event_cycle: {}\n", __func__, begin->instr_id, begin->ip,
               std::size(fetch_packet.instr_depend_on_me), begin->ready_time.time_since_epoch() / clock_period);
  }

  return L1I_bus.issue_read(fetch_packet);
}

long O3_CPU::promote_to_decode()
{
  auto is_decoded = [](const ooo_model_instr& x) {
    return x.decoded;
  };

  auto fetch_complete_and_ready = [time = current_time](const auto& x) {
    return x.fetch_completed && x.ready_time <= time;
  };

  champsim::bandwidth available_fetch_bandwidth{
      std::min(FETCH_WIDTH, std::min(champsim::bandwidth::maximum_type{static_cast<long>(DIB_HIT_BUFFER_SIZE - std::size(ctx().DIB_HIT_BUFFER))},
                                     champsim::bandwidth::maximum_type{static_cast<long>(DECODE_BUFFER_SIZE - std::size(ctx().DECODE_BUFFER))}))};

  auto fetched_check_end = std::find_if(std::begin(ctx().IFETCH_BUFFER), std::end(ctx().IFETCH_BUFFER), [](const ooo_model_instr& x) { return !x.fetch_completed; });
  // find the first not fetch completed
  auto [window_begin, window_end] = champsim::get_span_p(std::begin(ctx().IFETCH_BUFFER), fetched_check_end, available_fetch_bandwidth, fetch_complete_and_ready);
  auto decoded_window_end = std::stable_partition(window_begin, window_end, is_decoded); // reorder instructions
  auto mark_for_decode = [time = current_time, lat = DECODE_LATENCY, warmup = warmup](auto& x) {
    return x.ready_time = time + (warmup ? champsim::chrono::clock::duration{} : lat);
  };
  // to DIB_HIT_BUFFER. Warmup is free here for the same reason it is free in
  // mark_for_decode: warmup exists to populate structures, not to be timed, and
  // charging one path but not the other made the two front-end routes
  // asymmetric during warmup in a way nothing intended.
  auto mark_for_dib = [time = current_time, lat = DIB_HIT_LATENCY, warmup = warmup](auto& x) {
    return x.ready_time = time + (warmup ? champsim::chrono::clock::duration{} : lat);
  };

  std::for_each(window_begin, decoded_window_end, mark_for_dib); // assume DECODE_LATENCY = DIB_HIT_LATENCY
  std::move(window_begin, decoded_window_end, std::back_inserter(ctx().DIB_HIT_BUFFER));
  // to DECODE_BUFFER

  std::for_each(decoded_window_end, window_end, mark_for_decode);
  std::move(decoded_window_end, window_end, std::back_inserter(ctx().DECODE_BUFFER));

  long progress{std::distance(window_begin, window_end)};
  ctx().IFETCH_BUFFER.erase(window_begin, window_end);
  return progress;
}
long O3_CPU::decode_instruction()
{
  auto is_ready = [time = current_time](const auto& x) {
    return x.ready_time <= time;
  };

  auto dib_hit_buffer_begin = std::begin(ctx().DIB_HIT_BUFFER);
  auto dib_hit_buffer_end = dib_hit_buffer_begin;
  auto decode_buffer_begin = std::begin(ctx().DECODE_BUFFER);
  auto decode_buffer_end = decode_buffer_begin;

  champsim::bandwidth available_decode_bandwidth{DECODE_WIDTH};

  // bw move instructions to dispatch_buffer
  champsim::bandwidth available_dib_inorder_bandwidth{
      std::min(DIB_INORDER_WIDTH, champsim::bandwidth::maximum_type{static_cast<long>(DISPATCH_BUFFER_SIZE - std::size(ctx().DISPATCH_BUFFER))})};

  // conditions choose how many instructions sent to dispatch_buffer
  while (dib_hit_buffer_end != std::end(ctx().DIB_HIT_BUFFER) && decode_buffer_end != std::end(ctx().DECODE_BUFFER) && available_dib_inorder_bandwidth.has_remaining()
         && available_decode_bandwidth.has_remaining() && is_ready(std::min(*dib_hit_buffer_end, *decode_buffer_end, ooo_model_instr::program_order))) {
    if (ooo_model_instr::program_order(*dib_hit_buffer_end, *decode_buffer_end)) {
      dib_hit_buffer_end++;
      available_dib_inorder_bandwidth.consume();
    } else {
      decode_buffer_end++;
      available_dib_inorder_bandwidth.consume();
      available_decode_bandwidth.consume();
    }
  }
  while (dib_hit_buffer_end != std::end(ctx().DIB_HIT_BUFFER) && available_dib_inorder_bandwidth.has_remaining() && is_ready(*dib_hit_buffer_end)
         && (decode_buffer_end == std::end(ctx().DECODE_BUFFER) || ooo_model_instr::program_order(*dib_hit_buffer_end, *decode_buffer_end))) {
    dib_hit_buffer_end++;
    available_dib_inorder_bandwidth.consume();
  }
  while (decode_buffer_end != std::end(ctx().DECODE_BUFFER) && available_dib_inorder_bandwidth.has_remaining() && available_decode_bandwidth.has_remaining()
         && is_ready(*decode_buffer_end)
         && (dib_hit_buffer_end == std::end(ctx().DIB_HIT_BUFFER) || ooo_model_instr::program_order(*decode_buffer_end, *dib_hit_buffer_end))) {
    decode_buffer_end++;
    available_dib_inorder_bandwidth.consume();
    available_decode_bandwidth.consume();
  }

  // decode instructions have not decoded, merge instructions with dib_hit_buffer then send to dispatch_buffer
  auto do_decode = [&, this](auto& db_entry) {
    this->do_dib_update(db_entry);

    // Report each architectural destination register while the numbers are
    // still architectural: do_scheduling() overwrites this vector with
    // physical IDs, so by execute the original is gone.
    for (auto dreg : db_entry.destination_registers) {
      this->impl_branch_decode_notify(db_entry.instr_id, static_cast<uint8_t>(dreg));
    }

    // Resume fetch
    if (db_entry.branch_mispredicted) {
      // These branches detect the misprediction at decode
      if ((db_entry.branch == BRANCH_DIRECT_JUMP) || (db_entry.branch == BRANCH_DIRECT_CALL)
          || (((db_entry.branch == BRANCH_CONDITIONAL) || (db_entry.branch == BRANCH_OTHER)) && db_entry.branch_taken == db_entry.branch_prediction)) {
        // clear the branch_mispredicted bit so we don't attempt to resume fetch again at execute
        db_entry.branch_mispredicted = 0;
        // pay misprediction penalty
        this->resume_fetch_after_mispredict();
      }
    }
    // Add to dispatch
    db_entry.ready_time = this->current_time + (this->warmup ? champsim::chrono::clock::duration{} : this->DISPATCH_LATENCY);

    if constexpr (champsim::debug_print) {
      fmt::print("[DECODE] do_decode instr_id: {} time: {}\n", db_entry.instr_id, this->current_time.time_since_epoch() / this->clock_period);
    }
  };

  auto do_dib_hit = [&, this](auto& dib_entry) {
    dib_entry.ready_time = this->current_time + (this->warmup ? champsim::chrono::clock::duration{} : this->DISPATCH_LATENCY);
  };

  std::for_each(decode_buffer_begin, decode_buffer_end, do_decode);
  std::for_each(dib_hit_buffer_begin, dib_hit_buffer_end, do_dib_hit);

  long progress{std::distance(dib_hit_buffer_begin, dib_hit_buffer_end) + std::distance(decode_buffer_begin, decode_buffer_end)};

  std::merge(dib_hit_buffer_begin, dib_hit_buffer_end, decode_buffer_begin, decode_buffer_end, std::back_inserter(ctx().DISPATCH_BUFFER),
             ooo_model_instr::program_order);
  ctx().DECODE_BUFFER.erase(decode_buffer_begin, decode_buffer_end);
  ctx().DIB_HIT_BUFFER.erase(dib_hit_buffer_begin, dib_hit_buffer_end);

  return progress;
}

void O3_CPU::do_dib_update(const ooo_model_instr& instr) { ctx().DIB.fill(instr.ip); }

long O3_CPU::dispatch_instruction()
{
  champsim::bandwidth available_dispatch_bandwidth{DISPATCH_WIDTH};

  // dispatch DISPATCH_WIDTH instructions into the ROB
  while (available_dispatch_bandwidth.has_remaining() && !std::empty(ctx().DISPATCH_BUFFER) && ctx().DISPATCH_BUFFER.front().ready_time <= current_time
         && std::size(ctx().ROB) != ROB_SIZE
         && ((std::size_t)std::count_if(std::begin(ctx().LQ), std::end(ctx().LQ), [](const auto& lq_entry) { return !lq_entry.has_value(); })
             >= std::size(ctx().DISPATCH_BUFFER.front().source_memory))
         && ((std::size(ctx().DISPATCH_BUFFER.front().destination_memory) + std::size(ctx().SQ)) <= SQ_SIZE)) {
    ctx().ROB.push_back(std::move(ctx().DISPATCH_BUFFER.front()));
    ctx().DISPATCH_BUFFER.pop_front();
    do_memory_scheduling(ctx().ROB.back());

    available_dispatch_bandwidth.consume();
    ctx().ROB.back().ready_time = current_time + (warmup ? champsim::chrono::clock::duration{} : SCHEDULING_LATENCY);
  }

  return available_dispatch_bandwidth.amount_consumed();
}

void O3_CPU::throw_register_file_too_small(const ooo_model_instr& instr, unsigned long needed) const
{
  const auto free = shared_prf->count_free_registers();
  throw std::runtime_error{fmt::format(
      "runtime config: ooo_cpu.cpu{}.register_file_size = {} is too small for this trace: the oldest instruction (instr_id {}) needs {} register{} to "
      "rename, with {} free; the other {} hold architectural registers the trace has already used. A physical register is freed only when a newer "
      "write of the same architectural register retires, and no older instruction is in flight, so the core cannot proceed. The register file must "
      "hold every architectural register the trace uses, plus the registers one instruction renames.",
      cpu, REGISTER_FILE_SIZE, instr.instr_id, needed, needed == 1 ? "" : "s", free, REGISTER_FILE_SIZE - free)};
}

long O3_CPU::schedule_instruction()
{
  // Renaming is in program order: the walk stops at the first instruction the
  // free registers cannot cover. Skipping an unscheduled instruction that is not
  // ready yet keeps that order only because dispatch sets ready_time in program
  // order, so no younger unscheduled instruction is ready before it.
  champsim::bandwidth search_bw{SCHEDULER_SIZE};
  int progress{0};
  for (auto rob_it = std::begin(ctx().ROB); rob_it != std::end(ctx().ROB) && search_bw.has_remaining(); ++rob_it) {
    if (!rob_it->scheduled && rob_it->ready_time <= current_time) {
      // The free-register rename gate applies only to instructions being
      // renamed this cycle. An already-scheduled instruction has claimed its
      // registers, so evaluating the gate against it (and breaking when free
      // registers fall below its destination count) wrongly blocks scheduling.
// Renaming maps an unmapped source on its first read, so a repeat of it
      // allocates nothing: count each unmapped source register once.
      const auto& sources = rob_it->source_registers;
      unsigned long sources_to_allocate = 0;
      for (auto src_it = std::begin(sources); src_it != std::end(sources); ++src_it) {
        if (!ctx().isAllocated(*src_it) && std::find(std::begin(sources), src_it, *src_it) == src_it) {
          ++sources_to_allocate;
        }
      }
      if (shared_prf->count_free_registers() < (sources_to_allocate + rob_it->destination_registers.size())) {
        if (rob_it == std::begin(ctx().ROB)) {
          // Nothing older is in flight, and only a retiring write frees a register.
          throw_register_file_too_small(*rob_it, sources_to_allocate + rob_it->destination_registers.size());
        }
        break;
      }
      do_scheduling(*rob_it);
      ++progress;
    }

    if (!rob_it->executed) {
      search_bw.consume();
    }
  }

  return progress;
}

void O3_CPU::do_scheduling(ooo_model_instr& instr)
{
  // Mark register dependencies
  for (auto& src_reg : instr.source_registers) {
    // rename source register
    src_reg = ctx().rename_src_register(src_reg);
  }

  for (auto& dreg : instr.destination_registers) {
    // rename destination register
    dreg = ctx().rename_dest_register(dreg, instr.instr_id);
  }

  instr.scheduled = true;
}

long O3_CPU::execute_instruction()
{
  champsim::bandwidth exec_bw{EXEC_WIDTH};
  for (auto rob_it = std::begin(ctx().ROB); rob_it != std::end(ctx().ROB) && exec_bw.has_remaining(); ++rob_it) {
    if (rob_it->scheduled && !rob_it->executed && rob_it->ready_time <= current_time) {
      bool ready = std::all_of(std::begin(rob_it->source_registers), std::end(rob_it->source_registers),
                               [this](auto srcreg) { return shared_prf->isValid(srcreg); });
      if (ready) {
        do_execution(*rob_it);
        exec_bw.consume();
      }
    }
  }

  return exec_bw.amount_consumed();
}

void O3_CPU::do_execution(ooo_model_instr& instr)
{
  instr.executed = true;
  instr.ready_time = current_time + (warmup ? champsim::chrono::clock::duration{} : EXEC_LATENCY);

  // Mark LQ entries as ready to translate
  if (!std::empty(instr.source_memory)) {
    for (auto& lq_entry : ctx().LQ) {
      if (lq_entry.has_value() && lq_entry->instr_id == instr.instr_id) {
        lq_entry->ready_time = current_time + (warmup ? champsim::chrono::clock::duration{} : EXEC_LATENCY);
      }
    }
  }

  // Mark SQ entries as ready to translate
  if (!std::empty(instr.destination_memory)) {
    for (auto& sq_entry : ctx().SQ) {
      if (sq_entry.instr_id == instr.instr_id) {
        sq_entry.ready_time = current_time + (warmup ? champsim::chrono::clock::duration{} : EXEC_LATENCY);
      }
    }
  }

  if constexpr (champsim::debug_print) {
    fmt::print("[ROB] {} instr_id: {} ready_time: {}\n", __func__, instr.instr_id, instr.ready_time.time_since_epoch() / clock_period);
  }
}

void O3_CPU::do_memory_scheduling(ooo_model_instr& instr)
{
  // load
  for (auto& smem : instr.source_memory) {
    auto q_entry = std::find_if_not(std::begin(ctx().LQ), std::end(ctx().LQ), [](const auto& lq_entry) { return lq_entry.has_value(); });
    CHAMPSIM_ASSERT(q_entry != std::end(ctx().LQ));
    q_entry->emplace(smem, instr.instr_id, instr.ip, instr.asid); // add it to the load queue

    // Check for forwarding
    auto sq_it = std::max_element(std::begin(ctx().SQ), std::end(ctx().SQ), [smem](const auto& lhs, const auto& rhs) {
      return lhs.virtual_address != smem || (rhs.virtual_address == smem && LSQ_ENTRY::program_order(lhs, rhs));
    });
    if (sq_it != std::end(ctx().SQ) && sq_it->virtual_address == smem) {
      CHAMPSIM_ASSERT(sq_it->asid == instr.asid); // store-to-load forwarding must stay within one address space
      if (sq_it->fetch_issued) { // Store already executed
        (*q_entry)->finish(instr);
        q_entry->reset();
      } else {
        CHAMPSIM_ASSERT(sq_it->instr_id < instr.instr_id); // The found SQ entry is a prior store
        sq_it->lq_depend_on_me.emplace_back(*q_entry);     // Forward the load when the store finishes
        (*q_entry)->producer_id = sq_it->instr_id;         // The load waits on the store to finish

        if constexpr (champsim::debug_print) {
          fmt::print("[DISPATCH] {} instr_id: {} waits on: {}\n", __func__, instr.instr_id, sq_it->instr_id);
        }
      }
    }
  }

  // store
  for (auto& dmem : instr.destination_memory) {
    ctx().SQ.emplace_back(dmem, instr.instr_id, instr.ip, instr.asid); // add it to the store queue
  }

  if constexpr (champsim::debug_print) {
    fmt::print("[DISPATCH] {} instr_id: {} loads: {} stores: {} cycle: {}\n", __func__, instr.instr_id, std::size(instr.source_memory),
               std::size(instr.destination_memory), current_time.time_since_epoch() / clock_period);
  }
}

long O3_CPU::operate_lsq()
{
  champsim::bandwidth store_bw{SQ_WIDTH};

  const auto complete_id = std::empty(ctx().ROB) ? std::numeric_limits<uint64_t>::max() : ctx().ROB.front().instr_id;
  auto do_complete = [time = current_time, finished = LSQ_ENTRY::precedes(complete_id), this](const auto& x) {
    return finished(x) && x.ready_time <= time && this->do_complete_store(x);
  };

  auto unfetched_begin = std::partition_point(std::begin(ctx().SQ), std::end(ctx().SQ), [](const auto& x) { return x.fetch_issued; });
  auto [fetch_begin, fetch_end] =
      champsim::get_span_p(unfetched_begin, std::end(ctx().SQ), store_bw, [time = current_time](const auto& x) { return !x.fetch_issued && x.ready_time <= time; });
  store_bw.consume(std::distance(fetch_begin, fetch_end));
  std::for_each(fetch_begin, fetch_end, [time = current_time, this](auto& sq_entry) {
    this->do_finish_store(sq_entry);
    sq_entry.fetch_issued = true;
    sq_entry.ready_time = time;
  });

  auto [complete_begin, complete_end] = champsim::get_span_p(std::cbegin(ctx().SQ), std::cend(ctx().SQ), store_bw, do_complete);
  store_bw.consume(std::distance(complete_begin, complete_end));
  ctx().SQ.erase(complete_begin, complete_end);

  champsim::bandwidth load_bw{LQ_WIDTH};

  for (auto& lq_entry : ctx().LQ) {
    if (load_bw.has_remaining() && lq_entry.has_value() && lq_entry->producer_id == std::numeric_limits<uint64_t>::max() && !lq_entry->fetch_issued
        && lq_entry->ready_time < current_time) {
      auto success = execute_load(*lq_entry);
      if (success) {
        load_bw.consume();
        lq_entry->fetch_issued = true;
      }
    }
  }

  return store_bw.amount_consumed() + load_bw.amount_consumed();
}

void O3_CPU::do_finish_store(const LSQ_ENTRY& sq_entry)
{
  if constexpr (champsim::debug_print) {
    fmt::print("[SQ] {} instr_id: {} vaddr: {}\n", __func__, sq_entry.instr_id, sq_entry.virtual_address);
  }

  sq_entry.finish(std::begin(ctx().ROB), std::end(ctx().ROB));

  // Release dependent loads
  for (std::optional<LSQ_ENTRY>& dependent : sq_entry.lq_depend_on_me) {
    CHAMPSIM_ASSERT(dependent.has_value()); // LQ entry is still allocated
    CHAMPSIM_ASSERT(dependent->producer_id == sq_entry.instr_id);

    dependent->finish(std::begin(ctx().ROB), std::end(ctx().ROB));
    dependent.reset();
  }
}

bool O3_CPU::do_complete_store(const LSQ_ENTRY& sq_entry)
{
  CacheBus::request_type data_packet;
  data_packet.v_address = sq_entry.virtual_address;
  data_packet.instr_id = sq_entry.instr_id;
  data_packet.ip = sq_entry.ip;
  data_packet.asid[0] = sq_entry.asid[0];
  data_packet.asid[1] = sq_entry.asid[1];

  if constexpr (champsim::debug_print) {
    fmt::print("[SQ] {} instr_id: {} vaddr: {}\n", __func__, data_packet.instr_id, data_packet.v_address);
  }

  return L1D_bus.issue_write(data_packet);
}

bool O3_CPU::execute_load(const LSQ_ENTRY& lq_entry)
{
  CacheBus::request_type data_packet;
  data_packet.v_address = lq_entry.virtual_address;
  data_packet.instr_id = lq_entry.instr_id;
  data_packet.ip = lq_entry.ip;
  data_packet.asid[0] = lq_entry.asid[0];
  data_packet.asid[1] = lq_entry.asid[1];

  if constexpr (champsim::debug_print) {
    fmt::print("[LQ] {} instr_id: {} vaddr: {}\n", __func__, data_packet.instr_id, data_packet.v_address);
  }

  return L1D_bus.issue_read(data_packet);
}

void O3_CPU::do_complete_execution(ooo_model_instr& instr)
{
  for (auto dreg : instr.destination_registers) {
    // mark physical register's data as valid
    shared_prf->complete_dest_register(dreg);
  }

  instr.completed = true;

  // A branch has now resolved, out of program order. This is the structural
  // analogue of CBP6's execute-time update (cbp2025/lib/uarchsim.cc:352);
  // ChampSim's own last_branch_result already fired for it back at fetch.
  if (instr.is_branch) {
    impl_branch_execute_resolve(instr.instr_id, instr.ip, instr.branch_target, instr.branch_taken, instr.branch);
  }

  // Deliver the value this instruction's destination register received, where
  // it is knowable. Only a load is unambiguous: its destination register takes
  // the data it read, so require exactly one destination register and at least
  // one source memory operand. Everything else reports "no value", which leaves
  // the register marked unknown by the decode notification -- the correct
  // outcome, since an ALU result is not in the trace.
  //
  // This fires at execute, not fetch, deliberately. Handing a register value to
  // a predictor at fetch would leak the branch outcome: the flags at an x86 jcc
  // determine its direction outright.
  if (!std::empty(instr.destination_registers)) {
    bool has_value = false;
    uint64_t value = 0;
#if CHAMPSIM_TRACE_MEMORY_VALUES
    if (std::size(instr.destination_registers) == 1 && !std::empty(instr.source_memory)) {
      std::memcpy(&value, std::data(instr.source_memory_value.at(0)), sizeof(value));
      has_value = true;
    }
#endif
    impl_branch_execute_notify(instr.instr_id, has_value, value);
  }

  if (instr.branch_mispredicted) {
    resume_fetch_after_mispredict();
  }
}

// Restart fetch after a misprediction and charge the frozen interval, penalty
// included, to cycles_on_wrong_path.
void O3_CPU::resume_fetch_after_mispredict()
{
  ctx().fetch_resume_time = current_time + BRANCH_MISPREDICT_PENALTY;

  if (ctx().fetch_stalled_on_mispredict) {
    ctx().sim_stats.cycles_on_wrong_path += static_cast<uint64_t>((ctx().fetch_resume_time - ctx().fetch_stall_begin) / clock_period);
    ctx().fetch_stalled_on_mispredict = false;
  }
}

long O3_CPU::complete_inflight_instruction()
{
  // update ROB entries with completed executions
  champsim::bandwidth complete_bw{EXEC_WIDTH};
  for (auto rob_it = std::begin(ctx().ROB); rob_it != std::end(ctx().ROB) && complete_bw.has_remaining(); ++rob_it) {
    if (rob_it->executed && !rob_it->completed && (rob_it->ready_time <= current_time) && rob_it->completed_mem_ops == rob_it->num_mem_ops()) {
      do_complete_execution(*rob_it);
      complete_bw.consume();
    }
  }

  return complete_bw.amount_consumed();
}

long O3_CPU::handle_memory_return()
{
  long progress{0};

  for (champsim::bandwidth fetch_bw{FETCH_WIDTH}, l1i_bw{L1I_BANDWIDTH};
       fetch_bw.has_remaining() && l1i_bw.has_remaining() && !L1I_bus.lower_level->returned.empty(); l1i_bw.consume()) {
    auto& l1i_entry = L1I_bus.lower_level->returned.front();

    while (fetch_bw.has_remaining() && !l1i_entry.instr_depend_on_me.empty()) {
      auto fetched = std::find_if(std::begin(ctx().IFETCH_BUFFER), std::end(ctx().IFETCH_BUFFER), ooo_model_instr::matches_id(l1i_entry.instr_depend_on_me.front()));
      if (fetched != std::end(ctx().IFETCH_BUFFER) && champsim::block_number{fetched->ip} == champsim::block_number{l1i_entry.v_address} && fetched->fetch_issued) {
        fetched->fetch_completed = true;
        fetch_bw.consume();
        ++progress;

        if constexpr (champsim::debug_print) {
          fmt::print("[IFETCH] {} instr_id: {} fetch completed\n", __func__, fetched->instr_id);
        }
      }

      l1i_entry.instr_depend_on_me.erase(std::begin(l1i_entry.instr_depend_on_me));
    }

    // remove this entry if we have serviced all of its instructions
    if (l1i_entry.instr_depend_on_me.empty()) {
      L1I_bus.lower_level->returned.pop_front();
      ++progress;
    }
  }

  auto l1d_it = std::begin(L1D_bus.lower_level->returned);
  for (champsim::bandwidth l1d_bw{L1D_BANDWIDTH}; l1d_bw.has_remaining() && l1d_it != std::end(L1D_bus.lower_level->returned); l1d_bw.consume(), ++l1d_it) {
    for (auto& lq_entry : ctx().LQ) {
      if (lq_entry.has_value() && lq_entry->fetch_issued && champsim::block_number{lq_entry->virtual_address} == champsim::block_number{l1d_it->v_address}) {
        lq_entry->finish(std::begin(ctx().ROB), std::end(ctx().ROB));
        lq_entry.reset();
        ++progress;
      }
    }
    ++progress;
  }
  L1D_bus.lower_level->returned.erase(std::begin(L1D_bus.lower_level->returned), l1d_it);

  return progress;
}

long O3_CPU::retire_rob()
{
  auto [retire_begin, retire_end] =
      champsim::get_span_p(std::cbegin(ctx().ROB), std::cend(ctx().ROB), champsim::bandwidth{RETIRE_WIDTH}, [](const auto& x) { return x.completed; });
  CHAMPSIM_ASSERT(std::distance(retire_begin, retire_end) >= 0); // end succeeds begin
  if constexpr (champsim::debug_print) {
    std::for_each(retire_begin, retire_end, [cycle = current_time.time_since_epoch() / clock_period](const auto& x) {
      fmt::print("[ROB] retire_rob instr_id: {} is retired cycle: {}\n", x.instr_id, cycle);
    });
  }

  // commit register writes to backend RAT
  // and recycle the old physical registers
  for (auto rob_it = retire_begin; rob_it != retire_end; ++rob_it) {
    for (auto dreg : rob_it->destination_registers) {
      ctx().retire_dest_register(dreg);
    }
  }

  uint64_t cycles = current_time.time_since_epoch() / clock_period;
  handle_event<Event::RETIRE>(cpu, retire_begin, retire_end, cycles);

  auto retire_count = std::distance(retire_begin, retire_end);
  ctx().num_retired += retire_count;
  ctx().ROB.erase(retire_begin, retire_end);

  return retire_count;
}

void O3_CPU::impl_initialize_branch_predictor() const { branch_module_pimpl->impl_initialize_branch_predictor(); }

void O3_CPU::impl_branch_predictor_final_stats() const { branch_module_pimpl->impl_branch_predictor_final_stats(); }

void O3_CPU::impl_branch_execute_resolve(uint64_t instr_id, champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type) const
{
  branch_module_pimpl->impl_branch_execute_resolve(instr_id, ip, branch_target, taken, branch_type);
}

void O3_CPU::impl_branch_decode_notify(uint64_t instr_id, uint8_t arch_dst_reg) const
{
  branch_module_pimpl->impl_branch_decode_notify(instr_id, arch_dst_reg);
}

void O3_CPU::impl_branch_execute_notify(uint64_t instr_id, bool has_value, uint64_t value) const
{
  branch_module_pimpl->impl_branch_execute_notify(instr_id, has_value, value);
}

void O3_CPU::impl_last_branch_result(champsim::address ip, champsim::address target, bool taken, uint8_t branch_type) const
{
  branch_module_pimpl->impl_last_branch_result(ip, target, taken, branch_type);
}

bool O3_CPU::impl_predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type) const
{
  return branch_module_pimpl->impl_predict_branch(ip, predicted_target, always_taken, branch_type);
}

void O3_CPU::impl_initialize_btb() const { btb_module_pimpl->impl_initialize_btb(); }

void O3_CPU::impl_update_btb(champsim::address ip, champsim::address predicted_target, bool taken, uint8_t branch_type) const
{
  btb_module_pimpl->impl_update_btb(ip, predicted_target, taken, branch_type);
}

std::pair<champsim::address, bool> O3_CPU::impl_btb_prediction(champsim::address ip, uint8_t branch_type) const
{
  return btb_module_pimpl->impl_btb_prediction(ip, branch_type);
}

// LCOV_EXCL_START Exclude the following function from LCOV
void O3_CPU::print_deadlock()
{
  fmt::print("DEADLOCK! CPU {} cycle {}\n", cpu, current_time.time_since_epoch() / clock_period);

  auto instr_pack = [period = clock_period, this](const auto& entry) {
return std::tuple{entry.instr_id, entry.fetch_issued, entry.fetch_completed, entry.scheduled, entry.executed, entry.completed,
                      // Until do_scheduling renames an entry its operands are architectural IDs.
                      entry.scheduled ? std::to_string(shared_prf->count_reg_dependencies(entry)) : std::string{"-"},
                      entry.num_mem_ops() - entry.completed_mem_ops, entry.ready_time.time_since_epoch() / period};
  };
  std::string_view instr_fmt{
      "instr_id: {} fetch_issued: {} fetch_completed: {} scheduled: {} executed: {} completed: {} num_reg_dependent: {} num_mem_ops: {} event: {}"};
  champsim::range_print_deadlock(ctx().IFETCH_BUFFER, "cpu" + std::to_string(cpu) + "_IFETCH", instr_fmt, instr_pack);
  champsim::range_print_deadlock(ctx().DECODE_BUFFER, "cpu" + std::to_string(cpu) + "_DECODE", instr_fmt, instr_pack);
  champsim::range_print_deadlock(ctx().DISPATCH_BUFFER, "cpu" + std::to_string(cpu) + "_DISPATCH", instr_fmt, instr_pack);
  champsim::range_print_deadlock(ctx().ROB, "cpu" + std::to_string(cpu) + "_ROB", instr_fmt, instr_pack);

  // print the RATs, then the occupied physical registers
  ctx().print_deadlock();
  shared_prf->print_deadlock();

  // print LQ entry
  auto lq_pack = [period = clock_period](const auto& entry) {
    std::string depend_id{"-"};
    if (entry->producer_id != std::numeric_limits<uint64_t>::max()) {
      depend_id = std::to_string(entry->producer_id);
    }
    return std::tuple{entry->instr_id, entry->virtual_address, entry->fetch_issued, entry->ready_time.time_since_epoch() / period, depend_id};
  };
  std::string_view lq_fmt{"instr_id: {} address: {} fetch_issued: {} event_cycle: {} waits on {}"};

  auto sq_pack = [period = clock_period](const auto& entry) {
    // do_finish_store releases the waiting loads but not these references, so
    // a slot may since be empty or hold a younger load waiting on nothing here.
    std::vector<uint64_t> depend_ids;
    for (const std::optional<LSQ_ENTRY>& lq_entry : entry.lq_depend_on_me) {
      if (lq_entry.has_value() && lq_entry->producer_id == entry.instr_id) {
        depend_ids.push_back(lq_entry->instr_id);
      }
    }
    return std::tuple{entry.instr_id, entry.virtual_address, entry.fetch_issued, entry.ready_time.time_since_epoch() / period, depend_ids};
  };
  std::string_view sq_fmt{"instr_id: {} address: {} fetch_issued: {} event_cycle: {} LQ waiting: {}"};
  champsim::range_print_deadlock(ctx().LQ, "cpu" + std::to_string(cpu) + "_LQ", lq_fmt, lq_pack);
  champsim::range_print_deadlock(ctx().SQ, "cpu" + std::to_string(cpu) + "_SQ", sq_fmt, sq_pack);
}
// LCOV_EXCL_STOP

LSQ_ENTRY::LSQ_ENTRY(champsim::address addr, champsim::program_ordered<LSQ_ENTRY>::id_type id, champsim::address local_ip, std::array<uint8_t, 2> local_asid)
    : champsim::program_ordered<LSQ_ENTRY>{id}, virtual_address(addr), ip(local_ip), asid(local_asid)
{
}

void LSQ_ENTRY::finish(std::deque<ooo_model_instr>::iterator begin, std::deque<ooo_model_instr>::iterator end) const
{
  auto rob_entry = std::partition_point(begin, end, ooo_model_instr::precedes(this->instr_id));
  CHAMPSIM_ASSERT(rob_entry != end);
  finish(*rob_entry);
}

void LSQ_ENTRY::finish(ooo_model_instr& rob_entry) const
{
  CHAMPSIM_ASSERT(rob_entry.instr_id == this->instr_id);

  ++rob_entry.completed_mem_ops;
  CHAMPSIM_ASSERT(rob_entry.completed_mem_ops <= rob_entry.num_mem_ops());

  if constexpr (champsim::debug_print) {
    fmt::print("[LSQ] {} instr_id: {} full_address: {} remain_mem_ops: {}\n", __func__, instr_id, virtual_address,
               rob_entry.num_mem_ops() - rob_entry.completed_mem_ops);
  }
}

bool CacheBus::issue_read(request_type data_packet)
{
  data_packet.address = data_packet.v_address;
  data_packet.is_translated = false;
  data_packet.cpu = cpu;
  data_packet.type = access_type::LOAD;

  return lower_level->add_rq(data_packet);
}

bool CacheBus::issue_write(request_type data_packet)
{
  data_packet.address = data_packet.v_address;
  data_packet.is_translated = false;
  data_packet.cpu = cpu;
  data_packet.type = access_type::WRITE;
  data_packet.response_requested = false;

  return lower_level->add_wq(data_packet);
}
