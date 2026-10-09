#include "static_environment.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "access_type.h"
#include "champsim.h"
#include "champsim_assert.h"
#include "defaults.hpp"
#include "msl/bits.h"

#if __has_include("registry.inc")
#include "registry.inc"
#define CHAMPSIM_HAVE_REGISTRY 1
#endif

namespace
{
// One channel per EDGE, its queue geometry taken from the LOWER component --
// which is why three separate channels feed the STLB. Naming the edges keeps
// the wiring readable; the configuration layer derived the same graph by
// matching component names and printing &channels.at(N).
enum edge : std::size_t {
  ptw_to_l1d,
  dtlb_to_stlb,
  itlb_to_stlb,
  l1d_to_l2c,
  l1i_to_l2c,
  l2c_to_llc,
  stlb_to_ptw,
  l1d_to_dtlb,
  l1i_to_itlb,
  l2c_to_stlb,
  core_to_l1i,
  core_to_l1d,
  per_core_edge_count
};

constexpr std::size_t chan(std::size_t cpu, edge which) { return (cpu * static_cast<std::size_t>(per_core_edge_count)) + static_cast<std::size_t>(which); }

// The LLC -> DRAM edge is SHARED: one LLC and one memory controller, so one
// channel however many cores there are. It sits after the per-core block.
constexpr std::size_t llc_to_dram_chan(std::size_t cpus) { return cpus * static_cast<std::size_t>(per_core_edge_count); }

std::string lower(std::string text)
{
  std::transform(std::begin(text), std::end(text), std::begin(text), [](char chr) { return static_cast<char>(std::tolower(static_cast<unsigned char>(chr))); });
  return text;
}

// Runtime-configuration key prefixes: component names lower-cased, exactly as
// the statistics document's [config] section names them.
std::string cache_key(std::size_t cpu, std::string_view level) { return "cache." + lower(champsim::static_environment::cache_name(cpu, level)); }
std::string ptw_key(std::size_t cpu) { return "ptw." + lower(champsim::static_environment::ptw_name(cpu)); }
std::string core_key(std::size_t cpu) { return "ooo_cpu." + lower(champsim::static_environment::core_name(cpu)); }

champsim::channel queue(const champsim::runtime_config& cfg, const std::string& prefix, std::size_t rq, std::size_t pq, std::size_t wq, unsigned offset_bits,
                        bool match_offset)
{
  return champsim::channel{cfg.value<std::size_t>(prefix + ".rq_size", rq), cfg.value<std::size_t>(prefix + ".pq_size", pq),
                           cfg.value<std::size_t>(prefix + ".wq_size", wq), champsim::data::bits{offset_bits}, match_offset};
}

// MHz -> picoseconds, truncating, as the configuration layer did.
champsim::chrono::picoseconds period(const champsim::runtime_config& cfg, const std::string& key, double mhz)
{
  return champsim::chrono::picoseconds{static_cast<champsim::chrono::picoseconds::rep>(1000000.0 / cfg.positive_value<double>(key, mhz))};
}

std::optional<champsim::chrono::picoseconds> fixed_ptw_latency(const champsim::runtime_config& cfg, std::size_t cpu)
{
  const auto key = ptw_key(cpu);
  const auto model = cfg.value<std::string>(key + ".model", "detailed");
  if (model == "detailed") {
    return std::nullopt;
  }
  if (model != "fixed") {
    throw std::runtime_error{"runtime config: " + key + ".model must be 'detailed' or 'fixed'"};
  }

  using duration = champsim::chrono::picoseconds;
  const auto cycles = cfg.value<duration::rep>(key + ".fixed_latency", 200);
  const auto cpu_period = period(cfg, core_key(cpu) + ".frequency", 4000);
  if (cycles < 0 || cpu_period.count() <= 0 || cycles > duration::max().count() / cpu_period.count()) {
    throw std::runtime_error{"runtime config: " + key + ".fixed_latency must be nonnegative and representable in picoseconds at the CPU frequency"};
  }
  return cpu_period * cycles;
}

// The access types that activate a cache's prefetcher, as a comma-separated
// list of access_type names -- the JSON's own format ("LOAD,PREFETCH"), kept
// because the store holds scalars and this parameter is a set.
std::vector<access_type> prefetch_activate(const champsim::runtime_config& cfg, const std::string& prefix)
{
  const auto key = prefix + ".prefetch_activate";
  const auto spelled = cfg.value<std::string>(key, "LOAD,PREFETCH");

  std::vector<access_type> mask{};
  std::string_view rest{spelled};
  while (!std::empty(rest)) {
    const auto comma = rest.find(',');
    auto name = rest.substr(0, comma);
    rest = (comma == std::string_view::npos) ? std::string_view{} : rest.substr(comma + 1);

    // Tolerate spaces around the separator; reject anything else by name, so a
    // typo cannot silently disable prefetching on a cache.
    while (!std::empty(name) && name.front() == ' ') {
      name.remove_prefix(1);
    }
    while (!std::empty(name) && name.back() == ' ') {
      name.remove_suffix(1);
    }
    if (std::empty(name)) {
      continue;
    }

    const auto found = std::find(std::begin(access_type_names), std::end(access_type_names), name);
    if (found == std::end(access_type_names)) {
      throw std::runtime_error{
          fmt::format("runtime config: {} = '{}': '{}' is not an access type (expected one of {})", key, spelled, name, fmt::join(access_type_names, ", "))};
    }
    mask.push_back(static_cast<access_type>(std::distance(std::begin(access_type_names), found)));
  }
  return mask;
}

// Page-frame randomization: `false` disables it, and any integer is the seed
// (`true` means 1). That type-dependent meaning is the JSON's, preserved so a
// configuration carried over from it behaves identically.
std::optional<uint64_t> randomization(const champsim::runtime_config& cfg)
{
  if (cfg.holds<bool>("vmem.randomization")) {
    return cfg.value<bool>("vmem.randomization", true) ? std::optional<uint64_t>{1} : std::nullopt;
  }
  return std::optional<uint64_t>{cfg.value<uint64_t>("vmem.randomization", 1)};
}

champsim::bandwidth::maximum_type configured_bandwidth(const champsim::runtime_config& cfg, const std::string& key, long fallback)
{
  const auto value = cfg.value<long>(key, fallback);
  if (value < 0) {
    throw std::runtime_error{"runtime config: " + key + " must be nonnegative"};
  }
  return champsim::bandwidth::maximum_type{value};
}

champsim::data::bytes::rep configured_pte_page_size(const champsim::runtime_config& cfg)
{
  constexpr std::string_view key = "vmem.pte_page_size";
  const auto value = cfg.positive_value<champsim::data::bytes::rep>(key, 4096);
  if (value <= 1024 || value > PAGE_SIZE || !champsim::is_power_of_2(value)) {
    throw std::runtime_error{"runtime config: vmem.pte_page_size must be a power of two greater than 1024 and no larger than PAGE_SIZE"};
  }
  return value;
}

std::size_t configured_register_file_size(const champsim::runtime_config& cfg, const std::string& key)
{
  const auto value = cfg.value<std::size_t>(key, 128);
  constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<PHYSICAL_REGISTER_ID>::max());
  if (value == 0 || value > maximum) {
    throw std::runtime_error{"runtime config: " + key + " must be between 1 and " + std::to_string(maximum)};
  }
  return value;
}

