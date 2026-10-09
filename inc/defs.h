#ifndef CHAMPSIM_DEFS_H
#define CHAMPSIM_DEFS_H

#include <cstddef>
#include <cstdint>

/*
 * Compile-time machine constants.
 *
 * These are the parameters that cannot be runtime knobs: they size types and
 * are read during static initialization (champsim::defaults' namespace-scope
 * builders consume LOG2_BLOCK_SIZE/LOG2_PAGE_SIZE before main runs), so a
 * value parsed at startup would be captured stale.
 *
 * Everything else is configured at run time from a TOML file -- see
 * configs/README.md. Edit here and rebuild only to change the numbers below,
 * or the machine's shape in src/static_environment.cc.
 */
namespace champsim::defs
{
// One binary per core count: multi-core development follows single-core
// iteration, so this is deliberately not a runtime knob.
inline constexpr std::size_t num_cpus = 1;

// The compile-time bound on hardware contexts per core (SMT threads). Stage 3
// replicates per-context module state in arrays of this size, and the
// environment's module-selection guard refuses a core carrying more contexts
// than this. Stage 3 targets dual-context; raise it before a wider-SMT build.
inline constexpr std::size_t max_contexts = 2;

inline constexpr std::size_t block_size = 64;
inline constexpr std::size_t page_size = 4096;

// The default heartbeat interval; sim.heartbeat_frequency and the
// --heartbeat-frequency flag both override it at run time.
inline constexpr std::uint64_t heartbeat_frequency = 10000000;

// The modules a binary uses unless a runtime key selects another. These MUST
// name what inc/defaults.hpp bakes into the builders: static_environment skips
// the registry when a selection equals the value here, so a mismatch would
// make [config] report a module the run did not use.
//
// NOTE, for anyone comparing against results from before the migration: the
// deleted champsim_config.json selected "bimodal", overriding the C++ default
// below. A configuration-free run therefore predicts better than it used to --
// on 400.perlbench, 4.46 MPKI against bimodal's 8.38. Every campaign selects
// its predictor explicitly, so none of their numbers move; a default-build
// baseline does. Set ooo_cpu.<cpu>.branch_predictor = "bimodal" to get the old
// behaviour back without editing this file.
inline constexpr const char* default_branch_predictor = "hashed_perceptron";
inline constexpr const char* default_btb = "basic_btb";
inline constexpr const char* default_prefetcher = "no";
inline constexpr const char* default_replacement = "lru";
} // namespace champsim::defs

#endif
