#ifndef BTB_BASIC_BTB_H
#define BTB_BASIC_BTB_H

#include "address.h"
#include "direct_predictor.h"
#include "indirect_predictor.h"
#include "modules.h"
#include "return_stack.h"
#include "runtime_config.h"

class basic_btb : champsim::modules::btb
{
  return_stack ras{};
  indirect_predictor indirect{};
  direct_predictor direct{};

public:
  using btb::btb;
  basic_btb() : btb(nullptr) {}

  // void initialize_btb();
  void configure(const champsim::runtime_config& cfg, std::string_view prefix);
  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
};

#endif
