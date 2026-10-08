#include <catch.hpp>

#include <cstdint>
#include <vector>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// ChampSim gives branch predictors three hooks: initialize, predict, and
// last_branch_result. Prefetchers and replacement policies additionally get a
// final-stats hook, and CBP6 predictors need somewhere to run terminate() and
// report their own counters. These tests cover the added dispatch.

namespace
{
struct counting_branch_predictor : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;

  // Static because the module is constructed inside the core and there is no
  // handle to the instance from the test.
  static inline int final_stats_calls = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

  static void reset() { final_stats_calls = 0; }

  bool predict_branch(champsim::address /*ip*/, champsim::address /*predicted_target*/, bool /*always_taken*/, uint8_t /*branch_type*/,
                      champsim::address_space_id /*asid*/) { return false; }
  void last_branch_result(champsim::address /*ip*/, champsim::address /*target*/, bool /*taken*/, uint8_t /*type*/,
                          champsim::address_space_id /*asid*/) {}

  void branch_predictor_final_stats() { ++final_stats_calls; }
};

struct silent_branch_predictor : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;
  bool predict_branch(champsim::address /*ip*/, champsim::address /*predicted_target*/, bool /*always_taken*/, uint8_t /*branch_type*/,
                      champsim::address_space_id /*asid*/) { return false; }
  // deliberately no branch_predictor_final_stats
};
} // namespace

TEST_CASE("A branch predictor's final-stats hook is dispatched")
{
  counting_branch_predictor::reset();

  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).branch_predictor<counting_branch_predictor>()};

  REQUIRE(counting_branch_predictor::final_stats_calls == 0);

  uut.impl_branch_predictor_final_stats();

  REQUIRE(counting_branch_predictor::final_stats_calls == 1);
}

TEST_CASE("A branch predictor without a final-stats hook is not required to have one")
{
  // The hook is optional, exactly like the prefetcher's. A predictor that omits
  // it must still compile and run.
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).branch_predictor<silent_branch_predictor>()};

  REQUIRE_NOTHROW(uut.impl_branch_predictor_final_stats());
}

TEST_CASE("The final-stats trait detects the hook")
{
  STATIC_REQUIRE(champsim::modules::branch_predictor::has_final_stats<counting_branch_predictor>);
  STATIC_REQUIRE_FALSE(champsim::modules::branch_predictor::has_final_stats<silent_branch_predictor>);
}

namespace
{
// CBP6 splits the update in two: speculative history right after the
// prediction, and the non-speculative table update at the branch's execute
// cycle, out of program order. ChampSim resolves everything at fetch, so
// without an execute-time hook the two collapse and every branch predicts
// against tables its immediate predecessor already updated.
struct resolve_recording_predictor : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;

  struct record {
    uint64_t instr_id;
    uint64_t ip;
    bool taken;
    uint8_t branch_type;
  };

  static inline std::vector<record> resolved; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  static void reset() { resolved.clear(); }

  bool predict_branch(champsim::address /*ip*/, champsim::address /*predicted_target*/, bool /*always_taken*/, uint8_t /*branch_type*/,
                      champsim::address_space_id /*asid*/) { return false; }

  void branch_execute_resolve(uint64_t instr_id, champsim::address ip, champsim::address /*target*/, bool taken, uint8_t branch_type,
                              champsim::address_space_id /*asid*/)
  {
    resolved.push_back({instr_id, ip.to<uint64_t>(), taken, branch_type});
  }
};
} // namespace

TEST_CASE("A branch predictor is notified when a branch completes execution")
{
  resolve_recording_predictor::reset();

  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}
                 .rob_size(16)
                 .fetch_queues(&mock_L1I.queues)
                 .data_queues(&mock_L1D.queues)
                 .branch_predictor<resolve_recording_predictor>()};

  auto instr = champsim::test::instruction_with_ip(0x4000);
  instr.instr_id = 42;
  instr.is_branch = true;
  instr.branch = BRANCH_CONDITIONAL;
  instr.branch_taken = true;
  instr.executed = true;
  instr.completed = false;
  instr.ready_time = champsim::chrono::clock::time_point{};
  uut.ctx().ROB.push_back(instr);

  for (auto op : std::array<champsim::operable*, 3>{{&uut, &mock_L1I, &mock_L1D}}) {
    op->_operate();
  }

  REQUIRE(std::size(resolve_recording_predictor::resolved) == 1);
  REQUIRE(resolve_recording_predictor::resolved.at(0).instr_id == 42);
  REQUIRE(resolve_recording_predictor::resolved.at(0).ip == 0x4000);
  REQUIRE(resolve_recording_predictor::resolved.at(0).taken);
  REQUIRE(resolve_recording_predictor::resolved.at(0).branch_type == BRANCH_CONDITIONAL);
}

TEST_CASE("A non-branch instruction does not trigger the execute-resolve hook")
{
  resolve_recording_predictor::reset();

  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}
                 .rob_size(16)
                 .fetch_queues(&mock_L1I.queues)
                 .data_queues(&mock_L1D.queues)
                 .branch_predictor<resolve_recording_predictor>()};

  auto instr = champsim::test::instruction_with_ip(0x5000);
  instr.is_branch = false;
  instr.branch = NOT_BRANCH;
  instr.executed = true;
  instr.completed = false;
  instr.ready_time = champsim::chrono::clock::time_point{};
  uut.ctx().ROB.push_back(instr);

  for (auto op : std::array<champsim::operable*, 3>{{&uut, &mock_L1I, &mock_L1D}}) {
    op->_operate();
  }

  REQUIRE(std::empty(resolve_recording_predictor::resolved));
}
