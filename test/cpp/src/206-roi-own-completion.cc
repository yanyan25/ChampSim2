#include <catch.hpp>

#include "core_stats.h"
#include "defaults.hpp"
#include "mocks.hpp"
#include "ooo_cpu.h"
#include "stats_printer.h"

// Stage 5.3: per-context own-ROI-completion cycles (roi_cycles), the statistic
// SMT fairness needs. In co-phase the phase ends only when every context has
// retired >= -i, so the faster context retires past -i while its sibling
// catches up; roi_stats.cycles() (end - begin) is therefore the FULL phase, not
// that thread's own turnaround. roi_cycles latches, per context, the cycle at
// which it first retired `length` phase instructions -- the per-thread
// own-completion -- and the fairness script divides standalone cycles by it.
// Single-context it equals cycles(), so N=1 output is unchanged.

namespace
{
auto make_core(do_nothing_MRC& l1i, do_nothing_MRC& l1d)
{
  return O3_CPU{champsim::core_builder{champsim::defaults::default_core}.fetch_queues(&l1i.queues).data_queues(&l1d.queues)};
}
} // namespace

TEST_CASE("roi_cycles participates in the stats subtraction")
{
  // cpu_stats are differenced to produce a phase's numbers, so a new field has
  // to participate like the others (same rule as cycles_on_wrong_path, 180).
  cpu_stats later{};
  cpu_stats earlier{};
  later.roi_cycles = 900;
  earlier.roi_cycles = 250;

  const auto diff = later - earlier;
  REQUIRE(diff.roi_cycles == 650);
}

TEST_CASE("The TOML printer emits roi_cycles beside cycles")
{
  cpu_stats given{};
  given.name = "test_cpu";
  given.end_cycles = 700;
  given.roi_cycles = 500;

  const auto lines = champsim::toml_printer::format(given, "core.cpu0");

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"cycles = 700"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"roi_cycles = 500"}));
}

TEST_CASE("record_roi_completion latches the own-completion cycle and is idempotent")
{
  do_nothing_MRC l1i, l1d;
  auto uut = make_core(l1i, l1d);

  // A phase begun at cycle 20, now at cycle 100, whose context has retired 60
  // instructions past the phase begin; length is 50, so it has crossed.
  uut.ctx().sim_stats.begin_cycles = 20;
  uut.ctx().begin_phase_instr = 0;
  uut.ctx().num_retired = 60;
  uut.current_time = champsim::chrono::clock::time_point{100 * uut.clock_period};

  REQUIRE_FALSE(uut.ctx().roi_complete_recorded);
  uut.record_roi_completion(0, 50);

  REQUIRE(uut.ctx().roi_complete_recorded);
  // sim_cycle = (100 * clock_period / clock_period) - 20 = 80.
  REQUIRE(uut.ctx().sim_stats.roi_cycles == 80);
  REQUIRE(uut.ctx().sim_stats.roi_cycles == uut.sim_cycle());

  // Idempotent: the next cycle's call (sim_cycle now 81) must not overwrite the
  // first crossing.
  uut.current_time = champsim::chrono::clock::time_point{101 * uut.clock_period};
  uut.record_roi_completion(0, 50);
  REQUIRE(uut.ctx().sim_stats.roi_cycles == 80);
}

TEST_CASE("record_roi_completion does not latch before the context has crossed length")
{
  do_nothing_MRC l1i, l1d;
  auto uut = make_core(l1i, l1d);

  uut.ctx().sim_stats.begin_cycles = 0;
  uut.ctx().begin_phase_instr = 0;
  uut.ctx().num_retired = 49; // one short of length 50
  uut.current_time = champsim::chrono::clock::time_point{100 * uut.clock_period};

  uut.record_roi_completion(0, 50);

  REQUIRE_FALSE(uut.ctx().roi_complete_recorded);
  REQUIRE(uut.ctx().sim_stats.roi_cycles == 0);
}
