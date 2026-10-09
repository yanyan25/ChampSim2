/*
 * Hosts a CBP2025 (CBP6) conditional branch predictor inside ChampSim.
 *
 * The tenant predictor is used unmodified; this class supplies the environment
 * it expects. A tenant must provide the method set the championship's
 * submissions implement against (see cbp2025/cond_branch_predictor_interface.cc):
 *
 *     void setup()
 *     bool predict(uint64_t seq_no, uint8_t piece, uint64_t pc)
 *     void history_update(uint64_t seq_no, uint8_t piece, uint64_t pc, int brtype,
 *                         bool pred_dir, bool resolve_dir, uint64_t next_pc)
 *     void TrackOtherInst(uint64_t pc, int brtype, bool pred_dir,
 *                         bool resolve_dir, uint64_t next_pc)
 *     void update(uint64_t seq_no, uint8_t piece, uint64_t pc, bool resolve_dir,
 *                 bool pred_dir, uint64_t next_pc)
 *     void terminate()
 *
 * STORAGE REQUIREMENT: a host must have static storage duration. CBP6 tenants
 * routinely rely on zero-initialization that their own constructors do not
 * perform -- CBP2016 TAGE-SC-L's cbp_hist_t initializes only two of its members
 * and leaves IMLIcount indeterminate, and because its constructor is
 * user-provided, value-initialising the host with {} does not zero it either.
 * Static storage is zero-initialized before constructors run; an automatic or
 * heap-allocated host is undefined behaviour and crashes on the first
 * prediction. Tenant modules should hold their host in a function-local static.
 *
 * Timing note: CBP6 splits the update in two -- speculative history right after
 * the prediction (spec_update, cbp2025/lib/bp.cc:112) and the non-speculative
 * table update at the branch's execute cycle, out of program order
 * (cbp2025/lib/uarchsim.cc:352). ChampSim has a single resolution hook, fired at
 * fetch in program order (src/ooo_cpu.cc:166), so the two collapse into one
 * call here. Every branch therefore predicts against tables its immediate
 * predecessor has already updated, which is more favourable than the CBP6
 * environment the tenant was tuned in. Restoring the split needs an
 * execute-time hook in O3_CPU.
 */

#ifndef CBP6_HOST_H
#define CBP6_HOST_H

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <unordered_map>

#include "util/detect.h"
#include <string>

#include <cstdio>

#include <fmt/core.h>

#include "address.h"
#include "cbp6/cbp6_protocol.h"
#include "cbp6/cbp6_register_map.h"
#include "cbp6/cbp6_types.h"

namespace champsim::cbp6
{
// Deferring the table update to execute is a fidelity experiment, not a
// production setting, so it is opt-in and reported in the final stats.
inline bool delayed_update_from_env()
{
  // NOLINTNEXTLINE(concurrency-mt-unsafe): called once, during initialization
  const char* v = std::getenv("CBP6_DELAYED_UPDATE");
  return v != nullptr && *v != '\0' && *v != '0';
}

template <typename Tenant>
class host
{
  Tenant tenant_{};

  // ChampSim's instr_id is not passed to branch hooks, but predict and resolve
  // fire back to back for the same instruction inside do_predict_branch, so a
  // local counter identifies a dynamic branch just as well -- and avoids
  // reaching into the core's instruction queue to read one.
  uint64_t seq_no_{0};
  bool last_prediction_{false};

  // Validates the CBP6 call protocol over a whole run. Null unless
  // CBP6_PROTOCOL_CHECK is set; see cbp6_protocol.h.
  std::unique_ptr<protocol_checker> checker_{checker_from_env()};
  std::unique_ptr<call_log_writer> log_{writer_from_env()};
  bool reported_{false};

  // ChampSim's headline MPKI counts a miss when the BTB target is wrong OR the
  // direction is wrong, across every branch type (src/ooo_cpu.cc:151-153), so it
  // conflates the BTB and the direction predictor. CBP6's BrMisPKI counts
  // conditional-direction misses only, with perfect indirect prediction. These
  // count what the tenant is actually responsible for.
  uint64_t roi_conditional_{0};
  uint64_t roi_direction_misses_{0};

  // CBP6 performs the non-speculative table update at the branch's execute
  // cycle, out of program order (cbp2025/lib/uarchsim.cc:352); ChampSim resolves
  // at fetch, so by default the two collapse into one in-order call and every
  // branch predicts against tables its predecessor already updated -- more
  // favourable than the environment the tenant was tuned in. With
  // CBP6_DELAYED_UPDATE=1 the update is deferred to the execute-resolve hook,
  // restoring both the delay and the out-of-orderness. Off by default so the
  // other ChampSim predictors remain comparable.
  struct pending_update {
    uint64_t seq_no;
    uint64_t pc;
    bool pred_dir;
    bool resolve_dir;
    uint64_t next_pc;
  };
  std::unordered_map<uint64_t, pending_update> in_flight_;

