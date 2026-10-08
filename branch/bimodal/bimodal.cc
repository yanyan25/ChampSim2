#include "bimodal.h"

bool bimodal::predict_branch(champsim::address ip, champsim::address /*predicted_target*/, bool /*always_taken*/, uint8_t /*branch_type*/,
                             champsim::address_space_id asid)
{
  auto value = bimodal_table[hash(ip, asid)];
  return value.value() > (value.maximum / 2);
}

void bimodal::last_branch_result(champsim::address ip, champsim::address /*branch_target*/, bool taken, uint8_t /*branch_type*/,
                                 champsim::address_space_id asid)
{
  bimodal_table[hash(ip, asid)] += taken ? 1 : -1;
}
