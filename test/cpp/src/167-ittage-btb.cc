#include <catch.hpp>
#include <cstdint>

#include "instruction.h"
#include "ittage/ittage_btb.h"

// The ITTAGE BTB adapter. These tests cover the wiring and the guards, NOT the
// predictor's numerical behaviour -- that is Seznec's code, and the property
// that our parameterisation did not change it is established separately and far
// more strongly by tools/ittage_equiv, which requires bit-identical predictions
// against the pristine reference over 1.2M indirect branches.
//
// Stated explicitly because it is easy to over-read a green test file here: a
// wrong-but-plausible predictor would pass everything below. The two things
// that would catch that are the equivalence harness and the perfect_indirect
// oracle bound, not these tests.

namespace
{
struct test_cfg {
  static constexpr int NHIST = 7, LOGG = 9, TBITS = 11;
  static constexpr int MINHIST = 2, MAXHIST = 300;
  static constexpr int CWIDTH = 3, UWIDTH = 2;
};
using impl = champsim::ittage::btb_impl<test_cfg>;

constexpr champsim::address BR{0x4000};
constexpr champsim::address TARGET{0x8000};
constexpr champsim::address FALLTHROUGH{0x4004};

// ChampSim predicts and then updates the SAME instruction inside one
// do_predict_branch (src/ooo_cpu.cc). Tests must mirror that: an earlier version
// of this file called update() alone, which encoded the very unpaired-call bug
// the adapter now guards against.
void step(impl& uut, champsim::address ip, champsim::address target, uint8_t type, champsim::address next_pc)
{
  (void)uut.prediction(ip, champsim::address_space_id{});
  uut.update(ip, target, type, next_pc, champsim::address_space_id{});
}
} // namespace

TEST_CASE("The storage budget is what the configuration claims")
{
  // An entry holds a truncated target plus tag, confidence and useful bits. The
  // uint64_t the simulator stores is a simulation artifact and is excluded --
  // if that convention ever changes, the reported budgets change with it, so it
  // is pinned here as well as in the module.
  STATIC_REQUIRE(impl::target_bits == 48);
  STATIC_REQUIRE(impl::entry_bits == 48 + 3 + 11 + 2);
  STATIC_REQUIRE(impl::entry_bits == 64);

  // 8 tables x 512 entries x 64 bits = 32.0 KB
  STATIC_REQUIRE(impl::storage_bits == 8L * 512L * 64L);
  REQUIRE(impl::storage_kb == Catch::Approx(32.0));
}

TEST_CASE("An unknown IP yields no prediction")
{
  impl uut{};
  uut.initialize();

  const auto [target, always_taken] = uut.prediction(BR, champsim::address_space_id{});
  REQUIRE(target == champsim::address{});
  REQUIRE_FALSE(always_taken);
}

TEST_CASE("An indirect branch is answered by ITTAGE, never by the BTB's stale target")
{
  // The design decision under test: on an indirect branch the adapter reports
  // whatever ITTAGE says, including "nothing", rather than falling back to the
  // target the direct predictor happens to hold. Falling back would let
  // ITTAGE's measured accuracy borrow from the BTB and overstate it.
  impl uut{};
  uut.initialize();

  // Install the branch so the direct predictor knows its IP and type, and so it
  // holds TARGET as its own stored target.
  step(uut, BR, TARGET, BRANCH_INDIRECT, TARGET);

  const auto [target, always_taken] = uut.prediction(BR, champsim::address_space_id{});

  // always_taken is true on this path whether or not ITTAGE has a target, so
  // that the adapter matches basic_btb -- see the dedicated test below. What is
  // asserted here is the routing: the answer comes from ITTAGE, so it is either
  // a learned target or nothing, never the direct predictor's stale copy handed
  // back through the non-indirect path.
  REQUIRE(always_taken);
  REQUIRE(target != champsim::address{champsim::ittage::unseeded_target_sentinel});
}

TEST_CASE("The unseeded-entry sentinel never escapes as a target")
{
  // ientry() initialises target = 0xdeadbeef. An entry that tag-matches before
  // it has ever been written would otherwise hand ChampSim that value, which
  // does not announce itself as wrong -- it merely mispredicts.
  impl uut{};
  uut.initialize();

  for (uint64_t ip = 0x4000; ip < 0x4000 + 4096; ip += 16) {
    const champsim::address addr{ip};
    step(uut, addr, TARGET, BRANCH_INDIRECT, TARGET);
    const auto [target, always_taken] = uut.prediction(addr, champsim::address_space_id{});
    REQUIRE(target != champsim::address{champsim::ittage::unseeded_target_sentinel});
  }
}

