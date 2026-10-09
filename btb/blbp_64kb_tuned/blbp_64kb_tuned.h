/*
 * BLBP-style predictor, 64 KB, TUNED for our trace zoo -- the headline
 * configuration of the BLBP campaign. Identical machine and identical storage
 * to btb/blbp_64kb (same M, K, N, table geometry: the tuner mutates only
 * history intervals, the transfer curve and theta); only the constants differ.
 *
 * Provenance of the constants: parallel hill-climb over the 40 TRAINING
 * streams only (spec: docs/superpowers/specs/2026-08-13-blbp-design.md;
 * trajectory: cluster blbp_tune/gens/). Frozen at generation 35 of 40 with the
 * queue contended and per-generation gains below 0.2%: pooled train indirect
 * mispredicts 2,339,594 (paper constants) -> 1,929,199, a 17.5% reduction.
 * Test traces were never extracted into streams -- the tuner could not see
 * them by construction.
 */

#ifndef BTB_BLBP_64KB_TUNED_H
#define BTB_BLBP_64KB_TUNED_H

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "address.h"
#include "blbp/blbp_btb.h"
#include "defs.h"
#include "modules.h"
#include "runtime_config.h"

class blbp_64kb_tuned : champsim::modules::btb
{
  static std::array<champsim::blbp::btb_impl, champsim::defs::max_contexts> make_instances();
  std::array<champsim::blbp::btb_impl, champsim::defs::max_contexts> instances = make_instances();

public:
  using btb::btb;
  blbp_64kb_tuned() : btb(nullptr) {}

  // BLBP's state is fully self-initialised by its constructor, so it can be a
  // per-context member array -- replicated for each hardware context (and, as a
  // member, for each core). The environment allows it past the single-context
  // guard.
  static constexpr champsim::modules::smt_capability capability = champsim::modules::smt_capability::per_context;

  // Only the direct predictor is settable; this module's own tables are
  // vendored and compile-time. Keys: <prefix>.direct.sets / .direct.ways.
  void configure(const champsim::runtime_config& cfg, std::string_view prefix);
  void initialize_btb();
  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::context_index context, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::context_index context,
                  champsim::address_space_id asid);
};

#endif
