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

#ifndef OOO_CPU_H
#define OOO_CPU_H

#ifdef CHAMPSIM_MODULE
#define SET_ASIDE_CHAMPSIM_MODULE
#undef CHAMPSIM_MODULE
#endif

#include <array>
#include <bitset>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "bandwidth.h"
#include "champsim.h"
#include "channel.h"
#include "core_builder.h"
#include "core_context.h"
#include "core_stats.h"
#include "instruction.h"
#include "modules.h"
#include "operable.h"
#include "register_allocator.h"
#include "runtime_config.h"
#include "util/lru_table.h"
#include "util/to_underlying.h"

class CACHE;
class CacheBus
{
  using channel_type = champsim::channel;
  using request_type = typename channel_type::request_type;
  using response_type = typename channel_type::response_type;

  channel_type* lower_level;
  uint32_t cpu;

  friend class O3_CPU;

public:
  CacheBus(uint32_t cpu_idx, champsim::channel* ll) : lower_level(ll), cpu(cpu_idx) {}
  bool issue_read(request_type packet);
  bool issue_write(request_type packet);
};

// cpu
class O3_CPU : public champsim::operable
{
public:
  using stats_type = champsim::core_context::stats_type;

  uint32_t cpu = 0;

  bool show_heartbeat = true;

  // The core-shared physical register file and its free list. Held behind a
  // unique_ptr so that a move of O3_CPU (a std::vector reallocation) transfers
  // ownership without changing the heap object's address -- the contexts below
  // hold raw pointers to it and must never dangle.
  std::unique_ptr<RegisterAllocator> shared_prf;

  // Per-hardware-context pipeline state. Stage 1 builds exactly one. ctx()
  // computes the reference on every call and never caches it, so a later
  // vector growth cannot leave a stale reference behind.
  std::vector<champsim::core_context> contexts;

  // Front-end arbitration state (Stage 3.4). `fetch_round_robin` picks which
  // context a shared front-end width serves first this cycle (fetch/decode/DIB);
  // `dispatch_owner` names the context that owns the dispatch round. Both advance
  // once per operate(); see the D4 per-site table in SMT-experiment/stage3-plan.md.
  std::size_t fetch_round_robin = 0;
  std::size_t dispatch_owner = 0;

  // Back-end arbitration state (Stage 3.5). `backend_round_robin` picks which
  // context the shared back-end widths (schedule/retire/LSQ) serve first this
  // cycle; execute and complete issue by global age instead, so they read the
  // dispatch sequence rather than the rotation. `next_dispatch_sequence` is the
  // core-wide monotonic source of the cross-context age key stamped on every
  // dispatched instruction.
  std::size_t backend_round_robin = 0;
  uint64_t next_dispatch_sequence = 0;

  // Constants
  const std::size_t IFETCH_BUFFER_SIZE, DISPATCH_BUFFER_SIZE, DECODE_BUFFER_SIZE, REGISTER_FILE_SIZE, ROB_SIZE, SQ_SIZE, DIB_HIT_BUFFER_SIZE;
  champsim::bandwidth::maximum_type FETCH_WIDTH, DECODE_WIDTH, DISPATCH_WIDTH, SCHEDULER_SIZE, EXEC_WIDTH, DIB_INORDER_WIDTH;
  champsim::bandwidth::maximum_type LQ_WIDTH, SQ_WIDTH;
  champsim::bandwidth::maximum_type RETIRE_WIDTH;
  champsim::chrono::clock::duration BRANCH_MISPREDICT_PENALTY;
  champsim::chrono::clock::duration DISPATCH_LATENCY;
  champsim::chrono::clock::duration DECODE_LATENCY;
  champsim::chrono::clock::duration SCHEDULING_LATENCY;
  champsim::chrono::clock::duration EXEC_LATENCY;
  champsim::chrono::clock::duration DIB_HIT_LATENCY;

  champsim::bandwidth::maximum_type L1I_BANDWIDTH, L1D_BANDWIDTH;

  void resume_fetch_after_mispredict();                                // single-context convenience (tests)
  void resume_fetch_after_mispredict(champsim::core_context& c);

