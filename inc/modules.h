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

#ifndef MODULES_H
#define MODULES_H

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "access_type.h"
#include "address.h"
#include "block.h"
#include "champsim.h"

class CACHE;
class O3_CPU;
namespace champsim::modules
{
inline constexpr bool warn_if_any_missing = true;
template <typename T>
[[deprecated]] void does_not_have()
{
}

template <typename T>
struct bound_to {
  T* intern_;
  explicit bound_to(T* bind_arg) { bind(bind_arg); }
  void bind(T* bind_arg) { intern_ = bind_arg; }

  // The runtime-configuration hook, common to all four module kinds. Exactly
  // one signature is probed -- configure(const champsim::runtime_config&,
  // std::string_view) -- so a near-miss overload is a compile-time no-match,
  // never a double call (the pattern the widened update_btb/last_branch_result
  // probes have to guard against).
  template <typename U, typename... Args>
  static auto configure_member_impl(int) -> decltype(std::declval<U>().configure(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto configure_member_impl(long) -> std::false_type;

  template <typename U, typename... Args>
  constexpr static bool has_configure = decltype(configure_member_impl<U, Args...>(0))::value;
};

struct branch_predictor : public bound_to<O3_CPU> {
  explicit branch_predictor(O3_CPU* cpu) : bound_to<O3_CPU>(cpu) {}

  template <typename T, typename... Args>
  static auto initialize_member_impl(int) -> decltype(std::declval<T>().initialize_branch_predictor(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto initialize_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto last_branch_result_member_impl(int) -> decltype(std::declval<T>().last_branch_result(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto last_branch_result_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto predict_branch_member_impl(int) -> decltype(std::declval<T>().predict_branch(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto predict_branch_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto final_stats_member_impl(int) -> decltype(std::declval<T>().branch_predictor_final_stats(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto final_stats_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto execute_resolve_member_impl(int) -> decltype(std::declval<T>().branch_execute_resolve(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto execute_resolve_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto decode_notify_member_impl(int) -> decltype(std::declval<T>().branch_decode_notify(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto decode_notify_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto execute_notify_member_impl(int) -> decltype(std::declval<T>().branch_execute_notify(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto execute_notify_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  constexpr static bool has_initialize = decltype(initialize_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_final_stats = decltype(final_stats_member_impl<T, Args...>(0))::value;

  // Fired when a branch completes execution, out of program order. CBP6
  // predictors perform their non-speculative table update here; ChampSim
  // otherwise resolves everything at fetch.
  template <typename T, typename... Args>
  constexpr static bool has_execute_resolve = decltype(execute_resolve_member_impl<T, Args...>(0))::value;

  // Fired at decode for each architectural destination register, and at execute
  // completion with the value that register received, if one is known. CBP2025
  // predictors that correlate on register values need both: the first marks the
  // register's value unknown, the second supplies it.
  template <typename T, typename... Args>
  constexpr static bool has_decode_notify = decltype(decode_notify_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_execute_notify = decltype(execute_notify_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_last_branch_result = decltype(last_branch_result_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_predict_branch = decltype(predict_branch_member_impl<T, Args...>(0))::value;
};

struct btb : public bound_to<O3_CPU> {
  explicit btb(O3_CPU* cpu) : bound_to<O3_CPU>(cpu) {}

  template <typename T, typename... Args>
  static auto initialize_member_impl(int) -> decltype(std::declval<T>().initialize_btb(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto initialize_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto update_member_impl(int) -> decltype(std::declval<T>().update_btb(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto update_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto predict_branch_member_impl(int) -> decltype(std::declval<T>().btb_prediction(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto predict_branch_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  constexpr static bool has_initialize = decltype(initialize_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_update_btb = decltype(update_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_btb_prediction = decltype(predict_branch_member_impl<T, Args...>(0))::value;
};

struct prefetcher : public bound_to<CACHE> {
  explicit prefetcher(CACHE* cache) : bound_to<CACHE>(cache) {}
  bool prefetch_line(champsim::address pf_addr, bool fill_this_level, uint32_t prefetch_metadata) const;
  bool prefetch_line(champsim::address pf_addr, bool fill_this_level, uint32_t prefetch_metadata, champsim::address_space_id asid, uint32_t cpu) const;
  [[deprecated]] bool prefetch_line(uint64_t pf_addr, bool fill_this_level, uint32_t prefetch_metadata) const;

  template <typename T, typename... Args>
  static auto initiailize_memory_impl(int) -> decltype(std::declval<T>().prefetcher_initialize(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto initiailize_memory_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto cache_operate_member_impl(int) -> decltype(std::declval<T>().prefetcher_cache_operate(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto cache_operate_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto cache_fill_member_impl(int) -> decltype(std::declval<T>().prefetcher_cache_fill(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto cache_fill_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto cycle_operate_member_impl(int) -> decltype(std::declval<T>().prefetcher_cycle_operate(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto cycle_operate_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto final_stats_member_impl(int) -> decltype(std::declval<T>().prefetcher_final_stats(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto final_stats_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto branch_operate_member_impl(int) -> decltype(std::declval<T>().prefetcher_branch_operate(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto branch_operate_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  constexpr static bool has_initialize = decltype(initiailize_memory_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_cache_operate = decltype(cache_operate_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_cache_fill = decltype(cache_fill_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_cycle_operate = decltype(cycle_operate_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_final_stats = decltype(final_stats_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_branch_operate = decltype(branch_operate_member_impl<T, Args...>(0))::value;
};

struct replacement : public bound_to<CACHE> {
  explicit replacement(CACHE* cache) : bound_to<CACHE>(cache) {}

  template <typename T, typename... Args>
  static auto initialize_member_impl(int) -> decltype(std::declval<T>().initialize_replacement(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto initialize_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto find_victim_member_impl(int) -> decltype(std::declval<T>().find_victim(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto find_victim_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto update_state_member_impl(int) -> decltype(std::declval<T>().update_replacement_state(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto update_state_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto cache_fill_member_impl(int) -> decltype(std::declval<T>().replacement_cache_fill(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto cache_fill_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  static auto final_stats_member_impl(int) -> decltype(std::declval<T>().replacement_final_stats(std::declval<Args>()...), std::true_type{});
  template <typename, typename...>
  static auto final_stats_member_impl(long) -> std::false_type;

  template <typename T, typename... Args>
  constexpr static bool has_initialize = decltype(initialize_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_find_victim = decltype(find_victim_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_update_state = decltype(update_state_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_cache_fill = decltype(cache_fill_member_impl<T, Args...>(0))::value;

  template <typename T, typename... Args>
  constexpr static bool has_final_stats = decltype(final_stats_member_impl<T, Args...>(0))::value;
};

// ---------------------------------------------------------------------------
// SMT capability matrix (Stage 3.3). How a module's state is stored with
// respect to a core's hardware contexts. The environment consults this at
// configuration time and refuses a module that cannot serve the requested
// context count -- instead of discovering it at initialize time, after --knobs
// and the unconsulted-key check have already passed.
// ---------------------------------------------------------------------------
enum class smt_capability {
  unsupported, // namespace-scope / function-local singleton: one core, one context
  per_context, // state replicated per hardware context (member or static array)
  shared,      // single instance correctly shared across contexts (e.g. a read-only table)
};

// The distinction that decides `shared` vs `per_context` is *how the state is
// keyed*, not how many instances exist. A module that folds ASID into its table
// index/hash (so two hardware contexts of different address spaces land in
// disjoint entries) is `shared`: one per-core instance is safe because ASID does
// the isolation. A module whose tables are keyed only by IP -- and that must
// therefore keep a private copy per hardware context to avoid aliasing two
// threads' histories -- is `per_context`. Do NOT label an ASID-keyed single
// instance `per_context` to replicate it: that wastes state and defeats the
// sharing the module already provides.

// A module opts into a non-unsupported capability with a
// `static constexpr smt_capability capability` member. The default is
// `unsupported` -- the conservative choice, so a module that reaches into the
// core but has not declared otherwise is refused for SMT rather than silently
// sharing state. Safe per-instance modules are required to opt in. The
// overload set below is SFINAE over that member, so a module without one falls
// to the `long` overload instead of failing to compile.
template <typename T>
constexpr auto capability_value_impl(int) -> decltype(static_cast<smt_capability>(T::capability))
{
  return static_cast<smt_capability>(T::capability);
}
template <typename>
constexpr auto capability_value_impl(long) -> smt_capability
{
  return smt_capability::unsupported;
}

template <typename T>
struct smt_capability_of {
  static constexpr smt_capability value = capability_value_impl<T>(0);
};

// The most restrictive of two capabilities, used to fold a model's tuple of
// module types into one answer. The enum is ordered restrictive-first.
constexpr smt_capability min_smt_capability(smt_capability a, smt_capability b)
{
  return static_cast<smt_capability>(static_cast<int>(a) < static_cast<int>(b) ? static_cast<int>(a) : static_cast<int>(b));
}

// The configuration-time guard, shared by the environment's select_modules and
// its unit test. An `unsupported` module's state is a process-wide singleton,
// so it is refused for a second hardware context and for a second core. The
// caller names the module; the environment prefixes the configuration key.
inline void check_smt_capability(smt_capability cap, std::size_t cpu_index, std::size_t num_contexts, std::string_view module_name)
{
  if (cap == smt_capability::unsupported && (cpu_index != 0 || num_contexts > 1)) {
    throw std::runtime_error{std::string{module_name} + " keeps its state in namespace-scope globals and cannot be used on more than one core or context"};
  }
}
} // namespace champsim::modules

#endif
