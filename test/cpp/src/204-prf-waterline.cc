#include <algorithm>
#include <cstdint>

#include <catch.hpp>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Stage 5.1: the per-context register waterline. The physical register file is
// a shared pool allocated first-come-first-served, so without a reservation a
// context can drain it to zero and starve its sibling of the register it needs
// to rename its oldest instruction -- the one gating its in-order retirement,
// so both threads stall with nothing able to retire and free a register. The
// waterline reserves each context's next rename (need_k = distinct unmapped
// sources + destinations of its oldest unscheduled instruction) and gates a
// rename on free >= need + sum(need_k over the siblings). These cases drive
// both contexts live and assert the reservation holds without imposing a fixed
// per-context quota.

namespace
{
struct waterline_fixture {
  do_nothing_MRC l1i, l1d;
  O3_CPU dual{champsim::core_builder{}
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
                 .num_contexts(2)
                 .rob_size(32)
                 .register_file_size(8)
                 .schedule_width(champsim::bandwidth::maximum_type{128})};

  waterline_fixture() { dual.backend_round_robin = 0; } // context 0 walks first

  // A ready instruction writing one distinct architectural register and reading
  // none: its rename consumes exactly one physical register, so need_k is 1.
  static ooo_model_instr writer(uint64_t id, uint8_t reg)
  {
    auto instr = champsim::test::instruction_with_ip(id * 4);
    instr.instr_id = id;
    instr.destination_registers = {static_cast<PHYSICAL_REGISTER_ID>(reg)};
    instr.ready_time = champsim::chrono::clock::time_point::min();
    return instr;
  }

  long free_registers() const { return static_cast<long>(dual.contexts[0].reg_allocator->count_free_registers()); }

  long scheduled_count(std::size_t c) const
  {
    return static_cast<long>(std::count_if(std::begin(dual.contexts[c].ROB), std::end(dual.contexts[c].ROB), [](const auto& i) { return i.scheduled; }));
  }
};
} // namespace

TEST_CASE_METHOD(waterline_fixture, "A context cannot schedule past its sibling's rename reservation")
{
  // Eight single-register writers in context 0 would, under FCFS, consume the
  // whole eight-register file and leave context 1's single writer nothing to
  // rename. The waterline must stop context 0 at seven so one register stays
  // free for context 1's next rename.
  for (uint8_t reg = 1; reg <= 8; ++reg)
    dual.contexts[0].ROB.push_back(writer(reg, reg));
  dual.contexts[1].ROB.push_back(writer(100, 100));

  dual.schedule_instruction();

  REQUIRE(free_registers() == 1);          // the reservation, not the FCFS zero
  CHECK(scheduled_count(0) == 7);          // seven of the eight were renamed
  CHECK_FALSE(dual.contexts[0].ROB.back().scheduled); // the eighth was reserved out
  CHECK_FALSE(dual.contexts[1].ROB.front().scheduled); // sibling still needs its own + the reservation
}

TEST_CASE_METHOD(waterline_fixture, "A sibling with room to spare still schedules its oldest writer")
{
  // Six writers leave two registers free: after context 0 renames all six, the
  // sibling's single writer has its own need plus context 0's reservation (2),
  // so it schedules its oldest instruction instead of being starved.
  for (uint8_t reg = 1; reg <= 6; ++reg)
    dual.contexts[0].ROB.push_back(writer(reg, reg));
  dual.contexts[1].ROB.push_back(writer(100, 100));

  dual.schedule_instruction();

  REQUIRE(scheduled_count(0) == 6);
  CHECK(dual.contexts[1].ROB.front().scheduled); // the sibling's oldest is renamed
  CHECK(free_registers() == 1);
}
