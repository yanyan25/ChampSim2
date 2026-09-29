#include <catch.hpp>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// A load's completion is scoped by its address space as well as its virtual
// block. In one address space, the MSHR-fill broadcast the original scan
// modeled still completes every issued same-block load; across address spaces,
// a response completes only the load in its own asid. The load waiter identity
// is carried end-to-end on the request/response independent of the fetch
// dependency list instr_depend_on_me.

namespace
{
struct waiter_fixture {
  do_nothing_MRC l1i, l1d;
  O3_CPU cpu{champsim::core_builder{}
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
                 .rob_size(8)
                 .lq_size(8)
                 .sq_size(8)
                 .register_file_size(128)
                 .execute_width(champsim::bandwidth::maximum_type{1})
                 .retire_width(champsim::bandwidth::maximum_type{0})
                 .execute_latency(3)};

  waiter_fixture()
  {
    cpu.warmup = true;
    cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};
  }

  // Allocate the load's LQ entry, place its instruction in the ROB, and let the
  // LSQ issue the load onto the data bus.
  ooo_model_instr add_load(uint64_t id, champsim::address va, std::array<uint8_t, 2> asid = {0, 0})
  {
    auto instr = champsim::test::instruction_with_ip(id * 4);
    instr.instr_id = id;
    instr.asid = asid;
    instr.source_memory = {va};
    cpu.do_memory_scheduling(instr);

    instr.executed = true;
    instr.scheduled = true;
    cpu.ctx().ROB.push_back(instr);

    // Make the load eligible to issue this cycle.
    for (auto& entry : cpu.ctx().LQ) {
      if (entry && entry->instr_id == id) {
        entry->ready_time = champsim::chrono::clock::time_point{};
      }
    }
    cpu.operate_lsq();
    return instr;
  }

  std::size_t completed_loads() const
  {
    return std::count_if(std::begin(cpu.ctx().ROB), std::end(cpu.ctx().ROB), [](const auto& x) { return x.completed_mem_ops == 1; });
  }

  std::size_t occupied_lq_entries() const
  {
    return std::count_if(std::begin(cpu.ctx().LQ), std::end(cpu.ctx().LQ), [](const auto& x) { return x.has_value(); });
  }
};
} // namespace

SCENARIO("A same-block response completes every issued load in its address space")
{
  GIVEN("Two issued loads to the same cache block in one address space")
  {
    waiter_fixture f;
    const champsim::address va_a{0x1000};
    const champsim::address va_a2{0x1030}; // same 64-byte block as va_a
    f.add_load(1, va_a);
    f.add_load(2, va_a2);

    WHEN("The first load's response returns")
    {
      f.l1d.operate(); // mock turns both RQ entries into responses
      REQUIRE(std::size(f.l1d.queues.returned) == 2);
      auto first = f.l1d.queues.returned.at(0);
      f.l1d.queues.returned.clear();
      f.l1d.queues.returned.push_back(first);
      f.cpu.operate();

      THEN("Both same-block loads complete together")
      {
        REQUIRE(f.completed_loads() == 2);
        REQUIRE(f.occupied_lq_entries() == 0);
      }
    }

    AND_WHEN("We inspect the request the core put on the bus")
    {
      THEN("The load waiter field is populated and the fetch dependency list is untouched")
      {
        f.l1d.operate();
        REQUIRE(std::size(f.l1d.queues.returned) == 2);
        REQUIRE(f.l1d.queues.returned.at(0).load_waiters == std::vector<uint64_t>{0});
        REQUIRE(f.l1d.queues.returned.at(1).load_waiters == std::vector<uint64_t>{1});
        REQUIRE(f.l1d.queues.returned.at(0).instr_depend_on_me.empty());
        REQUIRE(f.l1d.queues.returned.at(1).instr_depend_on_me.empty());
      }
    }
  }
}

SCENARIO("Two address spaces sharing one virtual address are completed separately")
{
  GIVEN("Two loads, asid 250 and 251, to the same virtual address")
  {
    waiter_fixture f;
    const champsim::address va{0x2000};
    f.add_load(1, va, {250, 250});
    f.add_load(2, va, {251, 251});

    WHEN("The ASID-250 load's response returns first")
    {
      f.l1d.operate();
      REQUIRE(std::size(f.l1d.queues.returned) == 2);
      auto first = f.l1d.queues.returned.at(0);  // asid 250
      auto second = f.l1d.queues.returned.at(1); // asid 251
      f.l1d.queues.returned.clear();
      f.l1d.queues.returned.push_back(first);
      f.cpu.operate();

      THEN("The ASID-251 load sharing the VA is still outstanding")
      {
        REQUIRE(f.cpu.ctx().ROB.at(0).completed_mem_ops == 1);
        REQUIRE(f.cpu.ctx().ROB.at(1).completed_mem_ops == 0);
        REQUIRE(f.occupied_lq_entries() == 1);
        auto remaining = std::find_if(std::begin(f.cpu.ctx().LQ), std::end(f.cpu.ctx().LQ), [](const auto& x) { return x.has_value(); });
        REQUIRE(remaining != std::end(f.cpu.ctx().LQ));
        REQUIRE((*remaining)->instr_id == 2);
      }

      AND_WHEN("Its own response returns")
      {
        f.l1d.queues.returned.push_back(second);
        f.cpu.operate();

        THEN("Both loads complete, each by its own address space")
        {
          REQUIRE(f.completed_loads() == 2);
          REQUIRE(f.occupied_lq_entries() == 0);
        }
      }
    }
  }
}

SCENARIO("A merged response completes every same-block load it names")
{
  GIVEN("Two same-block loads are merged by the memory system")
  {
    waiter_fixture f;
    const champsim::address va{0x3000};
    f.add_load(1, va);
    f.add_load(2, va);
    f.l1d.operate();
    REQUIRE(std::size(f.l1d.queues.returned) == 2);

    // Simulate the merge: one response carries both waiter tokens.
    auto merged = f.l1d.queues.returned.at(0);
    merged.load_waiters = {0, 1};
    f.l1d.queues.returned.clear();
    f.l1d.queues.returned.push_back(merged);

    WHEN("The merged response is handled")
    {
      f.cpu.operate();

      THEN("Both loads complete from the single response")
      {
        REQUIRE(f.completed_loads() == 2);
        REQUIRE(f.occupied_lq_entries() == 0);
      }
    }
  }
}