// The simulation's time quantum: do_phase() ticks the shared clock by the
// SMALLEST clock_period among the operables, so a duration expressed in cycles
// has to be scaled by this and not by any one component's period.
//
// The selected backend supplies its clock period, and every other component's
// frequency participates in the sweep. A literal would be right only while nothing overrides a
// frequency: raise one component to 5000 MHz and a baked 250 ps overstates
// every cycle-denominated duration by 25%, silently.
champsim::chrono::picoseconds compute_time_quantum(const champsim::runtime_config& cfg, champsim::chrono::picoseconds memory_period)
{
  auto shortest = memory_period;
  const auto consider = [&cfg, &shortest](const std::string& key) {
    shortest = std::min(shortest, period(cfg, key, 4000));
  };

  for (std::size_t cpu = 0; cpu < champsim::defs::num_cpus; ++cpu) {
    consider(core_key(cpu) + ".frequency");
    consider(ptw_key(cpu) + ".frequency");
    for (const auto* level : {"DTLB", "ITLB", "L1D", "L1I", "L2C", "STLB"}) {
      consider(cache_key(cpu, level) + ".frequency");
    }
  }
  consider("cache.llc.frequency");
  return shortest;
}
} // namespace

champsim::chrono::picoseconds champsim::static_environment::time_quantum(const runtime_config& cfg)
{
  return time_quantum(cfg, period(cfg, "pmem.frequency", 1600.0));
}

