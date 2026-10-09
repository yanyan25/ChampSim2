#include <catch.hpp>

#include <algorithm>
#include <array>
#include <vector>

#include "../../../prefetcher/ip_stride/ip_stride.h"
#include "address.h"
#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
// A MemoryRequestConsumer that records the full packet of every PREFETCH it
// receives, so a test can assert which address space a prefetch was attributed
// to. The `release_MRC`/`do_nothing_MRC` mocks only keep the address or a count.
class prefetch_recording_MRC : public champsim::operable
{
public:
  champsim::channel queues{};
  std::vector<champsim::channel::request_type> prefetches{};

  long operate() override
  {
    auto record = [this](const auto& pkt) {
      if (pkt.type == access_type::PREFETCH)
        prefetches.push_back(pkt);
    };

    std::for_each(std::begin(queues.RQ), std::end(queues.RQ), record);
    std::for_each(std::begin(queues.WQ), std::end(queues.WQ), record);
    std::for_each(std::begin(queues.PQ), std::end(queues.PQ), record);

    queues.RQ.clear();
    queues.WQ.clear();
    queues.PQ.clear();

    return 1; // never deadlock
  }
};
} // namespace

// Stage 3.6 item 3: prefetch_line's origin is now explicit. ip_stride is the one
// prefetcher that defers its issue to prefetcher_cycle_operate, so before the fix
// the prefetch it fired there inherited whatever context the cache had last
// processed -- `CACHE::prefetch_line` stamped the last-writer cursor and left the
// ASID at its unset sentinel. Three accesses from one address space arm the
// lookahead; a sibling address space is then processed; the deferred prefetch must
// still carry the *triggering* address space, not the last one processed.
TEST_CASE("A deferred ip_stride prefetch is attributed to the triggering address space")
{
  prefetch_recording_MRC mock_ll;
  to_rq_MRP mock_ul;

  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("455-ip-stride-origin")
                .upper_levels({&mock_ul.queues})
                .lower_level(&mock_ll.queues)
                .prefetcher<ip_stride>()};

  std::array<champsim::operable*, 3> elements{{&mock_ll, &mock_ul, &uut}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }

  auto make_req = [](champsim::address addr, champsim::address ip, uint8_t asid, uint64_t instr_id) {
    champsim::channel::request_type req;
    req.address = addr;
    req.ip = ip;
    req.cpu = 0;
    req.asid[0] = asid;
    req.asid[1] = asid;
    req.instr_id = instr_id;
    return req;
  };

  const auto ip = champsim::address{0xcafecafe};
  constexpr int64_t stride = 1;

  // Address space 1: three same-IP strided accesses establish the stride and arm
  // the lookahead on the third.
  auto seed = make_req(champsim::address{0xffff'003f}, ip, 1, 1);
  auto test_a = make_req(champsim::address{champsim::block_number{seed.address} + stride}, ip, 1, 2);
  auto test_b = make_req(champsim::address{champsim::block_number{test_a.address} + stride}, ip, 1, 3);
  REQUIRE(mock_ul.issue(seed));
  REQUIRE(mock_ul.issue(test_a));
  REQUIRE(mock_ul.issue(test_b));

  // Address space 2: an unrelated access with a different IP, so it cannot touch
  // address space 1's stride tracker, but it is nonetheless the last access the
  // cache processes. A prefetch must not be attributed to it.
  auto sibling = make_req(champsim::address{0xdead'0000}, champsim::address{0xbeadbeef}, 2, 4);
  REQUIRE(mock_ul.issue(sibling));

  for (auto i = 0; i < 100; ++i)
    for (auto elem : elements)
      elem->_operate();

  // Every recorded prefetch carries the triggering address space, not the sibling's.
  REQUIRE(!mock_ll.prefetches.empty());
  for (const auto& pkt : mock_ll.prefetches) {
    CHECK(pkt.asid[0] == 1);
    CHECK(pkt.asid[1] == 1);
  }
}
