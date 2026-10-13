#include <catch.hpp>
#include <cmath>
#include <limits>
#include <sstream>
#include <fmt/core.h>

#include "cache_stats.h"
#include "core_stats.h"
#include "dram_stats.h"
#include "stats_printer.h"
#include <toml++/toml.hpp>

// The TOML statistics printer. These tests pin the exact emitted text, in the
// same spirit as 198/498/798 for the plain printer: the document is a public
// interface that analysis scripts parse, so a silent key rename is a breaking
// change and must fail here.
//
// Two rules the whole format rests on, asserted throughout:
//   * every ratio is emitted at two decimal places, AND the exact integer
//     numerator and denominator it came from are emitted alongside it, so
//     rounding never destroys information;
//   * a ratio with a zero denominator is `nan` -- a real TOML float literal --
//     rather than an omitted key, so the schema never varies between runs.

TEST_CASE("An empty core emits every key with a nan for each undefined ratio")
{
  cpu_stats given{};
  given.name = "test_cpu";

  std::vector<std::string> expected{"[core.cpu0]",
                                    "name = \"test_cpu\"",
                                    "instructions = 0",
                                    "cycles = 0",
                                    "roi_cycles = 0",
                                    "ipc = nan",
                                    "dib_lookups = 0",
                                    "dib_hits = 0",
                                    "dib_misses = 0",
                                    "dib_hit_rate = nan",
                                    "total_branches = 0",
                                    "total_mispredicts = 0",
                                    "mpki = nan",
                                    "branch_prediction_accuracy = nan",
                                    "total_rob_occupancy_at_mispredict = 0",
                                    "avg_rob_occupancy_at_mispredict = nan",
                                    "cycles_on_wrong_path = 0",
                                    "cyc_wpki = nan",
                                    "avg_cycles_per_mispredict = nan",
                                    "",
                                    "[core.cpu0.mispredict]",
                                    "branch_direct_jump = 0",
                                    "branch_indirect = 0",
                                    "branch_conditional = 0",
                                    "branch_direct_call = 0",
                                    "branch_indirect_call = 0",
                                    "branch_return = 0",
                                    "",
                                    "[core.cpu0.executed]",
                                    "branch_direct_jump = 0",
                                    "branch_indirect = 0",
                                    "branch_conditional = 0",
                                    "branch_direct_call = 0",
                                    "branch_indirect_call = 0",
                                    "branch_return = 0"};

  REQUIRE_THAT(champsim::toml_printer::format(given, "core.cpu0"), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("A core ratio is rounded to two decimals and keeps its exact operands")
{
  cpu_stats given{};
  given.name = "test_cpu";
  given.end_instrs = 300;
  given.end_cycles = 700;

  const auto lines = champsim::toml_printer::format(given, "core.cpu0");

  // 300/700 = 0.428571... -- the rounded value is a convenience, and both
  // operands survive exactly so the full-precision ratio is recoverable.
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"ipc = 0.43"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"instructions = 300"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"cycles = 700"}));
}

TEST_CASE("The DIB hit rate is emitted beside the counts it came from")
{
  // The decoded-instruction buffer's hit rate is the number anyone reads, but
  // the rounded percentage alone cannot be re-weighted across traces, so the
  // exact lookup/hit/miss counts sit beside it.
  cpu_stats given{};
  given.name = "test_cpu";
  given.end_instrs = 1000;
  given.end_cycles = 1000;
  given.dib_hits = 700;
  given.dib_misses = 323;

  const auto lines = champsim::toml_printer::format(given, "core.cpu0");

  // 1023 lookups against 1000 instructions, deliberately unequal: the rate's
  // denominator is the LOOKUP count, and a run's lookups never match its
  // retired instructions exactly (the pipeline is not drained between phases).
  // 700/1023 = 68.4262..., which also exercises the two-decimal rule -- against
  // `instrs()` it would read 70.00.
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"dib_lookups = 1023"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"dib_hits = 700"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"dib_misses = 323"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"dib_hit_rate = 68.43"}));
}

TEST_CASE("A DIB hit rate whose denominator is zero is nan, not a dropped key")
{
  cpu_stats given{};
  given.name = "test_cpu";
  given.end_instrs = 1000;
  given.end_cycles = 1000;

  const auto lines = champsim::toml_printer::format(given, "core.cpu0");

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"dib_lookups = 0"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"dib_hit_rate = nan"}));
}

