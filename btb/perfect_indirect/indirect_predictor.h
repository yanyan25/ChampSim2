#ifndef BTB_PERFECT_INDIRECT_INDIRECT_PREDICTOR_H
#define BTB_PERFECT_INDIRECT_INDIRECT_PREDICTOR_H

#include <array>
#include <bitset>
#include <cstdint>
#include <utility>

#include "address.h"
#include "champsim.h"
#include "msl/bits.h"

// Copied from btb/basic_btb/. ChampSim compiles EVERY module into EVERY binary,
// so an unqualified copy collides at link time; the namespace gives this copy
// distinct symbols. The code inside is unmodified.
namespace perfect_indirect_impl
{
struct indirect_predictor {
  static constexpr std::size_t size = 4096;
  std::array<champsim::address, size> predictor = {};
  std::bitset<champsim::msl::lg2(size)> conditional_history = {};

  std::pair<champsim::address, bool> prediction(champsim::address ip, champsim::address_space_id asid);
  void update_target(champsim::address ip, champsim::address branch_target, champsim::address_space_id asid);
  void update_direction(bool taken);
};

} // namespace perfect_indirect_impl

#endif
