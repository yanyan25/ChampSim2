#include "gshare.h"

std::size_t gshare::gs_table_hash(champsim::address ip, std::bitset<GLOBAL_HISTORY_LENGTH> bh_vector, champsim::address_space_id asid)
{
  constexpr champsim::data::bits LOG2_HISTORY_TABLE_SIZE{champsim::lg2(GS_HISTORY_TABLE_SIZE)};
  constexpr champsim::data::bits LENGTH{GLOBAL_HISTORY_LENGTH};

  std::size_t hash = bh_vector.to_ullong();
  hash ^= ip.slice<LOG2_HISTORY_TABLE_SIZE, champsim::data::bits{}>().to<std::size_t>();
  hash ^= ip.slice<LOG2_HISTORY_TABLE_SIZE + LENGTH, LENGTH>().to<std::size_t>();
  hash ^= ip.slice<LOG2_HISTORY_TABLE_SIZE + 2 * LENGTH, 2 * LENGTH>().to<std::size_t>();
  hash ^= champsim::asid_word(asid);

  return hash % GS_HISTORY_TABLE_SIZE;
}

bool gshare::predict_branch(champsim::address ip, champsim::address /*predicted_target*/, bool /*always_taken*/, uint8_t /*branch_type*/,
                            champsim::address_space_id asid)
{
  auto gs_hash = gs_table_hash(ip, branch_history_vector, asid);
  auto value = gs_history_table[gs_hash];
  return value.value() >= (value.maximum / 2);
}

void gshare::last_branch_result(champsim::address ip, champsim::address /*branch_target*/, bool taken, uint8_t /*branch_type*/, champsim::address_space_id asid)
{
  auto gs_hash = gs_table_hash(ip, branch_history_vector, asid);
  gs_history_table[gs_hash] += taken ? 1 : -1;

  // update branch history vector
  branch_history_vector <<= 1;
  branch_history_vector[0] = taken;
}
