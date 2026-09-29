#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "dram_controller.h"
#include "memory_backend.h"
#include "ramulator2_driver.h"
#include "runtime_config.h"
#include "static_environment.h"
#include "stats_printer.h"
#include "tracereader.h"
#include "vmem.h"

// A misspelled backend must fail at construction, before the simulation starts.
TEST_CASE("An unknown DRAM backend is rejected before simulation")
{
  champsim::runtime_config cfg;
  cfg.set("dram-model=does-not-exist");
  REQUIRE_THROWS_AS(champsim::static_environment{cfg}, std::runtime_error);
}

TEST_CASE("An invalid memory selector names the supported backends")
{
  champsim::runtime_config cfg;
  cfg.set("dram-model=does-not-exist");
  REQUIRE_THROWS_WITH(champsim::make_memory_backend(cfg, {}), Catch::Matchers::ContainsSubstring("legacy") && Catch::Matchers::ContainsSubstring("ramulator2"));
}

TEST_CASE("An unavailable Ramulator backend fails instead of falling back to legacy")
{
  if (champsim::ramulator2_available()) {
    SKIP("native backend is available in this build");
  }
  champsim::runtime_config cfg;
  cfg.set("dram-model=ramulator2");
  REQUIRE_THROWS_WITH(champsim::make_memory_backend(cfg, {}), Catch::Matchers::ContainsSubstring("ramulator2")
                                                                  && Catch::Matchers::ContainsSubstring("not available")
                                                                  && Catch::Matchers::ContainsSubstring("WITH_RAMULATOR2=1"));
}

TEST_CASE("Inactive legacy memory settings point to native YAML before driver construction")
{
  champsim::runtime_config cfg;
  cfg.set("dram-model=ramulator2");
  cfg.set("ramulator2.config=does-not-exist.yaml");
  cfg.set("pmem.frequency=3200");
  REQUIRE_THROWS_WITH(champsim::make_memory_backend(cfg, {}), Catch::Matchers::ContainsSubstring("pmem.frequency")
                                                                  && Catch::Matchers::ContainsSubstring("ramulator2.config")
                                                                  && Catch::Matchers::ContainsSubstring("YAML"));
}

TEST_CASE("Legacy rejects settings for the inactive Ramulator backend")
{
  champsim::runtime_config cfg;
  cfg.set("ramulator2.config=unused.yaml");
  REQUIRE_THROWS_WITH(champsim::make_memory_backend(cfg, {}), Catch::Matchers::ContainsSubstring("ramulator2.config"));
}

