#include <catch.hpp>

#include "channel.h"
#include "defaults.hpp"
#include "mocks.hpp"

// Step 3.2: the TLB tag array is keyed by address space as well as address. Two
// address spaces that map the same virtual page to the same physical page must
// occupy two distinct TLB entries; a hit in one address space must not satisfy a
// lookup in the other.

namespace
{
// A translator that returns the SAME physical page for every address space, so two
// address spaces resolve the same virtual page to the same physical page and the only
// thing that distinguishes their TLB entries is the address-space id.
struct aliasing_translator : champsim::operable {
  champsim::channel queues{};

  long operate() override
  {
    for (const auto& pkt : queues.RQ) {
      auto response = champsim::channel::response_type{pkt};
      response.data = champsim::address{0xAAAA000};
      queues.returned.push_back(response);
    }
    queues.RQ.clear();
    return 1; // never deadlock
  }
};
} // namespace

TEST_CASE("Two address spaces aliasing one physical page do not share a TLB entry")
{
  aliasing_translator mock_translator;
  release_MRC mock_ll;
  to_rq_MRP mock_ul{[](auto x, auto y) {
    return x.v_address == y.v_address;
  }};

  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("417-uut")
                .upper_levels({&mock_ul.queues})
                .lower_level(&mock_ll.queues)
                .lower_translate(&mock_translator.queues)
                .hit_latency(10)
                .fill_latency(3)
                .tag_bandwidth(champsim::bandwidth::maximum_type{10})};

  std::array<champsim::operable*, 4> elements{{&uut, &mock_ll, &mock_ul, &mock_translator}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }

  auto make_req = [](uint8_t asid0) {
    champsim::channel::request_type req;
    req.address = champsim::address{0xdead000};
    req.v_address = champsim::address{0xdead000};
    req.is_translated = false;
    req.asid[0] = asid0;
    req.asid[1] = asid0;
    req.cpu = 0;
    return req;
  };

  auto run = [&](int cycles) {
    for (int i = 0; i < cycles; ++i) {
      for (auto elem : elements)
        elem->_operate();
    }
  };

  // The first address space resolves the page, forwards one request downstream, and
  // fills a TLB entry once the lower level releases the fill.
  REQUIRE(mock_ul.issue(make_req(250)));
  run(50);
  REQUIRE(mock_ll.packet_count() == 1);
  mock_ll.release_all();
  run(50);

  // A second address space resolves the same virtual page to the same physical page.
  REQUIRE(mock_ul.issue(make_req(251)));
  run(50);

  // The second address space must miss the first address space's entry and issue its
  // own downstream request rather than hitting the aliased translation.
  REQUIRE(mock_ll.packet_count() == 2);
}
