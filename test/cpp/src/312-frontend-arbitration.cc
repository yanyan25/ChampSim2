#include <algorithm>
#include <cstddef>
#include <catch.hpp>

#include "defaults.hpp"
#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Stage 3.4: front-end arbitration. With two hardware contexts the shared
// front-end widths are partitioned each cycle by the D4 policy:
//   * fetch/decode/DIB -- rotating start, work-conserving: a context that
//     cannot use its share hands it to the sibling, so no width is wasted;
//   * dispatch -- one context owns the round, but a stalled owner yields the
//     remaining width to the sibling rather than burning it.
//
// This file pins the two invariants the acceptance criterion names: the
// aggregate events in one cycle never exceed the shared width, and a pair of
// hungry contexts uses the whole width (utilization). The work-conserving shape
// is pinned through check_dib() (a FETCH_WIDTH budget point that needs no branch
// predictor, so the arbitration is what is under test); the exclusive-round shape
// is pinned through dispatch_instruction().

namespace
{
constexpr long FETCH_W = 4;
constexpr long DISPATCH_W = 4;

struct dual_context_fixture {
  do_nothing_MRC l1i, l1d;
  O3_CPU cpu{champsim::core_builder{champsim::defaults::default_core}
                 .num_contexts(2)
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
                 .fetch_width(champsim::bandwidth::maximum_type{FETCH_W})
                 .dispatch_width(champsim::bandwidth::maximum_type{DISPATCH_W})
                 .rob_size(16)
                 .retire_width(champsim::bandwidth::maximum_type{0})};

  dual_context_fixture()
  {
    cpu.warmup = true;
    cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};
  }

  void fill_ifetch(std::size_t context, std::size_t n)
  {
    auto& c = cpu.contexts[context];
    for (std::size_t i = 0; i < n; ++i) {
      auto instr = champsim::test::instruction_with_ip(champsim::address{static_cast<uint64_t>(0x1000 * (context + 1) + 4 * i)});
      instr.instr_id = static_cast<uint64_t>(1000 * context + i);
      c.IFETCH_BUFFER.push_back(std::move(instr));
    }
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

  std::size_t dib_checked(std::size_t context) const
  {
    return static_cast<std::size_t>(std::count_if(std::begin(cpu.contexts[context].IFETCH_BUFFER), std::end(cpu.contexts[context].IFETCH_BUFFER),
                                                  [](const auto& x) { return x.dib_checked; }));
  }

  std::size_t rob_size(std::size_t context) const { return std::size(cpu.contexts[context].ROB); }
};
} // namespace

TEST_CASE_METHOD(dual_context_fixture, "Fetch bandwidth is fully used by two hungry contexts")
{
  fill_ifetch(0, 8);
  fill_ifetch(1, 8);

  const auto progress = cpu.check_dib();

  // Both contexts have work and the shared width is 4, so the aggregate is the
  // whole width -- no waste.
  CHECK(progress == FETCH_W);
  CHECK(dib_checked(0) + dib_checked(1) == static_cast<std::size_t>(FETCH_W));
}

TEST_CASE_METHOD(dual_context_fixture, "Fetch serves the rotating-start context first and recycles the remainder")
{
  fill_ifetch(0, 8);
  fill_ifetch(1, 1); // only one un-checked instruction in the rotating-start context
  cpu.fetch_round_robin = 1;

  const auto progress = cpu.check_dib();

  // Context 1 is the round-robin start and is served first; work-conserving
  // hands the rest of the width to context 0, so the width is still full.
  CHECK(progress == FETCH_W);
  CHECK(dib_checked(1) == 1);
  CHECK(dib_checked(0) == static_cast<std::size_t>(FETCH_W) - 1);
}

TEST_CASE_METHOD(dual_context_fixture, "Dispatch gives the owner an exclusive round")
{
  fill_dispatch(0, 8);
  fill_dispatch(1, 8);
  cpu.dispatch_owner = 0;

  const auto progress = cpu.dispatch_instruction();

  // The owner can make progress, so it keeps the whole round; the sibling is not
  // reached.
  CHECK(progress == DISPATCH_W);
  CHECK(rob_size(0) == static_cast<std::size_t>(DISPATCH_W));
  CHECK(rob_size(1) == 0);
}

TEST_CASE_METHOD(dual_context_fixture, "A stalled dispatch owner yields its round to the sibling")
{
  fill_dispatch(1, 8); // the owner (context 0) has nothing to dispatch
  cpu.dispatch_owner = 0;

  const auto progress = cpu.dispatch_instruction();

  // The owner made no progress, so the whole width goes to the hungry sibling.
  CHECK(progress == DISPATCH_W);
  CHECK(rob_size(0) == 0);
  CHECK(rob_size(1) == static_cast<std::size_t>(DISPATCH_W));
}
