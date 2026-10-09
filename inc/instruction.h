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

#ifndef INSTRUCTION_H
#define INSTRUCTION_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <string_view>
#include <vector>

#include "address.h"
#include "champsim.h"
#include "chrono.h"
#include "trace_instruction.h"
#include "util/detect.h"

// branch types
enum branch_type {
  BRANCH_DIRECT_JUMP = 0,
  BRANCH_INDIRECT,
  BRANCH_CONDITIONAL,
  BRANCH_DIRECT_CALL,
  BRANCH_INDIRECT_CALL,
  BRANCH_RETURN,
  BRANCH_OTHER,
  NOT_BRANCH
};

using PHYSICAL_REGISTER_ID = int16_t; // signed to use -1 to indicate no physical register

using namespace std::literals::string_view_literals;
inline constexpr std::array branch_type_names{"BRANCH_DIRECT_JUMP"sv, "BRANCH_INDIRECT"sv,      "BRANCH_CONDITIONAL"sv,
                                              "BRANCH_DIRECT_CALL"sv, "BRANCH_INDIRECT_CALL"sv, "BRANCH_RETURN"sv};

namespace champsim
{
template <typename T>
struct program_ordered {
  using id_type = uint64_t;
  id_type instr_id = 0;

  /**
   * Return a functor that matches this element's ID.
   * \overload
   */
  static auto matches_id(id_type id)
  {
    return [id](const T& instr) {
      return instr.instr_id == id;
    };
  }

  /**
   * Return a functor that matches this element's ID.
   */
  static auto matches_id(const T& instr) { return precedes(instr.instr_id); }

  /**
   * Order two elements of this type in the program.
   */
  static bool program_order(const T& lhs, const T& rhs) { return lhs.instr_id < rhs.instr_id; }

  /**
   * Return a functor that tests whether an instruction precededes the given instruction ID.
   * \overload
   */
  static auto precedes(id_type id)
  {
    return [id](const T& instr) {
      return instr.instr_id < id;
    };
  }

  /**
   * Return a functor that tests whether an instruction precededes the given instruction.
   */
  static auto precedes(const T& instr) { return precedes(instr.instr_id); }
};
} // namespace champsim

struct ooo_model_instr : champsim::program_ordered<ooo_model_instr> {
  champsim::address ip{};
  // Core-wide age, assigned exactly once at dispatch from a monotonic core
  // counter. It is the only key that orders two instructions across hardware
  // contexts: instr_id restarts at 0 in every context's trace (one-to-many), the
  // same trap the per-LQ-entry load_waiter_id closed for memory completion.
  // Within one context instr_id remains the program-order key.
  uint64_t dispatch_sequence = 0;
  champsim::chrono::clock::time_point ready_time{};

  bool is_branch = false;
  bool branch_taken = false;
  bool branch_prediction = false;
  bool branch_mispredicted = false; // A branch can be mispredicted even if the direction prediction is correct when the predicted target is not correct

  std::array<uint8_t, 2> asid = {std::numeric_limits<uint8_t>::max(), std::numeric_limits<uint8_t>::max()};

  branch_type branch{NOT_BRANCH};
  champsim::address branch_target{};

  bool dib_checked = false;
  bool fetch_issued = false;
  bool fetch_completed = false;
  bool decoded = false;
  bool scheduled = false;
  bool executed = false;
  bool completed = false;

  unsigned completed_mem_ops = 0;
  int num_reg_dependent = 0;

  std::vector<PHYSICAL_REGISTER_ID> destination_registers = {}; // output registers
  std::vector<PHYSICAL_REGISTER_ID> source_registers = {};      // input registers

  std::vector<champsim::address> destination_memory = {};
  std::vector<champsim::address> source_memory = {};

#if CHAMPSIM_TRACE_MEMORY_VALUES
  // v2 trace payload. Zeroed when the instruction came from a v1 record.
  //
  // These are indexed to match the compacted destination_memory / source_memory
  // vectors above -- NOT the raw trace slots. A v2 record may leave gaps (an
  // operand in slot 0 and slot 2, none in slot 1); the vectors above drop those
  // gaps, so the payload is compacted identically. Reading these by raw slot
  // index would silently attribute one operand's data to another.
  std::array<champsim::address, NUM_INSTR_DESTINATIONS> destination_memory_pa = {};
  std::array<champsim::address, NUM_INSTR_SOURCES> source_memory_pa = {};
  std::array<uint8_t, NUM_INSTR_DESTINATIONS> destination_memory_size = {};
  std::array<uint8_t, NUM_INSTR_SOURCES> source_memory_size = {};
  std::array<std::array<uint8_t, MAX_MEM_VALUE_SIZE>, NUM_INSTR_DESTINATIONS> destination_memory_value = {};
  std::array<std::array<uint8_t, MAX_MEM_VALUE_SIZE>, NUM_INSTR_SOURCES> source_memory_value = {};