  const long IN_QUEUE_SIZE;

  CacheBus L1I_bus, L1D_bus;
  CACHE* l1i;

  void initialize() final;
  long operate() final;
  void begin_phase() final;
  void end_phase(unsigned cpu) final;

  void initialize_instruction();
  long check_dib();
  long fetch_instruction();
  long promote_to_decode();
  long decode_instruction();
  long dispatch_instruction();
  long schedule_instruction();
  // The oldest instruction cannot rename, so the run can never proceed.
  [[noreturn]] void throw_register_file_too_small(const ooo_model_instr& instr, unsigned long needed) const;
  long execute_instruction();
  long operate_lsq();
  long complete_inflight_instruction();
  long handle_memory_return();
  long retire_rob();

  // Stage 3.4: per-context bodies behind each budget-point wrapper. They take a
  // reference to the one shared `champsim::bandwidth` for the site, consume from
  // it, and return the progress they contributed, so a wrapper can iterate
  // contexts and sum. The single-context `if constexpr` path calls these directly.
  void initialize_instruction_impl(champsim::core_context& c, champsim::context_index idx, champsim::bandwidth& fetch_bw);
  long check_dib_impl(champsim::core_context& c, champsim::bandwidth& fetch_bw);
  long promote_to_decode_impl(champsim::core_context& c, champsim::bandwidth& fetch_bw);
  long decode_instruction_impl(champsim::core_context& c, champsim::bandwidth& decode_bw, champsim::bandwidth& inorder_bw);
  long dispatch_instruction_impl(champsim::core_context& c, champsim::bandwidth& dispatch_bw);
  long schedule_instruction_impl(champsim::core_context& c, long& occupied, long capacity);
  long execute_instruction_impl(champsim::core_context& c, champsim::bandwidth& exec_bw);
  long complete_inflight_instruction_impl(champsim::core_context& c, champsim::bandwidth& complete_bw);
  long operate_lsq_impl(champsim::core_context& c, champsim::bandwidth& store_bw, champsim::bandwidth& load_bw);
  long retire_rob_impl(champsim::core_context& c, champsim::bandwidth& retire_bw);

  bool do_init_instruction(champsim::core_context& c, champsim::context_index idx, ooo_model_instr& instr);
  bool do_predict_branch(champsim::core_context& c, champsim::context_index idx, ooo_model_instr& instr);
  void do_check_dib(ooo_model_instr& instr); // single-context convenience (tests)
  void do_check_dib(champsim::core_context& c, ooo_model_instr& instr);
  bool do_fetch_instruction(std::deque<ooo_model_instr>::iterator begin, std::deque<ooo_model_instr>::iterator end);
  void do_dib_update(const ooo_model_instr& instr); // single-context convenience (tests)
  void do_dib_update(champsim::core_context& c, const ooo_model_instr& instr);
  void do_scheduling(ooo_model_instr& instr); // single-context convenience (tests)
  void do_scheduling(champsim::core_context& c, ooo_model_instr& instr);
  void do_execution(ooo_model_instr& instr); // single-context convenience (tests)
  void do_execution(champsim::core_context& c, ooo_model_instr& instr);
  void do_memory_scheduling(ooo_model_instr& instr); // single-context convenience (tests)
  void do_memory_scheduling(champsim::core_context& c, ooo_model_instr& instr);
  void do_complete_execution(champsim::core_context& c, ooo_model_instr& instr);
  void do_sq_forward_to_lq(LSQ_ENTRY& sq_entry, LSQ_ENTRY& lq_entry);

  void do_finish_store(const LSQ_ENTRY& sq_entry); // single-context convenience (tests)
  void do_finish_store(champsim::core_context& c, const LSQ_ENTRY& sq_entry);
  bool do_complete_store(const LSQ_ENTRY& sq_entry);
  bool execute_load(const LSQ_ENTRY& lq_entry);

  [[nodiscard]] auto roi_instr() const { return ctx().roi_stats.instrs(); }
  [[nodiscard]] auto roi_cycle() const { return ctx().roi_stats.cycles(); }
  [[nodiscard]] auto sim_instr() const { return ctx().num_retired - ctx().begin_phase_instr; }
  [[nodiscard]] auto sim_cycle() const { return (current_time.time_since_epoch() / clock_period) - ctx().sim_stats.begin_cycles; }

