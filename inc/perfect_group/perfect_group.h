/*
 * Per-GROUP perfect-target oracles, for decomposing target headroom by the
 * microarchitectural structure that supplies the target.
 *
 * ChampSim routes every branch's target to exactly one of three structures
 * (btb/basic_btb/basic_btb.cc):
 *
 *   group-BTB   CONDITIONAL, DIRECT_JUMP, DIRECT_CALL   -> the BTB's stored target
 *   group-IBTB  INDIRECT, INDIRECT_CALL                 -> the indirect predictor
 *   group-RAS   RETURN                                  -> the return address stack
 *
 * Each oracle here gives the true target to ONE group and leaves the other two
 * entirely real, so the three are mutually exclusive and their headrooms can be
 * added. btb/perfect_indirect is the group-IBTB member of this family and
 * predates it; these two complete the set.
 *
 * ALL THREE PRESERVE BTB GATING, and that is the load-bearing design decision.
 * `direct.check_hit(ip)` runs before any routing, and on a miss basic_btb
 * returns "no prediction" for every group -- the BTB does not merely store
 * targets, it is also what tells the frontend that a PC is a branch at all and
 * what kind. An indirect branch that misses the BTB never reaches the indirect
 * predictor; a return that misses never reaches the RAS.
 *
 * The consequence is that these three do NOT sum to btb/perfect_btb, which has
 * no BTB whatsoever (it ignores the IP and returns the trace's target, so no
 * detection miss is possible). The difference is the BTB's DETECTION role:
 *
 *     perfect_btb = perfect_direct + perfect_indirect + perfect_return
 *                   + (branches the BTB never identified as branches)
 *
 * That residual is a real, separately interesting quantity -- it is what a
 * decoupled frontend's fetch-directed prefetching would attack -- and it is
 * measured by subtraction rather than assumed to be zero.
 *
 * ALIASING GUARD. The BTB indexes and tags on ip>>2 (direct_predictor.h), so two
 * branches within 3 bytes share an entry and a branch of the WRONG type can land
 * on a group's path. An earlier version of btb/perfect_indirect handed the true
 * target to whatever aliased in, which cut SPEC direct-jump MPKI from 0.1496 to
 * 0.0909 and inflated the very denominator the oracle exists to define. Each
 * oracle below therefore checks the instruction's REAL branch type before
 * answering, and otherwise behaves exactly as basic_btb would.
 */

#ifndef INC_PERFECT_GROUP_PERFECT_GROUP_H
#define INC_PERFECT_GROUP_PERFECT_GROUP_H

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <fmt/core.h>

#include "address.h"
#include "champsim.h"
#include "instruction.h"
#include "msl/bits.h"
#include "msl/lru_table.h"

namespace champsim::perfect_group
{
// Which structure this oracle idealises. Exactly one per binary.
//
// `ideal_btb` is not a fourth group -- it is group-BTB's OTHER role. Measured
// on rubocop_w1, giving group-BTB perfect TARGETS is nearly worthless: direct
// jumps fall only 0.3668 -> 0.3552 MPKI where perfect_btb takes them to zero,
// because a direct branch's target never changes and the BTB's stored copy is
// already correct whenever the entry is present. What remains is the DETECTION
// role: entries that are not there at all. `ideal_btb` removes exactly that --
// it never misses, always knows the branch type, and has the right direct
// target -- while the REAL indirect predictor and REAL RAS still answer for
// their own groups. It therefore overlaps the other oracles by construction
// (un-gating hands them opportunities they did not previously get) and is
// reported alongside the decomposition, not as a term in it.
// The last two are the CUMULATIVE series: ideal_btb, then +perfect IBTB, then
// +perfect RAS. Nested rather than exclusive, so the increments telescope --
//     (s1-1) + (s2-s1) + (s3-s2) = s3-1
// which makes a stacked speedup chart legitimate where an exclusive split could
// not be drawn in speedup space at all. The last step should reproduce
// btb/perfect_btb exactly: detection perfect, direct targets from an infinite
// BTB (they never change), indirect perfect, returns perfect -- every target in
// the machine correct. That equality is the series' own correctness check.
//
// Attribution is ORDER-DEPENDENT: putting the BTB first credits the later steps
// with the un-gating benefit. Defensible because detection is genuinely prior in
// hardware -- it gates the other two -- but it must be stated, not assumed.
enum class group { direct_btb, ras, ideal_btb, ideal_ibtb, ideal_ibtb_ras };

// ---------------------------------------------------------------------------
// Copied verbatim from btb/basic_btb/ as of the campaign, made inline and
// namespaced (basic_btb has since gained a runtime-geometry resize() this
// shell deliberately lacks). ChampSim
// compiles every module into every binary, so a second non-inline definition
// would collide at link time with basic_btb's. Unmodified: an oracle must
// differ from basic_btb ONLY on its own group.
// ---------------------------------------------------------------------------
struct direct_predictor {
  enum class branch_info { INDIRECT, RETURN, ALWAYS_TAKEN, CONDITIONAL };

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

struct indirect_predictor {
  static constexpr std::size_t size = 4096;
  std::array<champsim::address, size> predictor = {};
  std::bitset<champsim::msl::lg2(size)> conditional_history = {};

