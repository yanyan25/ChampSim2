#include <catch.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "cbp6/cbp6_host.h"
#include "instruction.h"

namespace
{
// Records the exact CBP6 call sequence the host produces. The protocol -- which
// calls happen, in what order, with which arguments -- is the whole contract of
// the adapter, so it is asserted directly rather than inferred from accuracy.
struct recording_tenant {
  struct call {
    std::string name;
    uint64_t seq_no{};
    uint64_t pc{};
    int brtype{};
    bool pred_dir{};
    bool resolve_dir{};
    uint64_t next_pc{};
  };

  std::vector<call> calls;
  bool next_prediction = false;

  void setup() { calls.push_back({"setup", 0, 0, 0, false, false, 0}); }
  void terminate() { calls.push_back({"terminate", 0, 0, 0, false, false, 0}); }

  bool predict(uint64_t seq_no, uint8_t piece, uint64_t pc)
  {
    REQUIRE(piece == 0); // ChampSim has no uop concept
    calls.push_back({"predict", seq_no, pc, 0, false, false, 0});
    return next_prediction;
  }

  void history_update(uint64_t seq_no, uint8_t piece, uint64_t pc, int brtype, bool pred_dir, bool resolve_dir, uint64_t next_pc)
  {
    REQUIRE(piece == 0);
    calls.push_back({"history_update", seq_no, pc, brtype, pred_dir, resolve_dir, next_pc});
  }

  void TrackOtherInst(uint64_t pc, int brtype, bool pred_dir, bool resolve_dir, uint64_t next_pc)
  {
    calls.push_back({"TrackOtherInst", 0, pc, brtype, pred_dir, resolve_dir, next_pc});
  }

  void update(uint64_t seq_no, uint8_t piece, uint64_t pc, bool resolve_dir, bool pred_dir, uint64_t next_pc)
  {
    REQUIRE(piece == 0);
    calls.push_back({"update", seq_no, pc, 0, pred_dir, resolve_dir, next_pc});
  }
};

using host_type = champsim::cbp6::host<recording_tenant>;

constexpr champsim::address BR{0x4000};
constexpr champsim::address TGT{0x5000};
constexpr champsim::address FALLTHROUGH{0x4004};
} // namespace

TEST_CASE("A non-branch instruction never reaches the tenant predictor")
{
  host_type uut;

  // ChampSim calls predict_branch for every instruction (src/ooo_cpu.cc:135-138).
  REQUIRE(uut.predict(champsim::address{0x1000}, NOT_BRANCH));
  REQUIRE(std::empty(uut.tenant().calls));
}

TEST_CASE("A conditional branch is predicted, then history-updated, then updated")
{
  host_type uut;
  uut.tenant().next_prediction = true;

  const bool pred = uut.predict(BR, BRANCH_CONDITIONAL);
  REQUIRE(pred);
  uut.resolve(BR, true, BRANCH_CONDITIONAL, TGT);

  const auto& c = uut.tenant().calls;
  REQUIRE(std::size(c) == 3);
  REQUIRE(c.at(0).name == "predict");
  REQUIRE(c.at(1).name == "history_update");
  REQUIRE(c.at(2).name == "update");

  // All three must refer to the same dynamic branch.
  REQUIRE(c.at(0).seq_no == c.at(1).seq_no);
  REQUIRE(c.at(1).seq_no == c.at(2).seq_no);
  REQUIRE(c.at(0).pc == BR.to<uint64_t>());

  // The prediction handed back to ChampSim is the one reported to the tenant.
  REQUIRE(c.at(1).pred_dir == pred);
  REQUIRE(c.at(2).pred_dir == pred);
  REQUIRE(c.at(1).resolve_dir);
  REQUIRE(c.at(2).resolve_dir);
  REQUIRE(c.at(1).brtype == champsim::cbp6::BRTYPE_COND);
}

TEST_CASE("An unconditional branch is forced taken and only tracked")
{
  host_type uut;
  uut.tenant().next_prediction = false; // must be ignored

  // CBP6 forces pred_taken = true for non-conditional branches (lib/bp.cc:128,164).
  REQUIRE(uut.predict(BR, BRANCH_RETURN));
  uut.resolve(BR, true, BRANCH_RETURN, TGT);

  const auto& c = uut.tenant().calls;
  REQUIRE(std::size(c) == 1);
  REQUIRE(c.at(0).name == "TrackOtherInst");
  REQUIRE(c.at(0).brtype == (champsim::cbp6::BRTYPE_RETURN | champsim::cbp6::BRTYPE_INDIRECT));
  REQUIRE(c.at(0).next_pc == TGT.to<uint64_t>());
}

TEST_CASE("Sequence numbers are unique per conditional branch and stable within one")
{
  host_type uut;

  uut.predict(BR, BRANCH_CONDITIONAL);
  uut.resolve(BR, true, BRANCH_CONDITIONAL, TGT);
  uut.predict(BR, BRANCH_CONDITIONAL);
  uut.resolve(BR, true, BRANCH_CONDITIONAL, TGT);

  const auto& c = uut.tenant().calls;
  REQUIRE(std::size(c) == 6);
  const auto first = c.at(0).seq_no;
  const auto second = c.at(3).seq_no;
  REQUIRE(first != second);
  // ...and each branch's three calls share one sequence number.
  REQUIRE(c.at(1).seq_no == first);
  REQUIRE(c.at(2).seq_no == first);
  REQUIRE(c.at(4).seq_no == second);
  REQUIRE(c.at(5).seq_no == second);
}

TEST_CASE("A not-taken conditional branch reports its fall-through as next_pc")
{
  // TAGE-SC-L treats nextPC < PC as a backward (loop) branch in HistoryUpdate,
  // unconditionally -- not gated on taken. ChampSim's branch_target is zero for
  // a not-taken branch, so passing it through would make every not-taken
  // conditional look like a backward branch and corrupt IMLI.
  host_type uut;

  uut.predict(BR, BRANCH_CONDITIONAL);
  uut.resolve(BR, false, BRANCH_CONDITIONAL, FALLTHROUGH);

  const auto& c = uut.tenant().calls;
  REQUIRE(c.at(1).name == "history_update");
  REQUIRE(c.at(1).next_pc == FALLTHROUGH.to<uint64_t>());
  REQUIRE(c.at(1).next_pc > BR.to<uint64_t>()); // forward, i.e. not a loop
  REQUIRE_FALSE(c.at(1).resolve_dir);
}

TEST_CASE("initialize and terminate reach the tenant")
{
  host_type uut;
  uut.initialize();
  uut.finish(0);

  const auto& c = uut.tenant().calls;
  REQUIRE(std::size(c) == 2);
  REQUIRE(c.at(0).name == "setup");
  REQUIRE(c.at(1).name == "terminate");
}

TEST_CASE("A CBP6 tenant refuses to run on a second core or context")
{
  // Tenants keep their tables in namespace-scope globals, so every core's module
  // instance would share one predictor. This must fail loudly rather than
  // silently report wrong numbers for a multi-core run, or for a single core
  // carrying more than one SMT hardware context.
  REQUIRE_NOTHROW(champsim::cbp6::require_single_core(0, 1, "test_predictor"));
  REQUIRE_THROWS_AS(champsim::cbp6::require_single_core(1, 1, "test_predictor"), std::runtime_error);
  REQUIRE_THROWS_AS(champsim::cbp6::require_single_core(7, 1, "test_predictor"), std::runtime_error);
  REQUIRE_THROWS_AS(champsim::cbp6::require_single_core(0, 2, "test_predictor"), std::runtime_error);
}
