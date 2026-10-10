#include <algorithm>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unistd.h>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "channel.h"
#include "environment.h"
#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"
#include "operable.h"
#include "phase_info.h"
#include "ramulator2_driver.h"
#include "ramulator2_memory_backend.h"
#include "runtime_config.h"
#include "tracereader.h"

namespace champsim
{
std::vector<phase_stats> main(environment& env, std::vector<phase_info>& phases, std::vector<tracereader>& traces, const simulation_knobs& knobs);
}

namespace
{
using ps = long long;

// An independent scheduling reference, written from the documented global
// clock rules rather than from src/champsim.cc or src/operable.cc:
//   1. The global clock advances by q, the smallest period of any operable,
//      and every operable is then offered the clock.
//   2. An operable whose own time is behind the clock adds its period to its
//      time and operates, until it has caught up.
//   3. Within one tick, operables are offered the clock in order of their own
//      time, earliest first. Equal times keep environment order: do_cycle uses
//      std::sort, which promises no stability, but with fewer than 17
//      operables libstdc++ and libc++ both insertion-sort. The machines below
//      have six.
// In closed form: with period P >= q, operation m (at the operable's time m*P)
// runs on global tick floor((m-1)*P/q) + 1, never twice in one tick, and by the
// end of tick k the operable has operated ceil(k*q/P) times. Nothing here
// iterates the clock.
namespace reference
{
struct operation {
  long long tick;
  ps before; // the operable's own time when it is offered the clock
  std::size_t who;
  long long index; // 1-based
  [[nodiscard]] auto key() const { return std::tuple{tick, before, who}; }
};

ps quantum(const std::vector<ps>& periods) { return *std::min_element(periods.begin(), periods.end()); }
long long tick_of(ps period, long long index, ps q) { return ((index - 1) * period) / q + 1; }
long long operations_by(ps period, long long tick, ps q) { return (tick * q + period - 1) / period; }

operation nth(const std::vector<ps>& periods, std::size_t who, long long index)
{
  return {tick_of(periods.at(who), index, quantum(periods)), (index - 1) * periods.at(who), who, index};
}

// The first operation of `who` offered the clock after `after` has operated.
long long first_after(const std::vector<ps>& periods, std::size_t who, const operation& after)
{
  long long high = 1;
  while (!(nth(periods, who, high).key() > after.key())) {
    high *= 2;
  }
  long long low = 1;
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    if (nth(periods, who, middle).key() > after.key()) {
      high = middle;
    } else {
      low = middle + 1;
    }
  }
  return low;
}

std::vector<operation> schedule(const std::vector<ps>& periods, long long ticks)
{
  std::vector<operation> result;
  for (std::size_t who = 0; who < periods.size(); ++who) {
    for (long long index = 1; index <= operations_by(periods[who], ticks, quantum(periods)); ++index) {
      result.push_back(nth(periods, who, index));
    }
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.key() < right.key(); });
  return result;
}
} // namespace reference

TEST_CASE("The scheduling reference reproduces hand-derived tick tables")
{
  const auto ticks = [](std::vector<ps> periods, std::size_t who, long long count) {
    std::vector<long long> result;
    for (long long index = 1; index <= count; ++index) {
      result.push_back(reference::nth(periods, who, index).tick);
    }
    return result;
  };
  // Memory is operable 1. 833/250 = 3.33, so ticks 1, 4, 7, 10 (2499/250 = 9.996) and 14.
  REQUIRE(ticks({250, 833}, 1, 5) == std::vector<long long>{1, 4, 7, 10, 14});
  REQUIRE(ticks({250, 1000}, 1, 4) == std::vector<long long>{1, 5, 9, 13});
  REQUIRE(ticks({300, 833}, 1, 5) == std::vector<long long>{1, 3, 6, 9, 12});
  REQUIRE(ticks({257, 997}, 1, 5) == std::vector<long long>{1, 4, 8, 12, 16});
  // A core slower than native: the quantum is the native period.
  REQUIRE(ticks({2000, 833}, 0, 4) == std::vector<long long>{1, 3, 5, 8});
  REQUIRE(reference::operations_by(833, 14, 250) == 5);
  REQUIRE(reference::operations_by(2000, 8, 833) == 4);

  // A read queued by operable 0 (250 ps) on its first operation is seen by
  // memory (833 ps) later in tick 1; a completion in memory operation 3 (tick 7,
  // time 1666 before) is visible to operable 0 only on tick 8, because at tick 7
  // operable 0 was offered the clock first (1500 < 1666).
  const std::vector<ps> cache_then_memory{250, 833};
  REQUIRE(reference::first_after(cache_then_memory, 1, reference::nth(cache_then_memory, 0, 1)) == 1);
  REQUIRE(reference::first_after(cache_then_memory, 0, reference::nth(cache_then_memory, 1, 3)) == 8);
  // Equal periods tie on every tick; environment order decides.
  const std::vector<ps> memory_then_cache{833, 833};
  REQUIRE(reference::first_after(memory_then_cache, 0, reference::nth(memory_then_cache, 1, 1)) == 2);
  REQUIRE(reference::first_after(memory_then_cache, 1, reference::nth(memory_then_cache, 0, 1)) == 1);
}

struct log_entry {
  long long tick;
  std::size_t who;
  ps time;
};

// Environment order of the machine below, which is also the reference's.
constexpr std::size_t counter_index = 0, core_index = 1, l1i_index = 2, l1d_index = 3, llc_index = 4, memory_index = 5;

ps time_of(const champsim::operable& op) { return op.current_time.time_since_epoch().count(); }