  // RUNLTS and its relatives correlate on the values recently written to
  // architectural registers. ChampSim supplies them in two steps: a decode
  // notification marks a register's value unknown, and an execute notification
  // delivers it when knowable. The architectural register number has to be
  // remembered between the two, because scheduling rewrites
  // ooo_model_instr::destination_registers with physical IDs in place
  // (O3_CPU::do_scheduling) before execute is reached.
  std::unordered_map<uint64_t, unsigned> pending_reg_;
  uint64_t registers_marked_{0};
  uint64_t values_delivered_{0};

  template <typename U>
  using tenant_takes_decode_notify = decltype(std::declval<U>().decode_notify(uint64_t{}, uint8_t{}, uint64_t{}));
  template <typename U>
  using tenant_takes_execute_notify = decltype(std::declval<U>().execute_notify(uint64_t{}, uint8_t{}, uint64_t{}, uint64_t{}));
  bool delayed_{delayed_update_from_env()};

  struct checker_mode_init {
    explicit checker_mode_init(protocol_checker* c, bool delayed)
    {
      if (c != nullptr) {
        c->set_delayed(delayed);
      }
    }
  };
  checker_mode_init checker_mode_{checker_.get(), delayed_};

  void note(call_kind kind, int brtype, bool pred_dir, bool resolve_dir, uint64_t seq_no, uint64_t pc, uint64_t next_pc)
  {
    if (checker_ == nullptr && log_ == nullptr) {
      return;
    }
    const call_record rec{kind, brtype, pred_dir, resolve_dir, seq_no, pc, next_pc};
    if (checker_ != nullptr) {
      checker_->observe(rec);
    }
    if (log_ != nullptr) {
      log_->write(rec);
    }
  }

public:
  [[nodiscard]] Tenant& tenant() { return tenant_; }
  [[nodiscard]] const Tenant& tenant() const { return tenant_; }

  void initialize() { tenant_.setup(); }
  // finish() is normally driven by ChampSim's branch-predictor final-stats hook
  // (src/main.cc). Reporting from the destructor as well covers the paths that
  // hook does not reach -- an early exit, or a unit test -- so a validation run
  // always prints its verdict. Silence would otherwise be indistinguishable
  // from a clean run. report_protocol_check() is idempotent.
  ~host() { report_protocol_check(); }

  host() = default;
  host(const host&) = delete;
  host& operator=(const host&) = delete;
  host(host&&) = delete;
  host& operator=(host&&) = delete;

  void report_protocol_check()
  {
    if (checker_ == nullptr || reported_) {
      return;
    }
    reported_ = true;

    const bool ok = checker_->finish();
    fmt::print("\n*** CBP6 protocol check: {} records, {} conditional branches, {} other branches -> {}\n", checker_->records(),
               checker_->conditional_branches(), checker_->other_branches(), ok ? "OK" : "VIOLATIONS");
    // Only the first few matter; a broken protocol repeats its mistake.
    std::size_t shown = 0;
    for (const auto& v : checker_->violations()) {
      fmt::print("    violation: {}\n", v);
      if (++shown >= 10) {
        fmt::print("    ... and {} more\n", std::size(checker_->violations()) - shown);
        break;
      }
    }
    std::fflush(stdout);
  }

  [[nodiscard]] bool delayed_update() const { return delayed_; }

  [[nodiscard]] std::size_t pending_register_writes() const { return std::size(pending_reg_); }

  // Called at decode for each architectural destination register.
  void decode_notify(uint64_t instr_id, uint8_t arch_dst_reg)
  {
    if constexpr (champsim::is_detected_v<tenant_takes_decode_notify, Tenant>) {
      const auto slot = cbp6_register(arch_dst_reg);
      if (!slot.has_value()) {
        return; // not a data register, or a number the mapping does not know
      }

      tenant_.decode_notify(instr_id, 0, *slot);
      pending_reg_.insert_or_assign(instr_id, *slot);
      ++registers_marked_;
    }
  }

  // Called at execute completion. `has_value` is false whenever the value is not
  // in the trace -- an ALU result, for instance -- in which case the register
  // simply stays marked unknown from the decode notification.
  void execute_notify(uint64_t instr_id, bool has_value, uint64_t value)
  {
    if constexpr (champsim::is_detected_v<tenant_takes_execute_notify, Tenant>) {
      auto found = pending_reg_.find(instr_id);
      if (found == std::end(pending_reg_)) {
        return;
      }

      if (has_value) {
        tenant_.execute_notify(instr_id, 0, found->second, value);
        ++values_delivered_;
      }
      pending_reg_.erase(found);
    }
  }

