/*
 * basic_btb with a PERFECT INDIRECT TARGET PREDICTOR: an oracle, for bounding
 * what an indirect target predictor such as ITTAGE could achieve.
 *
 * Identical to basic_btb -- same 8K-entry direct predictor, same 64-entry RAS --
 * except that BRANCH_INDIRECT and BRANCH_INDIRECT_CALL get their true target,
 * read from the trace at prediction time, instead of going through the 4096-entry
 * tagless gshare target cache.
 *
 * WHY THIS AND NOT perfect_btb. perfect_btb makes EVERY branch target correct:
 * conditional-taken BTB misses, direct jumps, calls and returns as well. That
 * bounds all target headroom, which is the right ceiling for "how much do
 * branches cost", but it badly overstates what an indirect predictor can deliver.
 * On this suite, under a perfect direction predictor, indirect branches are
 * 0.559 MPKI of the 1.718 MPKI residual -- 32.6%. The other 67.4% is returns and
 * direct-BTB capacity, which ITTAGE cannot touch. This module isolates the 32.6%.
 *
 * PAIR IT WITH A REAL DIRECTION PREDICTOR. The headroom question for an indirect
 * predictor is "what is left on a machine that predicts directions as well as we
 * currently can", so the intended configuration is cbp6_tagescl64 (or whichever
 * conditional predictor is the reference) plus this module. Pairing it with
 * perfect_branch instead answers a different and less useful question.
 *
 * Unlike btb/perfect_btb, this module reports always_taken exactly as basic_btb
 * does, so it is safe underneath ANY direction predictor, including bimodal and
 * gshare. See btb/perfect_btb/perfect_btb.h for why that distinction matters.
 *
 * As with the other oracles, a reported drop in BRANCH_INDIRECT MPKI to zero
 * proves only that the module is wired in -- it reads the same field ChampSim
 * compares against. The trace metadata itself must be validated independently.
 */

#ifndef BTB_PERFECT_INDIRECT_H
#define BTB_PERFECT_INDIRECT_H

#include <cstdint>
#include <utility>

#include "address.h"
#include "direct_predictor.h"
#include "indirect_predictor.h"
#include "modules.h"
#include "return_stack.h"

class perfect_indirect : champsim::modules::btb
{
  perfect_indirect_impl::return_stack ras{};
  perfect_indirect_impl::direct_predictor direct{};
  // basic_btb's real predictor, used for branches that are NOT actually
  // indirect but alias into an INDIRECT-typed BTB entry. See the .cc.
  perfect_indirect_impl::indirect_predictor aliased{};

public:
  using btb::btb;
  perfect_indirect() : btb(nullptr) {}

  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
};

#endif
