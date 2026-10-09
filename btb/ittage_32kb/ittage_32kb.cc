// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant; the
// set-aside is performed here, at the outermost level. See
// branch/cbp6_tagescl64/cbp6_tagescl64.cc for the full explanation.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include <array>
#include <iterator>

#include "defs.h"
#include "ittage/ittage_btb.h"
#include "ittage_32kb.h"

namespace
{
// The storage parameters, and the only thing that distinguishes this module
// from the other ITTAGE configurations.
struct config {
  static constexpr int NHIST = 7;   // tagged tables (plus the base table)
  static constexpr int LOGG = 9;    // log2 entries per table
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
static_assert(impl_type::storage_bits == static_cast<long>(7 + 1) * (1L << 9) * 64, "storage budget drifted");

// Static storage duration, as with the CBP6 tenants: the vendored predictor's
// initialisation is not guaranteed to write every member, and static storage is
// zero-initialised before construction. One independent predictor per hardware
// context; the array stays function-local (rather than a member) so every element
// still gets that zero-init-before-construct treatment.
std::array<impl_type, champsim::defs::max_contexts>& shared()
{
  static std::array<impl_type, champsim::defs::max_contexts> instances;
  return instances;
}
} // namespace

void ittage_32kb::configure(const champsim::runtime_config& cfg, std::string_view prefix)
{
  for (auto& inst : shared())
    inst.configure_direct(cfg, prefix);
}

void ittage_32kb::initialize_btb()
{
  // initialize() is a no-op today, but run it per context so any future
  // per-instance initialisation stays correct.
  for (auto& inst : shared())
    inst.initialize();
}

std::pair<champsim::address, bool> ittage_32kb::btb_prediction(champsim::address ip, champsim::context_index context, champsim::address_space_id asid)
{
  return shared()[context.value].prediction(ip, asid);
}

void ittage_32kb::update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::context_index context,
                             champsim::address_space_id asid)
{
  // The architectural next PC: the target when taken, the fall-through when not.
  // ChampSim's branch_target is zero for a not-taken branch, and feeding zero
  // into ITTAGE's path history injects a constant into every subsequent index.
  // During this hook the branch is still input_queue.front() -- it is popped
  // only after do_init_instruction returns (src/ooo_cpu.cc) -- so the successor
  // is the following entry. The successor is read from the *context's own*
  // input queue.
  champsim::address next_pc = branch_target;
  if (!taken && intern_ != nullptr && context.value < intern_->contexts.size() && std::size(intern_->contexts[context.value].input_queue) > 1) {
    next_pc = std::next(std::begin(intern_->contexts[context.value].input_queue))->ip;
  }

  shared()[context.value].update(ip, branch_target, branch_type, next_pc, asid);
}
