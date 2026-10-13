#ifndef CORE_STATS_H
#define CORE_STATS_H

#include <cstdint>
#include <string>

#include "event_counter.h"
#include "instruction.h"

struct cpu_stats {
  std::string name;
  long long begin_instrs = 0;
  long long begin_cycles = 0;
  long long end_instrs = 0;
  long long end_cycles = 0;

  // Stage 5.3: the cycle at which THIS context retires its `length`-th phase
  // instruction -- its own ROI completion -- measured from the phase begin
  // (i.e. the delta `roi_cycles`, not an absolute counter). In co-phase the
  // faster context finishes before the phase ends, so `cycles()` (end - begin)
  // overstates its turnaround; `roi_cycles` is the per-thread own-completion
  // that fairness needs. Single-context it equals `cycles()`, so N=1 output is
  // unchanged.
  long long roi_cycles = 0;

  uint64_t total_rob_occupancy_at_branch_mispredict = 0;

  // Cycles fetch spent frozen after a misprediction, i.e. cycles that produced
  // no useful work. ChampSim does not fetch a wrong path, so this is the
  // equivalent of CBP2025's CycWP: the interval between detecting the
  // misprediction and fetch restarting, penalty included.
  uint64_t cycles_on_wrong_path = 0;

  champsim::stats::event_counter<branch_type> total_branch_types = {};
  champsim::stats::event_counter<branch_type> branch_type_misses = {};

  // Decoded-instruction-buffer accesses. One lookup is charged per instruction
  // that reaches the fetch stage, so the two counters sum to one lookup per
  // fetched instruction; the total is derived rather than counted separately
  // and so cannot drift from its parts.
  //
  // A hit means this instruction's window was DECODED earlier and is still
  // resident -- NOT merely that a neighbour was fetched alongside it. The DIB
  // is filled at decode (`do_dib_update`), long after `check_dib` has already
  // classified the whole fetch group, so a cold group misses in its entirety:
  // four instructions sharing a 16-byte window are four misses on their first
  // pass, and hit only when that code runs again. `dib_misses` therefore counts
  // instructions, not distinct windows, and is not a code-footprint proxy.
  uint64_t dib_hits = 0;
  uint64_t dib_misses = 0;

  [[nodiscard]] auto instrs() const { return end_instrs - begin_instrs; }
  [[nodiscard]] auto cycles() const { return end_cycles - begin_cycles; }
  [[nodiscard]] auto dib_lookups() const { return dib_hits + dib_misses; }
};

cpu_stats operator-(cpu_stats lhs, cpu_stats rhs);

#endif
