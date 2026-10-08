/*
 * The ChampSim BTB adapter around the BLBP-style predictor -- ittage_btb.h's
 * pattern, with BLBP replacing ITTAGE on the indirect path and one addition:
 * the conditional-outcome feed for BLBP's global history.
 *
 * Every ITTAGE-audit defect class is designed out from the start:
 *   A. always_taken = true on the ENTIRE indirect path, matching basic_btb --
 *      a conditional aliased into an INDIRECT-typed direct-BTB entry must not
 *      let a "no prediction" here override the direction predictor.
 *   B. The predictor's update() is self-contained (recomputes rows, y_out and
 *      candidates); update-without-predict is pinned identical to
 *      predict-then-update by test/cpp/src/169-blbp-predictor.cc.
 *   -- No sentinel exists to leak: an empty candidate set is std::nullopt and
 *      becomes {0, false} plus always_taken=true only via the documented path
 *      below.
 *   F. Single instance guarded single-core in the shell; construction happens
 *      once (no reinit-after-construct leak pattern).
 *
 * The invariance gate is the acceptance criterion, as with ITTAGE:
 * BRANCH_CONDITIONAL / BRANCH_RETURN / BRANCH_DIRECT_JUMP misses must be
 * bit-identical to basic_btb on every trace before any BLBP number is read.
 */

#ifndef INC_BLBP_BLBP_BTB_H
#define INC_BLBP_BLBP_BTB_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <fmt/core.h>

#include "address.h"
#include "blbp/blbp.h"
#include "champsim.h"
#include "msl/bits.h"
#include "msl/lru_table.h"
#include "runtime_config.h"

namespace champsim::blbp
{
// ---------------------------------------------------------------------------
// Copied verbatim from btb/basic_btb/ as of the campaign. The geometry is
// runtime-settable via `.direct.sets`/`.direct.ways` (see configure_direct
// below) and defaults to the 1024x8 the write-ups pin (same rationale as inc/ittage/ittage_btb.h:
// every module links into every binary, so a second non-inline definition would
// collide; and the comparison is honest only if everything except the indirect
// path is identical).
// ---------------------------------------------------------------------------
struct direct_predictor {
  enum class branch_info {
    INDIRECT,
    RETURN,
    ALWAYS_TAKEN,
    CONDITIONAL,
  };

  static constexpr std::size_t sets = 1024;
  static constexpr std::size_t ways = 8;

  struct btb_entry_t {
    champsim::address ip_tag{};
    champsim::address target{};
    branch_info type = branch_info::ALWAYS_TAKEN;
    champsim::address_space_id asid{};

    auto index() const
    {
      using namespace champsim::data::data_literals;
      return ip_tag.slice_upper<2_b>();
    }
    auto tag() const
    {
      using namespace champsim::data::data_literals;
      return ip_tag.slice_upper<2_b>().to<uint64_t>() ^ champsim::asid_word(asid);
    }
  };

  champsim::msl::lru_table<btb_entry_t> BTB{sets, ways};

  // Runtime-sized, mirroring btb/basic_btb/direct_predictor.h. Runs before any
  // lookup, so rebuilding the empty table loses nothing; the constexpr values
  // above stay the defaults, which is what keeps the recorded comparisons
  // reproducible.
  void resize(std::size_t new_sets, std::size_t new_ways) { BTB = champsim::msl::lru_table<btb_entry_t>{new_sets, new_ways}; }

  std::optional<btb_entry_t> check_hit(champsim::address ip, champsim::address_space_id asid)
  {
    return BTB.check_hit({ip, champsim::address{}, branch_info::ALWAYS_TAKEN, asid});
  }

  void update(champsim::address ip, champsim::address branch_target, uint8_t branch_type, champsim::address_space_id asid)
  {
    auto type = branch_info::ALWAYS_TAKEN;
    if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
      type = branch_info::INDIRECT;
    } else if (branch_type == BRANCH_RETURN) {
      type = branch_info::RETURN;
    } else if (branch_type == BRANCH_CONDITIONAL) {
      type = branch_info::CONDITIONAL;
    }

    auto opt_entry = BTB.check_hit({ip, branch_target, type, asid});
    if (opt_entry.has_value()) {
      opt_entry->type = type;
      if (branch_target != champsim::address{}) {
        opt_entry->target = branch_target;
      }
    }

    if (branch_target != champsim::address{}) {
      BTB.fill(opt_entry.value_or(btb_entry_t{ip, branch_target, type, asid}));
    }
  }
};

struct return_stack {
  static constexpr std::size_t max_size = 64;
  static constexpr std::size_t num_call_size_trackers = 1024;

  std::deque<champsim::address> stack;
  std::array<typename champsim::address::difference_type, num_call_size_trackers> call_size_trackers;