  // Per-context phase progress. The no-argument forms above remain the context-0
  // convenience used by the single-context path; these take the hardware context
  // index so do_phase can judge every context against `length` independently.
  [[nodiscard]] auto sim_instr(std::size_t c) const { return contexts[c].num_retired - contexts[c].begin_phase_instr; }
  [[nodiscard]] auto sim_cycle(std::size_t c) const { return (current_time.time_since_epoch() / clock_period) - contexts[c].sim_stats.begin_cycles; }

  // The hardware context this core's frontend and backend operate on.
  //
  // Stage 2 severs the inheritance: the core now owns a vector of contexts and
  // a shared physical register file, and ctx() returns the (single) context.
  // The reference is computed on every call and never cached in a member:
  // `contexts` lives in a std::vector that reallocates, so a cached reference
  // would dangle after any growth.
  champsim::core_context& ctx() { return contexts.front(); }
  const champsim::core_context& ctx() const { return contexts.front(); }

  void print_deadlock() final;

  struct branch_module_concept {
    virtual ~branch_module_concept() = default;

    // Deliver runtime-configuration values after construction, before any
    // other hook. prefix names this instance's knob table.
    virtual void impl_configure(const champsim::runtime_config& cfg, std::string_view prefix) = 0;

    virtual void impl_initialize_branch_predictor() = 0;
    virtual void impl_last_branch_result(champsim::address ip, champsim::address target, bool taken, uint8_t branch_type, champsim::address_space_id asid) = 0;
    virtual bool impl_predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type,
                                     champsim::address_space_id asid) = 0;
    virtual void impl_branch_predictor_final_stats() = 0;
    virtual void impl_branch_execute_resolve(uint64_t instr_id, champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type,
                                             champsim::address_space_id asid) = 0;
    virtual void impl_branch_decode_notify(uint64_t instr_id, uint8_t arch_dst_reg) = 0;
    virtual void impl_branch_execute_notify(uint64_t instr_id, bool has_value, uint64_t value) = 0;

