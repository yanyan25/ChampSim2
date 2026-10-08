/*
 * The ChampSim BTB adapter around the vendored ITTAGE predictor.
 *
 * Shared by every ITTAGE configuration (btb/ittage_32kb, btb/ittage_64kb, ...)
 * so that the adapter -- the part that is ours and therefore the part that can
 * be wrong -- exists exactly once and is audited once. A configuration module
 * is a ~30-line shell that names a CFG and forwards.
 *
 * Structurally identical to basic_btb: the same direct predictor for
 * presence/type/direct targets and the same return stack, with ITTAGE replacing
 * the 4096-entry tagless gshare target cache on the indirect path. Those two
 * helpers are copied from btb/basic_btb/ and marked below; they are inline here
 * because ChampSim compiles every module into every binary, so a second
 * non-inline definition of `direct_predictor` would collide at link time with
 * basic_btb's.
 *
 * FOUR FAILURE MODES, each of which degrades accuracy SILENTLY. Each has a
 * guard here and a test in test/cpp/src/. See
 * docs/superpowers/specs/2026-08-09-ittage-design.md section 4.
 *
 *   1. The 0xdeadbeef sentinel. ientry() initialises target = 0xdeadbeef, so an
 *      unseeded entry that happens to tag-match would hand ChampSim a garbage
 *      target -- which does not announce itself, it merely mispredicts. Treated
 *      as "no prediction" here. Deliberately NOT falling back to the direct
 *      predictor's stale target: ITTAGE's measured accuracy must not borrow
 *      from the BTB.
 *   2. A not-taken branch carries branch_target == 0 (src/tracereader.cc), and
 *      feeding that to the history injects a constant into every subsequent
 *      index. The caller passes the architectural next PC instead.
 *   3. Initialisation. The vendored reinit() is called from initialize(), and
 *      the copy fixes the reference's uninitialised-ghist bug, but the predictor
 *      is still held at static storage duration by the module shell, matching
 *      the CBP6 tenants.
 *   4. Direct-BTB gating. ITTAGE is only consulted when the direct predictor
 *      already knows this PC is an indirect branch. Measured on
 *      710.omnetpp_r.sp0: a perfect indirect oracle still leaves 3.8% of
 *      INDIRECT and 34% of INDIRECT_CALL mispredicts standing, because those
 *      branches missed the 8K direct BTB. Deliberately not papered over -- that
 *      ceiling is real and btb/perfect_indirect measures it.
 */

#ifndef INC_ITTAGE_ITTAGE_BTB_H
#define INC_ITTAGE_ITTAGE_BTB_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <fmt/core.h>

#include "address.h"
#include "champsim.h"
#include "ittage/ittage.hpp"
#include "msl/bits.h"
#include "msl/lru_table.h"
#include "runtime_config.h"

namespace champsim::ittage
{
// ---------------------------------------------------------------------------
// Copied verbatim from btb/basic_btb/direct_predictor.{h,cc} as of the
// campaign, made inline and namespaced. The geometry is runtime-settable via
// `.direct.sets`/`.direct.ways` (see configure_direct below) and defaults to the
// 1024x8 the write-ups pin. Otherwise unmodified: an ITTAGE configuration must
// differ from basic_btb only in the indirect path, or the comparison measures
// two things at once.
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

// ---------------------------------------------------------------------------
// Copied verbatim from btb/basic_btb/return_stack.{h,cc}, made inline and
// namespaced. Returns are handled by the RAS, not by ITTAGE.
// ---------------------------------------------------------------------------
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

// The sentinel the vendored ientry() constructor writes into an unseeded entry.
inline constexpr uint64_t unseeded_target_sentinel = 0xdeadbeefULL;

// ---------------------------------------------------------------------------
// The adapter proper.
// ---------------------------------------------------------------------------
template <typename CFG>
class btb_impl
{
  return_stack ras{};
  direct_predictor direct{};
  champsim_ittage::IPREDICTOR<CFG> predictor{};

public:
  // Storage accounting, asserted rather than asserted-in-prose. An entry holds a
  // truncated target plus the tag, confidence and useful bits; the uint64_t the
  // simulator actually stores is a simulation artifact and is excluded. 47 bits
  // is measured, not assumed: the widest instruction address observed across
  // 723.llvm_r.sp1, 714.cpython_r.sp0 and swe_agent_w00001 is 0x7ffff7fdc84e.
  static constexpr int target_bits = 48;
  static constexpr int entry_bits = target_bits + CFG::CWIDTH + CFG::TBITS + CFG::UWIDTH;
  static constexpr long storage_bits = static_cast<long>(CFG::NHIST + 1) * (1L << CFG::LOGG) * entry_bits;
  static constexpr double storage_kb = storage_bits / 8.0 / 1024.0;

  // Deliberately does NOT call predictor.reinit(): the vendored constructor
  // already calls it (ittage.hpp), and reinit() does `itable[i] = new ientry[..]`
  // for every table, so calling it a second time leaks the constructor's
  // allocation (96 KB / 192 KB depending on configuration, ASan-confirmed).
  void initialize() {}

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

