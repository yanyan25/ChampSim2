#ifndef CORE_CONTEXT_H
#define CORE_CONTEXT_H

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <queue>
#include <vector>

#include "core_stats.h"
#include "instruction.h"
#include "register_allocator.h"
#include "util/lru_table.h"

// Per-hardware-context (per-thread) pipeline state.
//
// A core_context holds everything a single hardware thread owns privately: its
// own frontend buffers, its own reorder buffer, its own load/store queues, its
// own register alias tables, and its own branch predictor state. A physical
// core with N hardware contexts will hold N core_context objects. Everything
// the threads share (L1 caches, the physical register file and its free list,
// bandwidth and capacity constants) stays on the core.
//
// Stage 1 has exactly one context per core and does not change the order of
// operations, so single-context results remain bit-identical.

struct LSQ_ENTRY : champsim::program_ordered<LSQ_ENTRY> {
  champsim::address virtual_address{};
  champsim::address ip{};
  champsim::chrono::clock::time_point ready_time{champsim::chrono::clock::time_point::max()};

  std::array<uint8_t, 2> asid = {std::numeric_limits<uint8_t>::max(), std::numeric_limits<uint8_t>::max()};
  bool fetch_issued = false;

  uint64_t producer_id = std::numeric_limits<uint64_t>::max();
  std::vector<std::reference_wrapper<std::optional<LSQ_ENTRY>>> lq_depend_on_me{};

  // Unique token naming this LQ entry as a load waiter. instr_id is one-to-many
  // with LQ entries (an instruction with several source-memory operands allocates
  // one entry each), so it cannot name a single waiter. The core assigns this
  // monotonically at allocation and echoes it through the memory system in
  // channel::request/response.load_waiters.
  uint64_t load_waiter_id = std::numeric_limits<uint64_t>::max();

  LSQ_ENTRY(champsim::address addr, champsim::program_ordered<LSQ_ENTRY>::id_type id, champsim::address ip, std::array<uint8_t, 2> asid);
  void finish(ooo_model_instr& rob_entry) const;
  void finish(std::deque<ooo_model_instr>::iterator begin, std::deque<ooo_model_instr>::iterator end) const;
};

namespace champsim
{

struct dib_shift {
  champsim::data::bits shamt;
  auto operator()(champsim::address val) const { return val.slice_upper(shamt); }
};
using dib_type = champsim::lru_table<champsim::address, dib_shift, dib_shift>;

struct core_context {
  using stats_type = cpu_stats;

  // Frontend and backend instruction buffers.
  std::deque<ooo_model_instr> IFETCH_BUFFER;
  std::deque<ooo_model_instr> DISPATCH_BUFFER;
  std::deque<ooo_model_instr> DECODE_BUFFER;
  std::deque<ooo_model_instr> ROB;
  std::deque<ooo_model_instr> DIB_HIT_BUFFER;
  std::deque<ooo_model_instr> input_queue;

  std::vector<std::optional<LSQ_ENTRY>> LQ;
  std::deque<LSQ_ENTRY> SQ;

  // Monotonic source of the per-LQ-entry load_waiter_id token.
  uint64_t next_load_waiter_id = 0;

  dib_type DIB;

  // Per-context register-rename state: the architectural alias tables. The
  // physical register file and its free list are shared and stay on the core
  // (RegisterAllocator); this context holds only a pointer to them. A rename
  // therefore reads and writes this pool through reg_allocator, which the core
  // guarantees outlives every context.
  std::array<PHYSICAL_REGISTER_ID, std::numeric_limits<uint8_t>::max() + 1> frontend_RAT, backend_RAT;
  RegisterAllocator* reg_allocator = nullptr;

  // Rename methods that read or write the per-context RATs. They are moved here
  // line-for-line from RegisterAllocator; the physical-register pool they touch
  // is the shared reg_allocator above.
  PHYSICAL_REGISTER_ID rename_dest_register(int16_t reg, champsim::program_ordered<ooo_model_instr>::id_type producer_id);
  PHYSICAL_REGISTER_ID rename_src_register(int16_t reg);
  void retire_dest_register(PHYSICAL_REGISTER_ID physreg);
  void reset_frontend_RAT();
  bool isAllocated(PHYSICAL_REGISTER_ID archreg) const;

  // The RAT half of the deadlock printer; RegisterAllocator::print_deadlock()
  // prints the physical-file half. Called back-to-back in that order by
  // O3_CPU::print_deadlock() so the combined output matches the original.
  void print_deadlock();

  // Fetch freeze bookkeeping for this context.
  champsim::chrono::clock::time_point fetch_resume_time{};
  champsim::chrono::clock::time_point fetch_stall_begin{};
  bool fetch_stalled_on_mispredict = false;

  // Per-context statistics and phase bookkeeping.
  stats_type roi_stats{}, sim_stats{};

  long long num_retired = 0;

  champsim::chrono::clock::time_point begin_phase_time{};
  long long begin_phase_instr = 0;
  champsim::chrono::clock::time_point finish_phase_time{};
  long long finish_phase_instr = 0;
  champsim::chrono::clock::time_point last_heartbeat_time{};
  long long last_heartbeat_instr = 0;

  // Minimal context bound to a shared physical register file. Tests use this to
  // exercise the RAT methods directly; the DIB geometry is irrelevant there.
  explicit core_context(RegisterAllocator* shared_prf) : DIB(1, 1), reg_allocator(shared_prf)
  {
    frontend_RAT.fill(-1); // default value for no mapping
    backend_RAT.fill(-1);
  }

  core_context(RegisterAllocator* shared_prf, std::size_t lq_size, std::size_t dib_set, std::size_t dib_way, unsigned long dib_window)
      : LQ(lq_size), DIB(dib_set, dib_way, {champsim::data::bits{champsim::lg2(dib_window)}}, {champsim::data::bits{champsim::lg2(dib_window)}}),
        reg_allocator(shared_prf)
  {
    frontend_RAT.fill(-1); // default value for no mapping
    backend_RAT.fill(-1);
  }
};

} // namespace champsim

#endif