TEST_CASE("Executed and mispredicted branches are counted in separate tables")
{
  // The executed census is the stat the JSON printer never carried: without it
  // the per-type miss RATE can only be approximated from a census of the trace,
  // which covers a different instruction window than the ROI.
  cpu_stats given{};
  given.name = "test_cpu";
  given.end_instrs = 1000;
  given.end_cycles = 1000;
  given.total_branch_types.set(branch_type::BRANCH_INDIRECT, 400);
  given.branch_type_misses.set(branch_type::BRANCH_INDIRECT, 100);

  const auto lines = champsim::toml_printer::format(given, "core.cpu0");

  const auto mispredict_at = std::distance(std::begin(lines), std::find(std::begin(lines), std::end(lines), "[core.cpu0.mispredict]"));
  const auto executed_at = std::distance(std::begin(lines), std::find(std::begin(lines), std::end(lines), "[core.cpu0.executed]"));
  REQUIRE(mispredict_at < executed_at);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"total_branches = 400"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"total_mispredicts = 100"}));
  // 100 misses in 1000 instructions
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"mpki = 100.00"}));
  // 300 of 400 branches correct
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"branch_prediction_accuracy = 75.00"}));

  // The counts land in their own tables, so the same key name appears twice
  // with different values -- once under each header.
  REQUIRE(std::count(std::begin(lines), std::end(lines), "branch_indirect = 400") == 1);
  REQUIRE(std::count(std::begin(lines), std::end(lines), "branch_indirect = 100") == 1);
}

TEST_CASE("An empty cache emits the full per-CPU access matrix")
{
  // Unlike the plain printer -- which emits nothing at all for a cache that saw
  // no traffic -- every key is always present, so a parser can index the
  // document without first checking which caches happened to be used.
  cache_stats given{};
  given.name = "test_cache";

  std::vector<std::string> expected{"[cache.test_cache]",
                                    "name = \"test_cache\"",
                                    "total_miss_latency_cycles = 0",
                                    "miss_latency = nan",
                                    "",
                                    "[cache.test_cache.prefetch]",
                                    "requested = 0",
                                    "issued = 0",
                                    "useful = 0",
                                    "useless = 0",
                                    "fill = 0"};

  // One table per CPU, so the key names do not change with the core count --
  // that is the property this pins, and spelling out cpu0 alone would instead
  // pin the core count.
  for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu) {
    expected.push_back("");
    expected.push_back("[cache.test_cache.cpu" + std::to_string(cpu) + "]");
    for (const auto* type : {"load", "rfo", "prefetch", "write", "translation"}) {
      for (const auto* field : {"hit", "miss", "miss_merge", "fill"}) {
        expected.push_back(std::string{type} + "_" + field + " = 0");
      }
    }
  }

  REQUIRE_THAT(champsim::toml_printer::format(given, "cache.test_cache"), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("Average miss latency excludes prefetch fills from its denominator")
{
  // The denominator is downstream DEMAND fills, i.e. every fill except the ones
  // the prefetcher caused. Emitting the fills per access type is what makes
  // that denominator reconstructible -- neither the plain nor the JSON printer
  // ever exposed it.
  cache_stats given{};
  given.name = "test_cache";
  given.fill.set({access_type::LOAD, 0}, 10);
  given.fill.set({access_type::PREFETCH, 0}, 5);
  given.total_miss_latency_cycles = 100;

  const auto lines = champsim::toml_printer::format(given, "cache.test_cache");

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"total_miss_latency_cycles = 100"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"load_fill = 10"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"prefetch_fill = 5"}));
  // 100 cycles over the 10 demand fills, with the 5 prefetch fills excluded
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"miss_latency = 10.00"}));
}

