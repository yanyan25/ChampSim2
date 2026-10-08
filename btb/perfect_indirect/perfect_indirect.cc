// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant; performing
// the set-aside here, at the outermost level, avoids the nesting. See
// branch/cbp6_tagescl64/cbp6_tagescl64.cc for the full explanation.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include "instruction.h"
#include "perfect_indirect.h"

std::pair<champsim::address, bool> perfect_indirect::btb_prediction(champsim::address ip, champsim::address_space_id asid)
{
  auto btb_entry = direct.check_hit(ip, asid);

  // No prediction for this IP. Deliberately NOT overridden: the oracle replaces
  // the indirect *target* predictor, not the BTB's knowledge that a given PC is
  // a branch at all. Overriding this would fold direct-BTB capacity into the
  // measurement and overstate what an indirect predictor can deliver.
  if (!btb_entry.has_value()) {
    return {champsim::address{}, false};
  }

  if (btb_entry->type == perfect_indirect_impl::direct_predictor::branch_info::RETURN) {
    return ras.prediction();
  }

  if (btb_entry->type == perfect_indirect_impl::direct_predictor::branch_info::INDIRECT) {
    // Only a GENUINE indirect branch gets the oracle answer. The direct BTB
    // indexes and tags on ip>>2 (direct_predictor.h), so two branches within 3
    // bytes share an entry -- and an earlier version of this module handed the
    // true target to whatever aliased in, including direct jumps. Measured, that
    // cut SPEC direct-jump MPKI from 0.1496 to 0.0909 (710.omnetpp_r.sp2: 0.8163
    // -> 0.0176), inflating the very headroom denominator this oracle exists to
    // define, and biasing it AGAINST the predictor under test.
    //
    // The instruction under prediction is input_queue.front() -- see
    // branch/perfect_branch/perfect_branch.h for why that holds.
    if (intern_ != nullptr && !std::empty(intern_->ctx().input_queue)) {
      const auto& instr = intern_->ctx().input_queue.front();
      if (instr.branch == BRANCH_INDIRECT || instr.branch == BRANCH_INDIRECT_CALL) {
        return {instr.branch_target, true};
      }
    }
    // Aliased non-indirect branch: behave exactly as basic_btb would, so this
    // module remains a faithful overlay that differs ONLY on real indirect
    // branches.
    return aliased.prediction(ip, asid);
  }

  return {btb_entry->target, btb_entry->type != perfect_indirect_impl::direct_predictor::branch_info::CONDITIONAL};
}

void perfect_indirect::update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid)
{
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    ras.push(ip);
  }

  // The oracle needs no training, but the aliased-branch fallback is basic_btb's
  // real predictor and must be trained exactly as basic_btb trains it.
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
    aliased.update_target(ip, branch_target, asid);
  }
  if (branch_type == BRANCH_CONDITIONAL) {
    aliased.update_direction(taken);
  }

  if (branch_type == BRANCH_RETURN) {
    ras.calibrate_call_size(branch_target);
  }

  direct.update(ip, branch_target, branch_type, asid);
}
