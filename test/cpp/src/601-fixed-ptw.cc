#include <algorithm>
#include <catch.hpp>
#include <initializer_list>
#include <memory>
#include <string_view>

#include "static_environment.h"
#include "vmem.h"

namespace
{
// Exercise the same configuration, walker and channels as a simulation. The
// lower cache is deliberately not operated: a fixed translation must not need it.
struct fixed_ptw_fixture {
  champsim::runtime_config cfg;
  std::unique_ptr<champsim::static_environment> env;

  // Not a default argument: GCC 11 and older then stop treating the constructor
  // below as an initializer-list constructor, and fixed_ptw_fixture{"a", "b"} fails.
  fixed_ptw_fixture() : fixed_ptw_fixture(std::initializer_list<std::string_view>{}) {}
  explicit fixed_ptw_fixture(std::initializer_list<std::string_view> overrides)
  {
    cfg.set("dram-model=legacy");
    cfg.set("pmem.bank_rows=64");
    cfg.set("vmem.randomization=false");
    cfg.set("vmem.minor_fault_penalty=9999");
    cfg.set("ptw.cpu0_ptw.model=fixed");
    for (auto value : overrides) {
      cfg.set(value);
    }
    env = std::make_unique<champsim::static_environment>(cfg);
    walker().warmup = false;
  }

  PageTableWalker& walker() { return env->ptw_view().front(); }
  CACHE& cache(std::string_view name)
  {
    const auto caches = env->cache_view();
    return *std::find_if(caches.begin(), caches.end(), [name](const CACHE& c) { return c.NAME == name; });
  }
  champsim::channel& input() { return *cache("cpu0_STLB").lower_level; }
  champsim::channel& lower() { return *cache("cpu0_L1D").upper_levels.front(); }
  void tick(int count = 1)
  {
    for (int i = 0; i < count; ++i) {
      walker()._operate();
    }
  }
  void issue(uint64_t address = 0x12345000, VirtualMemory::address_space_id asid = {0, 0}, bool response = true)
  {
    champsim::channel::request_type packet;
    packet.address = champsim::address{address};
    packet.v_address = packet.address;
    packet.cpu = 0;
    packet.asid[0] = asid[0];
    packet.asid[1] = asid[1];
    packet.response_requested = response;
    packet.pf_metadata = 37;
    packet.instr_depend_on_me = {11, 23};
    REQUIRE(input().add_rq(packet));
  }
};
} // namespace

TEST_CASE("Fixed PTW completes after 200 CPU cycles without lower-level traffic", "[fixed-ptw]")
{
  fixed_ptw_fixture f;
  f.issue();
  f.tick(); // Admission at 250 ps; deadline 50,250 ps.
  REQUIRE(f.lower().RQ.empty());
  f.tick(199);
  REQUIRE(f.input().returned.empty());
  REQUIRE(f.walker()._operate() == 1);
  REQUIRE(f.input().returned.size() == 1);
  const auto& response = f.input().returned.front();
  CHECK(response.address == champsim::address{0x12345000});
  CHECK(response.v_address == champsim::address{0x12345000});
  CHECK(response.data == champsim::address{0x100000}); // First unrandomized data page, no CR3 allocation.
  CHECK(response.pf_metadata == 37);
  CHECK(response.instr_depend_on_me == std::vector<uint64_t>{11, 23});
  CHECK(f.lower().RQ.empty());
  CHECK(f.lower().sim_stats.RQ_ACCESS == 0);
  CHECK(f.cfg.unconsulted_keys().empty());
  f.tick(201);
  CHECK(f.input().returned.size() == 1);
}

TEST_CASE("Fixed PTW delay uses CPU cycles and rounds completion to the walker clock", "[fixed-ptw]")
{
  fixed_ptw_fixture f{"ptw.cpu0_ptw.fixed_latency=3", "ooo_cpu.cpu0.frequency=2000", "ptw.cpu0_ptw.frequency=1600"};
  f.issue();
  f.tick();  // Admit at 625 ps. Three CPU cycles = 1,500 ps; deadline 2,125 ps.
  f.tick(2); // 1,875 ps is too early.
  REQUIRE(f.input().returned.empty());
  f.tick(); // 2,500 ps is the first walker tick at/after the deadline.
  REQUIRE(f.input().returned.size() == 1);
}

TEST_CASE("Fixed PTW zero delay still follows admission and completion ordering", "[fixed-ptw]")
{
  fixed_ptw_fixture f{"ptw.cpu0_ptw.fixed_latency=0"};
  f.issue();
  f.tick();
  REQUIRE(f.input().returned.empty());
  f.tick();
  REQUIRE(f.input().returned.size() == 1);
}

