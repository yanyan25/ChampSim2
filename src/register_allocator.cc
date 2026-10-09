#include "register_allocator.h"

#include <stdexcept>

#include "champsim_assert.h"

RegisterAllocator::RegisterAllocator(size_t num_physical_registers)
{
  constexpr auto maximum = static_cast<size_t>(std::numeric_limits<PHYSICAL_REGISTER_ID>::max());
  if (num_physical_registers == 0 || num_physical_registers > maximum) {
    throw std::invalid_argument{"register file size must be between 1 and 32767"};
  }
  for (size_t i = 0; i < num_physical_registers; ++i) {
    free_registers.push(static_cast<PHYSICAL_REGISTER_ID>(i));
  }
  physical_register_file = std::vector<physical_register>(num_physical_registers, {0, 0, false, false, {}});
}

void RegisterAllocator::complete_dest_register(PHYSICAL_REGISTER_ID physreg)
{
  // mark the physical register as valid
  physical_register_file.at(physreg).valid = true;
}

void RegisterAllocator::free_register(PHYSICAL_REGISTER_ID physreg)
{
  physical_register_file.at(physreg) = {255, 0, false, false, {}}; // arch_reg_index, producing_inst_id, valid, busy, context
  free_registers.push(physreg);
}

bool RegisterAllocator::isValid(PHYSICAL_REGISTER_ID physreg) const { return physical_register_file.at(physreg).valid; }

unsigned long RegisterAllocator::count_free_registers() const { return std::size(free_registers); }

int RegisterAllocator::count_reg_dependencies(const ooo_model_instr& instr) const
{
  // Outside tests only the deadlock printer calls this, and only for renamed
  // entries. An instruction not renamed yet still holds the trace's
  // architectural IDs, and an ID past the physical register file names no
  // physical register, so it waits on nothing; the guard keeps that safe.
  return static_cast<int>(std::count_if(std::begin(instr.source_registers), std::end(instr.source_registers), [this](auto reg) {
    return reg >= 0 && static_cast<std::size_t>(reg) < std::size(physical_register_file) && !isValid(reg);
  }));
}

void RegisterAllocator::print_deadlock()
{
  fmt::print("\nPhysical Register File\n");
  for (size_t i = 0; i < physical_register_file.size(); ++i) {
    fmt::print("Phys reg: {:3}\t Arch reg: {:3}\t Producer: {}\t Valid: {}\t Busy: {}\n", static_cast<int>(i),
               static_cast<int>(physical_register_file.at(i).arch_reg_index), physical_register_file.at(i).producing_instruction_id,
               physical_register_file.at(i).valid, physical_register_file.at(i).busy);
  }
  fmt::print("\n");
}
