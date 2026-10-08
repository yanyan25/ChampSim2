// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant; performing
// the set-aside here, at the outermost level, avoids the nesting. See
// branch/cbp6_tagescl64/cbp6_tagescl64.cc for the full explanation.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include "perfect_btb.h"

std::pair<champsim::address, bool> perfect_btb::btb_prediction(champsim::address /*ip*/, champsim::address_space_id /*asid*/)
{
  // intern_ is null in unit tests, which construct the module without a core.
  if (intern_ == nullptr || std::empty(intern_->ctx().input_queue)) {
    return {champsim::address{}, false};
  }

  // The instruction under prediction is input_queue.front(); its branch_target
  // is the architectural target (zero for a not-taken branch, which is exactly
  // what ChampSim compares against in that case).
  //
  // always_taken is false by construction -- see the header: reporting true here
  // would override the direction predictor and turn every correctly-predicted
  // not-taken conditional into a misprediction.
  return {intern_->ctx().input_queue.front().branch_target, false};
}