TEST_CASE("Prefetcher counters are separated from PREFETCH-type accesses")
{
  // Both are legitimately called "prefetch", so they live in different tables:
  // [<cache>.prefetch] counts what the prefetcher did, while the per-CPU table
  // counts accesses whose type was PREFETCH. Conflating them would silently
  // double-report.
  cache_stats given{};
  given.name = "test_cache";
  given.pf_requested = 7;
  given.pf_useful = 3;
  given.hits.set({access_type::PREFETCH, 0}, 11);

  const auto lines = champsim::toml_printer::format(given, "cache.test_cache");

  const auto prefetch_table = std::find(std::begin(lines), std::end(lines), "[cache.test_cache.prefetch]");
  const auto cpu_table = std::find(std::begin(lines), std::end(lines), "[cache.test_cache.cpu0]");
  REQUIRE(prefetch_table != std::end(lines));
  REQUIRE(cpu_table != std::end(lines));

  REQUIRE(std::find(prefetch_table, cpu_table, "requested = 7") != cpu_table);
  REQUIRE(std::find(prefetch_table, cpu_table, "useful = 3") != cpu_table);
  REQUIRE(std::find(cpu_table, std::end(lines), "prefetch_hit = 11") != std::end(lines));
}

TEST_CASE("A DRAM channel emits the write-queue-full count the JSON printer dropped")
{
  // WQ_FULL is reported by the plain printer and was absent from the JSON
  // entirely; dbus congestion is emitted as both the raw pair and the ratio.
  dram_stats given{};
  given.name = "test_dram";
  given.RQ_ROW_BUFFER_HIT = 5;
  given.RQ_ROW_BUFFER_MISS = 7;
  given.WQ_FULL = 3;
  given.dbus_cycle_congested = 90;
  given.dbus_count_congested = 4;
  given.refresh_cycles = 11;

  std::vector<std::string> expected{
      "[dram.channel0]",        "name = \"test_dram\"", "rq_row_buffer_hit = 5",     "rq_row_buffer_miss = 7",   "wq_row_buffer_hit = 0",
      "wq_row_buffer_miss = 0", "wq_full = 3",          "dbus_cycle_congested = 90", "dbus_count_congested = 4", "avg_dbus_congested_cycle = 22.50",
      "refreshes_issued = 11"};

  REQUIRE_THAT(champsim::toml_printer::format(given, "dram.channel0"), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("A DRAM channel that never congested reports nan, not a zero")
{
  // The plain printer writes "-" here. Zero would be a lie -- it would claim
  // the bus was congested for zero cycles on average, when in fact it never
  // congested and there is no average to report.
  dram_stats given{};
  given.name = "test_dram";

  const auto lines = champsim::toml_printer::format(given, "dram.channel0");

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"avg_dbus_congested_cycle = nan"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"dbus_count_congested = 0"}));
}

namespace
{
champsim::phase_stats one_of_everything()
{
  champsim::phase_stats stats{};
  stats.name = "Simulation";
  stats.trace_names = {"/traces/a.champsimtrace.xz"};

  cpu_stats core{};
  core.name = "CPU 0";
  core.end_instrs = 10;
  core.end_cycles = 20;
  stats.roi_cpu_stats = {core};
  stats.sim_cpu_stats = {core};

  cache_stats cache{};
  cache.name = "cpu0_L1D";
  stats.roi_cache_stats = {cache};
  stats.sim_cache_stats = {cache};

  dram_stats channel{};
  channel.name = "Channel 0";
  stats.roi_dram_stats = {channel};
  stats.sim_dram_stats = {channel};

  return stats;
}
} // namespace

TEST_CASE("The document opens with a meta table declaring the schema version")
{
  // A parser that meets a future revision of this format needs a way to tell
  // which one it is holding without guessing from the keys.
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines.at(0), Catch::Matchers::StartsWith("#"));
  REQUIRE(std::find(std::begin(lines), std::end(lines), "[meta]") != std::end(lines));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"schema_version = 1"}));
  // Reported, not assumed: [meta].num_cpus states the machine's core count, so
  // pinning the literal 1 would pin the build rather than the printer.
  REQUIRE_THAT(lines, Catch::Matchers::Contains("num_cpus = " + std::to_string(NUM_CPUS)));
}

TEST_CASE("A phase is keyed by its lower-cased name but preserves the original")
{
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"name = \"Simulation\""}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.trace]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"cpu0 = \"/traces/a.champsimtrace.xz\""}));
}

TEST_CASE("The region of interest is emitted and the whole-run section is not, by default")
{
  // A run with a single region of interest -- which is the usual case -- makes
  // the whole-run section a copy of the region-of-interest one, so it is off
  // unless asked for. [meta] records which, so a consumer can tell a suppressed
  // section from one this ChampSim never produced.
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.core.cpu0]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.cpu0_l1d]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.dram.channel0]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"sim_stats = false"}));

  const auto is_sim_table = [](const auto& line) {
    return line.rfind("[phase.simulation.sim.", 0) == 0;
  };
  REQUIRE(std::none_of(std::begin(lines), std::end(lines), is_sim_table));
}