champsim::chrono::picoseconds champsim::static_environment::time_quantum(const runtime_config& cfg, chrono::picoseconds memory_period)
{
  return compute_time_quantum(cfg, memory_period);
}

std::string champsim::static_environment::core_name(std::size_t cpu) { return "cpu" + std::to_string(cpu); }
std::string champsim::static_environment::cache_name(std::size_t cpu, std::string_view level) { return "cpu" + std::to_string(cpu) + "_" + std::string{level}; }
std::string champsim::static_environment::ptw_name(std::size_t cpu) { return "cpu" + std::to_string(cpu) + "_PTW"; }

champsim::static_environment::static_environment(const runtime_config& cfg)
    : channels([&cfg] {
        std::vector<channel> made{};
        made.reserve((defs::num_cpus * static_cast<std::size_t>(per_core_edge_count)) + 1);
        for (std::size_t cpu = 0; cpu < defs::num_cpus; ++cpu) {
          const auto l1i = cache_key(cpu, "L1I");
          const auto l1d = cache_key(cpu, "L1D");
          const auto l2c = cache_key(cpu, "L2C");
          const auto itlb = cache_key(cpu, "ITLB");
          const auto dtlb = cache_key(cpu, "DTLB");
          const auto stlb = cache_key(cpu, "STLB");
          const auto block_bits = champsim::lg2(BLOCK_SIZE);
          const auto page_bits = champsim::lg2(PAGE_SIZE);

          made.push_back(queue(cfg, l1d, 64, 8, 64, block_bits, true));
          made.push_back(queue(cfg, stlb, 32, 0, 32, page_bits, false));
          made.push_back(queue(cfg, stlb, 32, 0, 32, page_bits, false));
          made.push_back(queue(cfg, l2c, 32, 16, 32, block_bits, false));
          made.push_back(queue(cfg, l2c, 32, 16, 32, block_bits, false));
          made.push_back(queue(cfg, "cache.llc", 32, 32, 32, block_bits, false));
          made.push_back(channel{cfg.value<std::size_t>(ptw_key(cpu) + ".rq_size", 16), 0, 0, champsim::data::bits{page_bits}, false});
          made.push_back(queue(cfg, dtlb, 16, 0, 16, page_bits, true));
          made.push_back(queue(cfg, itlb, 16, 0, 16, page_bits, true));
          made.push_back(queue(cfg, stlb, 32, 0, 32, page_bits, false));
          made.push_back(queue(cfg, l1i, 64, 32, 64, block_bits, true));
          made.push_back(queue(cfg, l1d, 64, 8, 64, block_bits, true));
        }
        // The shared LLC -> DRAM feeder, unbounded: the memory controller keeps
        // its own queues.
        made.push_back(channel{std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max(),
                               champsim::data::bits{champsim::lg2(BLOCK_SIZE)}, false});
        return made;
      }()),
      memory(make_memory_backend(cfg, {&channels.at(llc_to_dram_chan(defs::num_cpus))})),
      vmem(champsim::data::bytes{configured_pte_page_size(cfg)}, cfg.value<std::size_t>("vmem.num_levels", 5),
           time_quantum(cfg, memory->clocked_component().clock_period) * cfg.value<champsim::chrono::picoseconds::rep>("vmem.minor_fault_penalty", 200),
           memory->size(), randomization(cfg))
{
  ptws.reserve(defs::num_cpus);
  for (std::size_t cpu = 0; cpu < defs::num_cpus; ++cpu) {
    const auto key = ptw_key(cpu);
    ptws.emplace_back(champsim::ptw_builder{champsim::defaults::default_ptw}
                          .name(ptw_name(cpu))
                          .upper_levels({&channels.at(chan(cpu, stlb_to_ptw))})
                          .virtual_memory(&vmem)
                          .cpu(static_cast<uint32_t>(cpu))
                          .asid({0, 0}) // Stage 2 single-context: the one address space is trace 0's ASID
                          .lower_level(&channels.at(chan(cpu, ptw_to_l1d)))
                          .mshr_size(cfg.value<uint32_t>(key + ".mshr_size", 5))
                          .tag_bandwidth(configured_bandwidth(cfg, key + ".max_read", 2))
                          .fill_bandwidth(configured_bandwidth(cfg, key + ".max_write", 2))
                          .clock_period(period(cfg, key + ".frequency", 4000))
                          .fixed_latency(fixed_ptw_latency(cfg, cpu))
                          .add_pscl(5, cfg.value<uint32_t>(key + ".pscl5_set", 1), cfg.value<uint32_t>(key + ".pscl5_way", 2))
                          .add_pscl(4, cfg.value<uint32_t>(key + ".pscl4_set", 1), cfg.value<uint32_t>(key + ".pscl4_way", 4))
                          .add_pscl(3, cfg.value<uint32_t>(key + ".pscl3_set", 2), cfg.value<uint32_t>(key + ".pscl3_way", 4))
                          .add_pscl(2, cfg.value<uint32_t>(key + ".pscl2_set", 4), cfg.value<uint32_t>(key + ".pscl2_way", 8)));
  }

  // Cache order is load-bearing: it is the per-cycle operate() order. The
  // configuration layer emitted them sorted by name, so LLC first, then the
  // per-core caches alphabetically.
  caches.reserve(1 + (defs::num_cpus * 6));
  caches.emplace_back(champsim::cache_builder{champsim::defaults::default_llc}
                          .name("LLC")
                          .upper_levels([this] {
                            std::vector<channel*> upper{};
                            for (std::size_t cpu = 0; cpu < defs::num_cpus; ++cpu) {
                              upper.push_back(&channels.at(chan(cpu, l2c_to_llc)));
                            }
                            return upper;
                          }())
                          .lower_level(&channels.at(llc_to_dram_chan(defs::num_cpus)))
                          .sets(cfg.positive_value<uint32_t>("cache.llc.sets", 2048))
                          .ways(cfg.positive_value<uint32_t>("cache.llc.ways", 16))
                          .pq_size(cfg.value<uint32_t>("cache.llc.pq_size", 32))
                          .mshr_size(cfg.value<uint32_t>("cache.llc.mshr_size", 64))
                          .latency(cfg.value<uint64_t>("cache.llc.latency", 20))
                          .tag_bandwidth(configured_bandwidth(cfg, "cache.llc.max_tag_check", 1))
                          .fill_bandwidth(configured_bandwidth(cfg, "cache.llc.max_fill", 1))
                          .offset_bits(champsim::data::bits{champsim::lg2(BLOCK_SIZE)})
                          .prefetch_activate(prefetch_activate(cfg, "cache.llc"))
                          .clock_period(period(cfg, "cache.llc.frequency", 4000))
                          .prefetch_as_load(cfg.value<bool>("cache.llc.prefetch_as_load", false))
                          .virtual_prefetch(cfg.value<bool>("cache.llc.virtual_prefetch", false))
                          .perfect(cfg.value<bool>("cache.llc.perfect", false)));

  for (std::size_t cpu = 0; cpu < defs::num_cpus; ++cpu) {
    const auto dtlb = cache_key(cpu, "DTLB");
    const auto itlb = cache_key(cpu, "ITLB");
    const auto l1d = cache_key(cpu, "L1D");
    const auto l1i = cache_key(cpu, "L1I");
    const auto l2c = cache_key(cpu, "L2C");
    const auto stlb = cache_key(cpu, "STLB");
    const auto page_bits = champsim::lg2(PAGE_SIZE);
    const auto block_bits = champsim::lg2(BLOCK_SIZE);

    caches.emplace_back(champsim::cache_builder{champsim::defaults::default_dtlb}
                            .name(cache_name(cpu, "DTLB"))
                            .upper_levels({&channels.at(chan(cpu, l1d_to_dtlb))})
                            .lower_level(&channels.at(chan(cpu, dtlb_to_stlb)))
                            .sets(cfg.positive_value<uint32_t>(dtlb + ".sets", 16))
                            .ways(cfg.positive_value<uint32_t>(dtlb + ".ways", 4))
                            .pq_size(cfg.value<uint32_t>(dtlb + ".pq_size", 0))
                            .mshr_size(cfg.value<uint32_t>(dtlb + ".mshr_size", 8))
                            .latency(cfg.value<uint64_t>(dtlb + ".latency", 1))
                            .tag_bandwidth(configured_bandwidth(cfg, dtlb + ".max_tag_check", 2))
                            .fill_bandwidth(configured_bandwidth(cfg, dtlb + ".max_fill", 2))
                            .offset_bits(champsim::data::bits{page_bits})
                            .prefetch_activate(prefetch_activate(cfg, dtlb))
                            .clock_period(period(cfg, dtlb + ".frequency", 4000))
                            .prefetch_as_load(cfg.value<bool>(dtlb + ".prefetch_as_load", false))
                            .perfect(cfg.value<bool>(dtlb + ".perfect", false))
                            .virtual_memory(&vmem));

    caches.emplace_back(champsim::cache_builder{champsim::defaults::default_itlb}
                            .name(cache_name(cpu, "ITLB"))
                            .upper_levels({&channels.at(chan(cpu, l1i_to_itlb))})
                            .lower_level(&channels.at(chan(cpu, itlb_to_stlb)))
                            .sets(cfg.positive_value<uint32_t>(itlb + ".sets", 16))
                            .ways(cfg.positive_value<uint32_t>(itlb + ".ways", 4))
                            .pq_size(cfg.value<uint32_t>(itlb + ".pq_size", 0))
                            .mshr_size(cfg.value<uint32_t>(itlb + ".mshr_size", 8))
                            .latency(cfg.value<uint64_t>(itlb + ".latency", 1))
                            .tag_bandwidth(configured_bandwidth(cfg, itlb + ".max_tag_check", 2))
                            .fill_bandwidth(configured_bandwidth(cfg, itlb + ".max_fill", 2))
                            .offset_bits(champsim::data::bits{page_bits})
                            .prefetch_activate(prefetch_activate(cfg, itlb))
                            .clock_period(period(cfg, itlb + ".frequency", 4000))
                            .prefetch_as_load(cfg.value<bool>(itlb + ".prefetch_as_load", false))
                            .perfect(cfg.value<bool>(itlb + ".perfect", false))
                            .virtual_memory(&vmem));

    caches.emplace_back(champsim::cache_builder{champsim::defaults::default_l1d}
                            .name(cache_name(cpu, "L1D"))
                            .upper_levels({&channels.at(chan(cpu, ptw_to_l1d)), &channels.at(chan(cpu, core_to_l1d))})
                            .lower_level(&channels.at(chan(cpu, l1d_to_l2c)))
                            .lower_translate(&channels.at(chan(cpu, l1d_to_dtlb)))
                            .sets(cfg.positive_value<uint32_t>(l1d + ".sets", 64))
                            .ways(cfg.positive_value<uint32_t>(l1d + ".ways", 12))
                            .pq_size(cfg.value<uint32_t>(l1d + ".pq_size", 8))
                            .mshr_size(cfg.value<uint32_t>(l1d + ".mshr_size", 16))
                            .latency(cfg.value<uint64_t>(l1d + ".latency", 5))
                            .tag_bandwidth(configured_bandwidth(cfg, l1d + ".max_tag_check", 2))
                            .fill_bandwidth(configured_bandwidth(cfg, l1d + ".max_fill", 2))
                            .offset_bits(champsim::data::bits{block_bits})
                            .prefetch_activate(prefetch_activate(cfg, l1d))
                            .clock_period(period(cfg, l1d + ".frequency", 4000))
                            .prefetch_as_load(cfg.value<bool>(l1d + ".prefetch_as_load", false))
                            .virtual_prefetch(cfg.value<bool>(l1d + ".virtual_prefetch", false))
                            .perfect(cfg.value<bool>(l1d + ".perfect", false)));

    caches.emplace_back(champsim::cache_builder{champsim::defaults::default_l1i}
                            .name(cache_name(cpu, "L1I"))
                            .upper_levels({&channels.at(chan(cpu, core_to_l1i))})
                            .lower_level(&channels.at(chan(cpu, l1i_to_l2c)))
                            .lower_translate(&channels.at(chan(cpu, l1i_to_itlb)))
                            .sets(cfg.positive_value<uint32_t>(l1i + ".sets", 64))
                            .ways(cfg.positive_value<uint32_t>(l1i + ".ways", 8))
                            .pq_size(cfg.value<uint32_t>(l1i + ".pq_size", 32))
                            .mshr_size(cfg.value<uint32_t>(l1i + ".mshr_size", 8))
                            .latency(cfg.value<uint64_t>(l1i + ".latency", 4))
                            .tag_bandwidth(configured_bandwidth(cfg, l1i + ".max_tag_check", 2))
                            .fill_bandwidth(configured_bandwidth(cfg, l1i + ".max_fill", 2))
                            .offset_bits(champsim::data::bits{block_bits})
                            .prefetch_activate(prefetch_activate(cfg, l1i))
                            .clock_period(period(cfg, l1i + ".frequency", 4000))
                            .prefetch_as_load(cfg.value<bool>(l1i + ".prefetch_as_load", false))
                            .virtual_prefetch(cfg.value<bool>(l1i + ".virtual_prefetch", true))
                            .perfect(cfg.value<bool>(l1i + ".perfect", false)));

    caches.emplace_back(champsim::cache_builder{champsim::defaults::default_l2c}
                            .name(cache_name(cpu, "L2C"))
                            .upper_levels({&channels.at(chan(cpu, l1d_to_l2c)), &channels.at(chan(cpu, l1i_to_l2c))})
                            .lower_level(&channels.at(chan(cpu, l2c_to_llc)))
                            .lower_translate(&channels.at(chan(cpu, l2c_to_stlb)))
                            .sets(cfg.positive_value<uint32_t>(l2c + ".sets", 1024))
                            .ways(cfg.positive_value<uint32_t>(l2c + ".ways", 8))
                            .pq_size(cfg.value<uint32_t>(l2c + ".pq_size", 16))
                            .mshr_size(cfg.value<uint32_t>(l2c + ".mshr_size", 32))
                            .latency(cfg.value<uint64_t>(l2c + ".latency", 10))
                            .tag_bandwidth(configured_bandwidth(cfg, l2c + ".max_tag_check", 1))
                            .fill_bandwidth(configured_bandwidth(cfg, l2c + ".max_fill", 1))
                            .offset_bits(champsim::data::bits{block_bits})
                            .prefetch_activate(prefetch_activate(cfg, l2c))
                            .clock_period(period(cfg, l2c + ".frequency", 4000))
                            .prefetch_as_load(cfg.value<bool>(l2c + ".prefetch_as_load", false))
                            .virtual_prefetch(cfg.value<bool>(l2c + ".virtual_prefetch", false))
                            .perfect(cfg.value<bool>(l2c + ".perfect", false)));

    caches.emplace_back(champsim::cache_builder{champsim::defaults::default_stlb}
                            .name(cache_name(cpu, "STLB"))
                            .upper_levels({&channels.at(chan(cpu, dtlb_to_stlb)), &channels.at(chan(cpu, itlb_to_stlb)), &channels.at(chan(cpu, l2c_to_stlb))})
                            .lower_level(&channels.at(chan(cpu, stlb_to_ptw)))
                            .sets(cfg.positive_value<uint32_t>(stlb + ".sets", 128))
                            .ways(cfg.positive_value<uint32_t>(stlb + ".ways", 12))
                            .pq_size(cfg.value<uint32_t>(stlb + ".pq_size", 0))
                            .mshr_size(cfg.value<uint32_t>(stlb + ".mshr_size", 16))
                            .latency(cfg.value<uint64_t>(stlb + ".latency", 8))
                            .tag_bandwidth(configured_bandwidth(cfg, stlb + ".max_tag_check", 1))
                            .fill_bandwidth(configured_bandwidth(cfg, stlb + ".max_fill", 1))
                            .offset_bits(champsim::data::bits{page_bits})
                            .prefetch_activate(prefetch_activate(cfg, stlb))
                            .clock_period(period(cfg, stlb + ".frequency", 4000))
                            .prefetch_as_load(cfg.value<bool>(stlb + ".prefetch_as_load", false))
                            .perfect(cfg.value<bool>(stlb + ".perfect", false))
                            .virtual_memory(&vmem));
  }

  cores.reserve(defs::num_cpus);
  for (std::size_t cpu = 0; cpu < defs::num_cpus; ++cpu) {
    const auto key = core_key(cpu);
    auto& l1i = caches.at(1 + (cpu * 6) + 3);
    auto& l1d = caches.at(1 + (cpu * 6) + 2);
    cores.emplace_back(champsim::core_builder{champsim::defaults::default_core}
                           .index(static_cast<uint32_t>(cpu))
                           .l1i(&l1i)
                           .l1i_bandwidth(l1i.MAX_TAG)
                           .l1d_bandwidth(l1d.MAX_TAG)
                           .fetch_queues(&channels.at(chan(cpu, core_to_l1i)))
                           .data_queues(&channels.at(chan(cpu, core_to_l1d)))
                           .ifetch_buffer_size(cfg.value<std::size_t>(key + ".ifetch_buffer_size", 64))
                           .decode_buffer_size(cfg.value<std::size_t>(key + ".decode_buffer_size", 32))
                           .dispatch_buffer_size(cfg.value<std::size_t>(key + ".dispatch_buffer_size", 32))
                           .register_file_size(configured_register_file_size(cfg, key + ".register_file_size"))
                           .rob_size(cfg.value<std::size_t>(key + ".rob_size", 352))
                           .lq_size(cfg.value<std::size_t>(key + ".lq_size", 128))
                           .sq_size(cfg.value<std::size_t>(key + ".sq_size", 72))
                           .fetch_width(configured_bandwidth(cfg, key + ".fetch_width", 6))
                           .decode_width(configured_bandwidth(cfg, key + ".decode_width", 6))
                           .dispatch_width(configured_bandwidth(cfg, key + ".dispatch_width", 6))
                           .schedule_width(configured_bandwidth(cfg, key + ".scheduler_size", 128))
                           .execute_width(configured_bandwidth(cfg, key + ".execute_width", 4))
                           .lq_width(configured_bandwidth(cfg, key + ".lq_width", 2))
                           .sq_width(configured_bandwidth(cfg, key + ".sq_width", 2))
                           .retire_width(configured_bandwidth(cfg, key + ".retire_width", 5))
                           .mispredict_penalty(cfg.value<unsigned>(key + ".mispredict_penalty", 1))
                           .decode_latency(cfg.value<unsigned>(key + ".decode_latency", 1))
                           .dispatch_latency(cfg.value<unsigned>(key + ".dispatch_latency", 1))
                           .schedule_latency(cfg.value<unsigned>(key + ".schedule_latency", 0))
                           .execute_latency(cfg.value<unsigned>(key + ".execute_latency", 0))
                           .clock_period(period(cfg, key + ".frequency", 4000))
                           .dib_set(cfg.positive_value<std::size_t>(key + ".dib.sets", 32))
                           .dib_way(cfg.positive_value<std::size_t>(key + ".dib.ways", 8))
                           .dib_window(cfg.value<std::size_t>(key + ".dib.window_size", 16))
                           // A DIB hit costs hit_latency where a miss costs decode_latency; both
                           // default to 1, which is the assumption promote_to_decode() calls out
                           // in its own comment. Intel states the ordering for these cores -- the
                           // uop-cache path has shorter latency than legacy decode -- but never
                           // the magnitude, so the gap is a knob to be swept, not a constant.
                           // hit_latency reads through value(), not positive_value(): a
                           // zero-cycle DIB hit is a meaningful thing to ask for. The other two
                           // are refused at zero, because zero drains nothing -- and
                           // hit_buffer_size of zero stalls the DECODE path too, since
                           // promote_to_decode() takes the min of both buffers' free space.
                           .dib_hit_latency(cfg.value<unsigned>(key + ".dib.hit_latency", 1))
                           .dib_inorder_width(champsim::bandwidth::maximum_type{cfg.positive_value<long>(key + ".dib.inorder_width", 5)})
                           .dib_hit_buffer_size(cfg.positive_value<std::size_t>(key + ".dib.hit_buffer_size", 32)));
  }

  select_modules(cfg);
}