  std::pair<champsim::address, bool> prediction(champsim::address ip, champsim::address_space_id asid)
  {
    using namespace champsim::data::data_literals;
    auto hash = ip.slice_upper<2_b>().to<unsigned long long>() ^ conditional_history.to_ullong() ^ champsim::asid_word(asid);
    return {predictor[hash % std::size(predictor)], true};
  }
  void update_target(champsim::address ip, champsim::address branch_target, champsim::address_space_id asid)
  {
    using namespace champsim::data::data_literals;
    auto hash = ip.slice_upper<2_b>().to<unsigned long long>() ^ conditional_history.to_ullong() ^ champsim::asid_word(asid);
    predictor[hash % std::size(predictor)] = branch_target;
  }
  void update_direction(bool taken)
  {
    conditional_history <<= 1;
    conditional_history.set(0, taken);
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

// Does this instruction genuinely belong to the oracle's group? Checked against
// the instruction's own branch type, never against the BTB entry's, because the
// entry may have been written by an aliasing neighbour.
template <group G>
[[nodiscard]] inline bool in_group(uint8_t branch_type)
{
  if constexpr (G == group::direct_btb) {
    return branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_DIRECT_JUMP || branch_type == BRANCH_DIRECT_CALL;
  } else {
    return branch_type == BRANCH_RETURN;
  }
}

// ---------------------------------------------------------------------------
// The oracle proper.
// ---------------------------------------------------------------------------
template <group G>
class btb_impl
{
  return_stack ras{};
  direct_predictor direct{};
  indirect_predictor indirect{};

public:
  std::pair<champsim::address, bool> prediction(champsim::address ip, const O3_CPU* intern, champsim::address_space_id asid)
  {
    if constexpr (G == group::ideal_btb || G == group::ideal_ibtb || G == group::ideal_ibtb_ras) {
      // A BTB that never misses. Detection, type and the direct target come for
      // free. What answers for the other two groups depends on how far along the
      // cumulative series this configuration sits.
      const ooo_model_instr* in = (intern != nullptr && !std::empty(intern->ctx().input_queue)) ? &intern->ctx().input_queue.front() : nullptr;
      if (in == nullptr || in->branch == NOT_BRANCH) {
        return {champsim::address{}, false};
      }
      if (in->branch == BRANCH_RETURN) {
        if constexpr (G == group::ideal_ibtb_ras) {
          return {in->branch_target, true}; // always_taken as basic_btb's RAS path reports it
        }
        return ras.prediction();
      }
      if (in->branch == BRANCH_INDIRECT || in->branch == BRANCH_INDIRECT_CALL) {
        if constexpr (G == group::ideal_ibtb || G == group::ideal_ibtb_ras) {
          // With detection already perfect there is no BTB miss to hide behind,
          // so indirect MPKI must come out EXACTLY zero here. The gated
          // btb/perfect_indirect could not reach zero for precisely that reason.
          return {in->branch_target, true};
        }
        return indirect.prediction(ip, asid);
      }
      // CONDITIONAL keeps always_taken = false so the direction predictor still
      // decides; the unconditional direct classes report true, as basic_btb does.
      return {in->branch_target, in->branch != BRANCH_CONDITIONAL};
    }

    auto btb_entry = direct.check_hit(ip, asid);

    // BTB gating, deliberately preserved -- see the file header. Overriding it
    // would fold the BTB's DETECTION role into whichever group this oracle
    // idealises, and the three groups would no longer be mutually exclusive.
    if (!btb_entry.has_value()) {
      return {champsim::address{}, false};
    }

    // The instruction under prediction is input_queue.front(); it is popped only
    // after do_init_instruction returns (src/ooo_cpu.cc:100-104).
    const ooo_model_instr* instr = (intern != nullptr && !std::empty(intern->ctx().input_queue)) ? &intern->ctx().input_queue.front() : nullptr;
    const bool oracle_applies = instr != nullptr && in_group<G>(instr->branch);

    if (btb_entry->type == direct_predictor::branch_info::RETURN) {
      if constexpr (G == group::ras) {
        // always_taken = true, matching basic_btb's RAS path.
        if (oracle_applies) {
          return {instr->branch_target, true};
        }
      }
      return ras.prediction();
    }

    if (btb_entry->type == direct_predictor::branch_info::INDIRECT) {
      // group-IBTB is never this oracle's group; btb/perfect_indirect covers it.
      return indirect.prediction(ip, asid);
    }

    // CONDITIONAL or ALWAYS_TAKEN -- group-BTB.
    if constexpr (G == group::direct_btb) {
      if (oracle_applies) {
        // always_taken exactly as basic_btb reports it, so a correctly-predicted
        // not-taken conditional is not forced taken and charged a miss. A
        // not-taken conditional carries branch_target == 0, which is precisely
        // what ChampSim compares against once it zeroes the predicted target.
        return {instr->branch_target, btb_entry->type != direct_predictor::branch_info::CONDITIONAL};
      }
    }
    return {btb_entry->target, btb_entry->type != direct_predictor::branch_info::CONDITIONAL};
  }

  // Every real structure is trained exactly as basic_btb trains it: the two
  // groups this oracle does NOT idealise must behave identically to baseline.
  void update(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid)
  {
    if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
      ras.push(ip);
    }
    if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
      indirect.update_target(ip, branch_target, asid);
    }
    if (branch_type == BRANCH_CONDITIONAL) {
      indirect.update_direction(taken);
    }
    if (branch_type == BRANCH_RETURN) {
      ras.calibrate_call_size(branch_target);
    }
    direct.update(ip, branch_target, branch_type, asid);
  }
};
} // namespace champsim::perfect_group

#endif