TEST_CASE("The whole-run section is emitted when it is asked for")
{
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given, true);

  for (const auto* section : {"roi", "sim"}) {
    REQUIRE_THAT(lines, Catch::Matchers::Contains(fmt::format("[phase.simulation.{}.core.cpu0]", section)));
    REQUIRE_THAT(lines, Catch::Matchers::Contains(fmt::format("[phase.simulation.{}.cache.cpu0_l1d]", section)));
    REQUIRE_THAT(lines, Catch::Matchers::Contains(fmt::format("[phase.simulation.{}.dram.channel0]", section)));
  }
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"sim_stats = true"}));
}

TEST_CASE("A component name that is not a bare TOML key is quoted in the path")
{
  // Cache names come straight from the configuration file and nothing
  // sanitises them. Upstream ChampSim names caches "cpu0->L1D"; '>' is not a
  // bare-key character, so an unquoted path would not parse.
  auto phase = one_of_everything();
  phase.roi_cache_stats.at(0).name = "cpu0->L1D";
  phase.sim_cache_stats.at(0).name = "cpu0->L1D";
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.\"cpu0->l1d\"]"}));
  // and the original spelling survives inside the table
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"name = \"cpu0->L1D\""}));
}

TEST_CASE("A trace path containing a quote or a backslash is escaped")
{
  auto phase = one_of_everything();
  phase.trace_names = {"/tmp/od\"d\\path"};
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"cpu0 = \"/tmp/od\\\"d\\\\path\""}));
}

TEST_CASE("Two cache names differing only in case do not collapse onto one table")
{
  // Lower-casing is not injective. TOML forbids defining a table twice, so a
  // collision does not corrupt one section -- it makes the WHOLE document
  // unparseable, and it does so silently at emit time. The configuration layer
  // keys caches on their exact name, so two separately-reachable caches really
  // can differ only in case; when the lower-cased form would collide, the exact
  // spelling is used instead, which is unique by construction.
  auto phase = one_of_everything();
  cache_stats other = phase.roi_cache_stats.at(0);
  phase.roi_cache_stats.at(0).name = "SharedL1";
  other.name = "sharedl1";
  phase.roi_cache_stats.push_back(other);
  phase.sim_cache_stats = phase.roi_cache_stats;
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  std::vector<std::string> headers{};
  std::copy_if(std::begin(lines), std::end(lines), std::back_inserter(headers),
               [](const auto& line) { return line.rfind("[phase.simulation.roi.cache.", 0) == 0; });

  auto unique_headers = headers;
  std::sort(std::begin(unique_headers), std::end(unique_headers));
  REQUIRE(std::adjacent_find(std::begin(unique_headers), std::end(unique_headers)) == std::end(unique_headers));

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.SharedL1]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.sharedl1]"}));
}

TEST_CASE("Names that do not collide are still lower-cased")
{
  // The disambiguation must not leak into the ordinary case: a name only keeps
  // its original spelling when lower-casing it would actually collide.
  auto phase = one_of_everything();
  cache_stats other = phase.roi_cache_stats.at(0);
  other.name = "cpu0_L1I";
  phase.roi_cache_stats.push_back(other);
  phase.sim_cache_stats = phase.roi_cache_stats;
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.cpu0_l1d]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.cpu0_l1i]"}));
}

// The [config] section and the run parameters in [meta] exist so a statistics
// document states which machine, and which run, produced it. [config] is the
// EFFECTIVE configuration: the key/value pairs the machine consulted while it
// was built, handed to the printer as a flat list on run_info. The printer
// builds the tree from that list and hashes it into [meta].build_id, naming no
// generated symbol to do so -- which is what keeps toml_printer.cc linkable
// into this test binary, and the static format() seam testable at all.

TEST_CASE("The run parameters are recorded in the meta table")
{
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  champsim::toml_printer::run_info info{};
  info.build_id = "0x2989172160dc027f";
  info.warmup_instructions = 50000000;
  info.simulation_instructions = 200000000;
  info.trace_version = 2;
  info.command_line = "bin/champsim -w 50000000 trace.xz";

  const auto lines = champsim::toml_printer::format(given, false, info);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"build_id = \"0x2989172160dc027f\""}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"warmup_instructions = 50000000"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"simulation_instructions = 200000000"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"trace_version = 2"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"command_line = \"bin/champsim -w 50000000 trace.xz\""}));
}

