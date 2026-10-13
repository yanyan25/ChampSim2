#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>

#include <catch.hpp>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Stage 5.1b: a genuinely undersized register file is a named config error even
// with two contexts. The waterline reserves each sibling's next rename, so when
// every context's oldest instruction is blocked and no instruction is in flight
// anywhere (renamed but not yet retired), no context can retire to free a
// register and the stall is permanent. The scheduler must throw the same
// "register_file_size is too small" error, now stating the combined requirement
// N >= committed + sum(need_k), instead of leaving the generic DEADLOCK guard to
// abort. When a sibling does have an instruction in flight, the scheduler must
// keep waiting (that instruction may retire and free a register) rather than
// throw.

namespace
{
ooo_model_instr instruction(uint64_t id, std::initializer_list<int16_t> sources, std::initializer_list<int16_t> destinations)
{
  auto instr = champsim::test::instruction_with_ip(id);
  instr.instr_id = id;
  instr.source_registers.assign(sources);
  instr.destination_registers.assign(destinations);
  instr.ready_time = champsim::chrono::clock::time_point{};
  return instr;
}

struct undersized_fixture {
  do_nothing_MRC l1i, l1d;
  O3_CPU dual{champsim::core_builder{}
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
                 .num_contexts(2)
                 .rob_size(32)
                 .register_file_size(4)
                 .schedule_width(champsim::bandwidth::maximum_type{128})};

  undersized_fixture() { dual.backend_round_robin = 0; }

  long free_registers() const { return static_cast<long>(dual.contexts[0].reg_allocator->count_free_registers()); }

  long scheduled_count(std::size_t c) const
  {
    return static_cast<long>(std::count_if(std::begin(dual.contexts[c].ROB), std::end(dual.contexts[c].ROB), [](const auto& i) { return i.scheduled; }));
  }
};
} // namespace

TEST_CASE_METHOD(undersized_fixture, "Two blocked heads with nothing in flight are a named undersized error")
{
  // Each instruction renames three destinations, so need_0 = need_1 = 3 and the
  // four-register file cannot cover sum(need_k) = 6. Nothing is in flight, so
  // nothing can retire to free a register: a config error, not a wait.
  dual.contexts[0].ROB.push_back(instruction(42, {}, {1, 2, 3}));
  dual.contexts[1].ROB.push_back(instruction(43, {}, {4, 5, 6}));

  REQUIRE_THROWS_AS(dual.schedule_instruction(), std::runtime_error);
  REQUIRE_THROWS_WITH(
      dual.schedule_instruction(),
      Catch::Matchers::ContainsSubstring("register_file_size = 4 is too small")
          && Catch::Matchers::ContainsSubstring("needs 3 registers to rename, with 4 free; the other 0 hold")
          && Catch::Matchers::ContainsSubstring("2 contexts together need 6 more registers")
          && Catch::Matchers::ContainsSubstring("at least 0 + 6 = 6"));
  REQUIRE_FALSE(dual.contexts[0].ROB.front().scheduled);
  REQUIRE_FALSE(dual.contexts[1].ROB.front().scheduled);
  REQUIRE(free_registers() == 4);
}

TEST_CASE_METHOD(undersized_fixture, "A blocked head waits while a sibling has an instruction in flight")
{
  // Context 0 renames three one-destination writers, leaving one register free;
  // context 1's oldest then needs two (its own plus context 0's reservation) and
  // is blocked -- but context 0 has three instructions in flight that may retire
  // and free registers, so this is waterline throttling, not an undersized error.
  dual.contexts[0].ROB.push_back(instruction(1, {}, {10}));
  dual.contexts[0].ROB.push_back(instruction(2, {}, {11}));
  dual.contexts[0].ROB.push_back(instruction(3, {}, {12}));
  dual.contexts[1].ROB.push_back(instruction(100, {}, {13}));

  REQUIRE_NOTHROW(dual.schedule_instruction());

  REQUIRE(scheduled_count(0) == 3);
  REQUIRE(scheduled_count(1) == 0);
  REQUIRE(free_registers() == 1);
}
