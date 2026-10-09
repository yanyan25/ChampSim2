#include <catch.hpp>

#include <array>

#include "channel.h"
#include "defaults.hpp"
#include "mocks.hpp"

// Stage 3.6 item 2: the L1D translation stash is partitioned by address space, so
// one context's burst of TLB misses cannot fill the stash and block the sibling's
// untranslated load. Before the fix the admission gate was a single global
// `translation_stash.size() < MSHR_SIZE`; now an untranslated entry is admissible
// while fewer than MSHR_SIZE of ITS OWN address space are already stashed.

TEST_CASE("One context's TLB-miss burst does not block a sibling's untranslated load")
{
  release_MRC mock_ll;                // lower level, never reached here
  champsim::channel core_channel;     // core_to_l1d: shared by all contexts
  champsim::channel translator_queue; // l1d -> dtlb: page-walk requests, never answered

  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("419-translation-stash-partition")
                .upper_levels({&core_channel})
                .lower_level(&mock_ll.queues)
                .lower_translate(&translator_queue)
                .hit_latency(1)
                .tag_bandwidth(champsim::bandwidth::maximum_type{16})
                .mshr_size(16)};

  std::array<champsim::operable*, 2> elements{{&uut, &mock_ll}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }

  auto make_req = [](uint8_t asid, uint64_t addr) {
    champsim::channel::request_type req;
    req.address = champsim::address{addr};
    req.v_address = champsim::address{addr};
    req.is_translated = false;
    req.type = access_type::LOAD;
    req.asid[0] = asid;
    req.asid[1] = asid;
    req.cpu = 0;
    return req;
  };

  // Context A floods the shared channel with MSHR_SIZE untranslated loads. The tag
  // bandwidth (16) admits them all in one cycle, and the next cycle moves them into
  // the stash, where they wait on the never-answered page walk.
  for (unsigned i = 0; i < 16; ++i)
    REQUIRE(core_channel.add_rq(make_req(1, 0x1000 * (i + 1))));
  REQUIRE(core_channel.rq_occupancy() == 16);

  uut._operate(); // admit all 16 to inflight_tag_check
  uut._operate(); // move them into the stash once ready
  REQUIRE(core_channel.rq_occupancy() == 0);
  REQUIRE(translator_queue.rq_occupancy() == 16); // 16 page walks issued, none returned

  // Context B now issues one untranslated load. It must still be admitted: its own
  // address space has nothing stashed, however full context A's share is.
  REQUIRE(core_channel.add_rq(make_req(2, 0x2000)));
  uut._operate();
  CHECK(core_channel.rq_occupancy() == 0);
}
