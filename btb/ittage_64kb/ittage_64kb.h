/*
 * ITTAGE indirect target predictor, paper-class budget.
 *
 * A configuration shell: it names the storage parameters and forwards to the
 * shared adapter in inc/ittage/ittage_btb.h, which is where all the logic and
 * all the failure-mode guards live. The predictor itself is Seznec's, vendored
 * once at inc/ittage/ittage.hpp.
 *
 * Storage: 64.0 KB by the accounting in inc/ittage/ittage_btb.h (48-bit target +
 * tag + confidence + useful bit per entry; the simulator's uint64_t is an
 * artifact and excluded). Asserted at compile time in the .cc.
 */

#ifndef BTB_ITTAGE_64KB_H
#define BTB_ITTAGE_64KB_H

#include <cstdint>
#include <string_view>
#include <utility>

#include "address.h"
#include "modules.h"
#include "runtime_config.h"

class ittage_64kb : champsim::modules::btb
{
public:
  using btb::btb;
  ittage_64kb() : btb(nullptr) {}

  // ITTAGE's vendored predictor relies on static zero-initialisation, so the
  // per-context storage is a function-local static array rather than a member
  // (see the .cc). The capability advertises per-context isolation.
  static constexpr champsim::modules::smt_capability capability = champsim::modules::smt_capability::per_context;

  // Only the direct predictor is settable; this module's own tables are
  // vendored and compile-time. Keys: <prefix>.direct.sets / .direct.ways.
  void configure(const champsim::runtime_config& cfg, std::string_view prefix);
  void initialize_btb();
  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::context_index context, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::context_index context,
                  champsim::address_space_id asid);
};

#endif
