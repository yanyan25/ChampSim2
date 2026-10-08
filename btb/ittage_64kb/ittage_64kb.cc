// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant; the
// set-aside is performed here, at the outermost level. See
// branch/cbp6_tagescl64/cbp6_tagescl64.cc for the full explanation.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include <iterator>

#include "cbp6/cbp6_host.h"
#include "ittage/ittage_btb.h"
#include "ittage_64kb.h"

namespace
{
// The storage parameters, and the only thing that distinguishes this module
// from the other ITTAGE configurations.
struct config {
  static constexpr int NHIST = 7;   // tagged tables (plus the base table)
  static constexpr int LOGG = 10;   // log2 entries per table
  static constexpr int TBITS = 11;  // tag width
  static constexpr int MINHIST = 2; // geometric history series bounds
  static constexpr int MAXHIST = 300;
  static constexpr int CWIDTH = 3; // confidence counter width
  static constexpr int UWIDTH = 2; // useful counter width
};

using impl_type = champsim::ittage::btb_impl<config>;

// Compile-time budget check, so a configuration edit cannot silently move the
// storage this module is supposed to represent.
static_assert(impl_type::entry_bits == 64, "entry layout changed; revisit the storage accounting");
static_assert(impl_type::storage_bits == static_cast<long>(7 + 1) * (1L << 10) * 64, "storage budget drifted");

// Static storage duration, as with the CBP6 tenants: the vendored predictor's
// initialisation is not guaranteed to write every member, and static storage is
// zero-initialised before construction.
impl_type& shared()
{
  static impl_type instance;
  return instance;
}
} // namespace

void ittage_64kb::configure(const champsim::runtime_config& cfg, std::string_view prefix) { shared().configure_direct(cfg, prefix); }

void ittage_64kb::initialize_btb()
{
  // The predictor, direct BTB and RAS live in one function-local static shared
  // by every instance, so a multi-core configuration would silently share one
  // predictor where basic_btb gives each core its own. intern_ is null in unit
  // tests, which construct the module without a core.
  if (intern_ != nullptr) {
    champsim::cbp6::require_single_core(intern_->cpu, "ittage_64kb");
  }
  shared().initialize();
}

std::pair<champsim::address, bool> ittage_64kb::btb_prediction(champsim::address ip, champsim::address_space_id asid) { return shared().prediction(ip, asid); }

void ittage_64kb::update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid)
{
  // The architectural next PC: the target when taken, the fall-through when not.
  // ChampSim's branch_target is zero for a not-taken branch, and feeding zero
  // into ITTAGE's path history injects a constant into every subsequent index.
  // During this hook the branch is still input_queue.front() -- it is popped
  // only after do_init_instruction returns (src/ooo_cpu.cc) -- so the successor
  // is the following entry.
  champsim::address next_pc = branch_target;
  if (!taken && intern_ != nullptr && std::size(intern_->ctx().input_queue) > 1) {
    next_pc = std::next(std::begin(intern_->ctx().input_queue))->ip;
  }

  shared().update(ip, branch_target, branch_type, next_pc, asid);
}