TEST_CASE("The run parameters keep their keys even when nothing is known")
{
  // Same rule as every ratio in this document: a key is never dropped, so a
  // parser can index the document without first checking what happened to be
  // available at the time.
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"build_id = \"\""}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"warmup_instructions = 0"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"command_line = \"\""}));
}

TEST_CASE("A command line containing a quote is escaped, not emitted raw")
{
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  champsim::toml_printer::run_info info{};
  info.command_line = "bin/champsim --toml \"a b.toml\"";

  const auto lines = champsim::toml_printer::format(given, false, info);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"command_line = \"bin/champsim --toml \\\"a b.toml\\\"\""}));
}

TEST_CASE("The configuration record is spliced into the document verbatim")
{
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  champsim::toml_printer::run_info info{};
  info.config_toml = "[config]\nexecutable_name = \"cbp_blbp64t\"\n\n[config.ooo_cpu.cpu0]\nbtb = \"blbp_64kb_tuned\"";

  const auto lines = champsim::toml_printer::format(given, false, info);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[config]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"executable_name = \"cbp_blbp64t\""}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[config.ooo_cpu.cpu0]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"btb = \"blbp_64kb_tuned\""}));

  // It must land after [meta] and before the first phase, so that neither
  // table swallows the other's keys.
  const auto find_at = [&lines](std::string_view header) {
    const auto pos = std::find(std::begin(lines), std::end(lines), header);
    REQUIRE(pos != std::end(lines)); // a header that is never found would make the ordering check vacuous
    return std::distance(std::begin(lines), pos);
  };
  const auto meta_at = find_at("[meta]");
  const auto config_at = find_at("[config]");
  const auto phase_at = find_at("[phase.simulation]");
  REQUIRE(meta_at < config_at);
  REQUIRE(config_at < phase_at);
}

TEST_CASE("A configuration record with no content still leaves an indexable table")
{
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE(std::find(std::begin(lines), std::end(lines), "[config]") != std::end(lines));
}

TEST_CASE("Blank lines around the configuration record do not accumulate")
{
  // The record arrives from a raw string literal that begins right after the
  // opening delimiter, so it always carries a leading newline of its own, and
  // append_block adds a separator of its own on top. Left alone the two would
  // compound every time the block is spliced.
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  champsim::toml_printer::run_info info{};
  info.config_toml = "\n\n[config]\nblock_size = 64\n\n\n";

  const auto lines = champsim::toml_printer::format(given, false, info);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[config]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"block_size = 64"}));

  // No two blank lines anywhere: one separator between blocks, never a run.
  const auto doubled =
      std::adjacent_find(std::begin(lines), std::end(lines), [](const std::string& lhs, const std::string& rhs) { return std::empty(lhs) && std::empty(rhs); });
  REQUIRE(doubled == std::end(lines));
}

TEST_CASE("The runtime configuration sources are recorded in meta")
{
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  champsim::toml_printer::run_info info{};
  info.config_files = "base.toml,override.toml";

  const auto lines = champsim::toml_printer::format(given, false, info);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"config_files = \"base.toml,override.toml\""}));
}

TEST_CASE("The applied runtime overrides land in their own table")
{
  // The baked [config] records what config.sh generated from; this table
  // records what THIS RUN changed on top of it, so the pair is a complete
  // statement of the machine that produced the numbers.
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  champsim::toml_printer::run_info info{};
  // Values arrive from the store already rendered in TOML syntax -- including
  // quoted, escaped strings (pinned by 098-runtime-config.cc).
  info.overrides = {{"cache.cpu0_l1d.sets", "128"}, {"ooo_cpu.cpu0.rob_size", "512"}, {"pmem.frequency", "1600.0"}, {"a.name", "\"with \\\"quotes\\\"\""}};

  const auto lines = champsim::toml_printer::format(given, false, info);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[config_override]"}));
  // Each entry is ONE flat key naming one knob, so the whole key is quoted --
  // a bare dotted key would parse as a nested table.
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"\"cache.cpu0_l1d.sets\" = 128"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"\"ooo_cpu.cpu0.rob_size\" = 512"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"\"pmem.frequency\" = 1600.0"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"\"a.name\" = \"with \\\"quotes\\\"\""}));

  // After [config] (so neither table swallows the other) and before the phases.
  const auto config_at = std::distance(std::begin(lines), std::find(std::begin(lines), std::end(lines), "[config]"));
  const auto override_at = std::distance(std::begin(lines), std::find(std::begin(lines), std::end(lines), "[config_override]"));
  const auto phase_at = std::distance(std::begin(lines), std::find(std::begin(lines), std::end(lines), "[phase.simulation]"));
  REQUIRE(config_at < override_at);
  REQUIRE(override_at < phase_at);
}