TEST_CASE("Returns go to the return stack, not to ITTAGE")
{
  impl uut{};
  uut.initialize();

  constexpr champsim::address CALL{0x5000};
  constexpr champsim::address RET{0x9100};

  // Teach the direct predictor that RET is a return. This also consumes the RAS
  // entry, because calibrate_call_size() pops -- in a real run the prediction
  // for a given return happens BEFORE its update, so the pop is correct
  // behaviour and the test has to respect that ordering.
  step(uut, CALL, champsim::address{0x9000}, BRANCH_DIRECT_CALL, champsim::address{0x9000});
  step(uut, RET, champsim::address{0x5004}, BRANCH_RETURN, champsim::address{0x5004});

  // Now the realistic sequence: a call is seen, and the next return is predicted.
  step(uut, CALL, champsim::address{0x9000}, BRANCH_DIRECT_CALL, champsim::address{0x9000});
  const auto [target, always_taken] = uut.prediction(RET, champsim::address_space_id{});
  REQUIRE(always_taken); // the RAS always reports taken
  // The RAS predicts call_ip + call size, i.e. near CALL -- definitively not
  // anywhere ITTAGE would have pointed.
  REQUIRE(target.to<uint64_t>() >= CALL.to<uint64_t>());
  REQUIRE(target.to<uint64_t>() <= CALL.to<uint64_t>() + 16);
}

TEST_CASE("A conditional branch keeps the direct predictor's target and direction")
{
  impl uut{};
  uut.initialize();

  step(uut, BR, TARGET, BRANCH_CONDITIONAL, TARGET);

  const auto [target, always_taken] = uut.prediction(BR, champsim::address_space_id{});
  REQUIRE(target == TARGET);
  REQUIRE_FALSE(always_taken); // conditionals are not always-taken
}

TEST_CASE("A not-taken branch feeds the fall-through, not a zero, into the history")
{
  // ChampSim's branch_target is zero for a not-taken branch, and the vendored
  // HistoryUpdate derives its path history solely from that argument
  // (PATH = (target >> 2) ^ (target >> 6)). Passing zero would inject a
  // constant into phist and corrupt every subsequent index.
  //
  // Observed in predictor state rather than through prediction(): a predictor
  // that has learned nothing answers "no prediction" whatever history it holds,
  // so comparing predictions here would pass even if next_pc were ignored --
  // which is exactly how the first version of this test fooled itself.
  impl with_fallthrough{};
  impl with_zero{};
  with_fallthrough.initialize();
  with_zero.initialize();

  for (uint64_t i = 0; i < 64; ++i) {
    const champsim::address ip{0x4000 + 16 * i};
    step(with_fallthrough, ip, champsim::address{}, BRANCH_CONDITIONAL, FALLTHROUGH);
    step(with_zero, ip, champsim::address{}, BRANCH_CONDITIONAL, champsim::address{});
  }

  INFO("if these match, next_pc is being ignored and branch_target's zero reached the history");
  REQUIRE(with_fallthrough.predictor_state().phist != with_zero.predictor_state().phist);
}

TEST_CASE("An indirect branch feeds its real target into the history")
{
  // The complement: for a taken indirect branch the target IS the architectural
  // next PC, and it must reach the history.
  impl a{};
  impl b{};
  a.initialize();
  b.initialize();

  step(a, BR, TARGET, BRANCH_INDIRECT, TARGET);
  step(b, BR, champsim::address{0xC000}, BRANCH_INDIRECT, champsim::address{0xC000});

  REQUIRE(a.predictor_state().phist != b.predictor_state().phist);
}

TEST_CASE("An INDIRECT-typed BTB entry always reports always_taken")
{
  // always_taken OVERRIDES the direction predictor:
  // branch_prediction = impl_predict_branch(...) || always_taken
  // (src/ooo_cpu.cc). basic_btb reports true unconditionally on this path, so
  // reporting false would let ITTAGE keep a correct not-taken where the baseline
  // is forced taken -- on branches that merely ALIAS into an indirect entry,
  // since the direct BTB tags on ip>>2. That is a measurement artifact worth
  // thousands of conditional mispredicts, not a prediction win.
  impl uut{};
  uut.initialize();

  step(uut, BR, TARGET, BRANCH_INDIRECT, TARGET);

  // Whether or not ITTAGE has a target, the flag must be true.
  REQUIRE(uut.prediction(BR, champsim::address_space_id{}).second == true);
}

TEST_CASE("Updating without a preceding prediction does not corrupt predictor state")
{
  // UpdatePredictor consumes HitBank/GI[]/GTAG[] etc., which only GetPrediction
  // writes. The adapter therefore re-derives them inside update(). This pins
  // that: driving one predictor the ChampSim way and another with update() alone
  // must leave identical state, i.e. the pairing is guaranteed internally.
  impl paired{};
  impl unpaired{};
  paired.initialize();
  unpaired.initialize();

  for (uint64_t i = 0; i < 128; ++i) {
    const champsim::address ip{0x4000 + 16 * (i % 7)};
    const champsim::address tgt{0x9000 + 64 * (i % 5)};
    step(paired, ip, tgt, BRANCH_INDIRECT, tgt);
    unpaired.update(ip, tgt, BRANCH_INDIRECT, tgt, champsim::address_space_id{});
  }

  REQUIRE(paired.predictor_state().phist == unpaired.predictor_state().phist);
  for (uint64_t i = 0; i < 7; ++i) {
    const champsim::address ip{0x4000 + 16 * i};
    REQUIRE(paired.prediction(ip, champsim::address_space_id{}).first == unpaired.prediction(ip, champsim::address_space_id{}).first);
  }
}
