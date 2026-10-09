/*
 * RUNLTS -- "Register-value-aware predictor Utilizing Nested Large TableS",
 * the CBP2025 (CBP6) winner by Koizumi et al. -- hosted in ChampSim.
 *
 * *** THIS IS NOT THE PREDICTOR THAT WON THE CHAMPIONSHIP. ***
 *
 * RUNLTS's headline contribution is a statistical-corrector component indexed by
 * a digest of recently produced architectural REGISTER VALUES, fed through
 * decode_notify()/execute_notify(). ChampSim traces carry register indices and
 * memory addresses but no register values, so that channel cannot be driven and
 * is left inert -- hence the _norv suffix. What runs here is RUNLTS minus its
 * distinguishing feature.
 *
 * The component degrades cleanly rather than breaking, and its inertness is
 * structural rather than merely observed: champsim::cbp6::host has no notion of
 * decode_notify()/execute_notify() and never calls them, so RegFileState entries
 * never become valid, register_values stays -1, the guard in predict() never
 * selects a register, and RBias contributes exactly nothing. Nothing asserts and
 * nothing looks wrong at runtime -- which is why the limitation is in the module
 * name. A result from this module must never be reported as "the CBP2025 winner".
 *
 * Measured cost of the missing channel, in CBP6's own simulator over 18 traces:
 * +6.33% MPKI, +0.84% CycWpPKI. See the design doc, section 2.1.
 *
 * Known defect in the submission, deliberately left as-is:
 * CBP2025_RUNLTS::TrackOtherInst forwards pred_dir and resolve_dir to the
 * implementation in swapped order. It is inert in both this adapter and the CBP6
 * framework because that path only ever carries unconditional branches, where
 * both are true. The protocol checker asserts that precondition rather than the
 * vendored source being patched.
 *
 * SINGLE CORE, SINGLE CONTEXT ONLY, as with the other CBP6 tenants: predictor
 * state lives in namespace-scope globals shared by every instance. No
 * smt_capability is declared, so it defaults to unsupported and the environment
 * refuses it at configuration time.
 */

#ifndef BRANCH_CBP6_RUNLTS_NORV_H
#define BRANCH_CBP6_RUNLTS_NORV_H

#include <cstdint>

#include "address.h"
#include "modules.h"

class cbp6_runlts_norv : champsim::modules::branch_predictor
{
public:
  using branch_predictor::branch_predictor;

  void initialize_branch_predictor();
  bool predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type, champsim::address_space_id asid);
  void last_branch_result(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
  void branch_predictor_final_stats();
  void branch_execute_resolve(uint64_t instr_id, champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type,
                              champsim::address_space_id asid);

  // The register-value channel. Live only when ChampSim is built with
  // CPPFLAGS=-DCHAMPSIM_TRACE_MEMORY_VALUES=1 and the trace carries memory
  // values; otherwise every execute notification reports "no value" and the
  // predictor runs exactly as it did before. The final-stats line says which.
  void branch_decode_notify(uint64_t instr_id, uint8_t arch_dst_reg);
  void branch_execute_notify(uint64_t instr_id, bool has_value, uint64_t value);
};

#endif