TEST_CASE("Fixed PTW warmup admissions bypass latency without losing requests at phase reset", "[fixed-ptw]")
{
  fixed_ptw_fixture f;
  f.walker().warmup = true;
  f.issue();
  f.tick();
  f.walker().warmup = false;
  f.walker().begin_phase();
  f.tick();
  REQUIRE(f.input().returned.size() == 1);
  REQUIRE(f.lower().RQ.empty());
}

TEST_CASE("Fixed PTW retains measured deadlines across phase resets", "[fixed-ptw]")
{
  fixed_ptw_fixture f{"ptw.cpu0_ptw.fixed_latency=4"};
  f.issue();
  f.tick(2);
  f.walker().warmup = true;
  f.walker().begin_phase();
  f.tick(2);
  REQUIRE(f.input().returned.empty());
  f.tick();
  REQUIRE(f.input().returned.size() == 1);
}

TEST_CASE("Fixed PTW bounds pending requests and retries the upstream head", "[fixed-ptw]")
{
  fixed_ptw_fixture f{"ptw.cpu0_ptw.fixed_latency=3", "ptw.cpu0_ptw.mshr_size=1"};
  f.issue();
  f.issue(0x23456000);
  f.tick();
  REQUIRE(f.input().RQ.size() == 1);
  REQUIRE(f.walker()._operate() == 0); // Waiting is not simulated progress.
  f.tick();
  REQUIRE(f.input().returned.empty());
  f.tick(); // Complete the first, then admit the second at this tick.
  REQUIRE(f.input().returned.size() == 1);
  REQUIRE(f.input().RQ.empty());
  f.tick(2);
  REQUIRE(f.input().returned.size() == 1);
  f.tick();
  REQUIRE(f.input().returned.size() == 2);
  CHECK(f.input().returned.back().address == champsim::address{0x23456000});
}

TEST_CASE("Fixed PTW applies admission and response bandwidth", "[fixed-ptw]")
{
  fixed_ptw_fixture f{"ptw.cpu0_ptw.fixed_latency=0", "ptw.cpu0_ptw.max_read=2", "ptw.cpu0_ptw.max_write=1"};
  for (uint64_t i = 0; i < 3; ++i) {
    f.issue(0x12345000 + i * 4096);
  }
  f.tick();
  REQUIRE(f.input().RQ.size() == 1);
  f.tick();
  REQUIRE(f.input().RQ.empty());
  REQUIRE(f.input().returned.size() == 1);
  f.tick();
  REQUIRE(f.input().returned.size() == 2);
  f.tick();
  REQUIRE(f.input().returned.size() == 3);
}

TEST_CASE("Fixed PTW releases no-response requests and reuses per-ASID page mappings", "[fixed-ptw]")
{
  fixed_ptw_fixture f{"ptw.cpu0_ptw.fixed_latency=0", "ptw.cpu0_ptw.mshr_size=1"};
  f.issue(0x12345000, {0, 0}, false);
  f.tick(2);
  REQUIRE(f.input().returned.empty());
  f.issue(0x12345080, {0, 0});
  f.tick(2);
  REQUIRE(f.input().returned.size() == 1);
  CHECK(f.input().returned.back().data == champsim::address{0x100000});
  f.issue(0x12345000, {1, 1});
  f.tick(2);
  REQUIRE(f.input().returned.size() == 2);
  CHECK(f.input().returned.back().data == champsim::address{0x101000});
}

TEST_CASE("Fixed PTW rejects invalid configuration before running", "[fixed-ptw]")
{
  for (auto invalid : {"ptw.cpu0_ptw.model=typo", "ptw.cpu0_ptw.fixed_latency=-1", "ptw.cpu0_ptw.fixed_latency=1.5",
                       "ptw.cpu0_ptw.fixed_latency=9223372036854775807", "ptw.cpu0_ptw.mshr_size=0", "ptw.cpu0_ptw.max_read=0", "ptw.cpu0_ptw.max_write=0"}) {
    CAPTURE(invalid);
    REQUIRE_THROWS_AS(fixed_ptw_fixture{invalid}, std::runtime_error);
  }
}

TEST_CASE("Detailed PTW does not silently consume the inactive fixed-latency setting", "[fixed-ptw]")
{
  fixed_ptw_fixture f{"ptw.cpu0_ptw.model=detailed", "ptw.cpu0_ptw.fixed_latency=200"};
  const auto unused = f.cfg.unconsulted_keys();
  REQUIRE(unused.size() == 1);
  CHECK(unused.front().find("fixed_latency") != std::string::npos);
}