    virtual champsim::modules::smt_capability capability() const = 0;
  };

  struct btb_module_concept {
    virtual ~btb_module_concept() = default;

    virtual void impl_configure(const champsim::runtime_config& cfg, std::string_view prefix) = 0;

    virtual void impl_initialize_btb() = 0;
    virtual void impl_update_btb(champsim::address ip, champsim::address predicted_target, bool taken, uint8_t branch_type, champsim::context_index context,
                                 champsim::address_space_id asid) = 0;
    virtual std::pair<champsim::address, bool> impl_btb_prediction(champsim::address ip, uint8_t branch_type, champsim::context_index context,
                                                                   champsim::address_space_id asid) = 0;

    virtual champsim::modules::smt_capability capability() const = 0;
  };

  template <typename... Bs>
  struct branch_module_model final : branch_module_concept {
    std::tuple<Bs...> intern_;
    explicit branch_module_model(O3_CPU* cpu) : intern_(Bs{cpu}...) { (void)cpu; /* silence -Wunused-but-set-parameter when sizeof...(Bs) == 0 */ }

    void impl_configure(const champsim::runtime_config& cfg, std::string_view prefix) final;

    void impl_initialize_branch_predictor() final;
    void impl_last_branch_result(champsim::address ip, champsim::address target, bool taken, uint8_t branch_type, champsim::address_space_id asid) final;
    [[nodiscard]] bool impl_predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type,
                                           champsim::address_space_id asid) final;
    void impl_branch_predictor_final_stats() final;
    void impl_branch_execute_resolve(uint64_t instr_id, champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type,
                                     champsim::address_space_id asid) final;
    void impl_branch_decode_notify(uint64_t instr_id, uint8_t arch_dst_reg) final;
    void impl_branch_execute_notify(uint64_t instr_id, bool has_value, uint64_t value) final;

    champsim::modules::smt_capability capability() const final;
  };

  template <typename... Ts>
  struct btb_module_model final : btb_module_concept {
    std::tuple<Ts...> intern_;
    explicit btb_module_model(O3_CPU* cpu) : intern_(Ts{cpu}...) { (void)cpu; /* silence -Wunused-but-set-parameter when sizeof...(Ts) == 0 */ }

    void impl_configure(const champsim::runtime_config& cfg, std::string_view prefix) final;

    void impl_initialize_btb() final;
    void impl_update_btb(champsim::address ip, champsim::address predicted_target, bool taken, uint8_t branch_type, champsim::context_index context,
                         champsim::address_space_id asid) final;
    [[nodiscard]] std::pair<champsim::address, bool> impl_btb_prediction(champsim::address ip, uint8_t branch_type, champsim::context_index context,
                                                                        champsim::address_space_id asid) final;

    champsim::modules::smt_capability capability() const final;
  };

  std::unique_ptr<branch_module_concept> branch_module_pimpl;
  std::unique_ptr<btb_module_concept> btb_module_pimpl;

  // Runtime module selection: the generated environment constructor replaces
  // the baked pimpl before any hook has fired. Safe exactly then -- module
  // state (including function-local statics in the CBP6/BLBP wrappers) is
  // created by hook calls, none of which have happened yet.
  void install_branch_module(std::unique_ptr<branch_module_concept> mod) { branch_module_pimpl = std::move(mod); }
  void install_btb_module(std::unique_ptr<btb_module_concept> mod) { btb_module_pimpl = std::move(mod); }

  // NOLINTBEGIN(readability-make-member-function-const): legacy modules use non-const hooks
  void impl_initialize_branch_predictor() const;
  void impl_last_branch_result(champsim::address ip, champsim::address target, bool taken, uint8_t branch_type, champsim::address_space_id asid) const;
  [[nodiscard]] bool impl_predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type,
                                         champsim::address_space_id asid) const;
  void impl_branch_predictor_final_stats() const;
  void impl_branch_execute_resolve(uint64_t instr_id, champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type,
                                   champsim::address_space_id asid) const;
  void impl_branch_decode_notify(uint64_t instr_id, uint8_t arch_dst_reg) const;
  void impl_branch_execute_notify(uint64_t instr_id, bool has_value, uint64_t value) const;

  void impl_initialize_btb() const;
  void impl_update_btb(champsim::address ip, champsim::address predicted_target, bool taken, uint8_t branch_type, champsim::context_index context,
                       champsim::address_space_id asid) const;
  [[nodiscard]] std::pair<champsim::address, bool> impl_btb_prediction(champsim::address ip, uint8_t branch_type, champsim::context_index context,
                                                                      champsim::address_space_id asid) const;
  // NOLINTEND(readability-make-member-function-const)

  template <typename... Bs, typename... Ts>
  explicit O3_CPU(champsim::core_builder<champsim::core_builder_module_type_holder<Bs...>, champsim::core_builder_module_type_holder<Ts...>> b)
      : champsim::operable(b.m_clock_period),
        cpu(b.m_cpu),
        shared_prf(std::make_unique<RegisterAllocator>(b.m_register_file_size)),
        contexts(b.m_num_contexts, champsim::core_context{shared_prf.get(), b.m_lq_size, b.m_dib_set, b.m_dib_way, b.m_dib_window}),
        IFETCH_BUFFER_SIZE(b.m_ifetch_buffer_size), DISPATCH_BUFFER_SIZE(b.m_dispatch_buffer_size), DECODE_BUFFER_SIZE(b.m_decode_buffer_size),
        REGISTER_FILE_SIZE(b.m_register_file_size), ROB_SIZE(b.m_rob_size), SQ_SIZE(b.m_sq_size), DIB_HIT_BUFFER_SIZE(b.m_dib_hit_buffer_size),
        FETCH_WIDTH(b.m_fetch_width), DECODE_WIDTH(b.m_decode_width), DISPATCH_WIDTH(b.m_dispatch_width), SCHEDULER_SIZE(b.m_schedule_width),
        EXEC_WIDTH(b.m_execute_width), DIB_INORDER_WIDTH(b.m_dib_inorder_width), LQ_WIDTH(b.m_lq_width), SQ_WIDTH(b.m_sq_width), RETIRE_WIDTH(b.m_retire_width),
        BRANCH_MISPREDICT_PENALTY(b.m_mispredict_penalty * b.m_clock_period), DISPATCH_LATENCY(b.m_dispatch_latency * b.m_clock_period),
        DECODE_LATENCY(b.m_decode_latency * b.m_clock_period), SCHEDULING_LATENCY(b.m_schedule_latency * b.m_clock_period),
        EXEC_LATENCY(b.m_execute_latency * b.m_clock_period), DIB_HIT_LATENCY(b.m_dib_hit_latency * b.m_clock_period), L1I_BANDWIDTH(b.m_l1i_bw),
        L1D_BANDWIDTH(b.m_l1d_bw), IN_QUEUE_SIZE(2 * champsim::to_underlying(b.m_fetch_width)), L1I_bus(b.m_cpu, b.m_fetch_queues),
        L1D_bus(b.m_cpu, b.m_data_queues), l1i(b.m_l1i), branch_module_pimpl(std::make_unique<branch_module_model<Bs...>>(this)),
        btb_module_pimpl(std::make_unique<btb_module_model<Ts...>>(this))
  {
    // Stamp each hardware context with its own index so rename/retire can key
    // the producer-identity assertions on thread identity rather than asid.
    for (std::size_t i = 0; i < contexts.size(); ++i) {
      contexts[i].ctx_index = champsim::context_index{static_cast<uint8_t>(i)};
    }
  }
};