void champsim::static_environment::select_modules(const runtime_config& cfg)
{
#ifdef CHAMPSIM_HAVE_REGISTRY
  using registry = champsim::configured::module_registry;
  for (std::size_t cpu = 0; cpu < defs::num_cpus; ++cpu) {
    auto& core = cores.at(cpu);
    const auto key = core_key(cpu);

    const auto bp = cfg.value<std::string>(key + ".branch_predictor", champsim::defs::default_branch_predictor);
    if (bp != champsim::defs::default_branch_predictor) {
      core.install_branch_module(registry::make_branch(bp, &core));
    }
    core.branch_module_pimpl->impl_configure(cfg, key + "." + bp);

    const auto btb = cfg.value<std::string>(key + ".btb", champsim::defs::default_btb);
    if (btb != champsim::defs::default_btb) {
      core.install_btb_module(registry::make_btb(btb, &core));
    }
    core.btb_module_pimpl->impl_configure(cfg, key + "." + btb);

    // Stage 3.3: refuse an SMT-unsafe module at configuration time, not at
    // initialize time (which is after --knobs and the unconsulted-key check).
    const auto num_contexts = core.contexts.size();
    if (num_contexts > champsim::defs::max_contexts) {
      throw std::runtime_error{"configuration requests " + std::to_string(num_contexts) + " hardware contexts on " + key +
                               " but this binary is compiled for at most " + std::to_string(champsim::defs::max_contexts)};
    }
    champsim::modules::check_smt_capability(core.branch_module_pimpl->capability(), cpu, num_contexts, key + ".branch_predictor");
    champsim::modules::check_smt_capability(core.btb_module_pimpl->capability(), cpu, num_contexts, key + ".btb");
  }

  for (auto& cache : caches) {
    const auto key = "cache." + lower(cache.NAME);

    const auto pref = cfg.value<std::string>(key + ".prefetcher", champsim::defs::default_prefetcher);
    if (pref != champsim::defs::default_prefetcher) {
      cache.install_prefetcher_module(registry::make_prefetcher(pref, &cache));
    }
    cache.pref_module_pimpl->impl_configure(cfg, key + "." + pref);

    const auto repl = cfg.value<std::string>(key + ".replacement", champsim::defs::default_replacement);
    if (repl != champsim::defs::default_replacement) {
      cache.install_replacement_module(registry::make_replacement(repl, &cache));
    }
    cache.repl_module_pimpl->impl_configure(cfg, key + "." + repl);
  }
#else
  (void)cfg;
#endif
}