  uint8_t privilege = 0;  // 0 = user, 1 = kernel
  uint8_t instr_type = 0; // 0 = int, 1 = fp, 2 = simd
#endif

  // these are indices of instructions in the ROB that depend on me
  std::vector<std::reference_wrapper<ooo_model_instr>> registers_instrs_depend_on_me;

private:
  // Distinguishes a v2 trace record from a v1 one: only v2 carries memory values.
  template <typename U>
  using has_v2_payload = decltype(std::declval<U>().source_memory_value);

  // Detected separately from has_v2_payload: reserved[] is parsed regardless of
  // CHAMPSIM_TRACE_MEMORY_VALUES, which only governs whether the memory-value
  // payload is carried into the core model.
  template <typename U>
  using has_reserved = decltype(std::declval<U>().reserved);

  template <typename T>
  ooo_model_instr(const T& instr, std::array<uint8_t, 2> local_asid)
      : ip(instr.ip), is_branch(instr.is_branch), branch_taken(instr.branch_taken), asid(local_asid)
  {
    std::remove_copy(std::begin(instr.destination_registers), std::end(instr.destination_registers), std::back_inserter(this->destination_registers), 0);
    std::remove_copy(std::begin(instr.source_registers), std::end(instr.source_registers), std::back_inserter(this->source_registers), 0);

    // Extract the v2 payload in raw-slot order. Compacting here in the same
    // order keeps payload index i aligned with source_memory[i] /
    // destination_memory[i].
#if CHAMPSIM_TRACE_MEMORY_VALUES
    if constexpr (champsim::is_detected_v<has_v2_payload, T>) {
      std::size_t compacted = 0;
      for (std::size_t slot = 0; slot < NUM_INSTR_SOURCES; ++slot) {
        if (instr.source_memory[slot] != 0) {
          source_memory_pa.at(compacted) = champsim::address{instr.source_memory_pa[slot]};
          source_memory_size.at(compacted) = instr.source_memory_size[slot];
          std::copy(std::begin(instr.source_memory_value[slot]), std::end(instr.source_memory_value[slot]), std::begin(source_memory_value.at(compacted)));
          ++compacted;
        }
      }

      compacted = 0;
      for (std::size_t slot = 0; slot < NUM_INSTR_DESTINATIONS; ++slot) {
        if (instr.destination_memory[slot] != 0) {
          destination_memory_pa.at(compacted) = champsim::address{instr.destination_memory_pa[slot]};
          destination_memory_size.at(compacted) = instr.destination_memory_size[slot];
          std::copy(std::begin(instr.destination_memory_value[slot]), std::end(instr.destination_memory_value[slot]),
                    std::begin(destination_memory_value.at(compacted)));
          ++compacted;
        }
      }

      privilege = instr.privilege;
      instr_type = instr.instr_type;
    }
#endif

    for (auto address : instr.destination_memory) {
      if (address != 0) {
        destination_memory.push_back(champsim::address{address});
      }
    }
    for (auto address : instr.source_memory) {
      if (address != 0) {
        source_memory.push_back(champsim::address{address});
      }
    }

    bool writes_sp = std::count(std::begin(destination_registers), std::end(destination_registers), champsim::REG_STACK_POINTER);
    bool writes_ip = std::count(std::begin(destination_registers), std::end(destination_registers), champsim::REG_INSTRUCTION_POINTER);
    bool reads_sp = std::count(std::begin(source_registers), std::end(source_registers), champsim::REG_STACK_POINTER);
    bool reads_flags = std::count(std::begin(source_registers), std::end(source_registers), champsim::REG_FLAGS);
    bool reads_ip = std::count(std::begin(source_registers), std::end(source_registers), champsim::REG_INSTRUCTION_POINTER);
    bool reads_other = std::count_if(std::begin(source_registers), std::end(source_registers), [](uint8_t r) {
      return r != champsim::REG_STACK_POINTER && r != champsim::REG_FLAGS && r != champsim::REG_INSTRUCTION_POINTER;
    });

    // A v2 record may state the branch type outright (reserved[0]), announced by
    // the feature bit in reserved[1]. When present it REPLACES the inference
    // below, because the two are not equally trustworthy: the tracer asks PIN,
    // which knows exactly; the inference reconstructs a semantic property from
    // which registers happened to survive the record's 2- and 4-slot operand
    // arrays. That reconstruction failed silently on every v2 trace produced
    // before the tracer recorded the flags register -- every conditional branch
    // matched the first arm below, became an always-taken BRANCH_DIRECT_JUMP,
    // and the direction predictor was never consulted.
    bool explicit_branch_type = false;
    auto traced_branch_type = static_cast<unsigned char>(NOT_BRANCH);
    if constexpr (champsim::is_detected_v<has_reserved, T>) {
      if ((instr.reserved[champsim::TRACE_RESERVED_FEATURES] & champsim::TRACE_FEATURE_EXPLICIT_BRANCH_TYPE) != 0) {
        traced_branch_type = instr.reserved[champsim::TRACE_RESERVED_BRANCH_TYPE];
        // Range-guard the enum. A corrupt byte, or a future tracer using a value
        // we do not know, must not be cast blindly into branch_type -- fall back
        // to inference rather than inventing a class.
        explicit_branch_type = (traced_branch_type <= static_cast<unsigned char>(NOT_BRANCH));
      }
    }

    // determine what kind of branch this is, if any
    if (explicit_branch_type) {
      branch = static_cast<branch_type>(traced_branch_type);
      is_branch = (branch != NOT_BRANCH);
      // The trace's direction bit is authoritative here. The inference path has
      // to overwrite it for the unconditional classes because it cannot tell a
      // not-taken conditional from a direct jump; the tracer has no such
      // ambiguity -- it records the real direction for conditionals and marks
      // calls and returns taken, which they unconditionally are.
      branch_taken = is_branch && (instr.branch_taken != 0);
    } else if (!reads_sp && !reads_flags && writes_ip && !reads_other) {
      // direct jump
      is_branch = true;
      branch_taken = true;
      branch = BRANCH_DIRECT_JUMP;
    } else if (!reads_sp && !reads_ip && !reads_flags && writes_ip && reads_other) {
      // indirect branch
      is_branch = true;
      branch_taken = true;
      branch = BRANCH_INDIRECT;
    } else if (!reads_sp && reads_ip && !writes_sp && writes_ip && (reads_flags || reads_other)) {
      // conditional branch
      is_branch = true;
      branch_taken = instr.branch_taken; // don't change this
      branch = BRANCH_CONDITIONAL;
    } else if (reads_sp && reads_ip && writes_sp && writes_ip && !reads_flags && !reads_other) {
      // direct call
      is_branch = true;
      branch_taken = true;
      branch = BRANCH_DIRECT_CALL;
    } else if (reads_sp && reads_ip && writes_sp && writes_ip && !reads_flags && reads_other) {
      // indirect call
      is_branch = true;
      branch_taken = true;
      branch = BRANCH_INDIRECT_CALL;
    } else if (reads_sp && !reads_ip && writes_sp && writes_ip) {
      // return
      is_branch = true;
      branch_taken = true;
      branch = BRANCH_RETURN;
    } else if (writes_ip) {
      // some other branch type that doesn't fit the above categories
      is_branch = true;
      branch_taken = instr.branch_taken; // don't change this
      branch = BRANCH_OTHER;
    } else {
      branch_taken = false;
    }
  }

public:
  ooo_model_instr(uint8_t cpu, const input_instr& instr) : ooo_model_instr(instr, {cpu, cpu}) {}
  // A v2 record names every field the shared constructor reads, so classification
  // and operand extraction are identical to v1 by construction.
  ooo_model_instr(uint8_t cpu, const input_instr_v2& instr) : ooo_model_instr(instr, {cpu, cpu}) {}
  ooo_model_instr(uint8_t /*cpu*/, const cloudsuite_instr& instr) : ooo_model_instr(instr, {instr.asid[0], instr.asid[1]}) {}

  [[nodiscard]] std::size_t num_mem_ops() const { return std::size(destination_memory) + std::size(source_memory); }
};

#endif
