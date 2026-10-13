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
#include "defs.h"
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

  // Advance the front-end round-robin start and the dispatch-owner pointer once
  // per cycle (Stage 3.4), and the back-end round-robin start once per cycle
  // (Stage 3.5). Each budget-point wrapper reads these to decide which context a
  // shared width serves first; they must not advance per site.
  fetch_round_robin = (fetch_round_robin + 1) % contexts.size();
  dispatch_owner = (dispatch_owner + 1) % contexts.size();
  backend_round_robin = (backend_round_robin + 1) % contexts.size();

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
  // Per-context phase bookkeeping. contexts.size() == 1 collapses this to the
  // single-context path byte-for-byte: the name gets no suffix and only one
  // context is touched.
  for (auto& c : contexts) {
    c.begin_phase_instr = c.num_retired;
    c.begin_phase_time = current_time;

    // Record where the next phase begins
    stats_type stats;
    stats.name = "CPU " + std::to_string(cpu);
    if (contexts.size() > 1 && c.ctx_index.value > 0)
      stats.name += " ctx " + std::to_string(c.ctx_index.value);
    stats.begin_instrs = c.num_retired;
    stats.begin_cycles = c.begin_phase_time.time_since_epoch() / clock_period;
    c.sim_stats = stats;
  }
}

void O3_CPU::end_phase(unsigned finished_cpu)
{
  for (auto& c : contexts) {
    // Record where the phase ended (overwrite if this is later)
    c.sim_stats.end_instrs = c.num_retired;
    c.sim_stats.end_cycles = current_time.time_since_epoch() / clock_period;

    if (finished_cpu == this->cpu) {
      c.finish_phase_instr = c.num_retired;
      c.finish_phase_time = current_time;

      c.roi_stats = c.sim_stats;
    }
  }
}

void O3_CPU::initialize_instruction()
{
  if constexpr (champsim::defs::max_contexts == 1) {
    champsim::bandwidth fetch_bw{FETCH_WIDTH};
    initialize_instruction_impl(ctx(), champsim::context_index{}, fetch_bw);
  } else {
    champsim::bandwidth fetch_bw{FETCH_WIDTH};
    for (std::size_t i = 0; i < contexts.size() && fetch_bw.has_remaining(); ++i) {
      const std::size_t c = (fetch_round_robin + i) % contexts.size();
      initialize_instruction_impl(contexts[c], champsim::context_index{static_cast<uint8_t>(c)}, fetch_bw);
    }
  }
}

void O3_CPU::initialize_instruction_impl(champsim::core_context& c, champsim::context_index idx, champsim::bandwidth& fetch_bw)
{
  champsim::bandwidth instrs_to_read_this_cycle{
      std::min(champsim::bandwidth::maximum_type{fetch_bw.amount_remaining()},
               champsim::bandwidth::maximum_type{static_cast<long>(IFETCH_BUFFER_SIZE - std::size(c.IFETCH_BUFFER))})};

  bool stop_fetch = false;
  while (current_time >= c.fetch_resume_time && instrs_to_read_this_cycle.has_remaining() && !stop_fetch && !std::empty(c.input_queue)) {
    instrs_to_read_this_cycle.consume();
    fetch_bw.consume();

    stop_fetch = do_init_instruction(c, idx, c.input_queue.front());

    // Add to IFETCH_BUFFER
    c.IFETCH_BUFFER.push_back(std::move(c.input_queue.front()));
    c.input_queue.pop_front();

    c.IFETCH_BUFFER.back().ready_time = current_time;
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

bool O3_CPU::do_predict_branch(champsim::core_context& c, champsim::context_index idx, ooo_model_instr& arch_instr)
{
  bool stop_fetch = false;

  // handle branch prediction for all instructions as at this point we do not know if the instruction is a branch
  c.sim_stats.total_branch_types.increment(arch_instr.branch);
  auto [predicted_branch_target, always_taken] = impl_btb_prediction(arch_instr.ip, arch_instr.branch, idx, arch_instr.asid);
  arch_instr.branch_prediction = impl_predict_branch(arch_instr.ip, predicted_branch_target, always_taken, arch_instr.branch, arch_instr.asid) || always_taken;
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
      c.sim_stats.total_rob_occupancy_at_branch_mispredict += std::size(c.ROB);
      c.sim_stats.branch_type_misses.increment(arch_instr.branch);
      if (!warmup) {
        c.fetch_resume_time = champsim::chrono::clock::time_point::max();
        stop_fetch = true;
        arch_instr.branch_mispredicted = true;

        // Only the first freeze starts the clock: a second misprediction can be
        // detected while fetch is already frozen, and the lost cycles overlap.
        if (!c.fetch_stalled_on_mispredict) {
          c.fetch_stalled_on_mispredict = true;
          c.fetch_stall_begin = current_time;
        }
      }
    } else {
      stop_fetch = arch_instr.branch_taken; // if correctly predicted taken, then we can't fetch anymore instructions this cycle
    }

    impl_update_btb(arch_instr.ip, arch_instr.branch_target, arch_instr.branch_taken, arch_instr.branch, idx, arch_instr.asid);
    impl_last_branch_result(arch_instr.ip, arch_instr.branch_target, arch_instr.branch_taken, arch_instr.branch, arch_instr.asid);
  }

  return stop_fetch;
}

bool O3_CPU::do_init_instruction(champsim::core_context& c, champsim::context_index idx, ooo_model_instr& arch_instr)
{
  // fast warmup eliminates register dependencies between instructions branch predictor, cache contents, and prefetchers are still warmed up
  if (warmup) {
    arch_instr.source_registers.clear();
    arch_instr.destination_registers.clear();
  }

  ::do_stack_pointer_folding(arch_instr);
  return do_predict_branch(c, idx, arch_instr);
}

long O3_CPU::check_dib()
{
  if constexpr (champsim::defs::max_contexts == 1) {
    champsim::bandwidth fetch_bw{FETCH_WIDTH};
    return check_dib_impl(ctx(), fetch_bw);
  } else {
    champsim::bandwidth fetch_bw{FETCH_WIDTH};
    long progress = 0;
    for (std::size_t i = 0; i < contexts.size() && fetch_bw.has_remaining(); ++i) {
      const std::size_t c = (fetch_round_robin + i) % contexts.size();
      progress += check_dib_impl(contexts[c], fetch_bw);
    }
    return progress;
  }
}

