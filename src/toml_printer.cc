/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
#include <numeric>
#include <ratio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <fmt/core.h>

#include "stats_printer.h"

namespace
{
using entry = std::pair<std::string, std::string>;

constexpr std::array branch_types{branch_type::BRANCH_DIRECT_JUMP, branch_type::BRANCH_INDIRECT,      branch_type::BRANCH_CONDITIONAL,
                                  branch_type::BRANCH_DIRECT_CALL, branch_type::BRANCH_INDIRECT_CALL, branch_type::BRANCH_RETURN};

constexpr std::array access_types{access_type::LOAD, access_type::RFO, access_type::PREFETCH, access_type::WRITE, access_type::TRANSLATION};

// Every ratio in this document is rounded for reading; the exact operands it
// was computed from are always emitted next to it, so nothing is lost.
std::string toml_float(double value) { return fmt::format("{:.2f}", value); }

// A ratio with no denominator is `nan`, a genuine TOML float literal, rather
// than an omitted key. Keeping the key present means the schema is identical
// for every run, so a parser never has to guard a lookup. This is strictly
// more faithful than the JSON printer, which rendered NaN and infinity
// indistinguishably as `null`.
template <typename N, typename D>
std::string toml_ratio(N num, D denom)
{
  if (denom > 0) {
    return toml_float(static_cast<double>(num) / static_cast<double>(denom));
  }
  return "nan";
}

// Every key in the document is lower case, including the ones derived from
// configured component names. Only the case is changed; characters that cannot
// appear in a bare key are quoted rather than substituted -- see `key` below --
// because substituting them would map distinct names onto one key.
//
// Lower-casing is itself non-injective, and that is handled by the caller: see
// `component_keys`.
std::string to_lower(std::string_view name)
{
  std::string out{};
  out.reserve(std::size(name));
  std::transform(std::begin(name), std::end(name), std::back_inserter(out),
                 [](char chr) { return static_cast<char>(std::tolower(static_cast<unsigned char>(chr))); });
  return out;
}

bool is_bare_key(std::string_view key)
{
  if (std::empty(key)) {
    return false;
  }
  return std::all_of(std::begin(key), std::end(key), [](char chr) { return std::isalnum(static_cast<unsigned char>(chr)) != 0 || chr == '_' || chr == '-'; });
}

// A TOML basic string. Control characters have no literal form and must be
// escaped, otherwise a single odd byte in a trace path would produce a
// document that will not parse.
std::string quote(std::string_view value)
{
  std::string out{"\""};
  for (auto chr : value) {
    switch (chr) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(chr) < 0x20 || chr == 0x7f) {
        out += fmt::format("\\u{:04X}", static_cast<unsigned>(static_cast<unsigned char>(chr)));
      } else {
        out.push_back(chr);
      }
      break;
    }
  }
  out.push_back('"');
  return out;
}

// A single dotted-path component. Cache names come from the configuration and
// are not sanitised anywhere, so a name that is not a bare key is quoted
// rather than allowed to change the shape of the document.
std::string key(std::string_view name) { return is_bare_key(name) ? std::string{name} : quote(name); }

// The table-path component for each of a set of named components.
//
// Lower-casing is not injective, and TOML rejects any document that defines the
// same table twice -- so two components whose names differ only in case would
// not corrupt their own section, they would make the WHOLE file unparseable,
// silently, at emit time. The configuration layer keys components on their
// exact name (config/util.py's combine_named), so two separately-reachable
// caches really can differ only in case. Where the lower-cased form would
// collide, the exact configured spelling is used instead: it is unique by
// construction, and the fallback is confined to the colliding names so the
// ordinary case still reads as lower case.
template <typename Component>
std::vector<std::string> component_keys(const std::vector<Component>& components)
{
  std::vector<std::string> folded{};
  std::transform(std::begin(components), std::end(components), std::back_inserter(folded), [](const auto& comp) { return to_lower(comp.name); });

  std::vector<std::string> keys{};
  for (std::size_t i = 0; i < std::size(components); ++i) {
    const bool collides = std::count(std::begin(folded), std::end(folded), folded.at(i)) > 1;
    keys.push_back(key(collides ? components.at(i).name : folded.at(i)));
  }
  return keys;
}

void append_block(std::vector<std::string>& lines, const std::vector<std::string>& block)
{
  if (!std::empty(lines) && !std::empty(block)) {
    lines.emplace_back("");
  }
  lines.insert(std::end(lines), std::begin(block), std::end(block));
}

