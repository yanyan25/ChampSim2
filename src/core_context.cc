#include "core_context.h"

#include <algorithm>
#include <cstddef>
#include <fmt/core.h>

#include "champsim_assert.h"

// The per-context rename methods, moved line-for-line from RegisterAllocator.
// The shared physical-register pool and its free list live behind
// reg_allocator, which the core guarantees outlives this context.

PHYSICAL_REGISTER_ID champsim::core_context::rename_dest_register(int16_t reg, champsim::program_ordered<ooo_model_instr>::id_type producer_id)
{
  CHAMPSIM_ASSERT(!reg_allocator->free_registers.empty());

  PHYSICAL_REGISTER_ID phys_reg = reg_allocator->free_registers.front();
  reg_allocator->free_registers.pop();
  frontend_RAT[reg] = phys_reg;
  reg_allocator->physical_register_file.at(phys_reg) = {(uint16_t)reg, producer_id, false, true, this->ctx_index}; // arch_reg_index, valid, busy, context

  return phys_reg;
}

PHYSICAL_REGISTER_ID champsim::core_context::rename_src_register(int16_t reg)
{
  PHYSICAL_REGISTER_ID phys = frontend_RAT[reg];

  if (phys < 0) {
    // allocate the register if it hasn't yet been mapped
    // (common due to the traces being slices in the middle of a program)
    CHAMPSIM_ASSERT(!reg_allocator->free_registers.empty());
    phys = reg_allocator->free_registers.front();
    reg_allocator->free_registers.pop();
    frontend_RAT[reg] = phys;
    backend_RAT[reg] = phys;                                              // we assume this register's last write has been committed
    reg_allocator->physical_register_file.at(phys) = {(uint16_t)reg, 0, true, true, this->ctx_index}; // arch_reg_index, producing_inst_id, valid, busy, context
  } else {
    // An already-mapped source must have been allocated by this thread's own
    // rename. Keyed on context, not asid: a shared address space still has a
    // RAT per thread, so a sibling's write to the same asid is a different
    // physical register.
    CHAMPSIM_ASSERT(reg_allocator->physical_register_file.at(phys).context.value == this->ctx_index.value);
  }

  return phys;
}

void champsim::core_context::retire_dest_register(PHYSICAL_REGISTER_ID physreg)
{
  // The committing instruction's destination must have been renamed by this
  // thread. Thread identity, not asid, is the key (see rename_src_register).
  CHAMPSIM_ASSERT(reg_allocator->physical_register_file.at(physreg).context.value == this->ctx_index.value);

  // grab the arch reg index, find old phys reg in backend RAT
  uint16_t arch_reg = reg_allocator->physical_register_file.at(physreg).arch_reg_index;
  PHYSICAL_REGISTER_ID old_phys_reg = backend_RAT[arch_reg];

  // update the backend RAT with the new phys reg
  backend_RAT[arch_reg] = physreg;

  // free the old phys reg
  if (old_phys_reg != -1) {
    reg_allocator->free_register(old_phys_reg);
  }
}

void champsim::core_context::reset_frontend_RAT()
{
  std::copy(std::begin(backend_RAT), std::end(backend_RAT), std::begin(frontend_RAT));
  // once wrong path is implemented:
  // find registers allocated by wrong-path instructions and free them
}

bool champsim::core_context::isAllocated(PHYSICAL_REGISTER_ID archreg) const
{
  // An architectural ID. A renamed instruction's operands hold physical IDs,
  // which can exceed the RAT, so asking this about them is a caller bug.
  CHAMPSIM_ASSERT(archreg >= 0 && static_cast<std::size_t>(archreg) < std::size(frontend_RAT));
  return frontend_RAT[static_cast<std::size_t>(archreg)] != -1;
}

// The RAT half of the deadlock printer, printed back-to-back with
// RegisterAllocator::print_deadlock() (the physical-file half) so the combined
// output matches the original single-function version.
void champsim::core_context::print_deadlock()
{
  fmt::print("Frontend Register Allocation Table        Backend Register Allocation Table\n");
  for (size_t i = 0; i < frontend_RAT.size(); ++i) {
    fmt::print("Arch reg: {:3}    Phys reg: {:3}            Arch reg: {:3}    Phys reg: {:3}\n", i, frontend_RAT[i], i, backend_RAT[i]);
  }

  if (reg_allocator->count_free_registers() == 0) {
    fmt::print("\n**WARNING!! WARNING!!** THE PHYSICAL REGISTER FILE IS COMPLETELY OCCUPIED.\n");
    fmt::print("It is extremely likely your register file size is too small.\n");
  }
}