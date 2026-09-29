/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VMEM_H
#define VMEM_H

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <random>

#include "address.h"
#include "champsim.h"
#include "chrono.h"

class MEMORY_CONTROLLER;

using pte_entry = champsim::data::size<long long, std::ratio<8>>;

class VirtualMemory
{
public:
  // An address space is identified by its ASID, not by a physical-core number.
  using address_space_id = std::array<uint8_t, 2>;

private:
  std::map<std::pair<address_space_id, champsim::page_number>, champsim::page_number> vpage_to_ppage_map;
  std::map<std::tuple<address_space_id, uint32_t, champsim::address_slice<champsim::dynamic_extent>>, champsim::address> page_table;
  std::optional<uint64_t> randomization_seed;
  const champsim::data::bytes physical_capacity;

public:
  const champsim::chrono::clock::duration minor_fault_penalty;
  const std::size_t pt_levels;
  const pte_entry pte_page_size; // Size of a PTE page

private:
  std::deque<champsim::page_number> ppage_free_list;
  champsim::page_number active_pte_page{};
  champsim::address_slice<champsim::dynamic_extent> next_pte_page;

  // champsim::page_number next_ppage;
  // champsim::page_number last_ppage;

  [[nodiscard]] champsim::page_number ppage_front() const;
  void ppage_pop();

  void shuffle_pages();
  void populate_pages();

public:
  /**
   * Initialize the virtual memory.
   * The size of the virtual memory space is determined from the size of a page table page and the number of levels in the hierarchy.
   *
   * :param page_table_page_size: The size of one page table page. This value must be less than the size of a physical page.
   * :param page_table_levels: The number of levels in the virtual memory table hierarchy.
   * :param minor_penalty: The latency of a minor page fault.
   * :param physical_capacity: The physical memory size, used to populate the free-page list.
   */
  VirtualMemory(champsim::data::bytes page_table_page_size, std::size_t page_table_levels, champsim::chrono::clock::duration minor_penalty,
                champsim::data::bytes physical_capacity_);
  VirtualMemory(champsim::data::bytes page_table_page_size, std::size_t page_table_levels, champsim::chrono::clock::duration minor_penalty,
                champsim::data::bytes physical_capacity_, std::optional<uint64_t> randomization_seed_);

  // Compatibility overloads for callers that own a legacy memory controller.
  VirtualMemory(champsim::data::bytes page_table_page_size, std::size_t page_table_levels, champsim::chrono::clock::duration minor_penalty,
                MEMORY_CONTROLLER& dram_);
  VirtualMemory(champsim::data::bytes page_table_page_size, std::size_t page_table_levels, champsim::chrono::clock::duration minor_penalty,
                MEMORY_CONTROLLER& dram_, std::optional<uint64_t> randomization_seed_);

  /**
   * Find the bit location of the lowest bit for the given page table level.
   */
  [[nodiscard]] champsim::data::bits shamt(std::size_t level) const;

  /**
   * Find the extent for the given page table level.
   */
  [[nodiscard]] champsim::dynamic_extent extent(std::size_t level) const;

  /**
   * Get the page table page offset of the address for the given level.
   */
  [[nodiscard]] uint64_t get_offset(champsim::address vaddr, std::size_t level) const;
  [[nodiscard]] uint64_t get_offset(champsim::page_number vaddr, std::size_t level) const;

  /**
   * The count of unallocated physical pages.
   */
  [[nodiscard]] std::size_t available_ppages() const;

  /**
   * Translate the given address from the virtual space to the physical space.
   * If a page translation does not already exist, one will be created and the minor fault penalty will be applied.
   *
   * :param asid: The address space making the request.
   * :param vaddr: The address to translate.
   *
   * :returns: A pair of the physical address and the latency to be applied to the translation.
   */
  std::pair<champsim::page_number, champsim::chrono::clock::duration> va_to_pa(address_space_id asid, champsim::page_number vaddr);

  /**
   * Find the address for the page table page for the given virtual address (under translation), and the given level.
   * If a page table page does not already exist, one will be created and the minor fault penalty will be applied.
   *
   * :param asid: The address space making the request.
   * :param vaddr: The address to translate.
   * :param level: The current level being translated.
   *
   * :returns: A pair of the page table page address and the latency to be applied to the operation.
   */
  std::pair<champsim::address, champsim::chrono::clock::duration> get_pte_pa(address_space_id asid, champsim::page_number vaddr, std::size_t level);
};

#endif
