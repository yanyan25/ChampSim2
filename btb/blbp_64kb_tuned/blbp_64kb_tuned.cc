// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant; the
// set-aside is performed here, at the outermost level. See
// branch/cbp6_tagescl64/cbp6_tagescl64.cc for the full explanation.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include "blbp/blbp_btb.h"
#include "blbp_64kb_tuned.h"

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

template <std::size_t... I>
std::array<champsim::blbp::btb_impl, sizeof...(I)> make_instances_impl(std::index_sequence<I...>)
{
  return {{(static_cast<void>(I), champsim::blbp::btb_impl{make_config()})...}};
}
} // namespace

std::array<champsim::blbp::btb_impl, champsim::defs::max_contexts> blbp_64kb_tuned::make_instances()
{
  return make_instances_impl(std::make_index_sequence<champsim::defs::max_contexts>{});
}

void blbp_64kb_tuned::configure(const champsim::runtime_config& cfg, std::string_view prefix)
{
  for (auto& inst : instances)
    inst.configure_direct(cfg, prefix);
}

void blbp_64kb_tuned::initialize_btb() {}

std::pair<champsim::address, bool> blbp_64kb_tuned::btb_prediction(champsim::address ip, champsim::context_index context,
                                                                   champsim::address_space_id asid)
{
  return instances[context.value].prediction(ip, asid);
}

void blbp_64kb_tuned::update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::context_index context,
                                 champsim::address_space_id asid)
{
  instances[context.value].update(ip, branch_target, taken, branch_type, asid);
}
