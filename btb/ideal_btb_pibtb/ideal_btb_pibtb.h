/*
 * step 2 of the cumulative series: an ideal BTB PLUS a perfect indirect
 * target predictor. Returns still come from the real RAS.
 *
 * Indirect MPKI must be EXACTLY zero here. btb/perfect_indirect could not reach
 * zero because a branch missing the direct BTB never consults the indirect
 * predictor at all; with detection perfect that escape hatch is gone.
 *
 * A configuration shell over inc/perfect_group/perfect_group.h, where the logic
 * and every guard live. Pair with a real direction predictor (cbp6_tagescl64).
 */

#ifndef BTB_IDEAL_BTB_PIBTB_H
#define BTB_IDEAL_BTB_PIBTB_H

#include <cstdint>
#include <utility>

#include "address.h"
#include "modules.h"
#include "perfect_group/perfect_group.h"

class ideal_btb_pibtb : champsim::modules::btb
{
  champsim::perfect_group::btb_impl<champsim::perfect_group::group::ideal_ibtb> impl{};

public:
  using btb::btb;
  ideal_btb_pibtb() : btb(nullptr) {}

  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
};

#endif
