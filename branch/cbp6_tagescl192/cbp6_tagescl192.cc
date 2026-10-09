// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant, so a module
// cannot simply #include "ooo_cpu.h". ooo_cpu.h:20-23 sets the flag aside and
// undefines CHAMPSIM_MODULE; the address.h it then includes finds the flag set
// on its own way out (inc/address.h, final block) and re-defines
// CHAMPSIM_MODULE -- so by the time ooo_cpu.h:45 reaches util/lru_table.h, the
// macro is back, and that header's #error fires because it sits outside its own
// include guard. No shipped branch or BTB module includes ooo_cpu.h, which is
// why the path is untested upstream.
//
// Performing the set-aside here, at the outermost level, avoids the nesting
// entirely. This is a workaround for that bug, not a requirement of the module
// interface.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include "cbp6_tagescl192.h"

// Every include the vendored header uses, hoisted to global scope so that the
// copies nested inside the anonymous namespace below become no-ops via their
// own include guards. Without this, <iostream> and friends would be pulled into
// the namespace.
#include <array>
#include <assert.h>
#include <inttypes.h>
#include <iostream>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <vector>

#include <iterator>
#include <stdexcept>

#include "cbp6/cbp6_host.h"

namespace
{
// The vendored predictor declares its tables as namespace-scope globals with
// external linkage (Bias, GGEHLA, ...) and uses the include guard
// _TAGE_PREDICTOR_H_, which its 64KB sibling also uses. Wrapping it in an
// anonymous namespace gives every one of those symbols internal linkage, so
// several CBP6 predictors can be linked into one binary -- which ChampSim does
// by default, since config.sh compiles every module it finds.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wreorder"
#pragma GCC diagnostic ignored "-Wparentheses"
#include "vendor/cbp2016_tage_sc_l_192kb.hpp"
#pragma GCC diagnostic pop

champsim::cbp6::host<CBP2016_TAGE_SC_L>& shared_host()
{
  static champsim::cbp6::host<CBP2016_TAGE_SC_L> instance;
  return instance;
}

} // namespace

void cbp6_tagescl192::initialize_branch_predictor() { shared_host().initialize(); }

bool cbp6_tagescl192::predict_branch(champsim::address ip, champsim::address /*predicted_target*/, bool /*always_taken*/, uint8_t branch_type,
                                     champsim::address_space_id /*asid*/)
{
  return shared_host().predict(ip, branch_type);
}

void cbp6_tagescl192::last_branch_result(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type,
                                         champsim::address_space_id /*asid*/)
{
  // The tenant needs the architectural next PC. ChampSim's branch_target is the
  // successor's IP for a taken branch but zero for a not-taken one
  // (src/tracereader.cc:31), and TAGE-SC-L tests `nextPC < PC` to detect
  // backward branches unconditionally -- not gated on taken -- so passing zero
  // makes every not-taken conditional look like a loop.
  //
  // Measured impact is small: on 400.perlbench (50M instr) passing branch_target
  // straight through gives 1.862 MPKI versus 1.863 here, which is noise. That is
  // consistent with the vendored header's own annotation that IMLI is worth
  // ~0.2% (`#define IMLI // 0.2 %`). This is kept because it is the semantically
  // correct value and costs nothing, not because it moves the number -- and a
  // tenant that leans harder on nextPC would care more.
  //
  // The successor's IP is the correct next PC in both cases. During this hook
  // the branch itself is still input_queue.front() -- it is popped only after
  // do_init_instruction returns (src/ooo_cpu.cc:98-102) -- so the successor is
  // the following entry. Reading it here is confined to the resolved-outcome
  // path, where ChampSim has already handed us the true direction and target;
  // reading ahead at prediction time would be a causality violation.
  champsim::address next_pc = branch_target;
  if (intern_ != nullptr && std::size(intern_->ctx().input_queue) > 1) {
    next_pc = std::next(std::begin(intern_->ctx().input_queue))->ip;
  }

  const auto instr_id = (intern_ != nullptr && !std::empty(intern_->ctx().input_queue)) ? intern_->ctx().input_queue.front().instr_id : 0;
  shared_host().resolve(ip, taken, branch_type, next_pc, intern_ == nullptr || !intern_->warmup, instr_id);
}

void cbp6_tagescl192::branch_predictor_final_stats()
{
  // ROI instruction count, so the reported MPKI matches the window ChampSim
  // reports its own statistics over.
  const auto roi_instructions = (intern_ != nullptr) ? intern_->ctx().roi_stats.instrs() : 0ULL;
  shared_host().finish(static_cast<uint64_t>(roi_instructions));
}

void cbp6_tagescl192::branch_execute_resolve(uint64_t instr_id, champsim::address /*ip*/, champsim::address /*branch_target*/, bool /*taken*/,
                                             uint8_t /*branch_type*/, champsim::address_space_id /*asid*/)
{
  shared_host().execute_resolve(instr_id);
}
