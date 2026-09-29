#include <catch.hpp>

#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"
#include "vmem.h"

// A perfect TLB hits on every lookup, like any perfect cache (446), but a TLB's
// response data IS the translation. It must therefore answer with the real
// translation from the virtual memory, not echo the request's data field --
// which the core never sets, so an echoing TLB mapped every page to physical
// page 0 and collapsed a workload's whole data footprint into 64 cache blocks
// (605.mcf ran at 10x its real IPC).
//
// The expected addresses below are hand-derived. An unrandomized virtual
// memory hands out frames in fault order starting at 1 MiB (see 601-fixed-ptw),
// and nothing but these translations allocates here, so the first page to
// fault gets frame 0x100, the second 0x101, the third 0x102.

namespace
{
constexpr champsim::address va_a{0x7fff'ffff'd1c0};  // page 0x7fffffffd, offset 0x1c0
constexpr champsim::address va_b{0x0000'0040'2a80};  // page 0x402,      offset 0xa80
constexpr champsim::address va_a2{0x7fff'ffff'd040}; // page of va_a,    offset 0x040

VirtualMemory unrandomized_vmem()
{
  return VirtualMemory{champsim::data::bytes{1 << 12}, 5, champsim::chrono::nanoseconds{640}, champsim::data::bytes{1ull << 30}};
}

// Issue one untranslated load, as the core's CacheBus does, and let it drain.
template <typename Elements>
void load(to_rq_MRP& core, Elements& elements, champsim::address va, VirtualMemory::address_space_id asid)
{
  static uint64_t id = 1;
  to_rq_MRP::request_type pkt;
  pkt.address = va;
  pkt.v_address = va;
  pkt.is_translated = false;
  pkt.cpu = 0;
  pkt.asid[0] = asid[0];
  pkt.asid[1] = asid[1];
  pkt.instr_id = id++;
  REQUIRE(core.issue(pkt));

  for (int i = 0; i < 200; ++i) {
    for (auto elem : elements) {
      elem->_operate();
    }
  }
}
} // namespace

SCENARIO("A perfect DTLB translates a data cache's requests to their real physical pages")
{
  GIVEN("An L1D translated by a perfect DTLB")
  {
    auto vmem = unrandomized_vmem();
    champsim::channel l1d_to_dtlb{};
    do_nothing_MRC l1d_lower;
    do_nothing_MRC dtlb_lower;
    to_rq_MRP core{[](auto x, auto y) {
      return x.v_address == y.v_address;
    }};

    CACHE dtlb{champsim::cache_builder{champsim::defaults::default_dtlb}
                   .name("447a-dtlb")
                   .upper_levels({&l1d_to_dtlb})
                   .lower_level(&dtlb_lower.queues)
                   .set_perfect()
                   .virtual_memory(&vmem)};
    CACHE l1d{champsim::cache_builder{champsim::defaults::default_l1d}
                  .name("447a-l1d")
                  .upper_levels({&core.queues})
                  .lower_level(&l1d_lower.queues)
                  .lower_translate(&l1d_to_dtlb)};

    std::array<champsim::operable*, 5> elements{{&l1d, &dtlb, &core, &l1d_lower, &dtlb_lower}};
    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    WHEN("Loads touch two pages, revisit the first, and a second address space touches the first page's virtual address")
    {
      load(core, elements, va_a, {0, 0});
      load(core, elements, va_b, {0, 0});
      load(core, elements, va_a2, {0, 0});
      load(core, elements, va_a, {1, 1});

      THEN("Each miss reaches the level below at its real physical address")
      {
        REQUIRE_THAT(l1d_lower.addresses, Catch::Matchers::RangeEquals(std::vector{champsim::address{0x1001c0}, champsim::address{0x101a80},
                                                                                   champsim::address{0x100040}, champsim::address{0x1021c0}}));
      }

      THEN("The perfect DTLB never walks") { REQUIRE(dtlb_lower.packet_count() == 0); }
    }
  }
}

