#include <cstddef>
#include <deque>
#include <vector>

#include <catch.hpp>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Stage 3.5: back-end arbitration. With two hardware contexts the shared back-end
// widths are partitioned each cycle by the same D4 policy as the front end:
//   * schedule/retire/LSQ -- rotating start, one shared width;
//   * execute/complete -- issue by global age (dispatch_sequence), not rotation,
//     because the reservation station is shared and age-priority is the standard
//     SMT issue policy;
//   * the scheduler holds an aggregate reservation-station occupancy (the count of
//     instructions scheduled but not yet executed) that must never exceed
//     SCHEDULER_SIZE, because a per-cycle search window that worked for one
//     context would let two threads each fill it;
//   * dispatch stamps a core-wide monotonic sequence number -- the only age key
//     that orders two instructions across contexts, since instr_id restarts at 0
//     in every trace.
//
// The file pins three things: (L1) a context whose sibling is idle replays its
// architectural event stream bit-for-bit against a solo core; (L2) the shared
// scheduler caps aggregate scheduled-not-executed at SCHEDULER_SIZE; and the
// dispatch sequence is core-wide monotonic. A fourth case pins retirement's
// independent in-order prefixes under one shared width.

namespace
{
constexpr long SCHED_W = 4;
constexpr long EXEC_W = 4;
constexpr long RETIRE_W = 4;
constexpr long DISPATCH_W = 4;

struct backend_fixture {
  do_nothing_MRC solo_l1i, solo_l1d;
  O3_CPU solo{champsim::core_builder{}
                 .fetch_queues(&solo_l1i.queues)
                 .data_queues(&solo_l1d.queues)
                 .num_contexts(1)
                 .rob_size(16)
                 .register_file_size(128)
                 .lq_size(8)
                 .sq_size(8)
                 .schedule_width(champsim::bandwidth::maximum_type{SCHED_W})
                 .execute_width(champsim::bandwidth::maximum_type{EXEC_W})
                 .retire_width(champsim::bandwidth::maximum_type{RETIRE_W})
                 .dispatch_width(champsim::bandwidth::maximum_type{DISPATCH_W})
                 .execute_latency(0)
                 .schedule_latency(0)};

  do_nothing_MRC dual_l1i, dual_l1d;
  O3_CPU dual{champsim::core_builder{}
                 .fetch_queues(&dual_l1i.queues)
                 .data_queues(&dual_l1d.queues)
                 .num_contexts(2)
                 .rob_size(16)
                 .register_file_size(128)
                 .lq_size(8)
                 .sq_size(8)
                 .schedule_width(champsim::bandwidth::maximum_type{SCHED_W})
                 .execute_width(champsim::bandwidth::maximum_type{EXEC_W})
                 .retire_width(champsim::bandwidth::maximum_type{RETIRE_W})
                 .dispatch_width(champsim::bandwidth::maximum_type{DISPATCH_W})
                 .execute_latency(0)
                 .schedule_latency(0)};

  backend_fixture()
  {
    for (auto* cpu : {&solo, &dual}) {
      cpu->warmup = true;
      cpu->current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};
    }
  }

  // A ready, unscheduled instruction that writes one architectural register and
  // reads none. Independent instructions like these all become schedulable in one
  // cycle and exercise the destination-register rename path.
  static ooo_model_instr ready_instr(uint64_t id, uint8_t reg)
  {
    auto instr = champsim::test::instruction_with_ip(id * 4);
    instr.instr_id = id;
    instr.destination_registers = {static_cast<PHYSICAL_REGISTER_ID>(reg)};
    instr.ready_time = champsim::chrono::clock::time_point::min();
    return instr;
  }

  // A serial chain: every instruction after the first reads register 7, which the
  // previous one wrote. This exercises rename_src_register, rename_dest_register
  // and retire_dest_register (the PRF retire accounting) in one stream.
  static std::deque<ooo_model_instr> dependent_chain(std::size_t n)
  {
    std::deque<ooo_model_instr> rob;
    for (std::size_t i = 0; i < n; ++i) {
      auto instr = ready_instr(i, 7);
      if (i != 0) {
        instr.source_registers = {7};
      }
      rob.push_back(std::move(instr));
    }
    return rob;
  }

  long scheduler_occupancy(const O3_CPU& cpu) const
  {
    long occupied = 0;
    for (const auto& c : cpu.contexts)
      occupied += static_cast<long>(std::count_if(std::begin(c.ROB), std::end(c.ROB), [](const auto& x) { return x.scheduled && !x.executed; }));
    return occupied;
  }
};
} // namespace

