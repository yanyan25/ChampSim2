#ifndef BTB_PERFECT_INDIRECT_DIRECT_PREDICTOR_H
#define BTB_PERFECT_INDIRECT_DIRECT_PREDICTOR_H

#include <cstdint>
#include <optional>

#include "address.h"
#include "champsim.h"
#include "msl/lru_table.h"

// Copied from btb/basic_btb/. ChampSim compiles EVERY module into EVERY binary
// (config.sh discovers and compiles every module it finds), so an unqualified copy of these
// classes collides at link time with basic_btb's. The namespace gives this copy
// distinct symbols; the code inside is unmodified.
namespace perfect_indirect_impl
{
struct direct_predictor {
  enum class branch_info {
    INDIRECT,
    RETURN,
    ALWAYS_TAKEN,
    CONDITIONAL,
  };

  static constexpr std::size_t sets = 1024;
  static constexpr std::size_t ways = 8;

  struct btb_entry_t {
    champsim::address ip_tag{};
    champsim::address target{};
    branch_info type = branch_info::ALWAYS_TAKEN;
    champsim::address_space_id asid{};

    auto index() const
    {
      using namespace champsim::data::data_literals;
      return ip_tag.slice_upper<2_b>();
    }
    auto tag() const
    {
      using namespace champsim::data::data_literals;
      return ip_tag.slice_upper<2_b>().to<uint64_t>() ^ champsim::asid_word(asid);
    }
  };

  champsim::msl::lru_table<btb_entry_t> BTB{sets, ways};
  std::optional<btb_entry_t> check_hit(champsim::address ip, champsim::address_space_id asid);
  void update(champsim::address ip, champsim::address branch_target, uint8_t branch_type, champsim::address_space_id asid);
};

} // namespace perfect_indirect_impl

#endif