TEST_CASE("The legacy backend preserves direct controller responses and timing across phases")
{
  champsim::runtime_config cfg;
  for (const auto* setting : {"pmem.frequency=1200", "pmem.data_rate=2400", "pmem.trp=7", "pmem.trcd=6", "pmem.tcas=5", "pmem.tras=11",
                              "pmem.refresh_period=64", "pmem.rq_size=2", "pmem.wq_size=2", "pmem.channels=2", "pmem.channel_width=4", "pmem.bank_rows=8192",
                              "pmem.bank_columns=256", "pmem.ranks=2", "pmem.bankgroups=2", "pmem.banks=2", "pmem.refreshes_per_period=4096"}) {
    cfg.set(setting);
  }
  champsim::channel wrapped_queue, direct_queue;
  auto backend = champsim::make_memory_backend(cfg, {&wrapped_queue});
  MEMORY_CONTROLLER direct{champsim::chrono::picoseconds{416},
                           champsim::chrono::picoseconds{833},
                           7,
                           6,
                           5,
                           11,
                           champsim::chrono::microseconds{64000},
                           {&direct_queue},
                           2,
                           2,
                           2,
                           champsim::data::bytes{4},
                           8192,
                           256,
                           2,
                           2,
                           2,
                           4096};
  auto& wrapped = backend->clocked_component();
  REQUIRE(dynamic_cast<MEMORY_CONTROLLER*>(&wrapped) != nullptr);
  REQUIRE(backend->size() == champsim::data::bytes{134217728});
  REQUIRE(backend->size() == direct.size());

  // Different addresses, payloads and dependencies catch dropped or misrouted completions.
  const auto request = [](uint64_t index) {
    champsim::channel::request_type result;
    result.address = champsim::address{0x100000 + index * 4096};
    result.v_address = champsim::address{0x900000 + index * 4096};
    result.data = champsim::address{0xbeef0000 + index};
    result.pf_metadata = static_cast<uint32_t>(index + 17);
    result.instr_depend_on_me = {index + 1, index + 100};
    result.response_requested = index % 4 != 0;
    return result;
  };
  const auto response_values = [](const champsim::channel::response_type& response) {
    return std::make_tuple(response.address, response.v_address, response.data, response.pf_metadata, response.instr_depend_on_me);
  };

  std::size_t completions = 0;
  bool saw_backpressure = false;
  champsim::chrono::clock clock;
  wrapped.begin_phase();
  direct.begin_phase();
  for (uint64_t tick = 0; tick < 600; ++tick) {
    if (tick == 2 || tick == 30) {
      // The second measured phase starts with requests still in flight.
      wrapped.end_phase(0);
      direct.end_phase(0);
      wrapped.warmup = false;
      direct.warmup = false;
      wrapped.begin_phase();
      direct.begin_phase();
    }
    if (tick < 10) {
      wrapped_queue.RQ.push_back(request(tick));
      direct_queue.RQ.push_back(request(tick));
    }
    if (tick == 4) {
      for (auto* queue : {&wrapped_queue, &direct_queue}) {
        queue->PQ.push_back(request(15));
        auto write = request(4);
        write.response_requested = false;
        queue->WQ.push_back(write);
      }
    }
    clock.tick(champsim::chrono::picoseconds{250});
    REQUIRE(wrapped.operate_on(clock) == direct.operate_on(clock));
    REQUIRE(wrapped.current_time == direct.current_time);
    REQUIRE(wrapped_queue.RQ.size() == direct_queue.RQ.size());
    REQUIRE(wrapped_queue.PQ.size() == direct_queue.PQ.size());
    REQUIRE(wrapped_queue.WQ.size() == direct_queue.WQ.size());
    saw_backpressure = saw_backpressure || wrapped_queue.RQ.size() > 2;
    REQUIRE(wrapped_queue.returned.size() == direct_queue.returned.size());
    while (!wrapped_queue.returned.empty()) {
      REQUIRE(response_values(wrapped_queue.returned.front()) == response_values(direct_queue.returned.front()));
      ++completions;
      wrapped_queue.returned.pop_front();
      direct_queue.returned.pop_front();
    }
  }
  REQUIRE(saw_backpressure);
  REQUIRE(completions == 8);
  wrapped.end_phase(0);
  direct.end_phase(0);
  const auto stats = backend->statistics();
  REQUIRE(stats.sim_dram.size() == 2);
  REQUIRE(stats.roi_dram.size() == 2);
  for (std::size_t i = 0; i < direct.channels.size(); ++i) {
    REQUIRE(champsim::plain_printer::format(stats.sim_dram[i]) == champsim::plain_printer::format(direct.channels[i].sim_stats));
    REQUIRE(champsim::plain_printer::format(stats.roi_dram[i]) == champsim::plain_printer::format(direct.channels[i].roi_stats));
  }
}

