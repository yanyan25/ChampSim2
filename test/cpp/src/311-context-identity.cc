#include <catch.hpp>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Stage 3.1: the identity asserts added at the pipeline hand-off points are
// exercised here with a NON-identity address-space mapping, so they cannot be
// satisfied by the N=1 all-zeros coincidence.
//
// Non-identity mapping chosen: an address-space value disjoint from the identity
// (option 2 of the three in the plan). asid {250,250} is neither the N=1 value
// {0,0} nor the core/context index 0, so any hand-off that threaded the wrong
// identity (cpu index, a hardcoded 0, the trace index) instead of the address
// space would trip an assert. A response or request that lost its asid would
// arrive here as the default {255,255} and likewise fail.

namespace
{
constexpr champsim::address_space_id asid_a{250, 250};

struct identity_fixture {
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

  identity_fixture()
  {
    cpu.warmup = true;
    cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};
  }
};
} // namespace

TEST_CASE_METHOD(identity_fixture, "A store forwards to a load only in its own address space")
{
  auto store = champsim::test::instruction_with_ip_and_asid(0x100, asid_a);
  store.instr_id = 1;
  store.destination_memory = {champsim::address{0x2000}};
  cpu.do_memory_scheduling(store);
  cpu.ctx().ROB.push_back(store);

  auto load = champsim::test::instruction_with_ip_and_asid(0x200, asid_a);
  load.instr_id = 2;
  load.source_memory = {champsim::address{0x2000}};
  cpu.do_memory_scheduling(load);
  cpu.ctx().ROB.push_back(load);

  // The load waits on the store (producer_id set); finishing the store forwards
  // to it. Both the forwarding assert and the completion assert compare asid.
  REQUIRE(std::size(cpu.ctx().SQ) == 1);
  cpu.do_finish_store(cpu.ctx().SQ.front());

  CHECK(cpu.ctx().ROB.at(0).completed_mem_ops == 1); // the store completed
  CHECK(cpu.ctx().ROB.at(1).completed_mem_ops == 1); // the load forwarded and completed
}

TEST_CASE_METHOD(identity_fixture, "A load completion preserves its address space")
{
  auto load = champsim::test::instruction_with_ip_and_asid(0x300, asid_a);
  load.instr_id = 3;
  load.source_memory = {champsim::address{0x3000}};
  load.executed = true;
  load.scheduled = true;
  cpu.do_memory_scheduling(load);
  cpu.ctx().ROB.push_back(load);

  for (auto& entry : cpu.ctx().LQ)
    if (entry && entry->instr_id == 3)
      entry->ready_time = champsim::chrono::clock::time_point{};
  cpu.operate_lsq();

  // The request on the bus carried the non-identity address space.
  REQUIRE(std::size(l1d.queues.RQ) == 1);
  CHECK(l1d.queues.RQ.front().asid[0] == 250);
  CHECK(l1d.queues.RQ.front().asid[1] == 250);

  l1d.operate(); // turn the request into a same-asid response
  REQUIRE(std::size(l1d.queues.returned) == 1);
  cpu.handle_memory_return();

  CHECK(cpu.ctx().ROB.front().completed_mem_ops == 1);
  CHECK(cpu.ctx().ROB.front().asid == asid_a);
}

TEST_CASE_METHOD(identity_fixture, "Fetch completion preserves its address space")
{
  auto instr = champsim::test::instruction_with_ip_and_asid(0x4000, asid_a);
  instr.instr_id = 7;
  instr.fetch_issued = true;
  cpu.ctx().IFETCH_BUFFER.push_back(instr);

  champsim::channel::response_type resp{champsim::address{0x4000}, champsim::address{0x4000}, champsim::address{}, 0, 0, 250, 250, {7}};
  l1i.queues.returned.push_back(resp);

  cpu.handle_memory_return();

  CHECK(cpu.ctx().IFETCH_BUFFER.front().fetch_completed);
}

TEST_CASE_METHOD(identity_fixture, "A DIB hit is keyed by its address space")
{
  cpu.do_dib_update(champsim::test::instruction_with_ip_and_asid(0x5000, asid_a));

  auto probe = champsim::test::instruction_with_ip_and_asid(0x5000, asid_a);
  cpu.do_check_dib(probe);

  CHECK(probe.dib_checked);
  CHECK(probe.fetch_completed);
  CHECK(probe.decoded);
}
