// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant; performing
// the set-aside here, at the outermost level, avoids the nesting. See
// branch/cbp6_tagescl64/cbp6_tagescl64.cc for the full explanation.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include "perfect_return.h"

std::pair<champsim::address, bool> perfect_return::btb_prediction(champsim::address ip, champsim::address_space_id asid)
{
  return impl.prediction(ip, intern_, asid);
}

void perfect_return::update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid)
{
  impl.update(ip, branch_target, taken, branch_type, asid);
}