  // Called when a branch completes execution, out of program order. Only does
  // anything in delayed mode.
  void execute_resolve(uint64_t instr_id)
  {
    auto found = in_flight_.find(instr_id);
    if (found == std::end(in_flight_)) {
      return; // not a conditional branch, or not in delayed mode
    }

    const auto& p = found->second;
    tenant_.update(p.seq_no, 0, p.pc, p.resolve_dir, p.pred_dir, p.next_pc);
    note(call_kind::update, BRTYPE_COND, p.pred_dir, p.resolve_dir, p.seq_no, p.pc, p.next_pc);
    in_flight_.erase(found);
  }

  // Called from ChampSim's branch-predictor final-stats hook.
  void finish(uint64_t roi_instructions)
  {
    tenant_.terminate();

    const double per_ki = (roi_instructions > 0) ? 1000.0 * static_cast<double>(roi_direction_misses_) / static_cast<double>(roi_instructions) : 0.0;
    const double rate = (roi_conditional_ > 0) ? 100.0 * static_cast<double>(roi_direction_misses_) / static_cast<double>(roi_conditional_) : 0.0;

    fmt::print("\n*** CBP6 conditional-direction only (excludes BTB/RAS target misses)\n");
    fmt::print("    conditional branches: {}\n", roi_conditional_);
    fmt::print("    direction mispredicts: {}  ({:.4g}% of conditionals)\n", roi_direction_misses_, rate);
    fmt::print("    direction MPKI: {:.4g}\n", per_ki);
    fmt::print("    update timing: {}\n", delayed_ ? "delayed to execute (CBP6-like)" : "immediate at fetch (ChampSim default)");

    // Whether the register-value channel was actually live. A dead channel is
    // otherwise invisible: the predictor keeps working and silently stops using
    // the feature it was built around.
    if (registers_marked_ > 0) {
      fmt::print("    register-value channel: {} register writes seen, {} values delivered ({:.4g}%)\n", registers_marked_, values_delivered_,
                 100.0 * static_cast<double>(values_delivered_) / static_cast<double>(registers_marked_));
    } else {
      fmt::print("    register-value channel: INERT (no register writes reported; predictor is running without it)\n");
    }
    if (!in_flight_.empty()) {
      fmt::print("    WARNING: {} branches still in flight at end of run\n", std::size(in_flight_));
    }
    if (log_ != nullptr && log_->exhausted()) {
      fmt::print("    WARNING: call log hit CBP6_CALL_DUMP_MAX and is TRUNCATED; a replay of it proves nothing about the rest of the run\n");
    }

    report_protocol_check();
  }

  // Returns the direction to report to ChampSim.
  bool predict(champsim::address ip, uint8_t branch_type)
  {
    if (!is_direction_predicted(branch_type)) {
      // Matches CBP6, which asks the tenant only about conditional branches and
      // forces pred_taken = true for every other class (lib/bp.cc:128,164).
      // Skipping the call also matters for correctness: tenants checkpoint
      // history on every predict() and only release it in update(), which runs
      // for conditional branches alone.
      return true;
    }

    ++seq_no_;
    last_prediction_ = tenant_.predict(seq_no_, 0, ip.to<uint64_t>());
    note(call_kind::predict, BRTYPE_COND, last_prediction_, false, seq_no_, ip.to<uint64_t>(), 0);
    return last_prediction_;
  }

  // `next_pc` must be the architectural next PC: the branch target when taken,
  // the fall-through when not. Passing ChampSim's branch_target directly is
  // wrong for a not-taken branch, where it is zero -- tenants test `nextPC < PC`
  // to detect backward (loop) branches, so zero makes every not-taken
  // conditional look like a loop.
  // `in_roi` excludes the warmup phase, so the counters match the window
  // ChampSim reports its own statistics over.
  void resolve(champsim::address ip, bool taken, uint8_t branch_type, champsim::address next_pc, bool in_roi = true, uint64_t instr_id = 0)
  {
    if (!is_branch(branch_type)) {
      return;
    }

    if (in_roi && is_direction_predicted(branch_type)) {
      ++roi_conditional_;
      if (last_prediction_ != taken) {
        ++roi_direction_misses_;
      }
    }

    const auto pc = ip.to<uint64_t>();
    const auto npc = next_pc.to<uint64_t>();
    const int brtype = brtype_of(branch_type);

    if (is_direction_predicted(branch_type)) {
      // Speculative history always happens here, matching CBP6's spec_update.
      tenant_.history_update(seq_no_, 0, pc, brtype, last_prediction_, taken, npc);
      note(call_kind::history_update, brtype, last_prediction_, taken, seq_no_, pc, npc);

      if (delayed_) {
        in_flight_.emplace(instr_id, pending_update{seq_no_, pc, last_prediction_, taken, npc});
      } else {
        tenant_.update(seq_no_, 0, pc, taken, last_prediction_, npc);
        note(call_kind::update, brtype, last_prediction_, taken, seq_no_, pc, npc);
      }
    } else {
      tenant_.TrackOtherInst(pc, brtype, true, taken, npc);
      note(call_kind::track_other, brtype, true, taken, 0, pc, npc);
    }
  }
};
} // namespace champsim::cbp6

#endif
