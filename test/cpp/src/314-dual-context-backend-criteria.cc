#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include <catch.hpp>

#include "address.h"
#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Stage 3.7 item 3: the five dual-context criteria that only a real two-context
// case can pin. Each TEST_CASE drives both contexts live and asserts the per-context
// architectural result, so a cross-context age key or a per-context structure that
// silently fell back to a single shared one fails here rather than passing on a
// replay against an empty sibling.
//
//   (1) two contexts writing the same architectural register get independent RAT
//       entries;
//   (2) one context's outstanding miss does not block the other's issue or retire;
//   (3) one context's unretired store is not mistaken for the other's ROB frontier;
//   (4) oldest-first is fair: two live contexts both drain to retirement;
//   (5) global oldest-first issues in a different order than a rotating walk.

namespace
{
constexpr long SCHED_W = 4;
constexpr long EXEC_W = 1; // one issue/completion slot per cycle so ORDER is observable
constexpr long RETIRE_W = 4;
constexpr long DISPATCH_W = 4;

struct dual_criteria_fixture {
  do_nothing_MRC l1i, l1d;
  O3_CPU dual{champsim::core_builder{}
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
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

  dual_criteria_fixture()
  {
    dual.warmup = true;
    dual.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};
  }

  // A ready, unscheduled instruction that writes one architectural register and
  // reads none. schedule_instruction renames its destination on the way through.
  static ooo_model_instr ready_instr(uint64_t id, uint8_t reg)
  {
    auto instr = champsim::test::instruction_with_ip(id * 4);
    instr.instr_id = id;
    instr.destination_registers = {static_cast<PHYSICAL_REGISTER_ID>(reg)};
    instr.ready_time = champsim::chrono::clock::time_point::min();
    return instr;
  }

  // A ready instruction with no destination register, already renamed. Marking
  // `scheduled` directly keeps it out of the rename path, so execute/complete/retire
  // can be driven without touching the physical register file.
  static ooo_model_instr ready_no_dest(uint64_t id)
  {
    auto instr = champsim::test::instruction_with_ip(id * 4);
    instr.instr_id = id;
    instr.scheduled = true;
    instr.ready_time = champsim::chrono::clock::time_point::min();
    return instr;
  }

  static ooo_model_instr completed_no_dest(uint64_t id)
  {
    auto instr = ready_no_dest(id);
    instr.executed = true;
    instr.completed = true;
    return instr;
  }
};
} // namespace

TEST_CASE_METHOD(dual_criteria_fixture, "Two contexts writing the same architectural register get independent RAT entries")
{
  dual.contexts[0].ROB = {ready_instr(0, 7)};
  dual.contexts[1].ROB = {ready_instr(1, 7)};

  dual.schedule_instruction();

  REQUIRE(dual.contexts[0].ROB.front().scheduled);
  REQUIRE(dual.contexts[1].ROB.front().scheduled);
  const auto p0 = dual.contexts[0].frontend_RAT[7];
  const auto p1 = dual.contexts[1].frontend_RAT[7];
  CHECK(p0 != -1);
  CHECK(p1 != -1);
  CHECK(p0 != p1); // a shared RAT would have been overwritten to one value
}

TEST_CASE_METHOD(dual_criteria_fixture, "A sibling's outstanding miss does not block the other context's issue and retire")
{
  auto a_load = champsim::test::instruction_with_ip(0x1000);
  a_load.instr_id = 0;
  a_load.source_memory = {champsim::address{0x4000}}; // one memory operand: the miss
  a_load.scheduled = true;
  a_load.executed = true;
  a_load.completed_mem_ops = 0; // its data never returns
  a_load.ready_time = champsim::chrono::clock::time_point::min();

  dual.contexts[0].ROB = {a_load};
  dual.contexts[1].ROB = {ready_instr(100, 3)};

  dual.schedule_instruction(); // renames the sibling's register-writing instruction
  REQUIRE(dual.contexts[1].ROB.front().scheduled);

  dual.execute_instruction();
  CHECK(dual.contexts[1].ROB.front().executed); // sibling issued despite the stuck miss
  CHECK_FALSE(dual.contexts[0].ROB.front().completed);

  dual.complete_inflight_instruction();
  CHECK(dual.contexts[1].ROB.front().completed);
  CHECK_FALSE(dual.contexts[0].ROB.front().completed); // the miss still holds it up

  dual.retire_rob();
  CHECK(dual.contexts[1].num_retired == 1);
  CHECK(dual.contexts[1].ROB.empty());
  REQUIRE(dual.contexts[0].ROB.size() == 1);
  CHECK_FALSE(dual.contexts[0].ROB.front().completed);
}