// Runs first on every tick (its period is the quantum and it is first in
// environment order), so the other probes read the global tick number from it.
struct tick_counter final : champsim::operable {
  std::vector<log_entry>& log;
  long long ticks = 0;
  std::vector<long long> begins, ends;
  tick_counter(ps period, std::vector<log_entry>& log_) : champsim::operable(champsim::chrono::picoseconds{period}), log(log_) {}
  long operate() override
  {
    ++ticks;
    log.push_back({ticks, counter_index, time_of(*this)});
    return 0;
  }
  void begin_phase() override { begins.push_back(ticks); }
  void end_phase(unsigned) override { ends.push_back(ticks); }
};

struct request_record {
  bool write, response;
  long long queued_by; // the probe's operation that queued it
  std::optional<log_entry> seen{};
  unsigned responses = 0;
};

// Stands in for the LLC: like CACHE::operate, it takes the feeder's responses
// before queueing new requests.
struct feeder_probe final : champsim::operable {
  champsim::channel& feeder;
  const tick_counter& counter;
  std::vector<log_entry>& log;
  std::function<void(feeder_probe&)> plan;
  long long ops = 0;
  unsigned phase = 0;
  std::vector<request_record> requests;
  feeder_probe(ps period, champsim::channel& feeder_, const tick_counter& counter_, std::vector<log_entry>& log_)
      : champsim::operable(champsim::chrono::picoseconds{period}), feeder(feeder_), counter(counter_), log(log_)
  {
  }
  long operate() override
  {
    ++ops;
    log.push_back({counter.ticks, llc_index, time_of(*this)});
    for (const auto& response : feeder.returned) {
      auto& record = requests.at(response.pf_metadata);
      ++record.responses;
      if (!record.seen) {
        record.seen = log_entry{counter.ticks, llc_index, time_of(*this)};
      }
    }
    feeder.returned.clear();
    if (plan) {
      plan(*this);
    }
    return 0;
  }
  void begin_phase() override { ++phase; }
  static uint64_t address(std::size_t id) { return 0x100000 + id * 64; }
  void queue(bool write, bool response)
  {
    champsim::channel::request_type packet;
    packet.address = champsim::address{address(requests.size())};
    packet.v_address = packet.address;
    packet.cpu = 0;
    packet.pf_metadata = static_cast<uint32_t>(requests.size());
    packet.response_requested = response;
    packet.type = write ? access_type::WRITE : access_type::LOAD;
    requests.push_back({write, response, ops});
    if (write) {
      feeder.add_wq(packet);
    } else {
      feeder.add_rq(packet);
    }
  }
};

struct attempt_record {
  long long tick;
  ps time;
  uint64_t native_ticks; // completed native ticks before this attempt
  uint64_t address;
  bool write, accepted;
};

struct callback_record {
  long long tick;
  ps time;
  uint64_t native_ticks; // including the tick it completed in
  uint64_t address;
  bool write;
};

// What the driver boundary saw, stamped with the global tick and the adapter's time.
struct recorder {
  const tick_counter* counter = nullptr;
  const champsim::operable* adapter = nullptr;
  std::vector<log_entry>* log = nullptr;
  uint64_t ticks = 0, finalizations = 0, ticks_at_finalize = 0, ticks_after_finalize = 0;
  std::vector<long long> resets;
  std::vector<attempt_record> attempts;
  std::vector<callback_record> callbacks;

  void tick()
  {
    if (finalizations != 0) {
      ++ticks_after_finalize;
    }
    ++ticks;
    log->push_back({counter->ticks, memory_index, time_of(*adapter)});
  }
  void attempt(uint64_t native_ticks, uint64_t address, bool write, bool accepted)
  {
    attempts.push_back({counter->ticks, time_of(*adapter), native_ticks, address, write, accepted});
  }
  static std::function<void()> observe(std::shared_ptr<recorder> self, uint64_t address, bool write, std::function<void()> done)
  {
    return [self, address, write, done = std::move(done)] {
      self->callbacks.push_back({self->counter->ticks, time_of(*self->adapter), self->ticks, address, write});
      if (done) {
        done();
      }
    };
  }
};

// Completes every accepted fragment `latency` native ticks after the send,
// counting the tick of the operation that sent it as the first.
class clock_driver final : public champsim::ramulator2_driver
{
  std::shared_ptr<recorder> record;
  ps period;
  std::size_t transaction;
  uint64_t latency, since_reset = 0;
  std::function<bool(const recorder&, uint64_t, bool)> accept;
  std::multimap<uint64_t, std::function<void()>> due;

public:
  clock_driver(std::shared_ptr<recorder> record_, ps period_, std::size_t transaction_, uint64_t latency_,
               std::function<bool(const recorder&, uint64_t, bool)> accept_ = {})
      : record(std::move(record_)), period(period_), transaction(transaction_), latency(latency_), accept(std::move(accept_))
  {
  }
  champsim::chrono::picoseconds clock_period() const override { return champsim::chrono::picoseconds{period}; }
  champsim::data::bytes size() const override { return champsim::data::bytes{1 << 24}; }
  std::size_t transaction_bytes() const override { return transaction; }
  bool send(bool write, uint64_t address, uint32_t, std::size_t, std::function<void()> done) override
  {
    const bool accepted = !accept || accept(*record, address, write);
    record->attempt(record->ticks, address, write, accepted);
    if (accepted) {
      due.emplace(record->ticks + latency, recorder::observe(record, address, write, std::move(done)));
    }
    return accepted;
  }
  void tick() override
  {
    record->tick();
    ++since_reset;
    std::vector<std::function<void()>> ready;
    const auto [first, last] = due.equal_range(record->ticks);
    for (auto it = first; it != last; ++it) {
      ready.push_back(std::move(it->second));
    }
    due.erase(first, last);
    for (auto& callback : ready) {
      callback();
    }
  }
  void reset_stats() override
  {
    record->resets.push_back(record->counter->ticks);
    since_reset = 0;
  }
  champsim::native_memory_statistics statistics() override
  {
    return {{{{"memory_system", "controller", "channel0", "cycles"}, static_cast<int64_t>(since_reset)}}, "cycles: " + std::to_string(since_reset)};
  }
  champsim::ramulator2_config_record config_record() const override { return {}; }
  void finalize() override
  {
    ++record->finalizations;
    record->ticks_at_finalize = record->ticks;
  }
};