// Split an already-rendered TOML blob into lines, dropping the blank lines at
// its edges. The blob comes from a C++ raw string literal that opens right
// after its delimiter, so it always carries a leading newline of its own; left
// alone, those would accumulate every time the block is spliced.
std::vector<std::string> split_lines(std::string_view text)
{
  std::vector<std::string> out{};
  while (!std::empty(text)) {
    const auto end = text.find('\n');
    out.emplace_back(text.substr(0, end));
    if (end == std::string_view::npos) {
      break;
    }
    text.remove_prefix(end + 1);
  }

  const auto blank = [](const std::string& line) {
    return std::empty(line);
  };
  while (!std::empty(out) && blank(out.front())) {
    out.erase(std::begin(out));
  }
  while (!std::empty(out) && blank(out.back())) {
    out.pop_back();
  }
  return out;
}

void emit_table(std::vector<std::string>& lines, std::string_view path, const std::vector<entry>& entries)
{
  if (!std::empty(lines)) {
    lines.emplace_back("");
  }
  lines.push_back(fmt::format("[{}]", path));
  for (const auto& [name, value] : entries) {
    lines.push_back(fmt::format("{} = {}", name, value));
  }
}
} // namespace

std::vector<std::string> champsim::toml_printer::format(O3_CPU::stats_type stats, std::string_view path)
{
  const auto total_branches = std::accumulate(std::begin(branch_types), std::end(branch_types), 0LL,
                                              [tbt = stats.total_branch_types](auto acc, auto next) { return acc + tbt.value_or(next, 0); });
  const auto total_mispredicts = std::accumulate(std::begin(branch_types), std::end(branch_types), 0LL,
                                                 [btm = stats.branch_type_misses](auto acc, auto next) { return acc + btm.value_or(next, 0); });

  std::vector<std::string> lines{};
  emit_table(lines, path,
             {{"name", quote(stats.name)},
              {"instructions", fmt::format("{}", stats.instrs())},
              {"cycles", fmt::format("{}", stats.cycles())},
              {"roi_cycles", fmt::format("{}", stats.roi_cycles)},
              {"ipc", toml_ratio(stats.instrs(), stats.cycles())},
              {"dib_lookups", fmt::format("{}", stats.dib_lookups())},
              {"dib_hits", fmt::format("{}", stats.dib_hits)},
              {"dib_misses", fmt::format("{}", stats.dib_misses)},
              {"dib_hit_rate", toml_ratio(100 * stats.dib_hits, stats.dib_lookups())},
              {"total_branches", fmt::format("{}", total_branches)},
              {"total_mispredicts", fmt::format("{}", total_mispredicts)},
              {"mpki", toml_ratio(std::kilo::num * total_mispredicts, stats.instrs())},
              {"branch_prediction_accuracy", toml_ratio(100 * (total_branches - total_mispredicts), total_branches)},
              {"total_rob_occupancy_at_mispredict", fmt::format("{}", stats.total_rob_occupancy_at_branch_mispredict)},
              {"avg_rob_occupancy_at_mispredict", toml_ratio(stats.total_rob_occupancy_at_branch_mispredict, total_mispredicts)},
              {"cycles_on_wrong_path", fmt::format("{}", stats.cycles_on_wrong_path)},
              {"cyc_wpki", toml_ratio(std::kilo::num * stats.cycles_on_wrong_path, stats.instrs())},
              {"avg_cycles_per_mispredict", toml_ratio(stats.cycles_on_wrong_path, total_mispredicts)}});

  std::vector<entry> mispredicts{};
  std::vector<entry> executed{};
  for (auto type : branch_types) {
    const auto name = to_lower(branch_type_names.at(champsim::to_underlying(type)));
    mispredicts.emplace_back(name, fmt::format("{}", stats.branch_type_misses.value_or(type, 0)));
    executed.emplace_back(name, fmt::format("{}", stats.total_branch_types.value_or(type, 0)));
  }
  emit_table(lines, fmt::format("{}.mispredict", path), mispredicts);
  emit_table(lines, fmt::format("{}.executed", path), executed);

  return lines;
}

