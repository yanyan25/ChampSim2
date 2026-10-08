/*
 * BLBP-style bit-level perceptron indirect target predictor, 64 KB budget --
 * iso-sized with btb/ittage_64kb, which is the comparison this module exists
 * for. A configuration shell over inc/blbp/blbp_btb.h; all logic and all
 * failure-mode guards live there and in inc/blbp/blbp.h.
 *
 * Storage: 63.98 KB by the exact accounting in the .cc (48-bit canonical
 * addresses per the ITTAGE audit; the simulator's wider internal types are an
 * artifact and excluded). M = 952 rows/table derives from the budget; the
 * paper never states M.
 */

#ifndef BTB_BLBP_64KB_H
#define BTB_BLBP_64KB_H

#include <cstdint>
#include <string_view>
#include <utility>

#include "address.h"
#include "modules.h"
#include "runtime_config.h"

class blbp_64kb : champsim::modules::btb
{
public:
  using btb::btb;
  blbp_64kb() : btb(nullptr) {}

  // Only the direct predictor is settable; this module's own tables are
  // vendored and compile-time. Keys: <prefix>.direct.sets / .direct.ways.
  void configure(const champsim::runtime_config& cfg, std::string_view prefix);
  void initialize_btb();
  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
};

#endif