  // Verification surface, not a functional interface. The adapter's contract
  // with the predictor -- notably that it forwards the ARCHITECTURAL next PC
  // rather than ChampSim's zero-for-not-taken branch_target -- is only
  // observable in predictor state, because a fresh predictor answers "no
  // prediction" regardless of what history it was fed. Used by
  // test/cpp/src/167-ittage-btb.cc and by the protocol checker.
  const champsim_ittage::IPREDICTOR<CFG>& predictor_state() const { return predictor; }

  std::pair<champsim::address, bool> prediction(champsim::address ip, champsim::address_space_id asid)
  {
    auto btb_entry = direct.check_hit(ip, asid);

    // Not known to be a branch. Unchanged from basic_btb: ITTAGE replaces the
    // indirect target predictor, not the BTB's knowledge that a PC is a branch.
    if (!btb_entry.has_value()) {
      return {champsim::address{}, false};
    }

    if (btb_entry->type == direct_predictor::branch_info::RETURN) {
      return ras.prediction();
    }

    if (btb_entry->type == direct_predictor::branch_info::INDIRECT) {
      // The ASID is folded into the PC index exactly as basic_btb folds it into
      // its gshare target cache (btb/basic_btb/indirect_predictor.cc): two
      // address spaces never share an indirect-target entry. In N=1 asid is
      // {0,0}, so the fold is a numeric no-op.
      const uint64_t target = predictor.GetPrediction(ip.to<uint64_t>() ^ champsim::asid_word(asid));
      // Failure mode 1: never let an unseeded entry's sentinel escape as a real
      // target. Reporting "no prediction" takes the misprediction honestly.
      if (target == unseeded_target_sentinel || target == 0) {
        // always_taken must be TRUE here, matching basic_btb's indirect path
        // (btb/basic_btb/indirect_predictor.cc:7) and perfect_indirect.
        //
        // It is not part of the target prediction: ChampSim computes
        // `branch_prediction = impl_predict_branch(...) || always_taken`
        // (src/ooo_cpu.cc:140), so the flag OVERRIDES the direction predictor.
        // Returning false here looked harmless -- for a genuine indirect branch
        // the cbp6 host already answers true -- but the direct BTB indexes AND
        // tags on ip>>2 (btb/basic_btb/direct_predictor.h:30,35), so two branches
        // within 3 bytes share an entry. A CONDITIONAL aliased into an
        // INDIRECT-typed entry then reached this path, and returning false let
        // TAGE-SC-L's correct not-taken stand where basic_btb was forced taken.
        // That handed ITTAGE ~4470 conditional mispredicts per 10M instructions
        // that the baseline ate -- a measurement artifact, not a prediction win.
        return {champsim::address{}, true};
      }
      return {champsim::address{target}, true};
    }

    return {btb_entry->target, btb_entry->type != direct_predictor::branch_info::CONDITIONAL};
  }

  // `next_pc` must be the ARCHITECTURAL next PC -- the target when taken, the
  // fall-through when not. Failure mode 2: ChampSim's branch_target is zero for
  // a not-taken branch, and feeding zero into the path history corrupts every
  // subsequent index and tag.
  void update(champsim::address ip, champsim::address branch_target, uint8_t branch_type, champsim::address next_pc, champsim::address_space_id asid)
  {
    if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
      ras.push(ip);
    }

    if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
      // UpdatePredictor consumes seven members that ONLY GetPrediction writes
      // (HitBank, AltBank, GI[], GTAG[], LongestMatchPred, alt_target,
      // tage_target). prediction() calls GetPrediction only when the direct BTB
      // hits and types the PC as indirect, but this arm runs for EVERY indirect
      // branch -- so on a direct-BTB miss the update would allocate, decrement
      // and overwrite entries indexed and tagged for the PREVIOUS indirect
      // branch, using this branch's target. Seznec's own driver always pairs
      // them (cbp2025/lib/bp.cc:151-157).
      //
      // Re-calling here is idempotent: GetPrediction only reads the tables and
      // recomputes those members, and nothing mutates predictor history between
      // ChampSim's prediction and update hooks for the same instruction. The
      // ASID fold must match prediction() exactly, or the two would address
      // different entries.
      (void)predictor.GetPrediction(ip.to<uint64_t>() ^ champsim::asid_word(asid));
      predictor.UpdatePredictor(ip.to<uint64_t>() ^ champsim::asid_word(asid), branch_target.to<uint64_t>());
    } else {
      // History only. Omitting this costs most of the paper's section 2.1
      // benefit (~16%) and does so silently, because the predictor still works.
      // The path history is a single shared stream, not a per-IP table, so it
      // is deliberately NOT folded with the ASID (same rule as gshare's GHR).
      predictor.TrackOtherInst(ip.to<uint64_t>(), next_pc.to<uint64_t>());
    }

    if (branch_type == BRANCH_RETURN) {
      ras.calibrate_call_size(branch_target);
    }

    direct.update(ip, branch_target, branch_type, asid);
  }
};
} // namespace champsim::ittage

#endif