std::vector<std::string> champsim::toml_printer::format(CACHE::stats_type stats, std::string_view path)
{
  using value_type = typename decltype(stats.fill)::value_type;

  // Demand fills only: the prefetcher's own fills did not stall anyone, so they
  // are not part of the average a demand miss waited. This matches the JSON
  // printer's cache-wide denominator rather than the plain printer's per-CPU
  // one -- the two disagree on a multi-core run, and since
  // total_miss_latency_cycles is itself cache-wide, the cache-wide denominator
  // is the one that divides like with like.
  auto demand_fills = stats.fill.total();
  for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu) {
    demand_fills -= stats.fill.value_or(std::pair{access_type::PREFETCH, cpu}, value_type{});
  }

  std::vector<std::string> lines{};
  emit_table(lines, path,
             {{"name", quote(stats.name)},
              {"total_miss_latency_cycles", fmt::format("{}", stats.total_miss_latency_cycles)},
              {"miss_latency", toml_ratio(stats.total_miss_latency_cycles, demand_fills)}});

  // What the prefetcher did, kept apart from accesses whose type is PREFETCH:
  // both are called "prefetch" and conflating them double-reports.
  emit_table(lines, fmt::format("{}.prefetch", path),
             {{"requested", fmt::format("{}", stats.pf_requested)},
              {"issued", fmt::format("{}", stats.pf_issued)},
              {"useful", fmt::format("{}", stats.pf_useful)},
              {"useless", fmt::format("{}", stats.pf_useless)},
              {"fill", fmt::format("{}", stats.pf_fill)}});

  for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu) {
    std::vector<entry> counters{};
    for (auto type : access_types) {
      const auto name = to_lower(access_type_names.at(champsim::to_underlying(type)));
      counters.emplace_back(fmt::format("{}_hit", name), fmt::format("{}", stats.hits.value_or(std::pair{type, cpu}, value_type{})));
      counters.emplace_back(fmt::format("{}_miss", name), fmt::format("{}", stats.misses.value_or(std::pair{type, cpu}, value_type{})));
      counters.emplace_back(fmt::format("{}_miss_merge", name), fmt::format("{}", stats.miss_merge.value_or(std::pair{type, cpu}, value_type{})));
      counters.emplace_back(fmt::format("{}_fill", name), fmt::format("{}", stats.fill.value_or(std::pair{type, cpu}, value_type{})));
    }
    emit_table(lines, fmt::format("{}.cpu{}", path, cpu), counters);
  }

  return lines;
}

std::vector<std::string> champsim::toml_printer::format(dram_stats stats, std::string_view path)
{
  std::vector<std::string> lines{};
  emit_table(lines, path,
             {{"name", quote(stats.name)},
              {"rq_row_buffer_hit", fmt::format("{}", stats.RQ_ROW_BUFFER_HIT)},
              {"rq_row_buffer_miss", fmt::format("{}", stats.RQ_ROW_BUFFER_MISS)},
              {"wq_row_buffer_hit", fmt::format("{}", stats.WQ_ROW_BUFFER_HIT)},
              {"wq_row_buffer_miss", fmt::format("{}", stats.WQ_ROW_BUFFER_MISS)},
              {"wq_full", fmt::format("{}", stats.WQ_FULL)},
              {"dbus_cycle_congested", fmt::format("{}", stats.dbus_cycle_congested)},
              {"dbus_count_congested", fmt::format("{}", stats.dbus_count_congested)},
              {"avg_dbus_congested_cycle", toml_ratio(stats.dbus_cycle_congested, stats.dbus_count_congested)},
              {"refreshes_issued", fmt::format("{}", stats.refresh_cycles)}});
  return lines;
}

std::vector<std::string> champsim::toml_printer::format(champsim::phase_stats& stats, bool include_sim)
{
  const auto root = fmt::format("phase.{}", key(to_lower(stats.name)));

  std::vector<std::string> lines{};
  emit_table(lines, root, {{"name", quote(stats.name)}});

  std::vector<entry> traces{};
  for (std::size_t i = 0; i < std::size(stats.trace_names); ++i) {
    traces.emplace_back(fmt::format("cpu{}", i), quote(stats.trace_names.at(i)));
  }
  emit_table(lines, fmt::format("{}.trace", root), traces);

  // The region of interest is always written. The whole-run section is a copy
  // of it whenever the run has a single region of interest -- the usual case --
  // so it is written only on request; [meta].sim_stats records which, so its
  // absence is never ambiguous. Note this is NOT the plain printer's rule,
  // which keys the same decision on NUM_CPUS > 1.
  const auto emit_section = [&lines, &root](std::string_view section, auto& cores, auto& caches, auto& channels, const auto& native) {
    for (std::size_t i = 0; i < std::size(cores); ++i) {
      append_block(lines, format(cores.at(i), fmt::format("{}.{}.core.cpu{}", root, section, i)));
    }
    const auto cache_keys = component_keys(caches);
    for (std::size_t i = 0; i < std::size(caches); ++i) {
      append_block(lines, format(caches.at(i), fmt::format("{}.{}.cache.{}", root, section, cache_keys.at(i))));
    }
    if (native) {
      append_block(lines, format(*native, fmt::format("{}.{}.ramulator2", root, section)));
    } else {
      for (std::size_t i = 0; i < std::size(channels); ++i) {
        append_block(lines, format(channels.at(i), fmt::format("{}.{}.dram.channel{}", root, section, i)));
      }
    }
  };

  emit_section("roi", stats.roi_cpu_stats, stats.roi_cache_stats, stats.roi_dram_stats, stats.roi_ramulator2);
  if (include_sim) {
    emit_section("sim", stats.sim_cpu_stats, stats.sim_cache_stats, stats.sim_dram_stats, stats.sim_ramulator2);
  }

  return lines;
}

