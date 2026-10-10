#include <catch.hpp>

#include <cstdint>
#include <vector>

#include "environment.h"
#include "operable.h"
#include "phase_info.h"
#include "tracereader.h"

namespace champsim
{
long do_cycle(std::vector<std::reference_wrapper<champsim::operable>>&, const std::vector<std::reference_wrapper<O3_CPU>>&, std::vector<tracereader>&,
              const std::vector<std::vector<std::size_t>>&, chrono::clock&);
}

// Stage 4.2: do_cycle feeds each hardware context its own trace, keyed by
// trace_index[core][context] (structural, not a flattened stride). The
// tracereader stamped a trace's instructions with asid {position, position}, so
// the asid a context sees is exactly the value named by its trace_index entry.
// Before 4.2 context 1 was never fed; these tests pin that it now is.
namespace
{
champsim::tracereader trace_with_asid(uint8_t asid) { return champsim::tracereader{[asid] { return ooo_model_instr{asid, input_instr{}}; }}; }
} // namespace

TEST_CASE("do_cycle feeds two contexts of one core from two traces with distinct asids")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(2)};
  std::vector<std::reference_wrapper<O3_CPU>> cpus{cpu};
  std::vector<std::reference_wrapper<champsim::operable>> operables{};
  std::vector<champsim::tracereader> traces;
  traces.push_back(trace_with_asid(0)); // asid {0,0}
  traces.push_back(trace_with_asid(1)); // asid {1,1}
  champsim::chrono::clock clock;

  std::vector<std::vector<std::size_t>> trace_index{{0, 1}};
  champsim::do_cycle(operables, cpus, traces, trace_index, clock);

  REQUIRE(!cpu.contexts[0].input_queue.empty());
  REQUIRE(!cpu.contexts[1].input_queue.empty());
  for (const auto& instr : cpu.contexts[0].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{0, 0});
  }
  for (const auto& instr : cpu.contexts[1].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{1, 1});
  }
}

TEST_CASE("do_cycle keys trace_index by (core, context), not by a shared stride")
{
  // Non-uniform context counts: a flat core_index * num_contexts + context
  // stride has no single num_contexts to divide by, so it would mis-index this
  // machine. The structural [core][context] key does not.
  O3_CPU core0{champsim::core_builder{}.index(0).num_contexts(2)};
  O3_CPU core1{champsim::core_builder{}.index(1).num_contexts(1)};
  std::vector<std::reference_wrapper<O3_CPU>> cpus{core0, core1};
  std::vector<std::reference_wrapper<champsim::operable>> operables{};
  std::vector<champsim::tracereader> traces;
  traces.push_back(trace_with_asid(0)); // asid {0,0}
  traces.push_back(trace_with_asid(1)); // asid {1,1}
  traces.push_back(trace_with_asid(2)); // asid {2,2}
  champsim::chrono::clock clock;

  std::vector<std::vector<std::size_t>> trace_index{{0, 1}, {2}};
  champsim::do_cycle(operables, cpus, traces, trace_index, clock);

  REQUIRE(!core0.contexts[0].input_queue.empty());
  REQUIRE(!core0.contexts[1].input_queue.empty());
  REQUIRE(!core1.contexts[0].input_queue.empty());
  for (const auto& instr : core0.contexts[0].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{0, 0});
  }
  for (const auto& instr : core0.contexts[1].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{1, 1});
  }
  for (const auto& instr : core1.contexts[0].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{2, 2});
  }
}