  return_stack() { std::fill(std::begin(call_size_trackers), std::end(call_size_trackers), 4); }

  std::pair<champsim::address, bool> prediction()
  {
    if (std::empty(stack)) {
      return {champsim::address{}, true};
    }
    auto target = stack.back();
    auto size = call_size_trackers[target.slice_lower<champsim::data::bits{champsim::msl::lg2(num_call_size_trackers)}>().to<std::size_t>()];
    return {target + size, true};
  }

  void push(champsim::address ip)
  {
    stack.push_back(ip);
    if (std::size(stack) > max_size) {
      stack.pop_front();
    }
  }

  void calibrate_call_size(champsim::address branch_target)
  {
    if (!std::empty(stack)) {
      auto call_ip = stack.back();
      stack.pop_back();

      static int num_times_returned_backwards = 0;
      if (call_ip > branch_target && num_times_returned_backwards < 10) {
        ++num_times_returned_backwards;
        fmt::print("[BTB] WARNING: target of return is a lower address than the corresponding call. This is usually a problem with your trace.\n");
      }

      auto estimated_call_instr_size = call_ip > branch_target ? champsim::uoffset(branch_target, call_ip) : champsim::uoffset(call_ip, branch_target);
      if (estimated_call_instr_size <= 10) {
        call_size_trackers[call_ip.slice_lower<champsim::data::bits{champsim::msl::lg2(num_call_size_trackers)}>().to<std::size_t>()] =
            estimated_call_instr_size;
      }
    }
  }
};

// ---------------------------------------------------------------------------
// The adapter proper.
// ---------------------------------------------------------------------------
class btb_impl
{
  return_stack ras{};
  direct_predictor direct{};
  predictor blbp_;

public:
  explicit btb_impl(predictor_config cfg) : blbp_(std::move(cfg)) {}

  // The direct predictor's geometry, and the only thing here a configuration can
  // move. The key is `.direct.sets`, not `.sets`: this module's own tables are
  // vendored and compile-time, so a bare `sets` would name the thing that
  // cannot be set.
  //
  // A caveat this cannot enforce: a comparison against another BTB arm is
  // honest only while every arm's direct path is identical. Changing this on one
  // arm and not another measures two things at once. The defaults are the
  // geometry the write-ups pin, so leaving these keys alone is always safe.
  void configure_direct(const champsim::runtime_config& cfg, std::string_view prefix)
  {
    // positive_value: lru_table validates sets itself but never ways, and a
    // zero-way table silently never hits.
    const auto s = cfg.positive_value<std::size_t>(std::string{prefix} + ".direct.sets", direct_predictor::sets);
    const auto w = cfg.positive_value<std::size_t>(std::string{prefix} + ".direct.ways", direct_predictor::ways);
    direct.resize(s, w);
  }

  std::pair<champsim::address, bool> prediction(champsim::address ip, champsim::address_space_id asid)
  {
    auto btb_entry = direct.check_hit(ip, asid);

    // Not known to be a branch: unchanged from basic_btb.
    if (!btb_entry.has_value()) {
      return {champsim::address{}, false};
    }

    if (btb_entry->type == direct_predictor::branch_info::RETURN) {
      return ras.prediction();
    }

    if (btb_entry->type == direct_predictor::branch_info::INDIRECT) {
      // always_taken TRUE whether or not BLBP has candidates (defect-A lesson:
      // this flag overrides the direction predictor for whatever aliases here,
      // and basic_btb reports true unconditionally on this path). The ASID is
      // folded into the PC index exactly as basic_btb folds it into its gshare
      // target cache; in N=1 it is a numeric no-op.
      const auto t = blbp_.predict(ip.to<uint64_t>() ^ champsim::asid_word(asid));
      if (!t.has_value()) {
        return {champsim::address{}, true};
      }
      return {champsim::address{*t}, true};
    }

    return {btb_entry->target, btb_entry->type != direct_predictor::branch_info::CONDITIONAL};
  }

  void update(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid)
  {
    if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
      ras.push(ip);
    }

    if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
      // Self-contained: recomputes everything predict() computed (defect B).
      // The ASID fold must match prediction() exactly.
      blbp_.update(ip.to<uint64_t>() ^ champsim::asid_word(asid), branch_target.to<uint64_t>());
    }

    if (branch_type == BRANCH_CONDITIONAL) {
      // BLBP's 630-bit global history records conditional OUTCOMES -- the same
      // place basic_btb feeds its indirect predictor's direction history. The
      // history is a single shared stream, so it is deliberately NOT folded
      // with the ASID.
      blbp_.note_conditional(taken);
    }

    if (branch_type == BRANCH_RETURN) {
      ras.calibrate_call_size(branch_target);
    }

    direct.update(ip, branch_target, branch_type, asid);
  }
};
} // namespace champsim::blbp

#endif
