/*
 * ITTAGE indirect target predictor, iso-ENTRY-COUNT with basic_btb's 4096-entry target cache (24.0 KB); 33% more storage.
 *
 * A configuration shell: it names the storage parameters and forwards to the
 * shared adapter in inc/ittage/ittage_btb.h, which is where all the logic and
 * all the failure-mode guards live. The predictor itself is Seznec's, vendored
 * once at inc/ittage/ittage.hpp.
 *
 * Storage: 32.0 KB by the accounting in inc/ittage/ittage_btb.h (48-bit target +
 * tag + confidence + useful bit per entry; the simulator's uint64_t is an
 * artifact and excluded). Asserted at compile time in the .cc.
 */

#ifndef BTB_ITTAGE_32KB_H
#define BTB_ITTAGE_32KB_H

#include <cstdint>
#include <string_view>
#include <utility>

#include "address.h"
#include "modules.h"
#include "runtime_config.h"

class ittage_32kb : champsim::modules::btb
{
public:
  using btb::btb;
  ittage_32kb() : btb(nullptr) {}

  // Only the direct predictor is settable; this module's own tables are
  // vendored and compile-time. Keys: <prefix>.direct.sets / .direct.ways.
  void configure(const champsim::runtime_config& cfg, std::string_view prefix);
  void initialize_btb();
  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
};

#endif
