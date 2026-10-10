#include <catch.hpp>
#include <tuple>

#include "environment.h"
#include "phase_info.h"
#include "tracereader.h"

namespace champsim
{
phase_stats do_phase(const phase_info&, environment&, std::vector<tracereader>&, chrono::clock&, const simulation_knobs&);
}

namespace
{
struct phase_environment : champsim::environment {
  O3_CPU first{champsim::core_builder{}.index(0)}, second{champsim::core_builder{}.index(1)};
  std::vector<std::pair<char, long long>> operations;
  std::vector<std::tuple<char, unsigned, long long>> endings;

  struct component : champsim::operable {
    phase_environment& env;
    char id;
    component(phase_environment& e, char i, long long period) : operable(champsim::chrono::picoseconds{period}), env(e), id(i) {}
    void begin_phase() override
    {
      if (id == 'F') {
        env.first.begin_phase();
        env.second.begin_phase();
      }
    }
    long operate() override
    {
      const auto tick = current_time.time_since_epoch().count();
      env.operations.emplace_back(id, tick);
      if (id == 'F') {
        ++env.first.ctx().num_retired;
        env.second.ctx().num_retired += (tick % 2 == 0);
        env.first.current_time = env.second.current_time = current_time;
      }
      return 1;
    }
    void end_phase(unsigned cpu) override { env.endings.emplace_back(id, cpu, env.fast.current_time.time_since_epoch().count()); }
  } slow{*this, 'S', 3}, fast{*this, 'F', 1}, medium{*this, 'M', 2};

  struct memory : champsim::memory_backend {
    component& op;
    explicit memory(component& c) : op(c) {}
    champsim::operable& clocked_component() override { return op; }
    champsim::data::bytes size() const override { return champsim::data::bytes{64}; }
    champsim::memory_statistics statistics() const override { return {}; }
    std::string_view name() const override { return "legacy"; }
  } mem{slow};

  std::vector<std::reference_wrapper<O3_CPU>> cpu_view() override { return {first, second}; }
  std::vector<std::reference_wrapper<CACHE>> cache_view() override { return {}; }
  std::vector<std::reference_wrapper<PageTableWalker>> ptw_view() override { return {}; }
  champsim::memory_backend& memory_view() override { return mem; }
  std::vector<std::reference_wrapper<champsim::operable>> operable_view() override { return {slow, fast, medium}; }
};
} // namespace

TEST_CASE("Phase scheduling preserves canonical ties and completes each CPU once")
{
  phase_environment env;
  std::vector<champsim::tracereader> traces;
  traces.emplace_back([] { return ooo_model_instr{0, input_instr{}}; });
  champsim::chrono::clock clock;
  champsim::phase_info phase{"Simulation", false, 3, {{0}, {0}}, {"synthetic"}};
  const auto stats = champsim::do_phase(phase, env, traces, clock, {});

  const std::vector<std::pair<char, long long>> expected_operations{{'S', 3}, {'F', 1}, {'M', 2}, {'F', 2}, {'F', 3}, {'M', 4},
                                                                    {'S', 6}, {'F', 4}, {'F', 5}, {'M', 6}, {'F', 6}};
  const std::vector<std::tuple<char, unsigned, long long>> expected_endings{{'S', 0, 3}, {'F', 0, 3}, {'M', 0, 3}, {'S', 1, 6}, {'F', 1, 6}, {'M', 1, 6}};
  CHECK(env.operations == expected_operations);
  CHECK(env.endings == expected_endings);
  CHECK(env.first.sim_instr() == 6);
  CHECK(env.second.sim_instr() == 3);
  CHECK(stats.trace_names == std::vector<std::string>{"synthetic", "synthetic"});
}

TEST_CASE("A trace reaching EOF completes every remaining CPU on that tick")
{
  struct finite_reader {
    unsigned remaining = 1;
    ooo_model_instr operator()()
    {
      --remaining;
      return ooo_model_instr{0, input_instr{}};
    }
    bool eof() const { return remaining == 0; }
  };
  phase_environment env;
  std::vector<champsim::tracereader> traces;
  traces.emplace_back(finite_reader{});
  champsim::chrono::clock clock;
  champsim::phase_info phase{"Warmup", true, 100, {{0}, {0}}, {"finite"}};
  champsim::do_phase(phase, env, traces, clock, {});

  const std::vector<std::tuple<char, unsigned, long long>> expected_endings{{'S', 0, 1}, {'F', 0, 1}, {'M', 0, 1}, {'S', 1, 1}, {'F', 1, 1}, {'M', 1, 1}};
  CHECK(env.endings == expected_endings);
  CHECK(env.first.sim_instr() == 1);
  CHECK(env.second.sim_instr() == 0);
  CHECK(env.first.ctx().input_queue.size() == 1);
  CHECK(env.second.ctx().input_queue.empty());
}