// Records the real native driver's boundary without changing it.
class recording_driver final : public champsim::ramulator2_driver
{
  std::unique_ptr<champsim::ramulator2_driver> inner;
  std::shared_ptr<recorder> record;

public:
  recording_driver(std::unique_ptr<champsim::ramulator2_driver> inner_, std::shared_ptr<recorder> record_)
      : inner(std::move(inner_)), record(std::move(record_))
  {
  }
  champsim::chrono::picoseconds clock_period() const override { return inner->clock_period(); }
  champsim::data::bytes size() const override { return inner->size(); }
  std::size_t transaction_bytes() const override { return inner->transaction_bytes(); }
  bool send(bool write, uint64_t address, uint32_t cpu, std::size_t bytes, std::function<void()> done) override
  {
    const auto before = record->ticks;
    const bool accepted = inner->send(write, address, cpu, bytes, recorder::observe(record, address, write, std::move(done)));
    record->attempt(before, address, write, accepted);
    return accepted;
  }
  void tick() override
  {
    record->tick();
    inner->tick();
  }
  void reset_stats() override
  {
    record->resets.push_back(record->counter->ticks);
    inner->reset_stats();
  }
  champsim::native_memory_statistics statistics() override { return inner->statistics(); }
  champsim::ramulator2_config_record config_record() const override { return inner->config_record(); }
  void finalize() override
  {
    ++record->finalizations;
    record->ticks_at_finalize = record->ticks;
    inner->finalize();
  }
};

// One real O3_CPU (so the real phase runner has something to finish), its two
// instant memory mocks, a feeder probe, and the real Ramulator adapter.
struct clock_machine final : champsim::environment {
  std::vector<log_entry> log;
  std::shared_ptr<recorder> record = std::make_shared<recorder>();
  champsim::channel feeder;
  std::unique_ptr<champsim::memory_backend> memory;
  std::vector<ps> periods;
  tick_counter counter;
  do_nothing_MRC l1i, l1d;
  O3_CPU core;
  feeder_probe llc;

  clock_machine(ps core_period, ps llc_period, const std::function<std::unique_ptr<champsim::ramulator2_driver>(std::shared_ptr<recorder>)>& driver)
      : memory(champsim::make_ramulator2_memory_backend(driver(record), {&feeder})),
        periods{0, core_period, core_period, core_period, llc_period, memory->clocked_component().clock_period.count()},
        counter(std::min({core_period, llc_period, periods[memory_index]}), log), core{champsim::core_builder{}
                                                                                           .clock_period(champsim::chrono::picoseconds{core_period})
                                                                                           .ifetch_buffer_size(16)
                                                                                           .decode_buffer_size(16)
                                                                                           .dispatch_buffer_size(16)
                                                                                           .register_file_size(128)
                                                                                           .rob_size(16)
                                                                                           .fetch_queues(&l1i.queues)
                                                                                           .data_queues(&l1d.queues)},
        llc(llc_period, feeder, counter, log)
  {
    periods[counter_index] = counter.clock_period.count();
    l1i.clock_period = l1d.clock_period = champsim::chrono::picoseconds{core_period};
    record->counter = &counter;
    record->adapter = &memory->clocked_component();
    record->log = &log;
  }
  std::vector<std::reference_wrapper<O3_CPU>> cpu_view() override { return {core}; }
  std::vector<std::reference_wrapper<CACHE>> cache_view() override { return {}; }
  std::vector<std::reference_wrapper<PageTableWalker>> ptw_view() override { return {}; }
  champsim::memory_backend& memory_view() override { return *memory; }
  std::vector<std::reference_wrapper<champsim::operable>> operable_view() override { return {counter, core, l1i, l1d, llc, memory->clocked_component()}; }

  std::vector<champsim::phase_stats> run(std::vector<champsim::phase_info> phases)
  {
    std::vector<champsim::tracereader> traces;
    traces.emplace_back([ip = uint64_t{0x400000}]() mutable {
      ip += 4;
      return champsim::test::instruction_with_ip(ip);
    });
    champsim::simulation_knobs knobs;
    knobs.deadlock_cycle = INT_MAX; // slow memory or a slow core idles for long stretches
    knobs.livelock_period = UINT64_MAX;
    for (auto& phase : phases) {
      phase.trace_index = {{0}};
      phase.trace_names = {"generated"};
    }
    return champsim::main(*this, phases, traces, knobs);
  }

  [[nodiscard]] long long ticks() const { return counter.ticks; }
  [[nodiscard]] reference::operation op(std::size_t who, long long index) const { return reference::nth(periods, who, index); }
  [[nodiscard]] long long memory_ops_by(long long tick) const { return reference::operations_by(periods[memory_index], tick, periods[counter_index]); }
  [[nodiscard]] long long memory_tick(long long index) const { return op(memory_index, index).tick; }
  // The probe operation that first finds in the feeder what memory operation `index` returned.
  [[nodiscard]] long long seen_after_memory(long long index) const { return reference::first_after(periods, llc_index, op(memory_index, index)); }
  [[nodiscard]] log_entry probe_entry(long long index) const { return {op(llc_index, index).tick, llc_index, index * periods[llc_index]}; }
};