TEST_CASE("An empty override set still emits an indexable table")
{
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE(std::find(std::begin(lines), std::end(lines), "[config_override]") != std::end(lines));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"config_files = \"\""}));
}

TEST_CASE("The effective configuration renders as a nested table tree")
{
  // Once there is no baked JSON to record, [config] is built from the keys the
  // machine actually consulted -- flat dotted paths with values already in
  // TOML syntax.
  const std::vector<std::pair<std::string, std::string>> effective{
      {"block_size", "64"}, {"cache.cpu0_l1d.sets", "64"}, {"cache.cpu0_l1d.ways", "12"}, {"ooo_cpu.cpu0.rob_size", "352"}};

  const auto lines = champsim::toml_printer::format_config(effective);

  REQUIRE(lines.at(0) == "[config]");
  REQUIRE(lines.at(1) == "block_size = 64");
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[config.cache.cpu0_l1d]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"sets = 64"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[config.ooo_cpu.cpu0]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"rob_size = 352"}));
}

TEST_CASE("A scalar is never emitted after a sub-table of the same table")
{
  // The trap: sorted dotted keys put ooo_cpu.cpu0.dib.sets BEFORE
  // ooo_cpu.cpu0.rob_size, so a naive pass emits the dib table first and
  // rob_size then lands inside it, silently reparenting the knob.
  const std::vector<std::pair<std::string, std::string>> effective{
      {"ooo_cpu.cpu0.dib.sets", "32"}, {"ooo_cpu.cpu0.dib.ways", "8"}, {"ooo_cpu.cpu0.rob_size", "352"}};

  const auto lines = champsim::toml_printer::format_config(effective);
  const auto rob_at = std::distance(std::begin(lines), std::find(std::begin(lines), std::end(lines), "rob_size = 352"));
  const auto dib_at = std::distance(std::begin(lines), std::find(std::begin(lines), std::end(lines), "[config.ooo_cpu.cpu0.dib]"));
  REQUIRE(rob_at < dib_at);
}

TEST_CASE("A key that is not a bare TOML key is quoted in the effective configuration")
{
  const std::vector<std::pair<std::string, std::string>> effective{{"cache.Weird Name.sets", "64"}};
  const auto lines = champsim::toml_printer::format_config(effective);
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[config.cache.\"Weird Name\"]"}));
}

TEST_CASE("An empty effective configuration still emits an indexable table")
{
  const auto lines = champsim::toml_printer::format_config({});
  REQUIRE(lines == std::vector<std::string>{"[config]"});
}

TEST_CASE("The configuration id is a content hash of the effective configuration")
{
  // With no build to identify, [meta] identifies the MACHINE: two runs that
  // simulate the same thing share an id however the configuration was
  // expressed -- a file, a --set, or a baked default.
  const std::vector<std::pair<std::string, std::string>> a{{"cache.llc.ways", "16"}, {"ooo_cpu.cpu0.rob_size", "352"}};
  const std::vector<std::pair<std::string, std::string>> b{{"cache.llc.ways", "16"}, {"ooo_cpu.cpu0.rob_size", "352"}};
  const std::vector<std::pair<std::string, std::string>> c{{"cache.llc.ways", "8"}, {"ooo_cpu.cpu0.rob_size", "352"}};

  REQUIRE(champsim::toml_printer::config_id(a) == champsim::toml_printer::config_id(b));
  REQUIRE(champsim::toml_printer::config_id(a) != champsim::toml_printer::config_id(c));
  REQUIRE_THAT(champsim::toml_printer::config_id(a), Catch::Matchers::StartsWith("0x"));
  REQUIRE(std::size(champsim::toml_printer::config_id(a)) == 18);
}

