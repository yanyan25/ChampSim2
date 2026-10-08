/*
 * group-RAS: RETURN gets its true target. The BTB's stored targets and
 * the indirect predictor are left entirely real.
 *
 * One member of the per-group oracle family in inc/perfect_group/perfect_group.h,
 * which is where all the logic and every failure-mode guard live. This is a
 * configuration shell.
 *
 * Pair it with a REAL direction predictor (cbp6_tagescl64): the question is what
 * is left on a machine that predicts directions as well as we currently can.
 *
 * As with every oracle here, this module reports always_taken exactly as
 * basic_btb does, so it is safe underneath any direction predictor. See
 * btb/perfect_btb/perfect_btb.h for why that distinction matters.
 */

#ifndef BTB_PERFECT_RETURN_H
#define BTB_PERFECT_RETURN_H

#include <cstdint>
#include <utility>

#include "address.h"
#include "modules.h"
#include "perfect_group/perfect_group.h"

class perfect_return : champsim::modules::btb
{
  champsim::perfect_group::btb_impl<champsim::perfect_group::group::ras> impl{};

public:
  using btb::btb;
  perfect_return() : btb(nullptr) {}

  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
};

#endif