// The phase containing a global tick: phase p owns ticks (begins[p], ends[p]].
std::size_t phase_of(const tick_counter& counter, long long tick)
{
  for (std::size_t phase = 0; phase < counter.ends.size(); ++phase) {
    if (tick > counter.begins.at(phase) && tick <= counter.ends.at(phase)) {
      return phase;
    }
  }
  return counter.ends.size();
}

void require_phase_bounds(const tick_counter& counter, std::size_t phases)
{
  REQUIRE(counter.begins.size() == phases);
  REQUIRE(counter.ends.size() == phases);
  REQUIRE(counter.begins.front() == 0);
  for (std::size_t phase = 1; phase < phases; ++phase) {
    REQUIRE(counter.begins[phase] == counter.ends[phase - 1]);
  }
  REQUIRE(counter.ends.back() == counter.ticks);
}

// Every logged operation -- the counter's, the probe's and each native tick
// with the adapter's time -- in the reference's order.
void require_schedule(const clock_machine& machine)
{
  std::vector<log_entry> expected;
  for (const auto& op : reference::schedule(machine.periods, machine.ticks())) {
    if (op.who == counter_index || op.who == llc_index || op.who == memory_index) {
      expected.push_back({op.tick, op.who, op.index * machine.periods[op.who]});
    }
  }
  REQUIRE(machine.log.size() == expected.size());
  const auto mismatch = std::mismatch(machine.log.begin(), machine.log.end(), expected.begin(), [](const auto& left, const auto& right) {
    return std::tie(left.tick, left.who, left.time) == std::tie(right.tick, right.who, right.time);
  });
  if (mismatch.first != machine.log.end()) {
    CAPTURE(std::distance(machine.log.begin(), mismatch.first), mismatch.first->tick, mismatch.first->who, mismatch.first->time, mismatch.second->tick,
            mismatch.second->who, mismatch.second->time);
    FAIL("the global clock diverged from the scheduling reference");
  }
  REQUIRE(machine.record->ticks == static_cast<uint64_t>(machine.memory_ops_by(machine.ticks())));
  for (const auto who : {core_index, l1i_index, l1d_index}) {
    const champsim::operable& op = who == core_index ? static_cast<const champsim::operable&>(machine.core) : who == l1i_index ? machine.l1i : machine.l1d;
    CAPTURE(who);
    REQUIRE(time_of(op) == reference::operations_by(machine.periods[who], machine.ticks(), machine.periods[counter_index]) * machine.periods[who]);
  }
}

void require_same(const std::optional<log_entry>& actual, const log_entry& expected)
{
  REQUIRE(actual.has_value());
  CAPTURE(actual->tick, actual->time, expected.tick, expected.time);
  REQUIRE(std::tie(actual->tick, actual->who, actual->time) == std::tie(expected.tick, expected.who, expected.time));
}

// What the reference predicts a measured phase's adapter counters to be.
struct phase_expectation {
  uint64_t accepted_reads = 0, accepted_writes = 0, completed_reads = 0, completed_writes = 0;
  uint64_t accepted_fragments = 0, completed_fragments = 0, rejected = 0;
  uint64_t outstanding_parents = 0, outstanding_fragments = 0, latency_ps = 0;
  int64_t cycles = 0;
};

void require_statistics(const champsim::ramulator2_statistics& stats, const phase_expectation& expected)
{
  CHECK(stats.accepted_reads == expected.accepted_reads);
  CHECK(stats.accepted_writes == expected.accepted_writes);
  CHECK(stats.completed_reads == expected.completed_reads);
  CHECK(stats.completed_writes == expected.completed_writes);
  CHECK(stats.accepted_fragments == expected.accepted_fragments);
  CHECK(stats.completed_fragments == expected.completed_fragments);
  CHECK(stats.rejected_submissions == expected.rejected);
  CHECK(stats.outstanding_parents == expected.outstanding_parents);
  CHECK(stats.outstanding_fragments == expected.outstanding_fragments);
  CHECK(stats.total_read_latency_ps == expected.latency_ps);
  CHECK(stats.read_latency_samples == expected.completed_reads);
  CHECK(stats.out_of_range_prefetches == 0);
  const auto cycles = std::find_if(stats.native.values.begin(), stats.native.values.end(), [](const auto& value) {
    return value.path == std::vector<std::string>{"memory_system", "controller", "channel0", "cycles"};
  });
  REQUIRE(cycles != stats.native.values.end());
  CHECK(std::get<int64_t>(cycles->value) == expected.cycles);
}

bool same_statistics(const champsim::ramulator2_statistics& left, const champsim::ramulator2_statistics& right)
{
  const auto counters = [](const champsim::ramulator2_statistics& stats) {
    return std::tuple{stats.accepted_reads,      stats.accepted_writes,       stats.completed_reads,       stats.completed_writes,
                      stats.accepted_fragments,  stats.completed_fragments,   stats.rejected_submissions,  stats.out_of_range_prefetches,
                      stats.outstanding_parents, stats.outstanding_fragments, stats.total_read_latency_ps, stats.read_latency_samples};
  };
  const auto values = [](const champsim::ramulator2_statistics& stats) {
    std::vector<std::pair<std::vector<std::string>, champsim::native_scalar>> result;
    for (const auto& value : stats.native.values) {
      result.emplace_back(value.path, value.value);
    }
    return result;
  };
  return counters(left) == counters(right) && values(left) == values(right) && left.native.yaml == right.native.yaml;
}

