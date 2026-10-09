// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant; the
// set-aside is performed here, at the outermost level. See
// branch/cbp6_tagescl64/cbp6_tagescl64.cc for the full explanation.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include "blbp/blbp_btb.h"
#include "blbp_64kb.h"

namespace
{
// The 64 KB configuration: paper constants throughout (the nearest-reproduction
// proxy); tools/blbp_tune produces the tuned variant separately.
champsim::blbp::predictor_config make_config()
{
  champsim::blbp::predictor_config c{}; // defaults ARE the paper constants
  c.engine.M = 952;                     // derived from the budget, below
  return c;
}

// Exact storage accounting, 48-bit canonical addresses (ITTAGE audit lesson:
// full-system agentic traces carry kernel-range targets; 47 bits was a biased
// sample). The simulator's wider types are artifacts and excluded.
constexpr long ibtb_bits = 4096L * (8 + 7 + 20 + 2 + 1); // tag+region+offset+SRRIP+valid (audit: valid bit is architectural)
constexpr long region_bits = 128L * (28 + 1);            // (48-20)-bit base + valid
constexpr long local_bits = 256L * 10;
constexpr long ghist_bits = 630;
constexpr long theta_bits = 12L * 8;
constexpr long weight_bits = 8L * 952 * 12 * 4; // N x M x K x 4b
constexpr long total_bits = ibtb_bits + region_bits + local_bits + ghist_bits + theta_bits + weight_bits;
static_assert(total_bits == 528214, "storage accounting drifted"); // 64.48 KB strictly; a strict-64KB config would use M=941

template <std::size_t... I>
std::array<champsim::blbp::btb_impl, sizeof...(I)> make_instances_impl(std::index_sequence<I...>)
{
  // One independent predictor per hardware context. btb_impl's constructor
  // fully initialises its state, so plain (non-static) members are safe.
  return {{(static_cast<void>(I), champsim::blbp::btb_impl{make_config()})...}};
}
} // namespace

std::array<champsim::blbp::btb_impl, champsim::defs::max_contexts> blbp_64kb::make_instances()
{
  return make_instances_impl(std::make_index_sequence<champsim::defs::max_contexts>{});
}

void blbp_64kb::configure(const champsim::runtime_config& cfg, std::string_view prefix)
{
  for (auto& inst : instances)
    inst.configure_direct(cfg, prefix);
}

void blbp_64kb::initialize_btb() {}

std::pair<champsim::address, bool> blbp_64kb::btb_prediction(champsim::address ip, champsim::context_index context, champsim::address_space_id asid)
{
  return instances[context.value].prediction(ip, asid);
}

void blbp_64kb::update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::context_index context,
                           champsim::address_space_id asid)
{
  instances[context.value].update(ip, branch_target, taken, branch_type, asid);
}
