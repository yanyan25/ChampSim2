#include <catch.hpp>

#include <algorithm>
#include <array>

#include "address.h"
#include "channel.h"
#include "defaults.hpp"
#include "mocks.hpp"

// Stage 3.7 item 1: the complete loop that 419 left open. 419 proves only that a
// sibling's untranslated load is *admitted* past a peer's TLB-miss burst; its
// translator is never answered, so it cannot show that the sibling's walk comes
// back, that the miss is filled, or that the response returns. Here both the
// translator and the lower level answer, so the whole loop -- walk out, walk back,
// tag check, fill, return -- is driven to completion for the sibling and the
// result is asserted on the sibling's own address space.

TEST_CASE("A sibling's untranslated load completes its translation and fill after a peer's TLB-miss burst")
{
  do_nothing_MRC mock_ll;         // lower level: answers the fill
  do_nothing_MRC mock_translator; // answers every page walk
  champsim::channel core_channel; // core_to_l1d, shared by both contexts

  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("456-l1d-translation-completion")
                .upper_levels({&core_channel})
                .lower_level(&mock_ll.queues)
                .lower_translate(&mock_translator.queues)
                .hit_latency(1)
                .tag_bandwidth(champsim::bandwidth::maximum_type{16})
                .mshr_size(16)};

  std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_translator}};
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

  // Context A floods the shared channel with MSHR_SIZE untranslated loads, filling
  // A's whole stash share and issuing MSHR_SIZE page walks.
  for (unsigned i = 0; i < 16; ++i)
    REQUIRE(core_channel.add_rq(make_req(1, 0x1000 * (i + 1))));
  uut._operate();
  uut._operate();
  REQUIRE(core_channel.rq_occupancy() == 0);
  REQUIRE(mock_translator.queues.rq_occupancy() == 16); // A's 16 walks are out

  // Context B issues one untranslated load. It must still be admitted (its own
  // stash share is empty), and its walk must go out too.
  constexpr uint64_t b_addr = 0x2abc; // offset 0xabc is unique among A's page-aligned loads
  const champsim::address b_vaddr{b_addr};
  REQUIRE(core_channel.add_rq(make_req(2, b_addr)));
  uut._operate();
  REQUIRE(core_channel.rq_occupancy() == 0); // B admitted: 419's invariant held up

  // Drive the whole loop to completion: the translator answers every walk, the
  // fills come back, and every response returns to the core channel.
  for (int i = 0; i < 200; ++i)
    for (auto elem : elements)
      elem->_operate();

  // B's page walk went out to the translator under B's own address.
  CHECK(std::find(std::begin(mock_translator.addresses), std::end(mock_translator.addresses), b_vaddr) != std::end(mock_translator.addresses));

  // B's translation completed: its miss reached the lower level at B's own page
  // offset (none of A's page-aligned loads shares offset 0xabc).
  const auto b_offset = champsim::page_offset{b_vaddr}.to<uint64_t>();
  CHECK(std::any_of(std::begin(mock_ll.addresses), std::end(mock_ll.addresses),
                    [b_offset](champsim::address a) { return champsim::page_offset{a}.to<uint64_t>() == b_offset; }));

  // B's fill completed: a response carrying B's address space came back to the core.
  CHECK(std::any_of(std::begin(core_channel.returned), std::end(core_channel.returned),
                    [&](const auto& r) { return r.asid[0] == 2 && r.asid[1] == 2 && r.v_address == b_vaddr; }));
}
