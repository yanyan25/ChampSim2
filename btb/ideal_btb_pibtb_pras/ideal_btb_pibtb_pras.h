/*
 * step 3, the last of the cumulative series: ideal BTB + perfect indirect
 * + perfect RAS. Every target in the machine is then correct, so this MUST
 * reproduce btb/perfect_btb. That equality is the series' correctness check --
 * if it does not hold, one of the three steps is wrong.
 *
 * A configuration shell over inc/perfect_group/perfect_group.h, where the logic
 * and every guard live. Pair with a real direction predictor (cbp6_tagescl64).
 */

#ifndef BTB_IDEAL_BTB_PIBTB_PRAS_H
#define BTB_IDEAL_BTB_PIBTB_PRAS_H

#include <cstdint>
#include <utility>

#include "address.h"
#include "modules.h"
#include "perfect_group/perfect_group.h"

class ideal_btb_pibtb_pras : champsim::modules::btb
{
  champsim::perfect_group::btb_impl<champsim::perfect_group::group::ideal_ibtb_ras> impl{};

public:
  using btb::btb;
  ideal_btb_pibtb_pras() : btb(nullptr) {}

  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
};

#endif