struct clock_set {
  ps core, llc, memory;
};
} // namespace

TEST_CASE("The global clock ticks the adapter, completes reads and shows responses when the reference predicts")
{
  const auto clocks = GENERATE(values<clock_set>({{250, 250, 1000},      // dividing
                                                  {250, 250, 833},       // non-dividing
                                                  {300, 300, 833},       // non-dividing
                                                  {257, 257, 997},       // non-dividing, both prime
                                                  {2000, 2000, 833},     // core slower than native
                                                  {1666, 1666, 833},     // core slower, dividing
                                                  {833, 833, 833},       // equal: every operable ties on every tick
                                                  {250, 1000, 833},      // the cache slower than the core and native
                                                  {11, 11, 30011},       // huge ratio, native slow
                                                  {30011, 30011, 11}})); // huge ratio, native fast
  const uint64_t latency = GENERATE(1u, 5u);
  const long long scale = std::max<long long>(1, clocks.memory / clocks.core);
  // The first measured phase: empty, one instruction, or long enough to matter.
  const long long first_roi = GENERATE(0, 1, 24);
  const long long first_length = first_roi == 24 ? 24 * scale : first_roi;
  CAPTURE(clocks.core, clocks.llc, clocks.memory, latency, first_length);

  clock_machine machine{clocks.core, clocks.llc, [&](auto record) {
                          return std::make_unique<clock_driver>(record, clocks.memory, 64, latency);
                        }};
  // Roughly four requests per native tick, in every phase: a read on every
  // queueing operation, a write on every third, every fifth read suppressed.
  const long long stride = std::max<long long>(1, clocks.memory / clocks.llc / 4);
  machine.llc.plan = [stride](feeder_probe& probe) {
    if (probe.ops % stride == 0) {
      probe.queue(false, probe.ops % (5 * stride) != 0);
      if (probe.ops % (3 * stride) == 0) {
        probe.queue(true, true);
      }
    }
  };
  const auto results = machine.run({{"warmup", true, 16 * scale, {}, {}}, {"first", false, first_length, {}, {}}, {"second", false, 32 * scale, {}, {}}});

  REQUIRE(results.size() == 2);
  require_phase_bounds(machine.counter, 3);
  if (first_length == 0) {
    // A zero-instruction phase is still one global tick.
    REQUIRE(machine.counter.ends[1] - machine.counter.begins[1] == 1);
  }
  require_schedule(machine);

  // Predict every request's native life from the reference alone.
  std::vector<phase_expectation> expected(3);
  std::vector<attempt_record> attempts;
  for (std::size_t id = 0; id < machine.llc.requests.size(); ++id) {
    const auto& request = machine.llc.requests[id];
    CAPTURE(id, request.write, request.response, request.queued_by);
    REQUIRE(request.responses <= 1);
    // The first native operation after the request was queued takes it.
    const auto taken = reference::first_after(machine.periods, memory_index, machine.op(llc_index, request.queued_by));
    const auto taken_tick = machine.memory_tick(taken);
    if (taken_tick > machine.ticks()) {
      REQUIRE_FALSE(request.seen);
      continue;
    }
    const auto phase = phase_of(machine.counter, taken_tick);
    if (phase == 0) {
      // Fast warmup: never submitted; a requested read comes straight back.
      const auto seen = machine.seen_after_memory(taken);
      if (!request.write && request.response && machine.op(llc_index, seen).tick <= machine.ticks()) {
        require_same(request.seen, machine.probe_entry(seen));
      } else {
        REQUIRE_FALSE(request.seen);
      }
      continue;
    }
    attempts.push_back({taken_tick, taken * clocks.memory, static_cast<uint64_t>(taken - 1), feeder_probe::address(id), request.write, true});
    auto& admitted_in = expected[phase];
    ++(request.write ? admitted_in.accepted_writes : admitted_in.accepted_reads);
    ++admitted_in.accepted_fragments;
    const auto completed = taken + static_cast<long long>(latency) - 1;
    const auto completed_tick = machine.memory_tick(completed);
    for (std::size_t later = phase; later < 3; ++later) {
      if (completed_tick > machine.counter.ends[later]) {
        ++expected[later].outstanding_parents;
        ++expected[later].outstanding_fragments;
      }
    }
    if (completed_tick > machine.ticks()) {
      REQUIRE_FALSE(request.seen);
      continue;
    }
    auto& completed_in = expected[phase_of(machine.counter, completed_tick)];
    ++(request.write ? completed_in.completed_writes : completed_in.completed_reads);
    ++completed_in.completed_fragments;
    if (!request.write) {
      completed_in.latency_ps += static_cast<uint64_t>((completed - taken) * clocks.memory);
    }
    const auto seen = machine.seen_after_memory(completed);
    if (!request.write && request.response && machine.op(llc_index, seen).tick <= machine.ticks()) {
      require_same(request.seen, machine.probe_entry(seen));
    } else {
      REQUIRE_FALSE(request.seen);
    }
  }

  // Each native operation submits its reads (RQ) before its writes (WQ), oldest first.
  std::stable_sort(attempts.begin(), attempts.end(),
                   [](const auto& left, const auto& right) { return std::tie(left.native_ticks, left.write) < std::tie(right.native_ticks, right.write); });
  REQUIRE(machine.record->attempts.size() == attempts.size());
  for (std::size_t i = 0; i < attempts.size(); ++i) {
    CAPTURE(i);
    const auto& actual = machine.record->attempts[i];
    REQUIRE(std::tie(actual.tick, actual.time, actual.native_ticks, actual.address, actual.write, actual.accepted)
            == std::tie(attempts[i].tick, attempts[i].time, attempts[i].native_ticks, attempts[i].address, attempts[i].write, attempts[i].accepted));
  }

  for (std::size_t phase = 1; phase < 3; ++phase) {
    CAPTURE(phase);
    expected[phase].cycles = machine.memory_ops_by(machine.counter.ends[phase]) - machine.memory_ops_by(machine.counter.begins[phase]);
    const auto& stats = results[phase - 1];
    require_statistics(stats.roi_ramulator2.value(), expected[phase]);
    // Nothing runs between the last CPU finishing and the phase's statistics.
    REQUIRE(same_statistics(stats.roi_ramulator2.value(), stats.sim_ramulator2.value()));
  }

  // One statistics reset per phase, before its first tick; one finalization
  // after the last tick, without ticking again.
  REQUIRE(machine.record->resets == machine.counter.begins);
  REQUIRE(machine.record->finalizations == 1);
  REQUIRE(machine.record->ticks_at_finalize == machine.record->ticks);
  REQUIRE(machine.record->ticks_after_finalize == 0);
  // The frozen snapshot is what the backend still reports after finalization.
  REQUIRE(same_statistics(machine.memory->statistics().roi_ramulator2.value(), results.back().roi_ramulator2.value()));
}

