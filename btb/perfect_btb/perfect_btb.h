/*
 * A perfect branch target buffer: an oracle, for measuring headroom.
 *
 * Returns the branch's actual target, read from the trace at prediction time, so
 * a target misprediction never occurs. Paired with the `perfect_branch` module
 * it removes every branch misprediction ChampSim can charge, which is the point:
 * the resulting IPC is the ceiling a workload would reach with branches
 * eliminated entirely as a bottleneck.
 *
 * ONLY VALID PAIRED WITH perfect_branch, or with a direction predictor that
 * predicts taken for every non-conditional branch. It is NOT a drop-in
 * replacement for basic_btb underneath an arbitrary predictor: basic_btb reports
 * always_taken = (branch_type != BRANCH_CONDITIONAL), and this module reports
 * false unconditionally (see below), so underneath bimodal/gshare/perceptron an
 * unconditional branch predicted not-taken would have its target zeroed and be
 * charged a misprediction that basic_btb would not have charged. The cbp6
 * tenants are unaffected -- champsim::cbp6::host returns true for every
 * non-direction-predicted class -- and so is perfect_branch, which returns the
 * architectural direction. Do not pair this module with a shipped ChampSim
 * predictor without fixing always_taken first.
 *
 * always_taken is deliberately reported as FALSE. ChampSim computes
 *
 *     branch_prediction = impl_predict_branch(...) || always_taken
 *
 * so reporting true would force every branch to be predicted taken and would
 * make a correctly-predicted not-taken conditional register as a misprediction.
 * Returning false leaves the direction entirely to the direction predictor,
 * which is what makes this module composable with either a real one or with
 * perfect_branch.
 *
 * The not-taken case still lines up: ChampSim zeroes predicted_branch_target
 * when the branch is predicted not-taken, and a not-taken branch's recorded
 * branch_target is itself zero (src/tracereader.cc), so predicted and actual
 * agree and no target miss is charged.
 *
 * See perfect_branch.h for why input_queue.front() is the instruction under
 * prediction, and for the empirical check that validates both modules: together
 * they must produce exactly 0 MPKI.
 */

#ifndef BTB_PERFECT_BTB_H
#define BTB_PERFECT_BTB_H

#include <utility>

#include "address.h"
#include "modules.h"

class perfect_btb : champsim::modules::btb
{
public:
  using btb::btb;

  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::address_space_id asid);
};

#endif