SCENARIO("A perfect STLB supplies real translations to the DTLB above it")
{
  GIVEN("An L1D translated by a normal DTLB backed by a perfect STLB")
  {
    auto vmem = unrandomized_vmem();
    champsim::channel l1d_to_dtlb{};
    champsim::channel dtlb_to_stlb{};
    do_nothing_MRC l1d_lower;
    do_nothing_MRC stlb_lower;
    to_rq_MRP core{[](auto x, auto y) {
      return x.v_address == y.v_address;
    }};

    CACHE stlb{champsim::cache_builder{champsim::defaults::default_stlb}
                   .name("447b-stlb")
                   .upper_levels({&dtlb_to_stlb})
                   .lower_level(&stlb_lower.queues)
                   .set_perfect()
                   .virtual_memory(&vmem)};
    // Built as the environment builds every TLB: holding the virtual memory
    // whether or not it is perfect. Only the perfect flag may make it answer.
    CACHE dtlb{champsim::cache_builder{champsim::defaults::default_dtlb}
                   .name("447b-dtlb")
                   .upper_levels({&l1d_to_dtlb})
                   .lower_level(&dtlb_to_stlb)
                   .virtual_memory(&vmem)};
    CACHE l1d{champsim::cache_builder{champsim::defaults::default_l1d}
                  .name("447b-l1d")
                  .upper_levels({&core.queues})
                  .lower_level(&l1d_lower.queues)
                  .lower_translate(&l1d_to_dtlb)};

    std::array<champsim::operable*, 6> elements{{&l1d, &dtlb, &stlb, &core, &l1d_lower, &stlb_lower}};
    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    WHEN("Loads touch two pages and then revisit the first")
    {
      load(core, elements, va_a, {0, 0});
      load(core, elements, va_b, {0, 0});
      load(core, elements, va_a2, {0, 0});

      THEN("Each miss reaches the level below at its real physical address")
      {
        REQUIRE_THAT(l1d_lower.addresses,
                     Catch::Matchers::RangeEquals(std::vector{champsim::address{0x1001c0}, champsim::address{0x101a80}, champsim::address{0x100040}}));
      }

      THEN("The perfect STLB never walks") { REQUIRE(stlb_lower.packet_count() == 0); }

      THEN("The DTLB is not perfect: its two cold pages miss to the STLB, and the revisit hits")
      {
        REQUIRE(stlb.sim_stats.hits.value_or(std::pair{access_type::LOAD, 0u}, 0) == 2);
      }
    }
  }
}

SCENARIO("A perfect TLB translates a request's address even when it carries no virtual address")
{
  // A TLB's own prefetch reaches the TLB below with address = VA and an empty
  // v_address, because prefetch_line sets v_address only for virtual_prefetch
  // caches -- which is why the walker translates address too (ptw.cc). The
  // demand maps page 0x402 to frame 0x100; the prefetch-shaped lookup of the
  // same page must return that frame, not fault virtual page 0 into 0x101.
  GIVEN("A perfect STLB fed directly")
  {
    auto vmem = unrandomized_vmem();
    champsim::channel upper{};
    do_nothing_MRC lower;
    CACHE stlb{champsim::cache_builder{champsim::defaults::default_stlb}
                   .name("447c-stlb")
                   .upper_levels({&upper})
                   .lower_level(&lower.queues)
                   .set_perfect()
                   .virtual_memory(&vmem)};

    std::array<champsim::operable*, 2> elements{{&stlb, &lower}};
    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    WHEN("A demand lookup and a prefetch-shaped lookup of the same page arrive")
    {
      champsim::channel::request_type demand;
      demand.address = va_b;
      demand.v_address = va_b;
      demand.is_translated = true;
      demand.cpu = 0;
      auto prefetch_shaped = demand;
      prefetch_shaped.v_address = champsim::address{};

      REQUIRE(upper.add_rq(demand));
      REQUIRE(upper.add_rq(prefetch_shaped));
      for (int i = 0; i < 50; ++i) {
        for (auto elem : elements) {
          elem->_operate();
        }
      }

      THEN("Both are answered with the page's one frame")
      {
        REQUIRE(std::size(upper.returned) == 2);
        REQUIRE(upper.returned.at(0).data == champsim::address{0x100000});
        REQUIRE(upper.returned.at(1).data == champsim::address{0x100000});
      }
    }
  }
}