TEST_CASE("A partially submitted head keeps its latency origin through a later warmup under the global clock")
{
  const auto clocks = GENERATE(values<clock_set>({{257, 257, 997}, {2000, 2000, 833}, {833, 833, 833}, {250, 1000, 833}}));
  const uint64_t latency = 3;
  CAPTURE(clocks.core, clocks.llc, clocks.memory);
  const auto block = feeder_probe::address(0);
  clock_machine machine{clocks.core, clocks.llc, [&](auto record) {
                          // The block's second 32-byte half is refused until the fourth phase has begun.
                          return std::make_unique<clock_driver>(record, clocks.memory, 32, latency, [block](const recorder& seen, uint64_t address, bool) {
                            return address != block + 32 || seen.resets.size() >= 4;
                          });
                        }};
  // What the backend reports during the later warmup, at its first and last probe operation.
  std::optional<champsim::memory_statistics> first_in_again, last_in_again;
  machine.llc.plan = [&](feeder_probe& probe) {
    if (probe.phase == 2 && probe.requests.empty()) {
      probe.queue(false, true);
    }
    if (probe.phase == 3) {
      last_in_again = machine.memory->statistics();
      if (!first_in_again) {
        first_in_again = last_in_again;
      }
    }
  };
  const auto results = machine.run({{"warmup", true, 16, {}, {}}, {"first", false, 64, {}, {}}, {"again", true, 64, {}, {}}, {"second", false, 64, {}, {}}});

  REQUIRE(results.size() == 2);
  require_phase_bounds(machine.counter, 4);
  // The first measured phase's ROI snapshot stays frozen through the next
  // phase, while the live counters it was taken from have been reset.
  REQUIRE(first_in_again.has_value());
  for (const auto& seen : {*first_in_again, *last_in_again}) {
    REQUIRE(same_statistics(seen.roi_ramulator2.value(), results[0].roi_ramulator2.value()));
    REQUIRE_FALSE(same_statistics(seen.sim_ramulator2.value(), results[0].roi_ramulator2.value()));
  }
  require_schedule(machine);
  REQUIRE(machine.llc.requests.size() == 1);
  const auto& request = machine.llc.requests.front();
  const auto& ends = machine.counter.ends;
  const auto first = reference::first_after(machine.periods, memory_index, machine.op(llc_index, request.queued_by));
  REQUIRE(phase_of(machine.counter, machine.memory_tick(first)) == 1);
  const auto last_in_first = machine.memory_ops_by(ends[1]);
  const auto resumed = machine.memory_ops_by(machine.counter.begins[3]) + 1;

  // The accepted half is never resent; the refused half is retried on every
  // measured native operation, never during the later warmup, and accepted on
  // the first operation of the next measured phase.
  std::vector<std::tuple<long long, uint64_t, bool>> attempts{{first, block, true}};
  for (auto index = first; index <= last_in_first; ++index) {
    attempts.emplace_back(index, block + 32, false);
  }
  attempts.emplace_back(resumed, block + 32, true);
  REQUIRE(machine.record->attempts.size() == attempts.size());
  for (std::size_t i = 0; i < attempts.size(); ++i) {
    CAPTURE(i);
    const auto& actual = machine.record->attempts[i];
    const auto [index, address, accepted] = attempts[i];
    const auto tick = machine.memory_tick(index);
    const auto time = index * clocks.memory;
    const auto native_ticks = static_cast<uint64_t>(index - 1);
    REQUIRE(std::tie(actual.tick, actual.time, actual.native_ticks, actual.address, actual.accepted) == std::tie(tick, time, native_ticks, address, accepted));
  }

  const auto first_done = first + static_cast<long long>(latency) - 1;
  const auto last_done = resumed + static_cast<long long>(latency) - 1;
  REQUIRE(machine.memory_tick(last_done) <= machine.ticks());
  REQUIRE(machine.record->callbacks.size() == 2);
  REQUIRE(machine.record->callbacks[0].native_ticks == static_cast<uint64_t>(first_done));
  REQUIRE(machine.record->callbacks[1].native_ticks == static_cast<uint64_t>(last_done));
  REQUIRE(request.responses == 1);
  require_same(request.seen, machine.probe_entry(machine.seen_after_memory(last_done)));

  const auto first_done_phase = phase_of(machine.counter, machine.memory_tick(first_done));
  phase_expectation before;
  before.accepted_reads = 1;
  before.accepted_fragments = 1;
  before.completed_fragments = first_done_phase == 1;
  before.rejected = static_cast<uint64_t>(last_in_first - first + 1);
  before.outstanding_parents = 1;
  before.outstanding_fragments = first_done_phase != 1;
  before.cycles = machine.memory_ops_by(ends[1]) - machine.memory_ops_by(machine.counter.begins[1]);
  require_statistics(results[0].roi_ramulator2.value(), before);

  phase_expectation after;
  after.accepted_fragments = 1;
  after.completed_fragments = 1 + (first_done_phase == 3);
  after.completed_reads = 1;
  // Measured from the first half's acceptance two phases earlier.
  after.latency_ps = static_cast<uint64_t>((last_done - first) * clocks.memory);
  after.cycles = machine.memory_ops_by(ends[3]) - machine.memory_ops_by(machine.counter.begins[3]);
  require_statistics(results[1].roi_ramulator2.value(), after);
  REQUIRE(machine.record->finalizations == 1);
  REQUIRE(machine.record->ticks_after_finalize == 0);
}

