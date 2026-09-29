#include <catch.hpp>

#include <array>

#include "dram_controller.h"
#include "vmem.h"

namespace
{
VirtualMemory make_vmem()
{
  constexpr champsim::data::bytes pte_page_size{1ull << 12};
  constexpr unsigned levels = 5;
  MEMORY_CONTROLLER dram{champsim::chrono::picoseconds{3200},
                         champsim::chrono::picoseconds{6400},
                         std::size_t{18},
                         std::size_t{18},
                         std::size_t{18},
                         std::size_t{38},
                         champsim::chrono::microseconds{64000},
                         {},
                         64,
                         64,
                         1,
                         champsim::data::bytes{8},
                         1024,
                         1024,
                         4,
                         4,
                         4,
                         8192};
  return VirtualMemory{pte_page_size, levels, std::chrono::nanoseconds{6400}, dram};
}
} // namespace

SCENARIO("The virtual memory remove PA asked by PTE")
{
  GIVEN("A random page access")
  {
    auto page_number = GENERATE(as<champsim::page_number>{}, 0xdeadbeef, 0x12345678, 0xabcdef01, 0x55555555, 0xaaaaaaaa);
    GIVEN("A large virtual memory") {
      constexpr unsigned levels = 5;
      constexpr champsim::data::bytes pte_page_size{1ull << 12};
      MEMORY_CONTROLLER dram{champsim::chrono::picoseconds{3200},
                            champsim::chrono::picoseconds{6400},
                            std::size_t{18},
                            std::size_t{18},
                            std::size_t{18},
                            std::size_t{38},
                            champsim::chrono::microseconds{64000},
                            {},
                            64,
                            64,
                            1,
                            champsim::data::bytes{8},
                            1024,
                            1024,
                            4,
                            4,
                            4,
                            8192};
      VirtualMemory uut{pte_page_size, levels, std::chrono::nanoseconds{6400}, dram};

      //we should re-reference many times at random addresses and ensure that we never allocate additional pages
      std::size_t new_size = uut.available_ppages();
      WHEN("PTE requires memory")
      {
        
        AND_WHEN("PTE ask for a page")
        {
          auto [paddr_a, delay_a] = uut.get_pte_pa({0, 0}, page_number, 1);

          THEN("The page table missed") { REQUIRE(delay_a > champsim::chrono::clock::duration::zero()); }
          new_size--;
          AND_WHEN("PTE asks for another page")
          {
            auto [paddr_b, delay_b] = uut.get_pte_pa({0, 0}, page_number, 2);
            new_size--;
            THEN("The page table missed") { REQUIRE(delay_b > champsim::chrono::clock::duration::zero()); }

            THEN("The pages are different") { REQUIRE(paddr_a != paddr_b); }

            THEN("The pages are remove from the available pages") { REQUIRE(new_size == uut.available_ppages()); }
            AND_WHEN("The page is reaccessed") {
              for(int i = 0; i < 10; i++) {
                auto [paddr, delay] = uut.get_pte_pa({0, 0}, page_number, 2);
                THEN("The page table hit") { REQUIRE(delay == champsim::chrono::clock::duration::zero()); }
                THEN("Another page is not allocated") { REQUIRE(new_size == uut.available_ppages()); }
                THEN("The same page is returned") { REQUIRE(paddr == paddr_b); }
              }
            }
          }
          AND_WHEN("The page is reaccessed") {
            for(int i = 0; i < 10; i++) {
              auto [paddr, delay] = uut.get_pte_pa({0, 0}, page_number, 1);
              THEN("The page table hit") { REQUIRE(delay == champsim::chrono::clock::duration::zero()); }
              THEN("Another page is not allocated") { REQUIRE(new_size == uut.available_ppages()); }
              THEN("The same page is returned") { REQUIRE(paddr == paddr_a); }
            }
          }
        }
      }
    }
  }
}

SCENARIO("The virtual memory keeps distinct address spaces apart by ASID")
{
  GIVEN("Two address spaces and the same virtual page")
  {
    const auto same_page = GENERATE(as<champsim::page_number>{}, 0xdeadbeef, 0x12345678, 0x55555555);
    const VirtualMemory::address_space_id asid_a{250, 250}; // disjoint from core/context indices, per recon §5.4
    const VirtualMemory::address_space_id asid_b{251, 251};

    WHEN("Each address space translates its own copy of the page")
    {
      VirtualMemory vmem = make_vmem();
      const auto pages_before = vmem.available_ppages();

      auto [pa_a, delay_a] = vmem.va_to_pa(asid_a, same_page);
      auto [pa_b, delay_b] = vmem.va_to_pa(asid_b, same_page);

      THEN("Both first accesses fault") {
        REQUIRE(delay_a > champsim::chrono::clock::duration::zero());
        REQUIRE(delay_b > champsim::chrono::clock::duration::zero());
      }

      THEN("Two distinct physical pages were allocated") { REQUIRE(pa_a != pa_b); }

      THEN("Exactly two pages were consumed") { REQUIRE(vmem.available_ppages() == pages_before - 2); }

      AND_WHEN("Each address space re-accesses its own copy")
      {
        auto [pa_a2, delay_a2] = vmem.va_to_pa(asid_a, same_page);
        auto [pa_b2, delay_b2] = vmem.va_to_pa(asid_b, same_page);

        THEN("The re-accesses hit their own mapping") {
          REQUIRE(delay_a2 == champsim::chrono::clock::duration::zero());
          REQUIRE(delay_b2 == champsim::chrono::clock::duration::zero());
        }

        THEN("The mappings stay with their address space") {
          REQUIRE(pa_a2 == pa_a);
          REQUIRE(pa_b2 == pa_b);
        }
      }
    }

    WHEN("Each address space builds page-table pages under the same VA")
    {
      VirtualMemory vmem = make_vmem();
      auto [pte_a, d_a] = vmem.get_pte_pa(asid_a, same_page, 2);
      auto [pte_b, d_b] = vmem.get_pte_pa(asid_b, same_page, 2);
      (void)d_a;
      (void)d_b;

      THEN("The page-table pages are distinct per address space") { REQUIRE(pte_a != pte_b); }

      AND_WHEN("They are requested again")
      {
        auto [pte_a2, d_a2] = vmem.get_pte_pa(asid_a, same_page, 2);
        auto [pte_b2, d_b2] = vmem.get_pte_pa(asid_b, same_page, 2);
        (void)d_a2;
        (void)d_b2;

        THEN("Each hit returns its own page-table page") {
          REQUIRE(pte_a2 == pte_a);
          REQUIRE(pte_b2 == pte_b);
        }
      }
    }
  }
}