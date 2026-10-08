// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant; the
// set-aside is performed here, at the outermost level. See
// branch/cbp6_tagescl64/cbp6_tagescl64.cc for the full explanation.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include "blbp/blbp_btb.h"
#include "blbp_64kb_tuned.h"
#include "cbp6/cbp6_host.h"

namespace
{
// The tuned constants, frozen from blbp_tune generation 35 (see header).
// Everything the tuner did NOT touch matches btb/blbp_64kb exactly.
champsim::blbp::predictor_config make_config()
{
  champsim::blbp::predictor_config c{};
  c.engine.M = 952;
  c.engine.theta_init = 16;
  c.engine.transfer = {1, 2, 5, 8, 11, 14, 18, 27};
  c.intervals = {{0, 9}, {1, 17}, {7, 26}, {28, 45}, {54, 71}, {159, 160}, {380, 486}};
  return c;
}

// Storage identical to the untuned shell by construction: the tuner's search
// space excludes every size-bearing parameter. Same accounting, same assert.
constexpr long total_bits = 4096L * (8 + 7 + 20 + 2 + 1) // IBTB: tag+region+offset+SRRIP+valid
                            + 128L * (28 + 1) + 256L * 10 + 630 + 12L * 8 + 8L * 952 * 12 * 4;
static_assert(total_bits == 528214, "storage accounting drifted"); // 64.48 KB strictly; a strict-64KB config would use M=941

champsim::blbp::btb_impl& shared()
{
  static champsim::blbp::btb_impl instance{make_config()};
  return instance;
}
} // namespace

void blbp_64kb_tuned::configure(const champsim::runtime_config& cfg, std::string_view prefix) { shared().configure_direct(cfg, prefix); }

void blbp_64kb_tuned::initialize_btb()
{
  if (intern_ != nullptr) {
    champsim::cbp6::require_single_core(intern_->cpu, intern_->contexts.size(), "blbp_64kb_tuned");
  }
}

std::pair<champsim::address, bool> blbp_64kb_tuned::btb_prediction(champsim::address ip,
                                                                   champsim::address_space_id asid) { return shared().prediction(ip, asid); }

void blbp_64kb_tuned::update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid)
{
  shared().update(ip, branch_target, taken, branch_type, asid);
}
