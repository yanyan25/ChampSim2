/*
 * A BTB that NEVER MISSES: perfect branch detection, perfect branch type, and
 * the correct target for every direct branch -- with the REAL indirect
 * predictor and the REAL return address stack still answering for their own
 * groups.
 *
 * This isolates the BTB's SECOND role. btb/perfect_direct idealises group-BTB's
 * targets and is nearly a no-op, because a direct branch's target never changes
 * and the BTB's stored copy is already right whenever the entry exists. What
 * actually costs is the entry NOT being there -- which also denies the indirect
 * predictor and the RAS any chance to answer, since basic_btb returns "no
 * prediction" on a BTB miss before it routes anywhere.
 *
 * Consequently this module OVERLAPS the per-group oracles and is not a term in
 * their decomposition: it hands the real IBTB and real RAS opportunities they
 * previously never got, so part of its gain shows up as their predictions
 * succeeding. Report it beside the decomposition, never inside it.
 *
 * Pair with a real direction predictor (cbp6_tagescl64). always_taken is
 * reported exactly as basic_btb reports it, so it is safe under any predictor.
 */

#ifndef BTB_IDEAL_BTB_H
#define BTB_IDEAL_BTB_H

#include <cstdint>
#include <utility>

#include "address.h"
#include "modules.h"
#include "perfect_group/perfect_group.h"

class ideal_btb : champsim::modules::btb
{
  champsim::perfect_group::btb_impl<champsim::perfect_group::group::ideal_btb> impl{};

public:
  using btb::btb;
  ideal_btb() : btb(nullptr) {}

  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
};

#endif