template <typename... Bs>
void O3_CPU::branch_module_model<Bs...>::impl_configure(const champsim::runtime_config& cfg, std::string_view prefix)
{
  [[maybe_unused]] auto process_one = [&](auto& b) {
    using namespace champsim::modules;
    if constexpr (branch_predictor::template has_configure<decltype(b), const champsim::runtime_config&, std::string_view>)
      b.configure(cfg, prefix);
  };

  std::apply([&](auto&... b) { (..., process_one(b)); }, intern_);
}

template <typename... Bs>
void O3_CPU::branch_module_model<Bs...>::impl_initialize_branch_predictor()
{
  [[maybe_unused]] auto process_one = [&](auto& b) {
    using namespace champsim::modules;
    if constexpr (branch_predictor::has_initialize<decltype(b)>)
      b.initialize_branch_predictor();
  };

  std::apply([&](auto&... b) { (..., process_one(b)); }, intern_);
}

template <typename... Bs>
void O3_CPU::branch_module_model<Bs...>::impl_branch_execute_resolve(uint64_t instr_id, champsim::address ip, champsim::address branch_target, bool taken,
                                                                     uint8_t branch_type, champsim::address_space_id asid)
{
  auto process_one = [&](auto& b) {
    using namespace champsim::modules;
    if constexpr (branch_predictor::has_execute_resolve<decltype(b), uint64_t, champsim::address, champsim::address, bool, uint8_t,
                                                        champsim::address_space_id>) {
      b.branch_execute_resolve(instr_id, ip, branch_target, taken, branch_type, asid);
    }
  };

  std::apply([&](auto&... b) { (..., process_one(b)); }, intern_);
}

template <typename... Bs>
void O3_CPU::branch_module_model<Bs...>::impl_branch_decode_notify(uint64_t instr_id, uint8_t arch_dst_reg)
{
  auto process_one = [&](auto& b) {
    using namespace champsim::modules;
    if constexpr (branch_predictor::has_decode_notify<decltype(b), uint64_t, uint8_t>) {
      b.branch_decode_notify(instr_id, arch_dst_reg);
    }
  };

  std::apply([&](auto&... b) { (..., process_one(b)); }, intern_);
}

template <typename... Bs>
void O3_CPU::branch_module_model<Bs...>::impl_branch_execute_notify(uint64_t instr_id, bool has_value, uint64_t value)
{
  auto process_one = [&](auto& b) {
    using namespace champsim::modules;
    if constexpr (branch_predictor::has_execute_notify<decltype(b), uint64_t, bool, uint64_t>) {
      b.branch_execute_notify(instr_id, has_value, value);
    }
  };

  std::apply([&](auto&... b) { (..., process_one(b)); }, intern_);
}

