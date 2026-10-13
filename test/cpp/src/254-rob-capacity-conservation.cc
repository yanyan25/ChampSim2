#include <cstddef>
#include <utility>

#include <catch.hpp>

#include "address.h"
#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Stage 5.2b: ROB total-capacity conservation. Before this change every context
// took a whole ROB_SIZE, so two contexts could hold 2 x ROB_SIZE in flight --
// a machine no real hardware can build. The core now conserves one ROB_SIZE
// budget across its contexts: under the `equal` partition each context gets a
// fixed ROB_SIZE / num_contexts quota, and the aggregate can never exceed
// ROB_SIZE. This file pins the formula directly (rob_capacity), the aggregate
// invariant while two contexts dispatch, and the N=1 collapse to the whole
// budget.

namespace
{
constexpr long DISPATCH_W = 4;
constexpr std::size_t ROB_SIZE = 16;
constexpr std::size_t HALF_ROB = ROB_SIZE / 2; // the equal quota with two contexts

struct dual_rob_fixture {
  do_nothing_MRC l1i, l1d;
  O3_CPU cpu{champsim::core_builder{}
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
                 .num_contexts(2)
                 .rob_size(ROB_SIZE)
                 .dispatch_width(champsim::bandwidth::maximum_type{DISPATCH_W})
                 .retire_width(champsim::bandwidth::maximum_type{0})};

  dual_rob_fixture()
  {
    cpu.warmup = true;
    cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};
  }

  void fill_dispatch(std::size_t context, std::size_t n)
  {
    auto& c = cpu.contexts[context];
    for (std::size_t i = 0; i < n; ++i) {
      auto instr = champsim::test::instruction_with_ip(champsim::address{static_cast<uint64_t>(0x2000 * (context + 1) + 4 * i)});
      instr.instr_id = static_cast<uint64_t>(2000 * context + i);
      instr.ready_time = champsim::chrono::clock::time_point::min();
      c.DISPATCH_BUFFER.push_back(std::move(instr));
    }
  }

  std::size_t rob_size(std::size_t context) const { return std::size(cpu.contexts[context].ROB); }
  std::size_t aggregate_rob() const { return rob_size(0) + rob_size(1); }
};

struct single_rob_fixture {
  do_nothing_MRC l1i, l1d;
  O3_CPU cpu{champsim::core_builder{}
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
                 .num_contexts(1)
                 .rob_size(ROB_SIZE)
                 .dispatch_width(champsim::bandwidth::maximum_type{DISPATCH_W})};

  single_rob_fixture()
  {
    cpu.warmup = true;
    cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};
  }

  void fill_dispatch(std::size_t n)
  {
    auto& c = cpu.contexts[0];
    for (std::size_t i = 0; i < n; ++i) {
      auto instr = champsim::test::instruction_with_ip(champsim::address{static_cast<uint64_t>(0x2000 + 4 * i)});
      instr.instr_id = static_cast<uint64_t>(i);
      instr.ready_time = champsim::chrono::clock::time_point::min();
      c.DISPATCH_BUFFER.push_back(std::move(instr));
    }
  }

  std::size_t rob_size() const { return std::size(cpu.contexts[0].ROB); }
};
} // namespace

TEST_CASE("rob_capacity is the equal partition of ROB_SIZE")
{
  do_nothing_MRC l1i, l1d;

  O3_CPU single{champsim::core_builder{}.fetch_queues(&l1i.queues).data_queues(&l1d.queues).num_contexts(1).rob_size(16)};
  REQUIRE(single.rob_capacity() == 16); // 16 / 1 == ROB_SIZE

  O3_CPU dual{champsim::core_builder{}.fetch_queues(&l1i.queues).data_queues(&l1d.queues).num_contexts(2).rob_size(16)};
  REQUIRE(dual.rob_capacity() == 8); // 16 / 2

  O3_CPU odd{champsim::core_builder{}.fetch_queues(&l1i.queues).data_queues(&l1d.queues).num_contexts(2).rob_size(15)};
  REQUIRE(odd.rob_capacity() == 7); // floor(15 / 2), so 7 + 7 <= 15 still conserves the budget
}

TEST_CASE_METHOD(dual_rob_fixture, "Two contexts cannot together exceed one ROB_SIZE budget")
{
  fill_dispatch(0, 32);
  fill_dispatch(1, 32);

  // Drive dispatch until both contexts stall at their equal quota. Before the
  // conservation change this reached 32 in flight; now the aggregate is pinned
  // at one ROB_SIZE and each context at half.
  for (int step = 0; step < 16; ++step) {
    cpu.dispatch_instruction();
    CHECK(aggregate_rob() <= ROB_SIZE);
    CHECK(rob_size(0) <= HALF_ROB);
    CHECK(rob_size(1) <= HALF_ROB);
  }

  REQUIRE(rob_size(0) == HALF_ROB);
  REQUIRE(rob_size(1) == HALF_ROB);
  REQUIRE(aggregate_rob() == ROB_SIZE);
}

TEST_CASE_METHOD(single_rob_fixture, "A single context keeps the whole ROB_SIZE budget")
{
  fill_dispatch(32);

  for (int step = 0; step < 8; ++step)
    cpu.dispatch_instruction();

  REQUIRE(rob_size() == ROB_SIZE); // ROB_SIZE / 1 == ROB_SIZE, not halved
}
