#ifndef BTB_BASIC_BTB_DIRECT_PREDICTOR_H
#define BTB_BASIC_BTB_DIRECT_PREDICTOR_H

#include <cstdint>
#include <optional>

#include "address.h"
#include "champsim.h"
#include "msl/lru_table.h"

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

  // Runtime geometry: rebuild the (empty) table before first use. The
  // constexpr sets/ways above remain the defaults.
  void resize(std::size_t new_sets, std::size_t new_ways) { BTB = champsim::msl::lru_table<btb_entry_t>{new_sets, new_ways}; }
  std::optional<btb_entry_t> check_hit(champsim::address ip, champsim::address_space_id asid);
  void update(champsim::address ip, champsim::address branch_target, uint8_t branch_type, champsim::address_space_id asid);
};

#endif