template <typename... Bs>
void O3_CPU::branch_module_model<Bs...>::impl_branch_predictor_final_stats()
{
  auto process_one = [](auto& b) {
    using namespace champsim::modules;
    if constexpr (branch_predictor::has_final_stats<decltype(b)>) {
      b.branch_predictor_final_stats();
    }
  };

  std::apply([&](auto&... b) { (..., process_one(b)); }, intern_);
}

template <typename... Bs>
void O3_CPU::branch_module_model<Bs...>::impl_last_branch_result(champsim::address ip, champsim::address target, bool taken, uint8_t branch_type,
                                                                 champsim::address_space_id asid)
{
  [[maybe_unused]] auto process_one = [&](auto& b) {
    using namespace champsim::modules;
    if constexpr (branch_predictor::has_last_branch_result<decltype(b), uint64_t, uint64_t, bool, uint8_t, champsim::address_space_id>)
      b.last_branch_result(ip.to<uint64_t>(), target.to<uint64_t>(), taken, branch_type, asid);
    if constexpr (branch_predictor::has_last_branch_result<decltype(b), champsim::address, champsim::address, bool, uint8_t, champsim::address_space_id>)
      b.last_branch_result(ip, target, taken, branch_type, asid);
  };

  std::apply([&](auto&... b) { (..., process_one(b)); }, intern_);
}

template <typename... Bs>
bool O3_CPU::branch_module_model<Bs...>::impl_predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type,
                                                             champsim::address_space_id asid)
{
  using return_type = bool;
  [[maybe_unused]] auto process_one = [&](auto& b) {
    using namespace champsim::modules;
    /* Strong addresses, full size */
    if constexpr (branch_predictor::has_predict_branch<decltype(b), champsim::address, champsim::address, bool, uint8_t, champsim::address_space_id>)
      return return_type{b.predict_branch(ip, predicted_target, always_taken, branch_type, asid)};

    /* Raw integer addresses, full size */
    if constexpr (branch_predictor::has_predict_branch<decltype(b), uint64_t, uint64_t, bool, uint8_t, champsim::address_space_id>)
      return return_type{b.predict_branch(ip.to<uint64_t>(), predicted_target.to<uint64_t>(), always_taken, branch_type, asid)};

    /* Strong addresses, short size */
    if constexpr (branch_predictor::has_predict_branch<decltype(b), champsim::address>)
      return return_type{b.predict_branch(ip)};

    /* Raw integer addresses, short size */
    if constexpr (branch_predictor::has_predict_branch<decltype(b), uint64_t>)
      return return_type{b.predict_branch(ip.to<uint64_t>())};

    return return_type{};
  };

  if constexpr (sizeof...(Bs)) {
    return std::apply([&](auto&... b) { return (..., process_one(b)); }, intern_);
  }
  return return_type{};
}

template <typename... Ts>
void O3_CPU::btb_module_model<Ts...>::impl_configure(const champsim::runtime_config& cfg, std::string_view prefix)
{
  [[maybe_unused]] auto process_one = [&](auto& b) {
    using namespace champsim::modules;
    if constexpr (btb::template has_configure<decltype(b), const champsim::runtime_config&, std::string_view>)
      b.configure(cfg, prefix);
  };

  std::apply([&](auto&... b) { (..., process_one(b)); }, intern_);
}

template <typename... Ts>
void O3_CPU::btb_module_model<Ts...>::impl_initialize_btb()
{
  [[maybe_unused]] auto process_one = [&](auto& t) {
    using namespace champsim::modules;
    if constexpr (btb::has_initialize<decltype(t)>)
      t.initialize_btb();
  };

  std::apply([&](auto&... t) { (..., process_one(t)); }, intern_);
}