TEST_CASE("Native TOML keeps parent admission, fragments and live work independent")
{
  champsim::phase_stats phase{};
  phase.name = "Simulation";
  phase.roi_ramulator2.emplace();
  phase.roi_ramulator2->accepted_reads = 2;
  phase.roi_ramulator2->accepted_fragments = 4;
  phase.roi_ramulator2->outstanding_parents = 1;
  phase.roi_ramulator2->out_of_range_prefetches = 5;
  phase.sim_ramulator2 = phase.roi_ramulator2;
  phase.sim_ramulator2->completed_reads = 1;
  phase.sim_ramulator2->out_of_range_prefetches = 6;
  std::vector<champsim::phase_stats> phases{phase};
  std::ostringstream output;
  champsim::toml_printer{output, true}.print(phases);
  const auto doc = toml::parse(output.str());
  const auto roi = doc["phase"]["simulation"]["roi"];
  REQUIRE(doc["meta"]["schema_version"].value<int>() == 2);
  REQUIRE(doc["meta"]["dram_model"].value<std::string>() == "ramulator2");
  REQUIRE(roi["ramulator2"]["adapter"]["accepted_reads"].value<int64_t>() == 2);
  REQUIRE(roi["ramulator2"]["adapter"]["accepted_fragments"].value<int64_t>() == 4);
  REQUIRE(roi["ramulator2"]["adapter"]["outstanding_parents"].value<int64_t>() == 1);
  REQUIRE(roi["ramulator2"]["adapter"]["out_of_range_prefetches"].value<int64_t>() == 5);
  REQUIRE(doc["phase"]["simulation"]["sim"]["ramulator2"]["adapter"]["completed_reads"].value<int64_t>() == 1);
  REQUIRE(doc["phase"]["simulation"]["sim"]["ramulator2"]["adapter"]["out_of_range_prefetches"].value<int64_t>() == 6);
  REQUIRE_FALSE(roi["dram"]);
  REQUIRE(output.str().find("dbus") == std::string::npos);
}