TEST_CASE_METHOD(backend_fixture, "A context replays its solo architectural event stream when its sibling is idle")
{
  // The same dependent chain into the solo core and into context 0 of the dual
  // core; context 1 is fed nothing. Every back-end stage must therefore produce
  // the same architectural events in context 0 as in the solo core: the same
  // retirement count and order, the same schedule/execute/complete flags, and the
  // same physical-register renaming.
  solo.ctx().ROB = dependent_chain(8);
  dual.contexts[0].ROB = dependent_chain(8);

  for (int cycle = 0; cycle < 20; ++cycle) {
    solo.operate();
    dual.operate();

    CAPTURE(cycle);
    CHECK(dual.contexts[0].num_retired == solo.ctx().num_retired);
    REQUIRE(dual.contexts[0].ROB.size() == solo.ctx().ROB.size());
    for (std::size_t i = 0; i < solo.ctx().ROB.size(); ++i) {
      CAPTURE(i);
      const auto& s = solo.ctx().ROB[i];
      const auto& d = dual.contexts[0].ROB[i];
      CHECK(d.instr_id == s.instr_id);
      CHECK(d.scheduled == s.scheduled);
      CHECK(d.executed == s.executed);
      CHECK(d.completed == s.completed);
      CHECK(d.destination_registers == s.destination_registers);
      CHECK(d.source_registers == s.source_registers);
    }
  }

  // The chain fully drained in both, and the architectural register 7 ended up
  // mapped to the same physical register on both sides.
  CHECK(solo.ctx().num_retired == 8);
  CHECK(dual.contexts[0].num_retired == 8);
  CHECK(dual.contexts[0].backend_RAT[7] == solo.ctx().backend_RAT[7]);
  CHECK(dual.contexts[0].frontend_RAT[7] == solo.ctx().frontend_RAT[7]);
}

TEST_CASE_METHOD(backend_fixture, "The shared scheduler never exceeds SCHEDULER_SIZE scheduled-not-executed")
{
  // Two hungry contexts, each with more ready instructions than the whole
  // reservation station. A shared per-cycle search window would let each context
  // schedule SCHED_W for an aggregate of 2*SCHED_W; the shared occupancy gate must
  // hold the aggregate at SCHED_W.
  for (std::size_t ctx : {0u, 1u}) {
    for (std::size_t i = 0; i < 8; ++i) {
      dual.contexts[ctx].ROB.push_back(ready_instr(1000 * ctx + i, static_cast<uint8_t>(i + 1)));
    }
  }

  dual.schedule_instruction();

  CHECK(scheduler_occupancy(dual) == SCHED_W);
}

TEST_CASE_METHOD(backend_fixture, "dispatch_sequence is a core-wide monotonic age key across contexts")
{
  auto fill_dispatch = [this](std::size_t ctx, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      auto instr = champsim::test::instruction_with_ip(0x2000 * (ctx + 1) + 4 * i);
      instr.instr_id = 1000 * ctx + i;
      instr.ready_time = champsim::chrono::clock::time_point::min();
      dual.contexts[ctx].DISPATCH_BUFFER.push_back(std::move(instr));
    }
  };

  fill_dispatch(0, DISPATCH_W);
  dual.dispatch_owner = 0;
  dual.dispatch_instruction();
  REQUIRE(dual.contexts[0].ROB.size() == static_cast<std::size_t>(DISPATCH_W));

  fill_dispatch(1, DISPATCH_W);
  dual.dispatch_owner = 1;
  dual.dispatch_instruction();
  REQUIRE(dual.contexts[1].ROB.size() == static_cast<std::size_t>(DISPATCH_W));

  // The counter did not restart for the second context: context 0 took 0..3, so
  // context 1 continues at 4. Within each context instr_id still orders entries;
  // across contexts only dispatch_sequence does.
  for (std::size_t i = 0; i < static_cast<std::size_t>(DISPATCH_W); ++i) {
    CAPTURE(i);
    CHECK(dual.contexts[0].ROB[i].dispatch_sequence == i);
    CHECK(dual.contexts[1].ROB[i].dispatch_sequence == static_cast<uint64_t>(DISPATCH_W) + i);
  }
}

TEST_CASE_METHOD(backend_fixture, "Retirement shares one width across contexts with independent in-order prefixes")
{
  auto completed_instr = [](uint64_t id) {
    auto instr = champsim::test::instruction_with_ip(id * 4);
    instr.instr_id = id;
    instr.scheduled = true;
    instr.executed = true;
    instr.completed = true;
    instr.ready_time = champsim::chrono::clock::time_point::min();
    return instr; // no destination registers: retire touches no physical register
  };

  dual.backend_round_robin = 0;
  // context 0 retires an in-order prefix of 2 (the completed entry behind the
  // not-completed one stays put); context 1 has a prefix of 3 but only the
  // remaining width of 2.
  auto head_of_context_0 = completed_instr(2);
  head_of_context_0.completed = false;
  dual.contexts[0].ROB = {completed_instr(0), completed_instr(1), head_of_context_0, completed_instr(3)};
  dual.contexts[1].ROB = {completed_instr(10), completed_instr(11), completed_instr(12)};

  const auto progress = dual.retire_rob();

  CHECK(progress == RETIRE_W);
  CHECK(dual.contexts[0].num_retired == 2);
  CHECK(dual.contexts[1].num_retired == 2);
  REQUIRE(dual.contexts[0].ROB.size() == 2);
  REQUIRE(dual.contexts[1].ROB.size() == 1);
  CHECK(dual.contexts[0].ROB.front().instr_id == 2); // the not-completed head stayed
  CHECK(dual.contexts[1].ROB.front().instr_id == 12);
}