namespace
{
// A node in the [config] tree: its own scalars, and its sub-tables. Built from
// flat dotted keys so the emitter can put every scalar before any sub-table --
// a scalar written after a table header would silently land inside it, and
// sorted dotted keys deliver them in exactly the wrong order
// (ooo_cpu.cpu0.dib.sets precedes ooo_cpu.cpu0.rob_size).
struct config_node {
  std::vector<entry> scalars{};
  std::map<std::string, config_node> tables{};
};

void emit_node(std::vector<std::string>& lines, const std::string& path, const config_node& node)
{
  lines.push_back(fmt::format("[{}]", path));
  for (const auto& [name, value] : node.scalars) {
    lines.push_back(fmt::format("{} = {}", name, value));
  }
  for (const auto& [name, child] : node.tables) {
    lines.emplace_back("");
    emit_node(lines, path + "." + name, child);
  }
}
} // namespace

namespace
{
// TOML integers are signed 64-bit. Oversized unsigned counters use decimal
// strings, preserving every digit instead of silently rounding through double.
std::string native_value(const champsim::native_scalar& value)
{
  return std::visit(
      [](const auto& scalar) -> std::string {
        using type = std::decay_t<decltype(scalar)>;
        if constexpr (std::is_same_v<type, std::string>) {
          return quote(scalar);
        } else if constexpr (std::is_same_v<type, uint64_t>) {
          const auto decimal = fmt::format("{}", scalar);
          return scalar > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? quote(decimal) : decimal;
        } else if constexpr (std::is_same_v<type, double>) {
          auto text = fmt::format("{}", scalar);
          if (std::isfinite(scalar) && text.find_first_of(".eE") == std::string::npos) {
            text += ".0";
          }
          return text;
        } else {
          return fmt::format("{}", scalar);
        }
      },
      value);
}
} // namespace

std::vector<std::string> champsim::toml_printer::format(const ramulator2_statistics& stats, std::string_view path)
{
  std::vector<std::string> lines;
  emit_table(lines, path, {{"native_yaml", quote(stats.native.yaml)}});
  emit_table(lines, fmt::format("{}.adapter", path),
             {{"accepted_reads", native_value(stats.accepted_reads)},
              {"accepted_writes", native_value(stats.accepted_writes)},
              {"completed_reads", native_value(stats.completed_reads)},
              {"completed_writes", native_value(stats.completed_writes)},
              {"accepted_fragments", native_value(stats.accepted_fragments)},
              {"completed_fragments", native_value(stats.completed_fragments)},
              {"rejected_submissions", native_value(stats.rejected_submissions)},
              {"out_of_range_prefetches", native_value(stats.out_of_range_prefetches)},
              {"outstanding_parents", native_value(stats.outstanding_parents)},
              {"outstanding_fragments", native_value(stats.outstanding_fragments)},
              {"total_read_latency_ps", native_value(stats.total_read_latency_ps)},
              {"read_latency_samples", native_value(stats.read_latency_samples)}});
  config_node root;
  for (const auto& statistic : stats.native.values) {
    auto* node = &root;
    for (std::size_t i = 0; i + 1 < statistic.path.size(); ++i) {
      node = &node->tables[key(statistic.path.at(i))];
    }
    node->scalars.emplace_back(key(statistic.path.at(statistic.path.size() - 1)), native_value(statistic.value));
  }
  lines.emplace_back("");
  emit_node(lines, fmt::format("{}.native", path), root);
  return lines;
}