TEST_CASE("Vmem capacity construction preserves controller-based page mappings")
{
  MEMORY_CONTROLLER direct{champsim::chrono::picoseconds{312},
                           champsim::chrono::picoseconds{625},
                           24,
                           24,
                           24,
                           52,
                           champsim::chrono::microseconds{32000},
                           {},
                           64,
                           64,
                           1,
                           champsim::data::bytes{8},
                           8192,
                           256,
                           1,
                           1,
                           1,
                           8192};
  for (const auto seed : {std::optional<uint64_t>{}, std::optional<uint64_t>{1}, std::optional<uint64_t>{27}}) {
    VirtualMemory by_controller{champsim::data::bytes{4096}, 5, champsim::chrono::picoseconds{50000}, direct, seed};
    VirtualMemory by_capacity{champsim::data::bytes{4096}, 5, champsim::chrono::picoseconds{50000}, direct.size(), seed};
    REQUIRE(by_capacity.available_ppages() == 3840);
    for (uint64_t page = 1; page < 65; ++page) {
      const auto asid = VirtualMemory::address_space_id{static_cast<uint8_t>(page % 2), static_cast<uint8_t>(page % 2)};
      REQUIRE(by_capacity.va_to_pa(asid, champsim::page_number{page * 3}) == by_controller.va_to_pa(asid, champsim::page_number{page * 3}));
      REQUIRE(by_capacity.get_pte_pa(asid, champsim::page_number{page * 3}, 1) == by_controller.get_pte_pa(asid, champsim::page_number{page * 3}, 1));
    }
    REQUIRE(by_capacity.available_ppages() == by_controller.available_ppages());
  }
}

namespace champsim
{
std::vector<phase_stats> main(environment& env, std::vector<phase_info>& phases, std::vector<tracereader>& traces, const simulation_knobs& knobs);
}

namespace
{
// No cores are needed to exercise the real phase runner's memory lifecycle.
struct phase_memory : champsim::memory_backend, champsim::operable {
  std::vector<std::string> events;
  unsigned phases = 0;
  champsim::operable& clocked_component() override { return *this; }
  champsim::data::bytes size() const override { return champsim::data::bytes{1 << 24}; }
  std::string_view name() const override { return "phase-test"; }
  void initialize() override { events.emplace_back("initialize"); }
  long operate() override
  {
    events.emplace_back("operate");
    return 1;
  }
  void begin_phase() override
  {
    events.emplace_back(warmup ? "warmup" : "simulation");
    ++phases;
  }
  champsim::memory_statistics statistics() const override
  {
    dram_stats stat;
    stat.RQ_ROW_BUFFER_MISS = phases;
    return {{stat}, {stat}};
  }
  void finalize() override { events.emplace_back("finalize"); }
};

struct phase_environment : champsim::environment {
  phase_memory memory;
  std::vector<std::reference_wrapper<O3_CPU>> cpu_view() override { return {}; }
  std::vector<std::reference_wrapper<CACHE>> cache_view() override { return {}; }
  std::vector<std::reference_wrapper<PageTableWalker>> ptw_view() override { return {}; }
  champsim::memory_backend& memory_view() override { return memory; }
  std::vector<std::reference_wrapper<champsim::operable>> operable_view() override { return {memory}; }
};
} // namespace

TEST_CASE("The phase runner snapshots every measured phase and finalizes memory once without draining")
{
  phase_environment env;
  std::vector<champsim::phase_info> phases{{"warmup", true, 0, {}, {}}, {"first", false, 0, {}, {}}, {"second", false, 0, {}, {}}};
  std::vector<champsim::tracereader> traces;
  const auto result = champsim::main(env, phases, traces, {});
  REQUIRE(env.memory.events == std::vector<std::string>{"initialize", "warmup", "simulation", "simulation", "finalize"});
  REQUIRE(result.size() == 2);
  REQUIRE(result[0].roi_dram_stats.at(0).RQ_ROW_BUFFER_MISS == 2);
  REQUIRE(result[1].roi_dram_stats.at(0).RQ_ROW_BUFFER_MISS == 3);
  REQUIRE(result[0].sim_dram_stats.at(0).RQ_ROW_BUFFER_MISS == 2);
  REQUIRE(result[1].sim_dram_stats.at(0).RQ_ROW_BUFFER_MISS == 3);
}
