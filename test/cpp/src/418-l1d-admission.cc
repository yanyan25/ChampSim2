#include <catch.hpp>

#include <array>

#include "channel.h"
#include "defaults.hpp"
#include "mocks.hpp"

// Stage 3.6 item 1: L1D admission made explicit. The core's data channel
// (core_to_l1d) is SHARED by every hardware context -- there is deliberately no
// per-context channel -- and L1D has exactly two upper channels (ptw_to_l1d +
// core_to_l1d) with max_tag_check == 2. CACHE::operate therefore grants one tag
// check per upper queue per cycle, so the core's data channel initiates at most
// one load (RQ) and one store (WQ) per cycle, however many requests lq_width lets
// the core push into the channel. This file pins that admission; the declaration
// of the two channels' sources lives at the L1D wiring in src/static_environment.cc.

TEST_CASE("L1D admits one load and one store per cycle from the shared core channel")
{
  release_MRC mock_ll;
  to_rq_MRP ptw_ul;               // ptw_to_l1d: no page-walk reads this cycle
  champsim::channel core_channel; // core_to_l1d: one channel shared across contexts

  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("418-l1d-admission")
                .upper_levels({&ptw_ul.queues, &core_channel})
                .lower_level(&mock_ll.queues)
                .hit_latency(5)
                .tag_bandwidth(champsim::bandwidth::maximum_type{2})};

  std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &ptw_ul}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }

  auto make_req = [](uint64_t addr, access_type type) {
    champsim::channel::request_type req;
    req.address = champsim::address{addr};
    req.v_address = champsim::address{addr};
    req.is_translated = true;
    req.type = type;
    req.cpu = 0;
    return req;
  };

  // A wide core (lq_width == sq_width == 4) pushes more requests than L1D can
  // admit in a cycle; the admission cap must be the cache's, not the core's width.
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(core_channel.add_rq(make_req(0x1000 * (i + 1), access_type::LOAD)));
    REQUIRE(core_channel.add_wq(make_req(0x2000 * (i + 1), access_type::WRITE)));
  }
  REQUIRE(core_channel.rq_occupancy() == 4);
  REQUIRE(core_channel.wq_occupancy() == 4);

  // Cycle 1: exactly one load and one store advance to the tag-check stage; the
  // rest stay queued in the shared channel.
  uut._operate();
  CHECK(core_channel.rq_occupancy() == 3);
  CHECK(core_channel.wq_occupancy() == 3);

  // Cycle 2: the same one-load + one-store admission repeats, independent of how
  // much remains queued.
  uut._operate();
  CHECK(core_channel.rq_occupancy() == 2);
  CHECK(core_channel.wq_occupancy() == 2);
}