std::string champsim::toml_printer::config_id(const std::vector<std::pair<std::string, std::string>>& effective)
{
  // FNV-1a: stable across runs, platforms and compilers, which std::hash is
  // not required to be -- this identifier is compared between machines.
  constexpr uint64_t offset_basis{1469598103934665603ULL};
  constexpr uint64_t prime{1099511628211ULL};
  uint64_t hash{offset_basis};
  const auto consume = [&hash](std::string_view text) {
    for (auto chr : text) {
      hash ^= static_cast<unsigned char>(chr);
      hash *= prime;
    }
  };
  for (const auto& [key, value] : effective) {
    consume(key);
    consume("=");
    consume(value);
    consume(";");
  }
  return fmt::format("0x{:016x}", hash);
}

std::vector<std::string> champsim::toml_printer::format_config(const std::vector<std::pair<std::string, std::string>>& effective)
{
  config_node root{};
  for (const auto& [dotted, value] : effective) {
    auto* node = &root;
    std::string_view rest{dotted};
    for (auto dot = rest.find('.'); dot != std::string_view::npos; dot = rest.find('.')) {
      node = &node->tables[key(rest.substr(0, dot))];
      rest.remove_prefix(dot + 1);
    }
    node->scalars.emplace_back(key(rest), value);
  }

  std::vector<std::string> lines{};
  emit_node(lines, "config", root);
  return lines;
}

std::vector<std::string> champsim::toml_printer::format(std::vector<phase_stats>& stats, bool include_sim, const run_info& info)
{
  std::vector<std::string> lines{fmt::format("{} Ratios are rounded to two decimals; the exact", document_signature),
                                 "# operands of every ratio are emitted alongside it. An undefined ratio", "# is `nan` rather than a missing key."};
  const bool native = info.ramulator2.has_value() || std::any_of(stats.begin(), stats.end(), [](const auto& phase) {
                        return phase.roi_ramulator2.has_value() || phase.sim_ramulator2.has_value();
                      });
  if (native) {
    lines.emplace_back("# Unsigned integers above INT64_MAX are exact decimal strings; native floats are unrounded.");
  }
  emit_table(lines, "meta",
             {{"schema_version", native ? "2" : "1"},
              {"num_cpus", fmt::format("{}", NUM_CPUS)},
              {"sim_stats", include_sim ? "true" : "false"},
              {"build_id", quote(info.build_id)},
              {"warmup_instructions", fmt::format("{}", info.warmup_instructions)},
              {"simulation_instructions", fmt::format("{}", info.simulation_instructions)},
              {"trace_version", fmt::format("{}", info.trace_version)},
              {"command_line", quote(info.command_line)},
              {"config_files", quote(info.config_files)}});
  if (native) {
    lines.emplace_back("dram_model = \"ramulator2\"");
  }
  if (info.ramulator2) {
    const auto& record = *info.ramulator2;
    emit_table(lines, "meta.ramulator2",
               {{"config", quote(record.path)},
                {"config_hash", quote(record.hash)},
                {"revision", quote(record.revision)},
                {"library_hash", quote(record.library_hash)},
                {"build", quote(record.build)},
                {"yaml", quote(record.yaml)}});
  }

  // The effective configuration, rendered by format_config(). The header is
  // emitted even when the record is empty -- as it is in every unit test that
  // does not supply one -- so a consumer can always index [config] rather than
  // testing for it.
  auto config_lines = split_lines(info.config_toml);
  if (std::empty(config_lines)) {
    config_lines.emplace_back("[config]");
  }
  append_block(lines, config_lines);

  // What this run changed on top of the baked configuration. Each entry names
  // one knob with a dotted path, so the whole key is quoted -- a bare dotted
  // key would parse as a nested table and collide with [config]'s own shape.
  // The values arrive already rendered in TOML syntax by the runtime store.
  std::vector<entry> override_entries{};
  for (const auto& [override_key, rendered] : info.overrides) {
    override_entries.emplace_back(quote(override_key), rendered);
  }
  emit_table(lines, "config_override", override_entries);

  for (auto& phase : stats) {
    append_block(lines, format(phase, include_sim));
  }

  return lines;
}

std::vector<std::string> champsim::toml_printer::format(std::vector<phase_stats>& stats, bool include_sim) { return format(stats, include_sim, run_info{}); }

void champsim::toml_printer::print(std::vector<phase_stats>& stats)
{
  for (const auto& line : format(stats, include_sim_stats, info_)) {
    stream << line << "\n";
  }
}
