/*
 * A perfect conditional-direction predictor: an oracle, for measuring headroom.
 *
 * This is NOT a predictor. It reads the branch's resolved direction out of the
 * trace at prediction time and returns it, so it never mispredicts a direction.
 * Its purpose is to bound what any direction predictor could achieve on a given
 * workload, so that a real predictor's result can be read as a fraction of the
 * available headroom rather than as a bare percentage.
 *
 * WHAT IT DOES NOT MAKE PERFECT: branch TARGETS. ChampSim charges a
 * misprediction when the BTB's predicted target differs from the actual target,
 * independently of the direction check (src/ooo_cpu.cc, do_predict_branch). So
 * this module paired with the ordinary BTB yields the ceiling for a *direction*
 * predictor -- the right ceiling for TAGE-SC-L, RUNLTS and DD-TAGE, which
 * predict direction only. Pair it with the `perfect_btb` module to remove target
 * misses as well and get the total branch-misprediction headroom.
 *
 * HOW THE ORACLE IS SAFE: O3_CPU::do_init_instruction is called as
 * do_init_instruction(input_queue.front()) and the entry is popped only after it
 * returns (src/ooo_cpu.cc, fetch loop), so during predict_branch the instruction
 * being predicted IS input_queue.front(). Reading its branch_taken is therefore
 * reading the instruction under prediction, not a later one. The same access
 * pattern is already used by the cbp6 modules to obtain the architectural next
 * PC.
 *
 * WHAT "0 MPKI" DOES AND DOES NOT PROVE. It is tempting to treat a reported
 * 0 MPKI as proof that the oracle is correct. It is not, and the mistake is
 * worth spelling out because it is so easy to make: this module returns
 * input_queue.front().branch_taken, and ChampSim's mispredict rule
 * (src/ooo_cpu.cc) compares arch_instr.branch_taken -- the SAME field of the
 * SAME object, since arch_instr is input_queue.front(). The comparison is
 * therefore an identity, and 0 MPKI follows structurally no matter what the
 * trace says. Corrupt the trace's direction bits and this module still reports
 * 100% accuracy. All that 0 MPKI establishes is that the module is wired in and
 * being called.
 *
 * The oracle is only as good as the trace metadata it reads, so that metadata is
 * validated INDEPENDENTLY, by a property the oracle cannot influence: a branch
 * recorded as not-taken must be followed by its fall-through address. See
 * scripts referenced in the research log; it passes on 100.000% of not-taken
 * branches across all 32 traces. That check matters here because these v2 traces
 * have shipped with broken branch metadata once already -- an earlier generation
 * omitted the flags register, which made ChampSim see zero conditional branches
 * and gave four different predictors an identical 21.93 MPKI.
 *
 * The useful plumbing check is CycWPKI == 0 rather than MPKI == 0: MPKI excludes
 * BRANCH_OTHER in ChampSim's printers, whereas CycWPKI counts every class.
 */

#ifndef BRANCH_PERFECT_BRANCH_H
#define BRANCH_PERFECT_BRANCH_H

#include <cstdint>

#include "address.h"
#include "modules.h"

class perfect_branch : champsim::modules::branch_predictor
{
public:
  using branch_predictor::branch_predictor;

  bool predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type, champsim::address_space_id asid);
};

#endif