TEST_CASE_METHOD(dual_criteria_fixture, "A sibling's unretired store is not mistaken for the other context's ROB frontier")
{
  auto a_store = champsim::test::instruction_with_ip(0x3000);
  a_store.instr_id = 7;
  a_store.destination_memory = {champsim::address{0x5000}}; // the store target
  a_store.scheduled = true;
  a_store.executed = true;
  a_store.completed = false;
  a_store.completed_mem_ops = 0; // its store data has not retired
  a_store.ready_time = champsim::chrono::clock::time_point::min();

  dual.backend_round_robin = 0;
  dual.contexts[0].ROB = {a_store, completed_no_dest(8)};
  dual.contexts[1].ROB = {completed_no_dest(9)};

  dual.retire_rob();

  // The sibling retires its own completed head...
  CHECK(dual.contexts[1].num_retired == 1);
  CHECK(dual.contexts[1].ROB.empty());
  // ...while the peer's unretired store blocks only the peer's own prefix.
  REQUIRE(dual.contexts[0].ROB.size() == 2);
  CHECK(dual.contexts[0].num_retired == 0);
  CHECK(dual.contexts[0].ROB.front().instr_id == 7);
  CHECK_FALSE(dual.contexts[0].ROB.front().completed);
  CHECK(dual.contexts[0].ROB.front().destination_memory.size() == 1);
}

TEST_CASE_METHOD(dual_criteria_fixture, "Oldest-first is fair: an older sibling is not starved behind a peer's younger batch")
{
  // One older ready instruction in context 1, four younger ready instructions in
  // context 0. Oldest-first must issue the older sibling first; a fixed-priority
  // or rotating walk that favours context 0 would push it behind four younger
  // peers -- starving older work for younger -- instead of issuing it now.
  auto c1 = ready_no_dest(20);
  c1.dispatch_sequence = 0;
  dual.contexts[1].ROB = {c1};
  for (int i = 0; i < 4; ++i) {
    auto c0 = ready_no_dest(100 + i);
    c0.dispatch_sequence = static_cast<uint64_t>(100 + i);
    dual.contexts[0].ROB.push_back(c0);
  }

  dual.backend_round_robin = 0; // a rotating walk would start at context 0
  dual.execute_instruction();   // EXEC_W == 1: a single issue slot this cycle

  CHECK(dual.contexts[1].ROB.front().executed); // the older sibling got the slot
  CHECK_FALSE(dual.contexts[0].ROB.front().executed);
}

TEST_CASE_METHOD(dual_criteria_fixture, "Global oldest-first issues in a different order than a rotating walk")
{
  // Sequences 0,3 in context 0 and 1,2 in context 1: the globally-oldest order is
  // 0,1,2,3, which crosses contexts twice (ctx0, ctx1, ctx1, ctx0). A rotating
  // per-context walk would instead emit 0,1,3,2. Assert the global-age order.
  auto c0a = ready_no_dest(10);
  c0a.dispatch_sequence = 0;
  auto c0b = ready_no_dest(11);
  c0b.dispatch_sequence = 3;
  auto c1a = ready_no_dest(20);
  c1a.dispatch_sequence = 1;
  auto c1b = ready_no_dest(21);
  c1b.dispatch_sequence = 2;

  dual.backend_round_robin = 0;
  dual.contexts[0].ROB = {c0a, c0b};
  dual.contexts[1].ROB = {c1a, c1b};

  std::vector<uint64_t> order;
  for (int i = 0; i < 4; ++i) {
    dual.execute_instruction(); // EXEC_W == 1: exactly one issues per call
    for (auto& c : dual.contexts)
      for (auto& instr : c.ROB)
        if (instr.executed && std::find(std::begin(order), std::end(order), instr.instr_id) == std::end(order))
          order.push_back(instr.instr_id);
  }

  CHECK(order == std::vector<uint64_t>{10, 20, 21, 11});
}
