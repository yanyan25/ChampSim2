#include <catch.hpp>
#include <stdexcept>

#include "instruction.h" // BRANCH_* constants, needed by the BTB headers below

#include "../../../branch/bimodal/bimodal.h"
#include "../../../branch/cbp6_ddtage/cbp6_ddtage.h"
#include "../../../branch/cbp6_runlts_norv/cbp6_runlts_norv.h"
#include "../../../branch/cbp6_tagescl192/cbp6_tagescl192.h"
#include "../../../branch/cbp6_tagescl64/cbp6_tagescl64.h"
#include "../../../branch/gshare/gshare.h"
#include "../../../branch/hashed_perceptron/hashed_perceptron.h"
#include "../../../branch/perceptron/perceptron.h"
#include "../../../btb/basic_btb/basic_btb.h"
#include "../../../btb/blbp_64kb/blbp_64kb.h"
#include "../../../btb/blbp_64kb_tuned/blbp_64kb_tuned.h"
#include "../../../btb/ittage_32kb/ittage_32kb.h"
#include "../../../btb/ittage_64kb/ittage_64kb.h"
#include "mocks.hpp"
#include "ooo_cpu.h"
#include "runtime_config.h"
#include "static_environment.h"

// Stage 3.3, capability matrix. How a module stores its state with respect to a
// core's hardware contexts, advertised either explicitly (a `static constexpr
// smt_capability capability` member) or by the conservative default
// (`unsupported`, so an SMT-unsafe module is refused rather than silently
// sharing state). The environment folds this over a module set with
// min_smt_capability and refuses at configuration time via check_smt_capability.
//
// The trait is a compile-time property; the guard and the fold are the runtime
// plumbing the environment's select_modules uses.

namespace
{
using champsim::modules::smt_capability;
using champsim::modules::smt_capability_of;
} // namespace

TEST_CASE("The per-context BTBs advertise per_context capability")
{
  REQUIRE(smt_capability_of<blbp_64kb>::value == smt_capability::per_context);
  REQUIRE(smt_capability_of<blbp_64kb_tuned>::value == smt_capability::per_context);
  REQUIRE(smt_capability_of<ittage_32kb>::value == smt_capability::per_context);
  REQUIRE(smt_capability_of<ittage_64kb>::value == smt_capability::per_context);
}

TEST_CASE("A module with no declared capability defaults to unsupported")
{
  // The CBP6 tenants keep their tables in namespace-scope globals, so they are
  // genuinely unsupported: one core, one context.
  REQUIRE(smt_capability_of<cbp6_tagescl64>::value == smt_capability::unsupported);
  REQUIRE(smt_capability_of<cbp6_tagescl192>::value == smt_capability::unsupported);
  REQUIRE(smt_capability_of<cbp6_runlts_norv>::value == smt_capability::unsupported);
  REQUIRE(smt_capability_of<cbp6_ddtage>::value == smt_capability::unsupported);
}

TEST_CASE("The ASID-keyed per-core modules advertise shared capability")
{
  // Stage 3.4 classification: these hold one instance per core whose tables fold
  // ASID into the index/hash (Stage 2.5), so two hardware contexts of different
  // address spaces land in disjoint entries. They are `shared`, not per_context:
  // replicating them would waste state the ASID keying already isolates.
  REQUIRE(smt_capability_of<basic_btb>::value == smt_capability::shared);
  REQUIRE(smt_capability_of<hashed_perceptron>::value == smt_capability::shared);
  REQUIRE(smt_capability_of<bimodal>::value == smt_capability::shared);
  REQUIRE(smt_capability_of<gshare>::value == smt_capability::shared);
  REQUIRE(smt_capability_of<perceptron>::value == smt_capability::shared);
}

TEST_CASE("min_smt_capability keeps the most restrictive capability")
{
  using champsim::modules::min_smt_capability;

  REQUIRE(min_smt_capability(smt_capability::unsupported, smt_capability::per_context) == smt_capability::unsupported);
  REQUIRE(min_smt_capability(smt_capability::unsupported, smt_capability::shared) == smt_capability::unsupported);
  REQUIRE(min_smt_capability(smt_capability::per_context, smt_capability::shared) == smt_capability::per_context);
  REQUIRE(min_smt_capability(smt_capability::shared, smt_capability::shared) == smt_capability::shared);
}

TEST_CASE("check_smt_capability refuses only unsupported modules past one core/context")
{
  using champsim::modules::check_smt_capability;

  // Single core, single context: even an unsupported module is safe.
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::unsupported, 0, 1, "x"));

  // A second context or a second core breaks an unsupported (singleton) module.
  REQUIRE_THROWS_AS(check_smt_capability(smt_capability::unsupported, 0, 2, "x"), std::runtime_error);
  REQUIRE_THROWS_AS(check_smt_capability(smt_capability::unsupported, 1, 1, "x"), std::runtime_error);
  REQUIRE_THROWS_AS(check_smt_capability(smt_capability::unsupported, 1, 2, "x"), std::runtime_error);

  // A declared capability is admitted anywhere.
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::per_context, 0, 2, "x"));
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::per_context, 1, 2, "x"));
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::shared, 1, 2, "x"));
}

TEST_CASE("The type-erased pimpl reports its module's capability")
{
  do_nothing_MRC mock_L1I, mock_L1D;

  O3_CPU per_context_core{champsim::core_builder{}.btb<blbp_64kb>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};
  REQUIRE(per_context_core.btb_module_pimpl->capability() == smt_capability::per_context);

  O3_CPU unsupported_core{champsim::core_builder{}.branch_predictor<cbp6_tagescl64>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};
  REQUIRE(unsupported_core.branch_module_pimpl->capability() == smt_capability::unsupported);

  O3_CPU shared_core{champsim::core_builder{}.btb<basic_btb>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};
  REQUIRE(shared_core.btb_module_pimpl->capability() == smt_capability::shared);
}

TEST_CASE("The environment refuses a cbp6 branch predictor with two hardware contexts")
{
  // Stage 5 (debt b) closure, the "reject" half end-to-end: the refusal must
  // reach the full static_environment construction chain, not just the
  // check_smt_capability function above. A cbp6 tenant keeps its tables in
  // namespace-scope globals, so selecting one with num_contexts=2 must throw at
  // construction time rather than silently share state.
  for (const auto* name : {"cbp6_tagescl64", "cbp6_tagescl192", "cbp6_runlts_norv", "cbp6_ddtage"}) {
    CAPTURE(name);
    champsim::runtime_config cfg;
    cfg.set("ooo_cpu.cpu0.num_contexts=2");
    cfg.set(std::string{"ooo_cpu.cpu0.branch_predictor="} + name);
    REQUIRE_THROWS_WITH(champsim::static_environment{cfg}, Catch::Matchers::ContainsSubstring("namespace-scope globals"));
  }
}

TEST_CASE("The environment admits a per-context BTB with two hardware contexts")
{
  // The positive control: the throw above is specific to the unsupported cbp6
  // predictor, not to num_contexts=2 itself. A per-context BTB (its state in a
  // per-context array) must build cleanly with two contexts.
  champsim::runtime_config cfg;
  cfg.set("ooo_cpu.cpu0.num_contexts=2");
  cfg.set("ooo_cpu.cpu0.btb=ittage_32kb");
  REQUIRE_NOTHROW(champsim::static_environment{cfg});
}