TEST_CASE("The native adapter table pins every counter in order, out-of-range prefetches beside rejected submissions")
{
  champsim::ramulator2_statistics given{};
  given.accepted_reads = 1;
  given.accepted_writes = 2;
  given.completed_reads = 3;
  given.completed_writes = 4;
  given.accepted_fragments = 5;
  given.completed_fragments = 6;
  given.rejected_submissions = 7;
  given.out_of_range_prefetches = 8;
  given.outstanding_parents = 9;
  given.outstanding_fragments = 10;
  given.total_read_latency_ps = 11;
  given.read_latency_samples = 12;

  const std::vector<std::string> expected{"[ramulator2]",
                                          "native_yaml = \"\"",
                                          "",
                                          "[ramulator2.adapter]",
                                          "accepted_reads = 1",
                                          "accepted_writes = 2",
                                          "completed_reads = 3",
                                          "completed_writes = 4",
                                          "accepted_fragments = 5",
                                          "completed_fragments = 6",
                                          "rejected_submissions = 7",
                                          "out_of_range_prefetches = 8",
                                          "outstanding_parents = 9",
                                          "outstanding_fragments = 10",
                                          "total_read_latency_ps = 11",
                                          "read_latency_samples = 12",
                                          "",
                                          "[ramulator2.native]"};
  REQUIRE_THAT(champsim::toml_printer::format(given, "ramulator2"), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("Native TOML preserves typed scalars, escaped paths, raw YAML and integer range")
{
  champsim::phase_stats phase{};
  phase.name = "Simulation";
  phase.roi_ramulator2.emplace();
  auto& stats = *phase.roi_ramulator2;
  stats.accepted_reads = std::numeric_limits<uint64_t>::max();
  stats.native.yaml = "native: \"quoted\"\n  values: [1, 2]\n";
  stats.native.values = {{{"memory_system", "controller", "channel0", "count"}, int64_t{42}},
                         {{"memory_system", "controller", "channel1", "count"}, uint64_t{43}},
                         {{"plugin.with.dots", "0", "name\"\n"}, std::string{"a\tb\\c"}},
                         {{"plugin.with.dots", "1", "enabled"}, true},
                         {{"whole"}, 2.0},
                         {{"precise"}, 0.123456789012345},
                         {{"nan"}, std::numeric_limits<double>::quiet_NaN()},
                         {{"positive_infinity"}, std::numeric_limits<double>::infinity()},
                         {{"negative_infinity"}, -std::numeric_limits<double>::infinity()},
                         {{"signed_min"}, std::numeric_limits<int64_t>::min()},
                         {{"unsigned_max"}, std::numeric_limits<uint64_t>::max()}};
  std::vector<champsim::phase_stats> phases{phase};
  std::ostringstream output;
  champsim::toml_printer{output}.print(phases);
  const auto doc = toml::parse(output.str());
  const auto memory = doc["phase"]["simulation"]["roi"]["ramulator2"];
  const auto native = memory["native"];
  REQUIRE(memory["native_yaml"].value<std::string>() == stats.native.yaml);
  REQUIRE(native["memory_system"]["controller"]["channel0"]["count"].value<int64_t>() == 42);
  REQUIRE(native["memory_system"]["controller"]["channel1"]["count"].value<int64_t>() == 43);
  REQUIRE(native["plugin.with.dots"]["0"]["name\"\n"].value<std::string>() == "a\tb\\c");
  REQUIRE(native["plugin.with.dots"]["1"]["enabled"].value<bool>() == true);
  REQUIRE(native["whole"].is_floating_point());
  REQUIRE(native["whole"].value<double>() == 2.0);
  REQUIRE(native["precise"].value<double>() == 0.123456789012345);
  REQUIRE(std::isnan(native["nan"].value_or(0.0)));
  REQUIRE(native["positive_infinity"].value<double>() == std::numeric_limits<double>::infinity());
  REQUIRE(native["negative_infinity"].value<double>() == -std::numeric_limits<double>::infinity());
  REQUIRE(native["signed_min"].value<int64_t>() == std::numeric_limits<int64_t>::min());
  // TOML's signed integer range cannot represent uint64_t max. A decimal
  // string keeps every digit; a float would silently lose low bits.
  REQUIRE(native["unsigned_max"].value<std::string>() == "18446744073709551615");
  REQUIRE(memory["adapter"]["accepted_reads"].value<std::string>() == "18446744073709551615");
  REQUIRE_FALSE(doc["phase"]["simulation"]["sim"]);
}

TEST_CASE("Native run metadata archives the original YAML and complete driver provenance")
{
  champsim::toml_printer::run_info info{};
  info.ramulator2.emplace();
  info.ramulator2->path = "/tmp/original name.yaml";
  info.ramulator2->hash = "1234567890abcdef";
  info.ramulator2->revision = "native-revision";
  info.ramulator2->library_hash = "fedcba0987654321";
  info.ramulator2->build = "{\"compiler\":\"g++\"}";
  info.ramulator2->yaml = "Frontend:\n  impl: External\n# untouched bytes\n";
  info.overrides = {{"ramulator2.config", "\"original name.yaml\""}};
  info.config_toml = "[config]\ndram-model = \"ramulator2\"\n[config.ramulator2]\nconfig = \"/tmp/original name.yaml\"\nconfig_hash = "
                     "\"1234567890abcdef\"\nrevision = \"native-revision\"";
  std::vector<champsim::phase_stats> phases;
  std::ostringstream output;
  champsim::toml_printer{output, false, info}.print(phases);
  const auto doc = toml::parse(output.str());
  REQUIRE(doc["meta"]["schema_version"].value<int>() == 2);
  REQUIRE(doc["meta"]["dram_model"].value<std::string>() == "ramulator2");
  const auto native = doc["meta"]["ramulator2"];
  REQUIRE(native["config"].value<std::string>() == "/tmp/original name.yaml");
  REQUIRE(native["config_hash"].value<std::string>() == "1234567890abcdef");
  REQUIRE(native["revision"].value<std::string>() == "native-revision");
  REQUIRE(native["library_hash"].value<std::string>() == "fedcba0987654321");
  REQUIRE(native["build"].value<std::string>() == info.ramulator2->build);
  REQUIRE(native["yaml"].value<std::string>() == info.ramulator2->yaml);
  REQUIRE(doc["config_override"]["ramulator2.config"].value<std::string>() == "original name.yaml");
  REQUIRE(doc["config"]["ramulator2"]["config"].value<std::string>() == "/tmp/original name.yaml");
}
