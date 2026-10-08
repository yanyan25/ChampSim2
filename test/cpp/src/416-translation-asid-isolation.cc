#include <catch.hpp>

#include "channel.h"
#include "defaults.hpp"
#include "mocks.hpp"

// Step 2.7: finish_translation() completes only the entries in the address space
// that issued the translation. Two address spaces resolving the same virtual page
// are two distinct translations; the physical page returned for one must not be
// handed to the other.

namespace
{
// The physical page a translation resolves to, keyed on the address-space id.
champsim::address page_for(uint8_t asid0) { return champsim::address{0xAAAA000ull + static_cast<uint64_t>(asid0) * 0x10000}; }

// A translator that keys the returned physical page on the request's address
// space, so two address spaces resolving the same virtual page receive
// observably different physical pages.
struct asid_keyed_translator : champsim::operable {
  champsim::channel queues{};

  long operate() override
  {
    for (const auto& pkt : queues.RQ) {
      auto response = champsim::channel::response_type{pkt};
      response.data = page_for(pkt.asid[0]);
      queues.returned.push_back(response);
    }
    queues.RQ.clear();
    return 1; // never deadlock
  }
};
} // namespace

TEST_CASE("Two address spaces resolving the same virtual page do not share a translation")
{
  asid_keyed_translator mock_translator;
  do_nothing_MRC mock_ll;
  to_rq_MRP mock_ul{[](auto x, auto y) {
    return x.v_address == y.v_address;
  }};

  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("416-uut")
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

  // Two requests, same virtual page, two address spaces.
  champsim::channel::request_type req_a;
  req_a.address = champsim::address{0xdead000};
  req_a.v_address = champsim::address{0xdead000};
  req_a.is_translated = false;
  req_a.asid[0] = 250;
  req_a.asid[1] = 250;
  req_a.cpu = 0;

  champsim::channel::request_type req_b;
  req_b.address = champsim::address{0xdead000};
  req_b.v_address = champsim::address{0xdead000};
  req_b.is_translated = false;
  req_b.asid[0] = 251;
  req_b.asid[1] = 251;
  req_b.cpu = 0;

  REQUIRE(mock_ul.issue(req_a));
  REQUIRE(mock_ul.issue(req_b));

  for (int i = 0; i < 20; ++i) {
    for (auto elem : elements)
      elem->_operate();
  }

  // Each address space resolved to its own physical page; the two must not have
  // been merged into one translation.
  REQUIRE(mock_ll.addresses.size() == 2);
  REQUIRE((mock_ll.addresses[0] == page_for(250) || mock_ll.addresses[0] == page_for(251)));
  REQUIRE((mock_ll.addresses[1] == page_for(250) || mock_ll.addresses[1] == page_for(251)));
  REQUIRE(mock_ll.addresses[0] != mock_ll.addresses[1]);
}