long O3_CPU::check_dib_impl(champsim::core_context& c, champsim::bandwidth& fetch_bw)
{
  // scan through IFETCH_BUFFER to find instructions that hit in the decoded instruction buffer
  auto begin = std::find_if(std::begin(c.IFETCH_BUFFER), std::end(c.IFETCH_BUFFER), [](const ooo_model_instr& x) { return !x.dib_checked; });
  auto [window_begin, window_end] =
      champsim::get_span(begin, std::end(c.IFETCH_BUFFER), champsim::bandwidth{champsim::bandwidth::maximum_type{fetch_bw.amount_remaining()}});
  std::for_each(window_begin, window_end, [this, &c](auto& ifetch_entry) { this->do_check_dib(c, ifetch_entry); });
  const long progress = std::distance(window_begin, window_end);
  fetch_bw.consume(progress);
  return progress;
}

void O3_CPU::do_check_dib(ooo_model_instr& instr) { do_check_dib(ctx(), instr); }

void O3_CPU::do_check_dib(champsim::core_context& c, ooo_model_instr& instr)
{
  // Check DIB to see if we recently fetched this line
  auto dib_result = c.DIB.check_hit({instr.ip, instr.asid});

  // Every instruction is checked exactly once -- `dib_checked` below gates
  // re-entry -- so this is the only site that charges a DIB lookup, and the
  // hit/miss counts sum to one lookup per fetched instruction. The fill is at
  // decode (`do_dib_update`), not here, so a whole cold fetch group misses.
  if (dib_result) {
    CHAMPSIM_ASSERT(dib_result->asid == instr.asid); // a DIB hit is keyed by (ip, asid)
    c.sim_stats.dib_hits++;

    // The cache line is in the L0, so we can mark this as complete
    instr.fetch_completed = true;

    // Also mark it as decoded
    instr.decoded = true;

    // It can be acted on immediately
    instr.ready_time = current_time;
  } else {
    c.sim_stats.dib_misses++;
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

  if constexpr (champsim::defs::max_contexts == 1) {
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
  } else {
    // Stage 3.4: share L1I_BANDWIDTH across contexts with a rotating start,
    // work-conserving like the other fetch-budget sites.
    champsim::bandwidth l1i_bw{L1I_BANDWIDTH};
    for (std::size_t i = 0; i < contexts.size() && l1i_bw.has_remaining(); ++i) {
      const std::size_t c = (fetch_round_robin + i) % contexts.size();
      auto& ifetch_buffer = contexts[c].IFETCH_BUFFER;
      auto l1i_req_begin = std::find_if(std::begin(ifetch_buffer), std::end(ifetch_buffer), fetch_ready);
      while (l1i_bw.has_remaining() && l1i_req_begin != std::end(ifetch_buffer)) {
        auto l1i_req_end = std::adjacent_find(l1i_req_begin, std::end(ifetch_buffer), no_match_ip);
        if (l1i_req_end != std::end(ifetch_buffer)) {
          l1i_req_end = std::next(l1i_req_end);
        }

        auto success = do_fetch_instruction(l1i_req_begin, l1i_req_end);
        if (success) {
          std::for_each(l1i_req_begin, l1i_req_end, [](auto& x) { x.fetch_issued = true; });
          ++progress;
        }

        l1i_bw.consume();
        l1i_req_begin = std::find_if(l1i_req_end, std::end(ifetch_buffer), fetch_ready);
      }
    }
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
  if constexpr (champsim::defs::max_contexts == 1) {
    champsim::bandwidth fetch_bw{FETCH_WIDTH};
    return promote_to_decode_impl(ctx(), fetch_bw);
  } else {
    champsim::bandwidth fetch_bw{FETCH_WIDTH};
    long progress = 0;
    for (std::size_t i = 0; i < contexts.size() && fetch_bw.has_remaining(); ++i) {
      const std::size_t c = (fetch_round_robin + i) % contexts.size();
      progress += promote_to_decode_impl(contexts[c], fetch_bw);
    }
    return progress;
  }
}

long O3_CPU::promote_to_decode_impl(champsim::core_context& c, champsim::bandwidth& fetch_bw)
{
  auto is_decoded = [](const ooo_model_instr& x) {
    return x.decoded;
  };

  auto fetch_complete_and_ready = [time = current_time](const auto& x) {
    return x.fetch_completed && x.ready_time <= time;
  };

  champsim::bandwidth available_fetch_bandwidth{
      std::min(champsim::bandwidth::maximum_type{fetch_bw.amount_remaining()},
               std::min(champsim::bandwidth::maximum_type{static_cast<long>(DIB_HIT_BUFFER_SIZE - std::size(c.DIB_HIT_BUFFER))},
                        champsim::bandwidth::maximum_type{static_cast<long>(DECODE_BUFFER_SIZE - std::size(c.DECODE_BUFFER))}))};

  auto fetched_check_end = std::find_if(std::begin(c.IFETCH_BUFFER), std::end(c.IFETCH_BUFFER), [](const ooo_model_instr& x) { return !x.fetch_completed; });
  // find the first not fetch completed
  auto [window_begin, window_end] = champsim::get_span_p(std::begin(c.IFETCH_BUFFER), fetched_check_end, available_fetch_bandwidth, fetch_complete_and_ready);
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
  std::move(window_begin, decoded_window_end, std::back_inserter(c.DIB_HIT_BUFFER));
  // to DECODE_BUFFER

  std::for_each(decoded_window_end, window_end, mark_for_decode);
  std::move(decoded_window_end, window_end, std::back_inserter(c.DECODE_BUFFER));

  const long progress{std::distance(window_begin, window_end)};
  c.IFETCH_BUFFER.erase(window_begin, window_end);
  fetch_bw.consume(progress);
  return progress;
}
long O3_CPU::decode_instruction()
{
  if constexpr (champsim::defs::max_contexts == 1) {
    champsim::bandwidth decode_bw{DECODE_WIDTH};
    champsim::bandwidth inorder_bw{DIB_INORDER_WIDTH};
    return decode_instruction_impl(ctx(), decode_bw, inorder_bw);
  } else {
    champsim::bandwidth decode_bw{DECODE_WIDTH};
    champsim::bandwidth inorder_bw{DIB_INORDER_WIDTH};
    long progress = 0;
    for (std::size_t i = 0; i < contexts.size() && (decode_bw.has_remaining() || inorder_bw.has_remaining()); ++i) {
      const std::size_t c = (fetch_round_robin + i) % contexts.size();
      progress += decode_instruction_impl(contexts[c], decode_bw, inorder_bw);
    }
    return progress;
  }
}

long O3_CPU::decode_instruction_impl(champsim::core_context& c, champsim::bandwidth& decode_bw, champsim::bandwidth& inorder_bw)
{
  auto is_ready = [time = current_time](const auto& x) {
    return x.ready_time <= time;
  };

  auto dib_hit_buffer_begin = std::begin(c.DIB_HIT_BUFFER);
  auto dib_hit_buffer_end = dib_hit_buffer_begin;
  auto decode_buffer_begin = std::begin(c.DECODE_BUFFER);
  auto decode_buffer_end = decode_buffer_begin;

  // bw move instructions to dispatch_buffer, capped by the shared inorder width
  // and this context's remaining dispatch-buffer space.
  champsim::bandwidth available_dib_inorder_bandwidth{
      std::min(champsim::bandwidth::maximum_type{inorder_bw.amount_remaining()},
               champsim::bandwidth::maximum_type{static_cast<long>(DISPATCH_BUFFER_SIZE - std::size(c.DISPATCH_BUFFER))})};

  // conditions choose how many instructions sent to dispatch_buffer
  while (dib_hit_buffer_end != std::end(c.DIB_HIT_BUFFER) && decode_buffer_end != std::end(c.DECODE_BUFFER) && available_dib_inorder_bandwidth.has_remaining()
         && decode_bw.has_remaining() && is_ready(std::min(*dib_hit_buffer_end, *decode_buffer_end, ooo_model_instr::program_order))) {
    if (ooo_model_instr::program_order(*dib_hit_buffer_end, *decode_buffer_end)) {
      dib_hit_buffer_end++;
      available_dib_inorder_bandwidth.consume();
    } else {
      decode_buffer_end++;
      available_dib_inorder_bandwidth.consume();
      decode_bw.consume();
    }
  }
  while (dib_hit_buffer_end != std::end(c.DIB_HIT_BUFFER) && available_dib_inorder_bandwidth.has_remaining() && is_ready(*dib_hit_buffer_end)
         && (decode_buffer_end == std::end(c.DECODE_BUFFER) || ooo_model_instr::program_order(*dib_hit_buffer_end, *decode_buffer_end))) {
    dib_hit_buffer_end++;
    available_dib_inorder_bandwidth.consume();
  }
  while (decode_buffer_end != std::end(c.DECODE_BUFFER) && available_dib_inorder_bandwidth.has_remaining() && decode_bw.has_remaining() && is_ready(*decode_buffer_end)
         && (dib_hit_buffer_end == std::end(c.DIB_HIT_BUFFER) || ooo_model_instr::program_order(*decode_buffer_end, *dib_hit_buffer_end))) {
    decode_buffer_end++;
    available_dib_inorder_bandwidth.consume();
    decode_bw.consume();
  }

  // decode instructions have not decoded, merge instructions with dib_hit_buffer then send to dispatch_buffer
  auto do_decode = [&, this](auto& db_entry) {
    this->do_dib_update(c, db_entry);

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
        this->resume_fetch_after_mispredict(c);
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

  const long progress{std::distance(dib_hit_buffer_begin, dib_hit_buffer_end) + std::distance(decode_buffer_begin, decode_buffer_end)};

  std::merge(dib_hit_buffer_begin, dib_hit_buffer_end, decode_buffer_begin, decode_buffer_end, std::back_inserter(c.DISPATCH_BUFFER),
             ooo_model_instr::program_order);
  c.DECODE_BUFFER.erase(decode_buffer_begin, decode_buffer_end);
  c.DIB_HIT_BUFFER.erase(dib_hit_buffer_begin, dib_hit_buffer_end);

  // Charge the shared inorder width for what this context actually moved.
  inorder_bw.consume(available_dib_inorder_bandwidth.amount_consumed());

  return progress;
}

void O3_CPU::do_dib_update(const ooo_model_instr& instr) { do_dib_update(ctx(), instr); }

void O3_CPU::do_dib_update(champsim::core_context& c, const ooo_model_instr& instr) { c.DIB.fill({instr.ip, instr.asid}); }

long O3_CPU::dispatch_instruction()
{
  if constexpr (champsim::defs::max_contexts == 1) {
    champsim::bandwidth dispatch_bw{DISPATCH_WIDTH};
    return dispatch_instruction_impl(ctx(), dispatch_bw);
  } else {
    champsim::bandwidth dispatch_bw{DISPATCH_WIDTH};
    const std::size_t owner = dispatch_owner % contexts.size();
    long progress = dispatch_instruction_impl(contexts[owner], dispatch_bw);

    // Exclusive round with yield: an owner that stalls outright (ROB/rename
    // full, nothing ready, downstream blocked) wastes its round, so hand the
    // remaining width to the sibling rather than burning it. Two hungry
    // contexts still alternate strictly, one round each.
    if (progress == 0 && dispatch_bw.has_remaining() && contexts.size() > 1) {
      progress += dispatch_instruction_impl(contexts[(owner + 1) % contexts.size()], dispatch_bw);
    }

    return progress;
  }
}

long O3_CPU::dispatch_instruction_impl(champsim::core_context& c, champsim::bandwidth& dispatch_bw)
{
  const long before = dispatch_bw.amount_consumed();

  // dispatch DISPATCH_WIDTH instructions into the ROB
  while (dispatch_bw.has_remaining() && !std::empty(c.DISPATCH_BUFFER) && c.DISPATCH_BUFFER.front().ready_time <= current_time && std::size(c.ROB) != ROB_SIZE
         && ((std::size_t)std::count_if(std::begin(c.LQ), std::end(c.LQ), [](const auto& lq_entry) { return !lq_entry.has_value(); })
             >= std::size(c.DISPATCH_BUFFER.front().source_memory))
         && ((std::size(c.DISPATCH_BUFFER.front().destination_memory) + std::size(c.SQ)) <= SQ_SIZE)) {
    c.ROB.push_back(std::move(c.DISPATCH_BUFFER.front()));
    c.DISPATCH_BUFFER.pop_front();
    do_memory_scheduling(c, c.ROB.back());

    // Stamp the core-wide age key. instr_id cannot order across contexts (each
    // trace restarts it at 0); this monotonic counter is the one such key.
    c.ROB.back().dispatch_sequence = next_dispatch_sequence++;

    dispatch_bw.consume();
    c.ROB.back().ready_time = current_time + (warmup ? champsim::chrono::clock::duration{} : SCHEDULING_LATENCY);
  }

  return dispatch_bw.amount_consumed() - before;
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

void O3_CPU::throw_register_file_too_small_smt(const ooo_model_instr& instr, unsigned long needed, unsigned long total_need) const
{
  const auto free = shared_prf->count_free_registers();
  throw std::runtime_error{fmt::format(
      "runtime config: ooo_cpu.cpu{}.register_file_size = {} is too small for this trace: the oldest instruction (instr_id {}) needs {} register{} to "
      "rename, with {} free; the other {} hold architectural registers the trace has already used. {} contexts together need {} more register{} to "
      "rename their oldest unscheduled instructions, so the file must hold at least {} + {} = {}; with no instruction in flight nothing can free a "
      "register, and no context can proceed.",
      cpu, REGISTER_FILE_SIZE, instr.instr_id, needed, needed == 1 ? "" : "s", free, REGISTER_FILE_SIZE - free, contexts.size(), total_need,
      total_need == 1 ? "" : "s", REGISTER_FILE_SIZE - free, total_need, (REGISTER_FILE_SIZE - free) + total_need)};
}

namespace
{
// Upper bound on need_k: NUM_INSTR_SOURCES distinct unmapped sources plus
// NUM_INSTR_DESTINATIONS destinations. The bound holds only because "need =
// distinct unmapped sources + destinations" is rename's billing rule; constexpr
// tracks a change to the NUM_INSTR_* constants but cannot track a change to that
// *semantic* -- if rename's register accounting itself ever changes, this bound
// (and the deadlock floor N >= C + sum(need_k) that rests on it) silently breaks
// and must be revisited. It guards the constants, not the rule.
constexpr unsigned long max_need = NUM_INSTR_SOURCES + NUM_INSTR_DESTINATIONS;

// The physical registers one rename consumes for a single unscheduled
// instruction: each distinct source register not yet mapped in the context's
// RAT, plus every destination. This is exactly the quantity the scheduler's
// per-candidate gate computes; factored out so the sibling-reservation pass can
// ask it of a context it is not currently scheduling.
unsigned long rename_need(const champsim::core_context& c, const ooo_model_instr& instr)
{
  const auto& sources = instr.source_registers;
  unsigned long sources_to_allocate = 0;
  for (auto src_it = std::begin(sources); src_it != std::end(sources); ++src_it) {
    if (!c.isAllocated(*src_it) && std::find(std::begin(sources), src_it, *src_it) == src_it)
      ++sources_to_allocate;
  }
  return sources_to_allocate + instr.destination_registers.size();
}

// A context's dynamic waterline (need_k): the rename requirement of its oldest
// unscheduled instruction -- the one gating its in-order retirement -- or 0 if it
// has none. Recomputed every scheduling round, so it tracks the context's
// committed set as that set grows.
unsigned long context_need(const champsim::core_context& c)
{
  for (const auto& instr : c.ROB)
    if (!instr.scheduled) {
      const auto need = rename_need(c, instr);
      CHAMPSIM_ASSERT(need <= max_need);
      return need;
    }
  return 0;
}
} // namespace

long O3_CPU::schedule_instruction()
{
  if constexpr (champsim::defs::max_contexts == 1) {
    // Single-context fast path: the original search window, byte-identical. The
    // walk stops at the first instruction the free registers cannot cover; an
    // unscheduled not-ready entry ends scheduling only because dispatch sets
    // ready_time in program order, so no younger entry is ready before it.
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
  } else {
    // Shared scheduler (Stage 3.5): SCHEDULER_SIZE is the reservation-station
    // capacity, i.e. the aggregate count of instructions scheduled but not yet
    // executed. The per-cycle search budget the single-context path uses only
    // bounds this count there; across contexts a shared scan budget would let
    // two threads each fill the window, doubling it. So the gate is the live
    // occupancy itself: schedule only while the shared count is below capacity.
    const long capacity = champsim::to_underlying(SCHEDULER_SIZE);
    long occupied = 0;
    for (const auto& c : contexts)
      occupied += static_cast<long>(std::count_if(std::begin(c.ROB), std::end(c.ROB), [](const auto& x) { return x.scheduled && !x.executed; }));

    // Per-context rename waterline (Stage 5.1): reserve each sibling's next
    // rename so no context can drain the shared pool past the point where its
    // sibling can no longer schedule its oldest instruction. need[c] is context
    // c's current requirement and total_need their sum. Both are snapshotted once
    // for the whole round -- safe because a context's need changes only when that
    // context itself renames (advancing its own ROB head), and this round renames
    // only the contexts it schedules, never their sibling's RAT or ROB. Anyone
    // who later interleaves renames across contexts inside one round breaks that
    // assumption and must recompute sibling_need, not trust the snapshot.
    std::array<unsigned long, champsim::defs::max_contexts> need{};
    unsigned long total_need = 0;
    for (std::size_t k = 0; k < contexts.size(); ++k) {
      need[k] = context_need(contexts[k]);
      total_need += need[k];
    }

    long progress = 0;
    for (std::size_t i = 0; i < contexts.size() && occupied < capacity; ++i) {
      const std::size_t c = (backend_round_robin + i) % contexts.size();
      const unsigned long sibling_need = total_need - need[c];
      progress += schedule_instruction_impl(contexts[c], occupied, capacity, sibling_need);
    }

    // L2 occupancy invariant, asserted every cycle: the shared reservation
    // station can never hold more than SCHEDULER_SIZE scheduled-not-executed
    // instructions, however the arbitration interleaves the contexts.
    long scheduled_not_executed = 0;
    for (const auto& c : contexts)
      scheduled_not_executed += static_cast<long>(std::count_if(std::begin(c.ROB), std::end(c.ROB), [](const auto& x) { return x.scheduled && !x.executed; }));
    CHAMPSIM_ASSERT(scheduled_not_executed <= champsim::to_underlying(SCHEDULER_SIZE));

    return progress;
  }
}

long O3_CPU::schedule_instruction_impl(champsim::core_context& c, long& occupied, long capacity, unsigned long sibling_need)
{
  // Renaming stays in program order: the walk stops at the first instruction the
  // free registers cannot cover. The register shortage stops only this context's
  // walk; with a single context the oldest instruction cannot free a register
  // and only a retiring write would, so it is a config error and throws. With a
  // sibling, that sibling may retire and free one, so this context just waits.
  // sibling_need is the rename requirement reserved for every other context (the
  // Stage 5.1 waterline): free must cover this instruction *and* each sibling's
  // next rename, so no context can starve its siblings out of the shared pool.
  int progress{0};
  for (auto rob_it = std::begin(c.ROB); rob_it != std::end(c.ROB) && occupied < capacity; ++rob_it) {
    if (!rob_it->scheduled && rob_it->ready_time <= current_time) {
      const auto& sources = rob_it->source_registers;
      unsigned long sources_to_allocate = 0;
      for (auto src_it = std::begin(sources); src_it != std::end(sources); ++src_it) {
        if (!c.isAllocated(*src_it) && std::find(std::begin(sources), src_it, *src_it) == src_it) {
          ++sources_to_allocate;
        }
      }
      if (shared_prf->count_free_registers() < (sources_to_allocate + rob_it->destination_registers.size() + sibling_need)) {
        if (rob_it == std::begin(c.ROB)) {
          // Stage 5.1b: this context's oldest instruction cannot rename, so this
          // context can retire nothing. A register is freed only when a retiring
          // superseding write recycles it, so the stall is a config error unless a
          // sibling still has an instruction in flight that may retire and free
          // one. Any scheduled instruction anywhere is in flight (renamed but not
          // yet retired), so its absence is the criterion for "no one can free".
          const bool any_in_flight = std::any_of(std::begin(contexts), std::end(contexts), [](const auto& ctx) {
            return std::any_of(std::begin(ctx.ROB), std::end(ctx.ROB), [](const auto& x) { return x.scheduled; });
          });
          if (!any_in_flight) {
            if (contexts.size() == 1) {
              throw_register_file_too_small(*rob_it, sources_to_allocate + rob_it->destination_registers.size());
            } else {
              unsigned long total_need = 0;
              for (const auto& ctx : contexts)
                total_need += context_need(ctx);
              throw_register_file_too_small_smt(*rob_it, sources_to_allocate + rob_it->destination_registers.size(), total_need);
            }
          }
        }
        break;
      }
      do_scheduling(c, *rob_it);
      ++progress;
      ++occupied;
    }
  }

  return progress;
}

void O3_CPU::do_scheduling(ooo_model_instr& instr) { do_scheduling(ctx(), instr); }

void O3_CPU::do_scheduling(champsim::core_context& c, ooo_model_instr& instr)
{
  // Mark register dependencies
  for (auto& src_reg : instr.source_registers) {
    // rename source register
    src_reg = c.rename_src_register(src_reg);
  }

  for (auto& dreg : instr.destination_registers) {
    // rename destination register
    dreg = c.rename_dest_register(dreg, instr.instr_id);
  }

  instr.scheduled = true;
}

long O3_CPU::execute_instruction()
{
  if constexpr (champsim::defs::max_contexts == 1) {
    champsim::bandwidth exec_bw{EXEC_WIDTH};
    return execute_instruction_impl(ctx(), exec_bw);
  } else {
    // Shared scheduler (Stage 3.5): issue by global age, not by a rotating
    // per-context walk. The reservation station is shared, so this cycle's
    // EXEC_WIDTH issue slots go to the globally oldest ready instructions,
    // keyed on dispatch_sequence -- the core-wide age key. instr_id cannot
    // order across contexts (it restarts at 0 in each trace); with a single
    // context dispatch_sequence is monotone with ROB position, so this loop
    // reduces to the single-context in-order walk bit-for-bit.
    champsim::bandwidth exec_bw{EXEC_WIDTH};
    long progress = 0;
    while (exec_bw.has_remaining()) {
      champsim::core_context* best_c = nullptr;
      ooo_model_instr* best = nullptr;
      uint64_t best_seq = std::numeric_limits<uint64_t>::max();

      for (auto& c : contexts) {
        for (auto& instr : c.ROB) {
          if (instr.scheduled && !instr.executed && instr.ready_time <= current_time
              && std::all_of(std::begin(instr.source_registers), std::end(instr.source_registers),
                             [this](auto srcreg) { return shared_prf->isValid(srcreg); })) {
            if (instr.dispatch_sequence < best_seq) {
              best_c = &c;
              best = &instr;
              best_seq = instr.dispatch_sequence;
            }
            break; // this context's oldest candidate; later entries are younger
          }
        }
      }

      if (best == nullptr)
        break;

      do_execution(*best_c, *best);
      exec_bw.consume();
      ++progress;
    }
    return progress;
  }
}

long O3_CPU::execute_instruction_impl(champsim::core_context& c, champsim::bandwidth& exec_bw)
{
  const long before = exec_bw.amount_consumed();
  for (auto rob_it = std::begin(c.ROB); rob_it != std::end(c.ROB) && exec_bw.has_remaining(); ++rob_it) {
    if (rob_it->scheduled && !rob_it->executed && rob_it->ready_time <= current_time) {
      bool ready = std::all_of(std::begin(rob_it->source_registers), std::end(rob_it->source_registers),
                               [this](auto srcreg) { return shared_prf->isValid(srcreg); });
      if (ready) {
        do_execution(c, *rob_it);
        exec_bw.consume();
      }
    }
  }

  return exec_bw.amount_consumed() - before;
}

void O3_CPU::do_execution(ooo_model_instr& instr) { do_execution(ctx(), instr); }

void O3_CPU::do_execution(champsim::core_context& c, ooo_model_instr& instr)
{
  instr.executed = true;
  instr.ready_time = current_time + (warmup ? champsim::chrono::clock::duration{} : EXEC_LATENCY);

  // Mark LQ entries as ready to translate
  if (!std::empty(instr.source_memory)) {
    for (auto& lq_entry : c.LQ) {
      if (lq_entry.has_value() && lq_entry->instr_id == instr.instr_id) {
        lq_entry->ready_time = current_time + (warmup ? champsim::chrono::clock::duration{} : EXEC_LATENCY);
      }
    }
  }

  // Mark SQ entries as ready to translate
  if (!std::empty(instr.destination_memory)) {
    for (auto& sq_entry : c.SQ) {
      if (sq_entry.instr_id == instr.instr_id) {
        sq_entry.ready_time = current_time + (warmup ? champsim::chrono::clock::duration{} : EXEC_LATENCY);
      }
    }
  }

  if constexpr (champsim::debug_print) {
    fmt::print("[ROB] {} instr_id: {} ready_time: {}\n", __func__, instr.instr_id, instr.ready_time.time_since_epoch() / clock_period);
  }
}

void O3_CPU::do_memory_scheduling(ooo_model_instr& instr) { do_memory_scheduling(ctx(), instr); }

void O3_CPU::do_memory_scheduling(champsim::core_context& c, ooo_model_instr& instr)
{
  // load
  for (auto& smem : instr.source_memory) {
    auto q_entry = std::find_if_not(std::begin(c.LQ), std::end(c.LQ), [](const auto& lq_entry) { return lq_entry.has_value(); });
    CHAMPSIM_ASSERT(q_entry != std::end(c.LQ));
    auto& new_lq = q_entry->emplace(smem, instr.instr_id, instr.ip, instr.asid); // add it to the load queue
    new_lq.load_waiter_id = c.next_load_waiter_id++;                             // name this entry as its own load waiter

    // Check for forwarding
    auto sq_it = std::max_element(std::begin(c.SQ), std::end(c.SQ), [smem](const auto& lhs, const auto& rhs) {
      return lhs.virtual_address != smem || (rhs.virtual_address == smem && LSQ_ENTRY::program_order(lhs, rhs));
    });
    if (sq_it != std::end(c.SQ) && sq_it->virtual_address == smem) {
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
    c.SQ.emplace_back(dmem, instr.instr_id, instr.ip, instr.asid); // add it to the store queue
  }

  if constexpr (champsim::debug_print) {
    fmt::print("[DISPATCH] {} instr_id: {} loads: {} stores: {} cycle: {}\n", __func__, instr.instr_id, std::size(instr.source_memory),
               std::size(instr.destination_memory), current_time.time_since_epoch() / clock_period);
  }
}

long O3_CPU::operate_lsq()
{
  if constexpr (champsim::defs::max_contexts == 1) {
    champsim::bandwidth store_bw{SQ_WIDTH};
    champsim::bandwidth load_bw{LQ_WIDTH};
    return operate_lsq_impl(ctx(), store_bw, load_bw);
  } else {
    champsim::bandwidth store_bw{SQ_WIDTH};
    champsim::bandwidth load_bw{LQ_WIDTH};
    long progress = 0;
    for (std::size_t i = 0; i < contexts.size() && (store_bw.has_remaining() || load_bw.has_remaining()); ++i) {
      const std::size_t c = (backend_round_robin + i) % contexts.size();
      progress += operate_lsq_impl(contexts[c], store_bw, load_bw);
    }
    return progress;
  }
}

long O3_CPU::operate_lsq_impl(champsim::core_context& c, champsim::bandwidth& store_bw, champsim::bandwidth& load_bw)
{
  const long before = store_bw.amount_consumed() + load_bw.amount_consumed();

  const auto complete_id = std::empty(c.ROB) ? std::numeric_limits<uint64_t>::max() : c.ROB.front().instr_id;
  auto do_complete = [time = current_time, finished = LSQ_ENTRY::precedes(complete_id), this](const auto& x) {
    return finished(x) && x.ready_time <= time && this->do_complete_store(x);
  };

  auto unfetched_begin = std::partition_point(std::begin(c.SQ), std::end(c.SQ), [](const auto& x) { return x.fetch_issued; });
  auto [fetch_begin, fetch_end] = champsim::get_span_p(unfetched_begin, std::end(c.SQ),
                                                       champsim::bandwidth{champsim::bandwidth::maximum_type{store_bw.amount_remaining()}},
                                                       [time = current_time](const auto& x) { return !x.fetch_issued && x.ready_time <= time; });
  store_bw.consume(std::distance(fetch_begin, fetch_end));
  std::for_each(fetch_begin, fetch_end, [time = current_time, this, &c](auto& sq_entry) {
    this->do_finish_store(c, sq_entry);
    sq_entry.fetch_issued = true;
    sq_entry.ready_time = time;
  });

  auto [complete_begin, complete_end] = champsim::get_span_p(std::cbegin(c.SQ), std::cend(c.SQ),
                                                             champsim::bandwidth{champsim::bandwidth::maximum_type{store_bw.amount_remaining()}}, do_complete);
  store_bw.consume(std::distance(complete_begin, complete_end));
  c.SQ.erase(complete_begin, complete_end);

  for (auto& lq_entry : c.LQ) {
    if (load_bw.has_remaining() && lq_entry.has_value() && lq_entry->producer_id == std::numeric_limits<uint64_t>::max() && !lq_entry->fetch_issued
        && lq_entry->ready_time < current_time) {
      auto success = execute_load(*lq_entry);
      if (success) {
        load_bw.consume();
        lq_entry->fetch_issued = true;
      }
    }
  }

  return (store_bw.amount_consumed() + load_bw.amount_consumed()) - before;
}

void O3_CPU::do_finish_store(const LSQ_ENTRY& sq_entry) { do_finish_store(ctx(), sq_entry); }

void O3_CPU::do_finish_store(champsim::core_context& c, const LSQ_ENTRY& sq_entry)
{
  if constexpr (champsim::debug_print) {
    fmt::print("[SQ] {} instr_id: {} vaddr: {}\n", __func__, sq_entry.instr_id, sq_entry.virtual_address);
  }

  sq_entry.finish(std::begin(c.ROB), std::end(c.ROB));

  // Release dependent loads
  for (std::optional<LSQ_ENTRY>& dependent : sq_entry.lq_depend_on_me) {
    CHAMPSIM_ASSERT(dependent.has_value()); // LQ entry is still allocated
    CHAMPSIM_ASSERT(dependent->producer_id == sq_entry.instr_id);
    CHAMPSIM_ASSERT(dependent->asid == sq_entry.asid); // a store forwards only to loads in its own address space

    dependent->finish(std::begin(c.ROB), std::end(c.ROB));
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
  // The load names itself as the only waiter on this request. The returned block
  // echoes this token end-to-end, so handle_memory_return can complete exactly
  // this LQ entry, even when another address space's load shares the same
  // virtual block. This is independent of instr_depend_on_me, which stays the
  // instruction-fetch dependency set.
  data_packet.load_waiters = {lq_entry.load_waiter_id};

  if constexpr (champsim::debug_print) {
    fmt::print("[LQ] {} instr_id: {} vaddr: {}\n", __func__, data_packet.instr_id, data_packet.v_address);
  }

  return L1D_bus.issue_read(data_packet);
}

void O3_CPU::do_complete_execution(champsim::core_context& c, ooo_model_instr& instr)
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
    impl_branch_execute_resolve(instr.instr_id, instr.ip, instr.branch_target, instr.branch_taken, instr.branch, instr.asid);
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
    resume_fetch_after_mispredict(c);
  }
}

// Restart fetch after a misprediction and charge the frozen interval, penalty
// included, to cycles_on_wrong_path.
void O3_CPU::resume_fetch_after_mispredict() { resume_fetch_after_mispredict(ctx()); }

void O3_CPU::resume_fetch_after_mispredict(champsim::core_context& c)
{
  c.fetch_resume_time = current_time + BRANCH_MISPREDICT_PENALTY;

  if (c.fetch_stalled_on_mispredict) {
    c.sim_stats.cycles_on_wrong_path += static_cast<uint64_t>((c.fetch_resume_time - c.fetch_stall_begin) / clock_period);
    c.fetch_stalled_on_mispredict = false;
  }
}

long O3_CPU::complete_inflight_instruction()
{
  if constexpr (champsim::defs::max_contexts == 1) {
    champsim::bandwidth complete_bw{EXEC_WIDTH};
    return complete_inflight_instruction_impl(ctx(), complete_bw);
  } else {
    // Shared scheduler (Stage 3.5): complete by global age, mirroring execute's
    // oldest-first issue. The writeback width is shared, so the globally oldest
    // executed instruction completes first, keyed on dispatch_sequence. With a
    // single context this is the in-order walk bit-for-bit.
    champsim::bandwidth complete_bw{EXEC_WIDTH};
    long progress = 0;
    while (complete_bw.has_remaining()) {
      champsim::core_context* best_c = nullptr;
      ooo_model_instr* best = nullptr;
      uint64_t best_seq = std::numeric_limits<uint64_t>::max();

      for (auto& c : contexts) {
        for (auto& instr : c.ROB) {
          if (instr.executed && !instr.completed && instr.ready_time <= current_time && instr.completed_mem_ops == instr.num_mem_ops()) {
            if (instr.dispatch_sequence < best_seq) {
              best_c = &c;
              best = &instr;
              best_seq = instr.dispatch_sequence;
            }
            break; // this context's oldest candidate; later entries are younger
          }
        }
      }

      if (best == nullptr)
        break;

      do_complete_execution(*best_c, *best);
      complete_bw.consume();
      ++progress;
    }
    return progress;
  }
}

long O3_CPU::complete_inflight_instruction_impl(champsim::core_context& c, champsim::bandwidth& complete_bw)
{
  const long before = complete_bw.amount_consumed();
  // update ROB entries with completed executions
  for (auto rob_it = std::begin(c.ROB); rob_it != std::end(c.ROB) && complete_bw.has_remaining(); ++rob_it) {
    if (rob_it->executed && !rob_it->completed && (rob_it->ready_time <= current_time) && rob_it->completed_mem_ops == rob_it->num_mem_ops()) {
      do_complete_execution(c, *rob_it);
      complete_bw.consume();
    }
  }

  return complete_bw.amount_consumed() - before;
}

long O3_CPU::handle_memory_return()
{
  long progress{0};

  if constexpr (champsim::defs::max_contexts == 1) {
    for (champsim::bandwidth fetch_bw{FETCH_WIDTH}, l1i_bw{L1I_BANDWIDTH};
         fetch_bw.has_remaining() && l1i_bw.has_remaining() && !L1I_bus.lower_level->returned.empty(); l1i_bw.consume()) {
      auto& l1i_entry = L1I_bus.lower_level->returned.front();

      while (fetch_bw.has_remaining() && !l1i_entry.instr_depend_on_me.empty()) {
        auto fetched = std::find_if(std::begin(ctx().IFETCH_BUFFER), std::end(ctx().IFETCH_BUFFER), ooo_model_instr::matches_id(l1i_entry.instr_depend_on_me.front()));
        if (fetched != std::end(ctx().IFETCH_BUFFER) && champsim::block_number{fetched->ip} == champsim::block_number{l1i_entry.v_address} && fetched->fetch_issued) {
          CHAMPSIM_ASSERT(fetched->asid[0] == l1i_entry.asid[0] && fetched->asid[1] == l1i_entry.asid[1]); // fetch completion stays within one address space
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
  } else {
    // Stage 3.4: a returned L1I line names its dependents by instr_id, which may
    // live in any context's IFETCH_BUFFER; search them all. FETCH_WIDTH and
    // L1I_BANDWIDTH stay shared across contexts.
    for (champsim::bandwidth fetch_bw{FETCH_WIDTH}, l1i_bw{L1I_BANDWIDTH};
         fetch_bw.has_remaining() && l1i_bw.has_remaining() && !L1I_bus.lower_level->returned.empty(); l1i_bw.consume()) {
      auto& l1i_entry = L1I_bus.lower_level->returned.front();

      while (fetch_bw.has_remaining() && !l1i_entry.instr_depend_on_me.empty()) {
        bool matched = false;
        for (auto& c : contexts) {
          auto fetched = std::find_if(std::begin(c.IFETCH_BUFFER), std::end(c.IFETCH_BUFFER), ooo_model_instr::matches_id(l1i_entry.instr_depend_on_me.front()));
          if (fetched != std::end(c.IFETCH_BUFFER) && champsim::block_number{fetched->ip} == champsim::block_number{l1i_entry.v_address} && fetched->fetch_issued) {
            CHAMPSIM_ASSERT(fetched->asid[0] == l1i_entry.asid[0] && fetched->asid[1] == l1i_entry.asid[1]); // fetch completion stays within one address space
            fetched->fetch_completed = true;
            matched = true;
            break;
          }
        }

        if (matched) {
          fetch_bw.consume();
          ++progress;

          if constexpr (champsim::debug_print) {
            fmt::print("[IFETCH] {} instr_id: {} fetch completed\n", __func__, l1i_entry.instr_depend_on_me.front());
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
  }

  auto l1d_it = std::begin(L1D_bus.lower_level->returned);
  for (champsim::bandwidth l1d_bw{L1D_BANDWIDTH}; l1d_bw.has_remaining() && l1d_it != std::end(L1D_bus.lower_level->returned); l1d_bw.consume(), ++l1d_it) {
    // A returned L1D line completes every issued load that shares its block and
    // address space, whichever context owns that load (block+asid completion, as
    // before Stage 3). Each load and its ROB entry live in the same context.
    for (auto& c : contexts) {
      for (auto& lq_entry : c.LQ) {
        if (lq_entry.has_value() && lq_entry->fetch_issued
            && champsim::block_number{lq_entry->virtual_address} == champsim::block_number{l1d_it->v_address} && lq_entry->asid[0] == l1d_it->asid[0]
            && lq_entry->asid[1] == l1d_it->asid[1]) {
          lq_entry->finish(std::begin(c.ROB), std::end(c.ROB));
          lq_entry.reset();
          ++progress;
        }
      }
    }
    ++progress;
  }
  L1D_bus.lower_level->returned.erase(std::begin(L1D_bus.lower_level->returned), l1d_it);

  return progress;
}

long O3_CPU::retire_rob()
{
  if constexpr (champsim::defs::max_contexts == 1) {
    champsim::bandwidth retire_bw{RETIRE_WIDTH};
    return retire_rob_impl(ctx(), retire_bw);
  } else {
    // Independent in-order prefixes: each context retires its own completed
    // prefix, sharing one aggregate RETIRE_WIDTH with a rotating start.
    champsim::bandwidth retire_bw{RETIRE_WIDTH};
    long progress = 0;
    for (std::size_t i = 0; i < contexts.size() && retire_bw.has_remaining(); ++i) {
      const std::size_t c = (backend_round_robin + i) % contexts.size();
      progress += retire_rob_impl(contexts[c], retire_bw);
    }
    return progress;
  }
}

long O3_CPU::retire_rob_impl(champsim::core_context& c, champsim::bandwidth& retire_bw)
{
  auto [retire_begin, retire_end] = champsim::get_span_p(std::cbegin(c.ROB), std::cend(c.ROB),
                                                         champsim::bandwidth{champsim::bandwidth::maximum_type{retire_bw.amount_remaining()}},
                                                         [](const auto& x) { return x.completed; });
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
      c.retire_dest_register(dreg);
    }
  }

  uint64_t cycles = current_time.time_since_epoch() / clock_period;
  handle_event<Event::RETIRE>(cpu, retire_begin, retire_end, cycles);

  auto retire_count = std::distance(retire_begin, retire_end);
  c.num_retired += retire_count;
  retire_bw.consume(retire_count);
  c.ROB.erase(retire_begin, retire_end);

  return retire_count;
}

void O3_CPU::impl_initialize_branch_predictor() const { branch_module_pimpl->impl_initialize_branch_predictor(); }

void O3_CPU::impl_branch_predictor_final_stats() const { branch_module_pimpl->impl_branch_predictor_final_stats(); }

void O3_CPU::impl_branch_execute_resolve(uint64_t instr_id, champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type,
                                         champsim::address_space_id asid) const
{
  branch_module_pimpl->impl_branch_execute_resolve(instr_id, ip, branch_target, taken, branch_type, asid);
}

void O3_CPU::impl_branch_decode_notify(uint64_t instr_id, uint8_t arch_dst_reg) const
{
  branch_module_pimpl->impl_branch_decode_notify(instr_id, arch_dst_reg);
}

void O3_CPU::impl_branch_execute_notify(uint64_t instr_id, bool has_value, uint64_t value) const
{
  branch_module_pimpl->impl_branch_execute_notify(instr_id, has_value, value);
}

void O3_CPU::impl_last_branch_result(champsim::address ip, champsim::address target, bool taken, uint8_t branch_type, champsim::address_space_id asid) const
{
  branch_module_pimpl->impl_last_branch_result(ip, target, taken, branch_type, asid);
}

bool O3_CPU::impl_predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type,
                                 champsim::address_space_id asid) const
{
  return branch_module_pimpl->impl_predict_branch(ip, predicted_target, always_taken, branch_type, asid);
}

void O3_CPU::impl_initialize_btb() const { btb_module_pimpl->impl_initialize_btb(); }

void O3_CPU::impl_update_btb(champsim::address ip, champsim::address predicted_target, bool taken, uint8_t branch_type, champsim::context_index context,
                             champsim::address_space_id asid) const
{
  btb_module_pimpl->impl_update_btb(ip, predicted_target, taken, branch_type, context, asid);
}

std::pair<champsim::address, bool> O3_CPU::impl_btb_prediction(champsim::address ip, uint8_t branch_type, champsim::context_index context,
                                                               champsim::address_space_id asid) const
{
  return btb_module_pimpl->impl_btb_prediction(ip, branch_type, context, asid);
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
  CHAMPSIM_ASSERT(rob_entry.asid == this->asid); // memory completion stays within one address space

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