std::vector<std::reference_wrapper<O3_CPU>> champsim::static_environment::cpu_view() { return {std::begin(cores), std::end(cores)}; }
std::vector<std::reference_wrapper<CACHE>> champsim::static_environment::cache_view() { return {std::begin(caches), std::end(caches)}; }
std::vector<std::reference_wrapper<PageTableWalker>> champsim::static_environment::ptw_view() { return {std::begin(ptws), std::end(ptws)}; }
champsim::memory_backend& champsim::static_environment::memory_view() { return *memory; }

std::vector<std::reference_wrapper<champsim::operable>> champsim::static_environment::operable_view()
{
  // Order is the per-cycle operate() order: cores, caches, PTWs, DRAM.
  std::vector<std::reference_wrapper<operable>> retval{};
  auto make_ref = [](auto& elem) {
    return std::ref<operable>(elem);
  };
  std::transform(std::begin(cores), std::end(cores), std::back_inserter(retval), make_ref);
  std::transform(std::begin(caches), std::end(caches), std::back_inserter(retval), make_ref);
  std::transform(std::begin(ptws), std::end(ptws), std::back_inserter(retval), make_ref);
  retval.push_back(std::ref(memory->clocked_component()));

  // Pin the topology formula: one core + (1 + 6 per core) caches + one PTW per
  // core + one DRAM controller = 8 * num_cpus + 2 operables, independent of the
  // number of hardware contexts. do_cycle() orders operables by current_time with
  // std::sort, whose equivalence stability libstdc++ only guarantees up to 16
  // elements; one core is 10 operables (safe) and a two-core machine is 18, so
  // multi-core already exceeds that bound today. The formula is keyed on
  // num_cpus, not num_contexts, so a per-context TLB/PTW/cache replication would
  // grow this size while num_cpus stayed fixed -- the assert then fails, which
  // is exactly the anti-pattern to reject.
  CHAMPSIM_ASSERT(retval.size() == 8 * champsim::defs::num_cpus + 2);
  return retval;
}
