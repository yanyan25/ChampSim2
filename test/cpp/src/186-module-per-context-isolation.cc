#include <catch.hpp>
#include <cstdint>

#include "instruction.h" // BRANCH_* constants, needed by the BTB headers below

#include "../../../btb/blbp_64kb/blbp_64kb.h"
#include "../../../btb/blbp_64kb_tuned/blbp_64kb_tuned.h"
#include "../../../btb/ittage_32kb/ittage_32kb.h"
#include "../../../btb/ittage_64kb/ittage_64kb.h"

// Stage 3.3, module-level per-context isolation. BLBP and ITTAGE replicate their
// state in a std::array indexed by champsim::context_index, so training one
// hardware context's direct BTB must not be visible to another. These tests
// drive the module hooks directly with two distinct context indices and check
// that each context's answer comes from its own slice.
//
// The direct-BTB path is used because it is deterministic and independent of the
// indirect predictor: a BRANCH_DIRECT_JUMP with taken=true stores the target in
// the direct predictor and (for ITTAGE) only touches the path history, never the
// core's input queue -- so a module constructed without a core (intern_ ==
// nullptr) is safe to drive here.

namespace
{
constexpr champsim::address ip{0x110000};
constexpr champsim::address target_a{0xaaa000};
constexpr champsim::address target_b{0xbbb000};
constexpr champsim::address_space_id asid{0, 0};

// Train context {0} toward target_a and context {1} toward target_b, then check
// each reads its own back. The cross-checks are the isolation assertion: if the
// two contexts shared one direct BTB, the later write would win on both reads.
template <typename Btb>
void check_isolation()
{
  Btb uut; // default-constructed without a core: intern_ == nullptr

  uut.update_btb(ip, target_a, true, BRANCH_DIRECT_JUMP, champsim::context_index{0}, asid);
  uut.update_btb(ip, target_b, true, BRANCH_DIRECT_JUMP, champsim::context_index{1}, asid);

  const auto [pred_a, taken_a] = uut.btb_prediction(ip, champsim::context_index{0}, asid);
  const auto [pred_b, taken_b] = uut.btb_prediction(ip, champsim::context_index{1}, asid);

  REQUIRE(pred_a == target_a);
  REQUIRE(taken_a);
  REQUIRE(pred_b == target_b);
  REQUIRE(taken_b);
}
} // namespace

TEST_CASE("blbp_64kb keeps each context's direct BTB separate") { check_isolation<blbp_64kb>(); }

TEST_CASE("blbp_64kb_tuned keeps each context's direct BTB separate") { check_isolation<blbp_64kb_tuned>(); }

TEST_CASE("ittage_32kb keeps each context's direct BTB separate") { check_isolation<ittage_32kb>(); }

TEST_CASE("ittage_64kb keeps each context's direct BTB separate") { check_isolation<ittage_64kb>(); }
