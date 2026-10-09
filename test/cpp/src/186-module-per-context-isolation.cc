#include <catch.hpp>

#include "../../../branch/cbp6_ddtage/cbp6_ddtage.h"
#include "../../../branch/cbp6_runlts_norv/cbp6_runlts_norv.h"
#include "../../../branch/cbp6_tagescl192/cbp6_tagescl192.h"
#include "../../../branch/cbp6_tagescl64/cbp6_tagescl64.h"
#include "../../../btb/blbp_64kb/blbp_64kb.h"
#include "../../../btb/blbp_64kb_tuned/blbp_64kb_tuned.h"
#include "../../../btb/ittage_32kb/ittage_32kb.h"
#include "../../../btb/ittage_64kb/ittage_64kb.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Step 2.7: the eight modules that keep their state in namespace-scope globals
// (the four CBP6 branch predictors and the four vendored BTBs) are process-wide
// singletons that cannot be replicated per hardware context. They already refuse
// a second core; a single core carrying a second SMT context must refuse too,
// because SMT would reuse the same shared tables across both contexts.
//
// The second context is manufactured with cpu.contexts.emplace_back(cpu.ctx().
// reg_allocator), which shares the register allocator. This is a test-only
// device to push contexts.size() past 1 and trip the throw; it is not the
// per-context construction Stage 3 will introduce. Do not copy it as the
// canonical way to add a context.

namespace
{
struct two_context_cpu {
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU cpu{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

  two_context_cpu() { cpu.contexts.emplace_back(cpu.ctx().reg_allocator); }
};
} // namespace

TEST_CASE_METHOD(two_context_cpu, "The global-state branch predictors refuse a second hardware context")
{
  cbp6_tagescl64 tagescl64{&cpu};
  cbp6_tagescl192 tagescl192{&cpu};
  cbp6_runlts_norv runlts_norv{&cpu};
  cbp6_ddtage ddtage{&cpu};

  REQUIRE_THROWS_AS(tagescl64.initialize_branch_predictor(), std::runtime_error);
  REQUIRE_THROWS_AS(tagescl192.initialize_branch_predictor(), std::runtime_error);
  REQUIRE_THROWS_AS(runlts_norv.initialize_branch_predictor(), std::runtime_error);
  REQUIRE_THROWS_AS(ddtage.initialize_branch_predictor(), std::runtime_error);
}

TEST_CASE_METHOD(two_context_cpu, "The global-state BTBs refuse a second hardware context")
{
  ittage_32kb ittage32{&cpu};
  ittage_64kb ittage64{&cpu};
  blbp_64kb blbp{&cpu};
  blbp_64kb_tuned blbp_tuned{&cpu};

  REQUIRE_THROWS_AS(ittage32.initialize_btb(), std::runtime_error);
  REQUIRE_THROWS_AS(ittage64.initialize_btb(), std::runtime_error);
  REQUIRE_THROWS_AS(blbp.initialize_btb(), std::runtime_error);
  REQUIRE_THROWS_AS(blbp_tuned.initialize_btb(), std::runtime_error);
}