template <typename... Ts>
void O3_CPU::btb_module_model<Ts...>::impl_update_btb(champsim::address ip, champsim::address predicted_target, bool taken, uint8_t branch_type,
                                                      champsim::context_index context, champsim::address_space_id asid)
{
  [[maybe_unused]] auto process_one = [&](auto& t) {
    using namespace champsim::modules;
    /* Per-context modules: context index between branch type and asid */
    if constexpr (btb::has_update_btb<decltype(t), champsim::address, champsim::address, bool, uint8_t, champsim::context_index, champsim::address_space_id>)
      t.update_btb(ip, predicted_target, taken, branch_type, context, asid);
    if constexpr (btb::has_update_btb<decltype(t), champsim::address, champsim::address, bool, uint8_t, champsim::address_space_id>)
      t.update_btb(ip, predicted_target, taken, branch_type, asid);
    if constexpr (btb::has_update_btb<decltype(t), uint64_t, uint64_t, bool, uint8_t, champsim::address_space_id>)
      t.update_btb(ip.to<uint64_t>(), predicted_target.to<uint64_t>(), taken, branch_type, asid);
  };

  std::apply([&](auto&... t) { (..., process_one(t)); }, intern_);
}

template <typename... Ts>
std::pair<champsim::address, bool> O3_CPU::btb_module_model<Ts...>::impl_btb_prediction(champsim::address ip, uint8_t branch_type,
                                                                                        champsim::context_index context, champsim::address_space_id asid)
{
  using return_type = std::pair<champsim::address, bool>;
  [[maybe_unused]] auto process_one = [&](auto& t) {
    using namespace champsim::modules;

    /* Per-context modules: context index between ip and asid (no branch type) */
    if constexpr (btb::has_btb_prediction<decltype(t), champsim::address, champsim::context_index, champsim::address_space_id>)
      return return_type{t.btb_prediction(ip, context, asid)};

    /* Strong addresses, asid only (every shipped BTB) */
    if constexpr (btb::has_btb_prediction<decltype(t), champsim::address, champsim::address_space_id>)
      return return_type{t.btb_prediction(ip, asid)};

    /* Strong addresses, full size */
    if constexpr (btb::has_btb_prediction<decltype(t), champsim::address, uint8_t, champsim::address_space_id>)
      return return_type{t.btb_prediction(ip, branch_type, asid)};

    /* Strong addresses, short size */
    if constexpr (btb::has_btb_prediction<decltype(t), champsim::address>)
      return return_type{t.btb_prediction(ip)};

    /* Raw integer addresses, asid only */
    if constexpr (btb::has_btb_prediction<decltype(t), uint64_t, champsim::address_space_id>)
      return return_type{t.btb_prediction(ip.to<uint64_t>(), asid)};

    /* Raw integer addresses, full size */
    if constexpr (btb::has_btb_prediction<decltype(t), uint64_t, uint8_t, champsim::address_space_id>)
      return return_type{t.btb_prediction(ip.to<uint64_t>(), branch_type, asid)};

    /* Raw integer addresses, short size */
    if constexpr (btb::has_btb_prediction<decltype(t), uint64_t>)
      return return_type{t.btb_prediction(ip.to<uint64_t>())};

    return return_type{};
  };

  if constexpr (sizeof...(Ts) > 0) {
    return std::apply([&](auto&... t) { return (..., process_one(t)); }, intern_);
  }
  return return_type{};
}

template <typename... Bs>
champsim::modules::smt_capability O3_CPU::branch_module_model<Bs...>::capability() const
{
  using namespace champsim::modules;
  smt_capability result = smt_capability::shared; // no module: nothing shared across contexts
  if constexpr (sizeof...(Bs) > 0) {
    ((result = min_smt_capability(result, smt_capability_of<Bs>::value)), ...);
  }
  return result;
}

template <typename... Ts>
champsim::modules::smt_capability O3_CPU::btb_module_model<Ts...>::capability() const
{
  using namespace champsim::modules;
  smt_capability result = smt_capability::shared; // no module: nothing shared across contexts
  if constexpr (sizeof...(Ts) > 0) {
    ((result = min_smt_capability(result, smt_capability_of<Ts>::value)), ...);
  }
  return result;
}

#ifdef SET_ASIDE_CHAMPSIM_MODULE
#undef SET_ASIDE_CHAMPSIM_MODULE
#define CHAMPSIM_MODULE
#endif

#endif
