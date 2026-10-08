// See the note in perfect_branch.cc's sibling header on why including ooo_cpu.h
// requires performing the set-aside at the outermost level: ChampSim's
// SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include "perfect_branch.h"

bool perfect_branch::predict_branch(champsim::address /*ip*/, champsim::address /*predicted_target*/, bool /*always_taken*/, uint8_t /*branch_type*/,
                                    champsim::address_space_id /*asid*/)
{
  // intern_ is null in unit tests, which construct the module without a core.
  // Predicting not-taken is the conventional fallback and keeps the module
  // constructible in isolation.
  if (intern_ == nullptr || std::empty(intern_->ctx().input_queue)) {
    return false;
  }

  // The instruction under prediction is input_queue.front() -- see the header.
  // ChampSim calls the branch predictor for EVERY instruction, not only
  // branches, because at this point it does not yet know which it has; for a
  // non-branch, branch_taken is false and the result is discarded, so returning
  // it unconditionally is correct.
  return intern_->ctx().input_queue.front().branch_taken;
}