namespace
{
struct native_case {
  const char* fixture;
  const char* from;
  const char* to;
  std::size_t fragments; // native transactions per 64-byte cache block
  ps core;
  long long first, again, second; // instructions
};

std::string edited_fixture(const native_case& sample)
{
  std::ifstream file(std::string{"configs/ramulator2/"} + sample.fixture + ".yaml");
  if (!file) {
    throw std::runtime_error("missing native test fixture");
  }
  std::string text{std::istreambuf_iterator<char>{file}, {}};
  if (sample.from != nullptr) {
    const auto at = text.find(sample.from);
    if (at == std::string::npos) {
      throw std::runtime_error("invalid native fixture edit");
    }
    text.replace(at, std::string{sample.from}.size(), sample.to);
  }
  return text;
}

struct scratch_yaml {
  std::filesystem::path path;
  explicit scratch_yaml(const std::string& text)
  {
    path = std::filesystem::temp_directory_path() / ("champsim-clock-test-" + std::to_string(::getpid()) + ".yaml");
    std::ofstream file(path);
    file << text;
  }
  ~scratch_yaml()
  {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

struct native_parent {
  bool write = false;
  std::vector<attempt_record> accepted;
  std::vector<callback_record> completions;
};

int64_t native_value(const champsim::ramulator2_statistics& stats, const std::vector<std::string>& path)
{
  const auto found = std::find_if(stats.native.values.begin(), stats.native.values.end(), [&](const auto& value) { return value.path == path; });
  if (found == stats.native.values.end()) {
    throw std::runtime_error("missing native statistic");
  }
  return std::get<int64_t>(found->value);
}

struct native_outcome {
  std::size_t carried = 0, straddling = 0;
};

// One real-native run, checked against the reference and the driver boundary.
native_outcome native_run(const native_case& sample, long long first_length)
{
  CAPTURE(sample.fixture, sample.core, first_length);
  scratch_yaml yaml{edited_fixture(sample)};
  champsim::runtime_config cfg;
  cfg.set("ramulator2.config=" + yaml.path.string());
  clock_machine machine{sample.core, sample.core, [&](auto record) {
                          return std::make_unique<recording_driver>(champsim::make_ramulator2_driver(cfg), record);
                        }};
  // A read on every probe operation, and a write on every 61st: native's write
  // mode would starve reads outright if writes arrived faster than it serves them.
  machine.llc.plan = [](feeder_probe& probe) {
    probe.queue(false, true);
    if (probe.ops % 61 == 0) {
      probe.queue(true, true);
    }
  };
  const auto results = machine.run(
      {{"warmup", true, 64, {}, {}}, {"first", false, first_length, {}, {}}, {"again", true, sample.again, {}, {}}, {"second", false, sample.second, {}, {}}});

  REQUIRE(results.size() == 2);
  require_phase_bounds(machine.counter, 4);
  // Every native tick runs on the reference's global tick with the reference's time.
  require_schedule(machine);

  const auto& counter = machine.counter;
  const auto period = machine.periods[memory_index];
  const auto& record = *machine.record;
  const auto in_warmup = [&](long long tick) {
    const auto phase = phase_of(counter, tick);
    return phase == 0 || phase == 2;
  };
  REQUIRE(std::none_of(record.attempts.begin(), record.attempts.end(), [&](const auto& attempt) { return in_warmup(attempt.tick); }));

  // Parents as the driver boundary saw them, keyed by cache block.
  std::map<uint64_t, native_parent> parents;
  for (const auto& attempt : record.attempts) {
    if (attempt.accepted) {
      auto& parent = parents[attempt.address - attempt.address % BLOCK_SIZE];
      parent.write = attempt.write;
      parent.accepted.push_back(attempt);
    }
  }
  for (const auto& callback : record.callbacks) {
    auto found = parents.find(callback.address - callback.address % BLOCK_SIZE);
    REQUIRE(found != parents.end());
    found->second.completions.push_back(callback);
    REQUIRE(found->second.completions.size() <= found->second.accepted.size());
  }

  std::vector<phase_expectation> expected(4);
  std::vector<uint64_t> read_callbacks(4);
  std::size_t carried = 0, straddling = 0, checked_responses = 0;
  for (const auto& [block, parent] : parents) {
    CAPTURE(block, parent.write);
    const auto id = static_cast<std::size_t>((block - feeder_probe::address(0)) / BLOCK_SIZE);
    const auto& request = machine.llc.requests.at(id);
    REQUIRE(request.write == parent.write);
    const auto& origin = parent.accepted.front();
    // A request is never admitted before the first native operation that runs after it was queued.
    const auto visible = reference::first_after(machine.periods, memory_index, machine.op(llc_index, request.queued_by));
    REQUIRE(static_cast<long long>(origin.native_ticks) + 1 >= visible);
    REQUIRE(origin.time == static_cast<ps>(origin.native_ticks + 1) * period);

    const auto admitted_phase = phase_of(counter, origin.tick);
    ++(parent.write ? expected[admitted_phase].accepted_writes : expected[admitted_phase].accepted_reads);
    std::optional<callback_record> done;
    if (parent.completions.size() == sample.fragments) {
      done = parent.completions.back();
      const auto done_phase = phase_of(counter, done->tick);
      ++(parent.write ? expected[done_phase].completed_writes : expected[done_phase].completed_reads);
      if (!parent.write) {
        expected[done_phase].latency_ps += static_cast<uint64_t>(done->time - origin.time);
        carried += done_phase == 3 && admitted_phase == 1;
        // Seen by the probe on the first operation after the native operation that completed it.
        const auto seen = machine.seen_after_memory(static_cast<long long>(done->native_ticks));
        if (machine.op(llc_index, seen).tick <= machine.ticks()) {
          require_same(request.seen, machine.probe_entry(seen));
          ++checked_responses;
        } else {
          REQUIRE_FALSE(request.seen);
        }
      }
    }
    straddling += parent.accepted.size() > 1 && phase_of(counter, origin.tick) == 1 && phase_of(counter, parent.accepted.back().tick) == 3;
    for (auto phase : {1u, 3u}) {
      if (origin.tick <= counter.ends[phase] && (!done || done->tick > counter.ends[phase])) {
        ++expected[phase].outstanding_parents;
      }
    }
  }
  for (const auto& attempt : record.attempts) {
    auto& phase = expected[phase_of(counter, attempt.tick)];
    ++(attempt.accepted ? phase.accepted_fragments : phase.rejected);
    for (auto end : {1u, 3u}) {
      expected[end].outstanding_fragments += attempt.accepted && attempt.tick <= counter.ends[end];
    }
  }
  for (const auto& callback : record.callbacks) {
    const auto phase = phase_of(counter, callback.tick);
    ++expected[phase].completed_fragments;
    read_callbacks[phase] += !callback.write;
    for (auto end : {1u, 3u}) {
      expected[end].outstanding_fragments -= callback.tick <= counter.ends[end];
    }
  }

  for (auto phase : {1u, 3u}) {
    CAPTURE(phase);
    expected[phase].cycles = machine.memory_ops_by(counter.ends[phase]) - machine.memory_ops_by(counter.begins[phase]);
    const auto& stats = results[phase / 2].roi_ramulator2.value();
    require_statistics(stats, expected[phase]);
    CHECK(static_cast<uint64_t>(native_value(stats, {"memory_system", "total_num_read_requests"})
                                + native_value(stats, {"memory_system", "total_num_write_requests"}))
          == expected[phase].accepted_fragments);
    if (sample.fragments == 1) {
      // The adapter's latency starts at the operation that sent the request and
      // native's one tick earlier, before that tick: one tick apart per read.
      CHECK(expected[phase].latency_ps
            == static_cast<uint64_t>(native_value(stats, {"memory_system", "controller", "channel0", "read_latency"})
                                     - static_cast<int64_t>(read_callbacks[phase]))
                   * static_cast<uint64_t>(period));
    }
  }
  CHECK(checked_responses > 0);
  // Native work is still pending when the run retires; finalization neither
  // ticks nor delivers it, and the frozen snapshot does not move.
  CHECK(results.back().roi_ramulator2->outstanding_parents > 0);
  REQUIRE(record.finalizations == 1);
  REQUIRE(record.ticks_at_finalize == record.ticks);
  REQUIRE(record.ticks_after_finalize == 0);
  const auto callbacks = record.callbacks.size();
  REQUIRE(same_statistics(machine.memory->statistics().roi_ramulator2.value(), results.back().roi_ramulator2.value()));
  REQUIRE(record.callbacks.size() == callbacks);
  return {carried, straddling};
}
} // namespace

TEST_CASE("Real native ticks, latency origins, snapshots and finalization follow the global clock", "[native-required]")
{
  if (!champsim::ramulator2_available()) {
    SKIP("native build disabled");
  }
  // DDR4 with a faster core whose period does not divide tCK (257/833 ps).
  // LPDDR5 with a core slower than native (2000/1453 ps) and a one-slot read
  // buffer, which leaves the second 32-byte half of a block for a later tick.
  const auto sample = GENERATE(values<native_case>(
      {{"ddr4", nullptr, nullptr, 1, 257, 1500, 8, 1500}, {"lpddr5", "read_buffer_size: 32", "read_buffer_size: 1", 2, 2000, 300, 4, 300}}));
  // Whether a head is half-admitted when the first measured phase ends depends
  // on native timing, so end that phase on six consecutive instructions.
  native_outcome total;
  for (long long offset = 0; offset < 6; ++offset) {
    const auto outcome = native_run(sample, sample.first + offset);
    total.carried += outcome.carried;
    total.straddling += outcome.straddling;
  }
  // Reads admitted before the later warmup and completed after it, and (with
  // 32-byte transactions) blocks whose halves were admitted on either side of it.
  CHECK(total.carried > 0);
  if (sample.fragments > 1) {
    CHECK(total.straddling > 0);
  }
}
