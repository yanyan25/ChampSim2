# Critique of the ChampSim2 SMT roadmap

Date: 2026-09-21. Status: independent review of [the SMT roadmap](2026-09-21-champsim2-smt-roadmap.md) and its [Sniper](2026-09-21-sniper-scout.md) and [Scarab](2026-09-21-scarab-scout.md) scouts against `feat/smt` at `81275433`. No simulator code was changed and nothing was built. Where a claim was measured, the existing non-native release binary was run, and the text says so.

Citations are `path:line` relative to the repository root unless they name `snipersim` or `scarab`. Roadmap line numbers refer to the roadmap file as it stood on 2026-09-21. "Decision N" means the roadmap's numbered decision (lines 180–184); §12 proposes a replacement register.

## Verdict

The roadmap's direction is sound, and most of its code citations are accurate. The following should stand:

- one `O3_CPU` per physical core, with contexts inside it (approach 1). That leaves the channel graph, the cache order and the `501-static-environment.cc` pins untouched;
- separate identities for physical core, hardware context and address space, with `cpu` staying physical;
- one ordered ROB per context, private rename maps, and independent in-order retirement;
- core-wide budgets created once per cycle, which avoids Sniper's per-context issue and commit budgets;
- synthetic translation instead of recorded v2 physical addresses;
- refusing to sum IPCs measured over different intervals, and asking for per-context progress detection.

It should not yet be frozen as the basis for the Stage 1 design. These problems survived adversarial verification and should be resolved first:

1. **A fixed per-context PRF quota would dominate every SMT result** (§1). The PRF, not the ROB, is the structure that binds at the shipped configurations. ChampSim pins one physical register per architectural register a context has touched, so a 50/50 split leaves a context 29–41% of its standalone rename capacity, and the deadlock floor depends on the trace.
2. **The scheduler "inspection budget" is also a reservation-station capacity** (§2). A cross-context fair scan loses that bound and can nearly double it at the default and `lnc.toml` geometries. The Stage 3 exit checks would not notice.
3. **One-context parity has no named gate, and Stage 2 contradicts it** (§8). The repository's exact gate (`tools/perf/compare_optimization.py` plus the 50M long gate) is not mentioned. Completing loads "through explicit waiters" changes single-context timing. Stage 2 has no parity check of its own.
4. **Module isolation is keyed on the wrong property** (§6). Fourteen predictor/BTB modules read `O3_CPU::input_queue` as "the instruction under prediction". The eight oracle modules among them pass a global-state guard, eight other modules share one static predictor across instances, and the fork's reference configuration uses two of those.
5. **The measurement contract re-specifies what ChampSim already does per CPU, and misses what is new** (§7). Cache and DRAM ROI already runs to the last finisher. Trace repetition is implicit. The standalone reference run is undefined. The missing wrong-path model biases SMT results in a known direction that the roadmap never states (§4).
6. **The address-space mechanism is left open, and the cheaper option was never costed** (§5). The existing request `asid` field is dead plumbing. Both reference simulators tag addresses at trace ingest, which in ChampSim would qualify every VA-keyed structure at once with exact one-context parity.
7. **Approach 1 is the right choice, but not for the reasons the roadmap gives** (§9.1). Under the milestone's own fixed quotas and replicated predictors, composing two `O3_CPU`s would deliver most of milestone 1 from existing code. Approach 1 is needed because the shared PRF and scheduler that §1 and §2 recommend cannot be built across instances.
8. **The stage cuts are off** (§9.2–§9.4):
   - Stage 2's exit evidence needs a two-context core that only Stage 3 delivers.
   - Stage 4 is at once the first real-trace integration and a rewrite of the diagnostics that would explain its failures.
   - Decisions that need measurement are frozen before any stage produces real-trace data, although a few `--set` runs answer the PRF question today.
   - Stage 1's touch points omit the tests and modules its extraction must rewrite (§8.4).

The review also found defects that exist today regardless of SMT (§10).

## How the review was done

1. **Lens reviews.** Eleven reviewers each owned one lens:
   - frontend;
   - backend (dispatch to retire, rename, LSQ);
   - cache and channel;
   - virtual memory, PTW and TLBs;
   - environment, phases and statistics;
   - modules;
   - memory backends;
   - performance, testing and process;
   - SMT architecture and methodology, against the literature;
   - re-verification of the Sniper scout;
   - re-verification of the Scarab scout.
2. **Adversarial verification.** Each finding went to an independent verifier told to refute it. The verifier re-read the code, checked that the roadmap really says or omits what the finding claims, and judged the severity. Each critical or high finding had its own verifier; medium and low findings were checked in batches of up to six.
3. **Completeness critics.** Three critics then looked for what the lenses missed: an implementer's walkthrough of Stages 1–4, cross-cutting validity threats, and the roadmap's internal consistency. Their findings went through the same verification.

Of the 122 lens findings, 24 were confirmed, 97 were confirmed with a correction, and 1 was refuted. Verifiers lowered 52 severities; of the 26 findings first rated high, 5 remain high. All 16 critic findings survived: 9 confirmed and 7 confirmed with corrections. This document states the corrected versions throughout. Separately, about 150 checks of roadmap and scout statements (with overlap between lenses) found 105 accurate, 40 partly accurate and 4 inaccurate.

Several measurements used `bin/x86_64-linux-gnu/x86-64-v2/release/9a079df9…/sim/champsim`. It was built minutes before the last source commit, and no `src/`, `inc/` or module change landed after it, but it records no source commit, so treat those numbers as indicative. It also predates `ed01cca7`, which changed one-context scheduling: the PRF 4000 columns of §1 and §9.4 are unaffected, and the other IPC figures shift by fractions of a percent without changing any conclusion. The published alias `bin/champsim` does not start (§10.4).

Severity: **High** means the plan as written would bias results, or a stage's exit evidence could not catch the bug. **Medium** means a real hazard or gap the plan does not address. **Low** means an inaccuracy or a refinement.

## 1. Physical register file: the fixed quota becomes the SMT result [High]

Roadmap: "Physical registers | Fixed capacity per context initially, summing to the configured core PRF budget; account for first-use/live-in allocations" (line 73); Decision 1, "minimum viable PRF capacity for two live contexts" (line 180); dynamic sharing deferred to Stage 5 (lines 84, 148).

What the code does:

- **Renaming.** It happens at schedule time (`do_scheduling`, `src/ooo_cpu.cc:461-475`), not at dispatch. Dispatch never checks the PRF (`:416-435`).
- **Pinning.** `rename_src_register` gives every never-written source a physical register and writes it into *both* RATs (`src/register_allocator.cc:33-48`: "we assume this register's last write has been committed"). `retire_dest_register` frees only the previous mapping (`:56-69`). So every architectural register a context has touched since warmup permanently holds one physical register. Warmup clears register operands at fetch (`src/ooo_cpu.cc:184-187`), so the count restarts at the ROI.
- **Deadlock floor.** A context is deadlock-free only if its quota covers that committed set plus the next instruction's need, in practice U + 2. U depends on the trace and the run length. Distinct architectural registers in a read-only scan of trace bytes 10–15:

  | Trace | Distinct architectural registers |
  |---|---|
  | 605.mcf | 20 |
  | 708.sqlite.sp0 | 32 |
  | 710.omnetpp.sp0 | 35 |
  | 721.gcc.sp1 | 38 (22 at 0.5M instructions, 37 at 8.5M) |
  | 621.wrf | 60 at 30M instructions |

- **How an undersized PRF fails.** It ends in a generic `DEADLOCK` abort after `sim.deadlock_cycle`. mcf aborts at 20 and 21 registers and runs at 22; wrf aborts at 48 and runs at 56. `RegisterAllocator::print_deadlock` warns only when the free count is exactly zero, which it was not for mcf.
- **The PRF is the binding structure.** Measured IPC:

  | Configuration | PRF 4000 | Default (PRF 128, ROB 352) | PRF 64 | ROB halved to 176 |
  |---|---|---|---|---|
  | Default, mcf | 0.608 | 0.260 | 0.144 | 0.2597 |
  | Default, gcc | 1.768 | 1.679 | 1.428 | 1.674 |

  | `lnc.toml` | Base (PRF 200, ROB 576) | PRF 100 | ROB 288 | PRF 4000 |
  |---|---|---|---|---|
  | mcf | 0.4498 | 0.2645 | 0.4500 | 0.7713 |
  | gcc | 2.007 | 1.855 | 1.996 | 2.039 |

**Why it matters.** Splitting the default PRF 64/64 leaves a context 26–44 rename registers, 29–41% of its standalone capacity, against 50% of the ROB. The PRF split, not SMT interaction, would then dominate every Stage 3–5 number:

- it hurts MLP-bound threads most (mcf −44%, gcc −15% at half the PRF);
- it removes a real interference channel: one thread filling the shared rename pool.

Real SMT2 cores share the PRF. Marr et al. (Intel Technology Journal, 2002) describe Pentium 4 Hyper-Threading as having "two RATs, one for each logical processor", with operands "renamed ... to physical registers in a shared physical register pool", while the ROB and load/store buffers are partitioned. Zen 4 watermarks both register files (AMD Software Optimization Guide 57647, Table 4).

**Recommendations.**

- Make the baseline a shared PRF with per-context RATs and a per-context watermark. The reserve kept for the sibling must be dynamic, because its committed set grows lazily. It must cover that set plus one instruction's worst case, which also rules out cross-context deadlock. Keep the fixed split as a knob for sensitivity studies.
- Accept that the shared pool is the harder implementation. A fixed split can reuse one unchanged `RegisterAllocator` per context; a shared pool needs per-context RATs inside one allocator, plus occupancy tracking. The bias justifies the cost.
- When a quota cannot hold a context's committed set, throw a runtime error that names `ooo_cpu.cpuN.register_file_size` and the context. Do not leave it to the deadlock guard. Report per-context committed-register counts.
- Restate Decision 1 as this rule. No constant can be frozen, because the floor depends on the trace.

**Related: the retire path has no context.** `retire_dest_register` recovers the architectural register from `physical_register_file[physreg].arch_reg_index` and updates `backend_RAT` (`src/register_allocator.cc:56-69`), and `count_free_registers` is core-global (`:81`). A shared-PRF design must record the context in each physical-register entry. The Stage 1 isolation test (line 110) should interleave the full rename → complete → retire → free lifecycle across contexts, not only renaming. With private RATs, a rename-only test passes by construction.

## 2. The scheduler window is a capacity, not only a search width [High]

Roadmap: "Scheduler inspection budget | Core-wide, with a defined fair scan across contexts; it remains ChampSim's search-window abstraction" (line 77); Stage 3 "fair shared scheduler traversal" (line 124); Stage 3 exit "aggregate occupancy never exceeds each configured capacity" (line 128), whose list of capacities omits the scheduler.

**What the code does.** `schedule_instruction` walks from the ROB head and charges the `SCHEDULER_SIZE` budget only for unexecuted entries (`src/ooo_cpu.cc:437-458`). `execute_instruction` then runs any scheduled, ready entry anywhere in the ROB (`:477-492`). Entries leave the ROB only at retirement, and `executed` is never cleared. So with one context:

- the number of scheduled-but-unexecuted entries never exceeds `SCHEDULER_SIZE`;
- the scheduled set is always a prefix of the ROB.

The window is a reservation-station capacity in disguise. CLAUDE.md calls `scheduler_size` "a per-cycle search bandwidth, not the size of a structure", the same framing the roadmap inherits.

**The hazard.** A cross-context scan breaks that bound whenever it lets one context's window exceed its long-run share: a rotating start, interleaving that hands leftover budget to the other context, or dynamic re-division. Example with S = 4:

- Cycle 1: context B is empty, so A schedules 4.
- Cycle 2: an interleaved scan (A1, B1, A2, B2) spends the budget and schedules B1 and B2. A3 and A4 stay scheduled.
- Result: 6 instructions waiting against S = 4.

The structural bound on the aggregate is the sum over contexts of min(S, ROB quota). That is up to 256 against 128, both at the default (ROB 352, S 128) and in `lnc.toml` (ROB 576, S 128).

How much of that is reached depends on the PRF, because every scheduled entry with a destination holds a register:

- Under the roadmap's fixed PRF quotas, register pressure caps the inflation. At the default PRF there is essentially none; at `lnc.toml` it is roughly 140–170 against 128 (estimate).
- It becomes material when a context's PRF share exceeds S, when many in-flight instructions have no destination, and under the shared PRF recommended in §1. The two fixes therefore have to land together.

A static S/n split per context keeps the bound. The effect is extra scheduling capacity, not starvation: `schedule_instruction` has no per-cycle scheduling width beyond the window, so the sibling loses at most a cycle of dispatch-to-schedule latency on alternate cycles.

A further coupling: the register-shortage `break` (`:445-446`) ends the whole walk. In a single cross-context scan, context A running out of its register quota would stop B's scheduling, the very coupling the design is meant to prevent.

**Real hardware** shares the scheduler with a per-thread limit. Marr et al.: "there is a limit on the number of active entries that a logical processor can have in each scheduler's queue". Zen 4's integer and FP schedulers are watermarked.

**Recommendations.**

- Model the scheduler as an explicit shared occupancy: a count of scheduled-and-unexecuted entries, at most `SCHEDULER_SIZE`, with a per-context watermark. With one context the scheduled set is a prefix, so this matches today's window exactly and preserves parity.
- Make a register shortage stop only the affected context's walk.
- Add a per-cycle `CHAMPSIM_ASSERT` on that occupancy, and the S = 4 microtest, to the Stage 3 exit.
- Do not use `instr_id` as a cross-context age. IDs are drawn when `do_cycle` refills the input queue (`src/champsim.cc:54-58`, `inc/tracereader.h:70`), so a context frozen on a mispredict holds older IDs than instructions its sibling dispatched earlier. If any stage selects oldest-first across contexts (execute and complete walk the ROB oldest-first today), stamp a core-wide dispatch sequence number at ROB insertion.

## 3. Arbitration and budgets need a site-by-site map [Medium]

The core has more budget sites than the roadmap's "fetch/decode/dispatch/execute/complete/retire budgets" row suggests. All lines are in `src/ooo_cpu.cc`.

| Knob | Consumed at |
|---|---|
| `FETCH_WIDTH` | four independent sites: `initialize_instruction` (`:93-94`), `check_dib` (`:197`), `promote_to_decode` (`:295-297`), the L1I return (`:719`) |
| `L1I_BANDWIDTH` | `fetch_instruction` (`:249`) and the L1I return (`:719`). It is the L1I's `max_tag_check` (default 2, `src/static_environment.cc:429`), not a core knob |
| `DECODE_WIDTH`, `DIB_INORDER_WIDTH` | decode consumes both (`:336-340`); the latter is also capped by `DISPATCH_BUFFER` space |
| `DISPATCH_WIDTH`, `SCHEDULER_SIZE` | `:418`, `:439` |
| `EXEC_WIDTH` | execute (`:479`) *and* completion (`:704`) |
| `SQ_WIDTH` | one object for both store issue and the post-retirement drain (`:563`) |
| `LQ_WIDTH`, `L1D_BANDWIDTH`, `RETIRE_WIDTH` | `:584`, `:746`, `:764` |

Consequences:

- **The roadmap under-counts decode and fetch.** Its "decode ... must also consume one aggregate budget" is incomplete, because decode consumes two. "One-context-per-cycle fetch" does not say which of the four `FETCH_WIDTH` sites rotates. The natural answer is `initialize_instruction`, the only stage gated by `fetch_resume_time`.
- **Widths can multiply silently.** `get_span` and `get_span_p` take `champsim::bandwidth` by value and never consume it (`inc/util/span.h:30-43`), and `retire_rob` passes a temporary. Wrapping those calls in a per-context loop gives every context the full width with no error, because `bandwidth::consume` throws only on the object actually consumed. That reproduces the Sniper per-scan counter bug the roadmap warns about (line 29) by a different route. The rule: one budget object per site per cycle, passed by reference and consumed after each context's span. The exit evidence should count events per site per cycle, because no statistic records promote moves or fetch completions today.
- **Decide on work conservation.** Sniper gives the whole dispatch cycle to the first context that dispatches anything and discards the leftover width (`snipersim .../rob_smt_timer.cc:560, 713-716`). Its eligibility test excludes only a *full* partition (`:704`). With fixed quotas, a context a few entries below its quota would claim the cycle and waste most of the width. Decide whether ChampSim2's rotation is work-conserving. Name the rotation rule for each stage: Sniper itself uses two, with dispatch restarting after the context that dispatched and issue advancing by one every cycle. Add a utilization check with both contexts backlogged, because "never exceeds each width" cannot detect under-use.
- **Choose a retirement policy.** Intel's optimization manual describes retirement alternating between logical processors each cycle, with all bandwidth going to one when the other cannot retire (§2.7.4; this is the NetBurst-era description). The roadmap inspects both contexts under one aggregate budget. Either is defensible; choose one and document it.
- **Keep the eligibility sampling point explicit.** ChampSim runs its stages from retire back to fetch (`src/ooo_cpu.cc:36-55`), while Sniper runs dispatch, issue, commit, so eligibility sees different occupancy. The roadmap already says to keep ChampSim's order (line 88); keep that explicit when adapting the policy.
- **The L1 caches have a hidden admission budget that wiring changes.** `CACHE::operate` divides the remaining tag-check bandwidth across its upper channels as max(remaining / upper_count, 1). Each of an upper's WQ, RQ and PQ then gets min(per-upper, remaining) (`src/cache.cc:498-513`).

  The L1D has two uppers, `ptw_to_l1d` and `core_to_l1d` (`src/static_environment.cc:348`), and `max_tag_check` defaults to 2. So the core's RQ admits at most one load per cycle, and its WQ at most one store, whatever `lq_width` is.

  Measured with `lnc.toml` (v2 traces, 1M/3M):
  - Standalone L1D load demand is 0.32–0.38 per cycle (cpython, ntest, zstd, omnetpp), and cpython also issues 0.38 writes per cycle.
  - An SMT pair roughly doubles that demand against the same caps.
  - The single-load cap already costs cpython about 1.7% single-threaded.

  Giving each context its own core→L1D channel, the natural way to route responses, makes three uppers at one admission each. That raises the aggregate load cap to two per cycle, a change driven by wiring rather than by any knob. Stage 3's "never exceeds each configured width" cannot see this, because no configured width governs it.

  State the per-cycle core admission into shared L1s as an explicit policy, independent of channel count, and add a Stage 3 check that counts L1D tag initiations per cycle.

## 4. The missing wrong-path model biases SMT results, and the roadmap never says so [Medium]

ChampSim knows at fetch that a branch is mispredicted, because it compares against the trace. It then freezes new fetch until the branch resolves plus the penalty (`src/ooo_cpu.cc:153-169, 691-699`; `test/cpp/src/180-wrong-path-cycles.cc:10-11`). Instructions already buffered before the branch still drain through L1I, decode, dispatch and execute. No wrong-path instruction ever occupies a shared resource.

**The bias.** Under rotating eligibility the frozen context is ineligible, so its sibling gets every fetch turn for the rest of the misprediction shadow. Real hardware spends those turns on wrong-path fetch, decode, dispatch and execute bandwidth, and on cache, TLB and BTB pollution (partly offset by wrong-path prefetching). The model therefore underestimates the interference a mispredicting co-runner inflicts, and throughput (STP) is optimistic for branch-heavy pairs.

- **Scope in the first milestone.** Under the roadmap's fixed ROB/LQ/SQ/PRF quotas, wrong-path occupancy would have landed in the mispredicting context's own partitions. So the milestone-1 error is confined to shared bandwidth and shared caches. It grows once Stage 5 shares capacity.
- **Magnitude.** Tullsen et al. (ISCA'96, Table 3) measured wrong-path instructions at 24% and 7% of fetched instructions with 1 and 4 threads. The effect is real but second-order.
- **Sniper shares it.** Sniper's dispatch skips frontend-stalled threads (`snipersim .../rob_smt_timer.cc:627, 677-687`), so the planned Sniper cross-check cannot reveal the bias.
- **Effect on fetch policies.** BRCOUNT-style policies lose their wrong-path benefit in this model. ICOUNT's benefit against issue-queue clog can still be modelled.
- **Effect on Stage 3.** The exit check "a long miss or misprediction in context A allows context B to issue and retire" is still a useful regression guard, because fetch-stall state is a per-core scalar today (`inc/ooo_cpu.h:141-146`). It cannot distinguish a freeze model from a wrong-path model.

**Recommendations.**

- State the limitation and its direction in the measurement contract.
- Report each co-runner's standalone CycWPKI beside results for branch-heavy pairs.
- Optionally add a knob under which a frozen context stays eligible and uses up its fetch, decode and dispatch turns without delivering instructions. Keep it off the one-context parity path.
- List a real wrong-path model next to the port model in Decision 5.

## 5. Address-space identity: choose the mechanism first [Medium]

### 5.1 The existing `asid` field is dead plumbing

The roadmap says `channel.h` "carries `cpu` and small ASIDs on requests" (line 56). The field exists, but:

- the core never sets it on fetch, load or store packets, or in `CacheBus` (`src/ooo_cpu.cc:268-283, 618-644, 904-923`);
- the `CACHE::tag_lookup_type` and `fill_type` constructors do not copy it (`src/cache.cc:96-106`), so every cache forwards the 0xFF default;
- `prefetch_line` never sets it;
- the PTW and legacy DRAM copy it, and nothing anywhere compares it.

The instruction-level `asid` is `{trace index, trace index}` for v1 and v2 traces (`inc/instruction.h:292-295`, via `src/main.cc:365-366`). For CloudSuite traces it is the recorded value (`:296`), which can collide across traces. Treat address-space plumbing as new work, and key nothing on the existing field.

### 5.2 More surfaces must become address-space-aware than the Translation row lists

The Translation row (line 79) lists tags, merges, responses, PTW roots and walk-cache keys. The full set:

- **TLB tag, MSHR merge and completion.** DTLB, ITLB and STLB are the same `CACHE` class as the physically tagged caches. `matches_address` (`src/cache.cc:156-161`) is the one matcher for tag hits, MSHR merges, in-flight merges and completion. Qualifying it naively would also qualify L1D, L2C and LLC, which must stay keyed on the physical block. The backends treat duplicate same-block reads differently: legacy DRAM coalesces them into one response, while the native adapter keeps them separate (`src/dram_controller.cc:494-516` versus `src/ramulator2_memory_backend.cc:159-160`). So a context-qualified LLC MSHR would hang on legacy and double the traffic on native.
- **`finish_translation` in every translating cache** (L1D, L1I, L2C). It stamps every untranslated entry with the same virtual page with the returned physical page (`src/cache.cc:683-710`). Qualifying the TLB alone leaves this in place, so a TLB-level test can pass while context B's entry receives context A's page.
- **PSCL tags and the CR3 root** (`inc/ptw.h:37-46, 95`; `src/ptw.cc:39, 67-78`). The translation value always comes from vmem (`src/ptw.cc:237, 249`), so an unqualified PSCL corrupts only timing. Context B's walks can hit context A's entries: PSCL4/3/2 hits skip 1–3 levels, B skips its own minor faults, B hits PTE lines A warmed, and the block-address match can complete B's walk on A's response. A "translates distinctly" test cannot see any of this; walk-level tests can, using `test/cpp/src/600-ptw-path.cc:138-205` as the template.
- **vmem keys** (`src/vmem.cc:134-136, 152-156`). `inc/vmem.h` says `cpu_num` "is currently used as an address space ID".
- **The fixed-latency PTW's own `va_to_pa(packet.cpu, ...)`** (`src/ptw.cc:221`). The roadmap never mentions fixed mode. Fixed mode must keep skipping root allocation (`test/cpp/src/601-fixed-ptw.cc:72`).
- **Virtual prefetches** (§6.7), and **perfect TLBs**, which answer with `vmem->va_to_pa(handle_pkt.cpu, ...)` directly in `CACHE::try_hit` (`src/cache.cc:281-285` at `fcd13089`).
- **Frame allocation** (§7.6).

### 5.3 The cheaper mechanism the roadmap did not cost

Both reference simulators tag addresses at trace ingest:

- Sniper masks each IP, data address and branch target and ORs an explicit application ID into bits 48 and up (`snipersim common/trace_frontend/trace_thread.cc:246-263`, `trace_thread.h:37`). It then walks a per-application page table.
- Scarab packs `proc_id` into bits 63:58 (`scarab src/globals/utils.c:673-679`, applied at `uop_generator.c:849-856`).

The roadmap rejects Scarab's packing only because it is an *implicit* core-ID mapping (line 42). It never weighs an *explicit*, trace-assigned ASID in unused high VA/IP bits, which is compatible with its own identity rule (line 86).

**Why it would work in ChampSim.** Every VA-keyed structure there includes the high bits:

- vmem (`src/vmem.cc:136, 154-156`);
- PSCL (`inc/ptw.h:45`);
- the DIB (`inc/ooo_cpu.h:108-111`);
- store forwarding (`src/ooo_cpu.cc:534`);
- L1D completion (`:748`);
- `finish_translation` (`src/cache.cc:685-686`);
- TLB tags.

Branch targets are derived from the next IP (`src/tracereader.cc:32`), and responses carry `v_address`. So one tagging function in the per-context reader would qualify all of these for independent traces, and ASID 0 would give bit-exact one-context parity.

**Costs to weigh:**

- The CR3 root stays shared, a timing approximation unless roots become per-ASID.
- Predictors that hash the full IP, and IP-indexed tables, see tagged IPs. The standalone baseline must therefore run in the same ASID slot.
- Tagging must mask sign-extended high-half addresses first, as both reference simulators do.
- The untagged `basic_btb` indirect table and RAS still need per-context instances.
- It cannot express shared memory, which is out of scope until Stage 6.
- Existing tests use full-width VAs (`test/cpp/src/600-ptw-path.cc:181`, `601-multiple-inflight-walks.cc:54`).

**Recommendation.** Make "explicit field or packed ASID" a Stage 1 identity-contract decision, costed against the site list above. Packed ASID is the smaller, parity-safe option for milestone 1; the explicit plumbing belongs to the cooperating-thread project. If the explicit field is chosen, make an ASID-qualified address its own type so that a bare VA comparison fails to compile; the existing `asid` field shows how a carried side field fails silently. Either way, the Stage 2 exit tests should check:

- the final physical address at L1D, L1I and L2C;
- PSCL and walk-step isolation;
- fixed-PTW mode;
- a merge of two different VAs that map to the same physical address.

## 6. Modules: the guard checks the wrong property [Medium]

### 6.1 Fourteen modules read the core's input queue

The core's contract is that `input_queue.front()` is the instruction under prediction inside `do_predict_branch`, and is popped only afterwards (`src/ooo_cpu.cc:100-104`). Modules also read `input_queue[1]` as the successor PC. Fourteen of the 21 branch and BTB modules rely on this:

- `perfect_branch`, `perfect_btb`, `perfect_indirect`;
- the five `perfect_group` shells (`perfect_direct`, `perfect_return`, `ideal_btb`, `ideal_btb_pibtb`, `ideal_btb_pibtb_pras`), through `inc/perfect_group/perfect_group.h:253,288`;
- the four `cbp6_*` hosts, which also take the protocol `instr_id` from `front()`;
- `ittage_32kb` and `ittage_64kb`.

Modules bind to `O3_CPU*` (`inc/modules.h:59-60, 126-127`), so a per-context instance cannot tell which context it is predicting for. The roadmap names only ITTAGE's successor read, and prescribes "context-local successor information". But the eight oracle modules read `front()` and have no global state; their only static is a warning counter. A rejection keyed on "global-state adapters" (line 118) would therefore admit them.

If a context-0 compatibility alias for `input_queue` survived Stage 1, context 1's `perfect_branch` and `perfect_btb` would silently return context 0's outcomes and targets. That would corrupt exactly the oracle headroom decomposition this fork's BTB research relies on.

**Recommendation.**

- Delete the `O3_CPU`-level `input_queue` during the extraction, so every reach-in fails to compile.
- Pass a context handle through the hooks that exposes the instruction under prediction and its successor PC.
- Add a two-context oracle test: identical IP streams with opposite outcomes must give zero mispredicts in both contexts.

### 6.2 Per-context instances of eight modules are still one predictor

`cbp6_*` (4), `ittage_*` (2) and `blbp_*` (2) forward every hook to a function-local static (for example `branch/cbp6_tagescl64/cbp6_tagescl64.cc:62-66`, `btb/ittage_64kb/ittage_64kb.cc:38-42`, `btb/blbp_64kb/blbp_64kb.cc:37-41`). The cost of a per-context fix differs by family:

- **BLBP:** the state is ordinary members, so a member instance works.
- **ITTAGE:** it needs static-storage zero-initialisation, so use a bounded static array indexed by context.
- **CBP6:** the tables are namespace-scope globals in the vendored headers, re-allocated by the tenant constructor. Either re-include the unmodified header inside per-context namespaces, or reject.

Publish this as a capability matrix under Decision 3.

### 6.3 The guard does not run at configuration time

`require_single_core` runs inside `initialize_branch_predictor` and `initialize_btb`. `champsim::main` calls these (`src/champsim.cc:200-202`) after the unconsulted-key check and after `--knobs` has already returned (`src/main.cc:302-334`). The guard also checks the physical core index, not the context.

Meeting line 118 needs a static per-module SMT capability trait: unsupported, per-context or shared. It should default to unsupported for any module that includes `ooo_cpu.h`, and `select_modules` (`src/static_environment.cc:475-513`) should check it and throw with the key name, so `--knobs` reports the error.

`select_modules` also skips the registry when the selection equals the default (`:484, :490`). A naive per-context extension would therefore leave context 1 without its own instance in the default configuration.

### 6.4 The reference configuration cannot run SMT

`configs/lnc.toml` selects `cbp6_tagescl64` and `ittage_64kb` (`:75-76`). The roadmap should say so. Then either schedule per-context ITTAGE support in Stage 2 together with a named substitute direction predictor, whose single-thread delta is measured, or accept that SMT on the LNC configuration waits for CBP6 work.

### 6.5 Replication is the no-interference configuration

Real SMT cores share predictor and BTB tables and replicate only history and the return stack. Marr et al. describe the global history array as shared and tagged with a logical processor ID. Zen 4 shares the BTB and has a 32-entry RAS per thread (AMD SOG §2.14, §2.8.1.3).

The roadmap already calls replication an explicit modelling choice (line 84). What is missing:

- a way to report it: no hook reports predictor storage;
- a configurable alternative: an iso-storage variant (two half-size instances) cannot be configured, because `hashed_perceptron`, `bimodal`, `gshare`, `perceptron` and CBP6 are sized by `constexpr`.

For multiprogrammed pairs, the literature suggests little interaction when tables scale with the thread count (Hily and Seznec, PACT'96). This is therefore a labelling and capacity-accounting issue, not a first-order error.

**Recommendation.**

- Label replicated results as an upper bound.
- Add a `storage_bits()` hook or a `[config]` total.
- Settle the context argument in the hook API during Stage 2, so a shared-table model does not need a second API change.

### 6.6 A sharper predictor exit test

ChampSim predicts and updates at fetch, in each context's trace order, with no wrong path (`src/ooo_cpu.cc:133-179`). So under replication, every timing-independent predictor's per-context mispredict count at trace position N must equal its standalone count exactly, whatever the sibling does.

Use that as the Stage 2/3 exit check, with a pathological sibling: same IPs, opposite outcomes. It catches shared statics, wrong-queue reads and successor leakage with zero tolerance. It does not hold for CBP6's delayed update or runlts's decode/execute hooks, which are excluded anyway.

### 6.7 Prefetchers are shared and have no origin identity

`prefetch_line` takes no context. It stamps each prefetch with the cache's `cpu` member, a last-writer cursor that every fill, hit and miss overwrites (`src/cache.cc:172, 250, 358, 636`).

- **Synchronous issue is safe.** Prefetches issued from `prefetcher_cache_operate` (`next_line`, `spp_dev`, `va_ampm_lite`) carry the triggering access's cpu.
- **Deferred issue is not.** `ip_stride` issues from `prefetcher_cycle_operate` after all tag checks (`src/cache.cc:565`; `prefetcher/ip_stride/ip_stride.cc:30-43`), so it takes whichever context the cache handled last.
- **Virtual prefetches make it a correctness problem.** The L1I defaults to `virtual_prefetch = true` (`src/static_environment.cc:381`). An L1I VA prefetch would be translated in an undefined address space, and vmem would allocate its pages there on demand.
- **`branch_operate` has no packet at all** (`src/ooo_cpu.cc:151`).

All of this is latent while every default prefetcher is `no`. The roadmap's "check prefetch-origin metadata" (line 116) names the area but not the mechanism.

**Recommendation.** Give `prefetch_line` an explicit origin, or rely on packed ASIDs. Until then, when there is more than one context, reject a prefetch-capable module on a `virtual_prefetch` cache. Do not reject on `virtual_prefetch` alone, or every default two-context configuration is refused.

### 6.8 Replacement policies index `NUM_CPUS` arrays without bounds checks

DRRIP's PSEL and SHiP's SHCT and sampler are sized by `NUM_CPUS` and indexed by `triggering_cpu` without bounds checks (`replacement/drrip/drrip.cc:12, 55, 61`; `replacement/ship/ship.cc:12, 16, 48, 57, 65, 96`). So "`cpu` stays physical" must be an invariant for every consumer of a request's `cpu`, not only for the memory backends, where lines 60 and 116 state it.

With `cpu` physical, both contexts share one SHiP SHCT indexed by IP bits, so identical IPs from two address spaces alias. That is defensible, hardware-like behaviour, but `lnc.toml` selects SHiP at the LLC (`:197`), so document it.

## 7. Phases, measurement and statistics [Medium]

### 7.1 ChampSim already implements the per-CPU contract

What ChampSim already does per CPU:

- `do_phase` runs until every CPU completes (`src/champsim.cc:94`).
- Faster CPUs overshoot the warmup quota and keep interfering.
- All operables begin the next phase together (`:71-74`).
- When a CPU completes, `end_phase(cpu)` is broadcast, and only that `O3_CPU` snapshots its ROI; it keeps running (`src/ooo_cpu.cc:77-89`).
- Every `end_phase` call overwrites `sim_stats.end_*`, so each CPU's `sim` section already spans [barrier, last finisher].
- Begin and end instruction offsets are stored (`inc/core_stats.h:43-46`); only their differences are printed.

`test/cpp/src/502-phase-order.cc:62-105` pins this.

Rewrite Stage 4 as "generalize the per-CPU machinery to contexts, bit-identical for one context and for `NUM_CPUS > 1` without SMT". Then scope the design to what is actually new:

- context indexing of `phase_complete`, `livelock_instr`, `trace_index`, `end_phase`, and per-context retire counts and statistics;
- shared-structure ROI semantics (§7.2);
- a lap counter and a termination reason;
- absolute offsets in the output;
- per-context stuck detection (§7.7).

### 7.2 Cache and memory ROI already runs to the last finisher

- `CACHE::end_phase` discards its argument (`finished_cpu = finished_cpu;`, `src/cache.cc:915`) and copies every counter.
- `DRAM_CHANNEL::end_phase` and the Ramulator2 backend do the same (`src/dram_controller.cc:444`, `src/ramulator2_memory_backend.cc:245`). The last call wins, which `test/cpp/src/705-ramulator2-backend.cc:557-578` pins.
- Cache counters are keyed by (access type, physical cpu) (`inc/cache_stats.h:21-24`), so both contexts collapse into one row.
- Prefetch counters are cache-wide.
- Writebacks carry the evicting fill's cpu, and cache blocks record no owner (`inc/block.h:24-34`, `src/cache.cc:194`).

Decide explicitly between two options:

- shared-structure statistics are core-wide over the common interval, which is nearly free today; or
- they gain a (type, cpu, context) key with per-context snapshot hooks separate from `end_phase`.

Per-context TLB and cache attribution also needs a defined rule for merges, writebacks and prefetches.

### 7.3 Trace repetition is not explicit

`repeat = simulation_given` (`src/main.cc:365`): every run given `-i` wraps its traces, and `repeatable::eof()` always returns false (`inc/repeatable.h:47`). The stop-all on any EOF (`src/champsim.cc:144-147`) is therefore reachable only when `-i` is omitted.

- The roadmap's "Trace repetition remains explicit" (line 138) is wrong.
- Its "ends on any EOF" (line 59) is literally true but omits this coupling.
- `repeatable` counts no laps.
- `bulk_tracereader::eof()` normally withholds the last buffered record (`inc/tracereader.h:105`), so a lap is usually N−1 records. That matters for "replay the same measured regions".

### 7.4 The standalone reference is undefined

STP (weighted speedup) and slowdown divide by single-program performance with every resource available (Eyerman and Eeckhout, IEEE Micro 2008). The roadmap says only "standalone IPC using the same trace region and measurement contract" (line 150).

With fixed quotas and no recombination, a partitioned core with one active context is neither SMT nor single-thread. Real cores recombine partitioned resources when a sibling halts (Marr et al.; Zen 4's retire queue holds 320 entries in non-SMT mode and 160 per thread in SMT mode).

**Recommendation.**

- State that standalone IPC comes from a one-context run of the same binary and configuration over the same region, which is also the Stage 1 parity mode.
- Forbid measuring after a "drain" at a non-repeating EOF. Even with recombination, such an ROI would straddle SMT and single-thread modes.

### 7.5 Define the interval and the metrics

The "common core measurement interval" has a start but no end (line 138). Two candidates:

- [barrier, last finisher] already exists. It includes the early finisher's instructions after its ROI.
- [barrier, first ROI end] keeps every context inside its own ROI.

Emit the chosen endpoints in the TOML. Stage 5 already plans weighted speedup and per-thread slowdown; name ANTT and maximum slowdown as well.

### 7.6 Warmup offsets, co-phase and page placement

- **Warmup offsets are an artefact.** Warmup is functional. It clears register dependencies and zeroes core stage latencies, cache hit and fill latency, and legacy and native DRAM latency (`src/ooo_cpu.cc:158, 184-187, 303-311, 390, 431, 497`; `src/cache.cc:416, 427, 672`; `src/dram_controller.cc:140-159`); only the detailed PTW's vmem minor-fault penalty is still charged. So the two contexts' relative progress at the barrier is set by widths and queues, not by their real relative speed, and it changes with warmup length. SMT throughput depends strongly on this co-phase (Van Biesbrouck, Eeckhout and Calder, ISPASS'06). Add an offset-sensitivity sweep on a subset of pairs.
- **Selection and pairing rules are missing.** The traces are weighted SimPoints, so state a pairing rule. State a pair-selection rule: all pairs of a small suite, or stratified mixes with confidence intervals. Check that (A,B) and (B,A) agree, to expose context-index bias, and that an (A,A) pair shows near-equal IPC, to check fairness.
- **Page placement adds noise.** Both contexts draw frames from one shuffled free list in fault order (`src/vmem.cc:97-101, 136-141`), so each trace's placement differs from its standalone run. Changing only the seed moved cycles by 0.2–0.3%, but small-count events moved far more: omnetpp's LLC accesses ranged 2,413–4,016 across seeds while its LLC misses stayed at 547. Report a standalone seed-sweep noise floor beside per-thread slowdowns and event deltas. Per-ASID free lists would not remove this noise, because first-touch order depends on timing.
- **No two address spaces can share a physical page, including code.** vmem gives every (address space, virtual page) its own frame (`src/vmem.cc:136-141`). Real processes running the same binary share text and library frames through the page cache. Under SMT the L1I and L2C are shared inside the core, so a same-binary pair, whether SPECrate-style or the symmetric (A,A) control, holds two copies of its code in every physically tagged instruction-side structure. That duplicated footprint would be read as SMT interference; data-side and TLB duplication are realistic. State this as a milestone assumption. Either avoid same-binary pairs as the symmetric control or report them with the caveat. Make sure the §5 mechanism does not rule out a later "shared code frames for replays of the same trace" mode.

### 7.7 Progress detection

Today's checks:

- Deadlock detection sums progress over all operables (`src/champsim.cc:48-51, 103-107`), so a healthy sibling already hides a stuck core.
- The livelock check is per `O3_CPU` (`:109-131`).
- With repetition on, a context that never retires while its sibling progresses would run forever.

Per-context detection has four constraints:

- It must live in the core. Backend progress has no source, and legacy refresh counts as progress on every tick (`src/dram_controller.cc:227-228`).
- It must inherit the native 10 µs and fixed-PTW allowances, not the 500-tick default.
- It should not abort at the per-CPU 0.01 IPC threshold, which would kill low-bandwidth memory/memory pairs that run fine today.
- It is best built on the livelock mechanism, with per-context retire counts.

### 7.8 Statistics semantics and output

**Counter meanings.** Some core counters need a defined per-context or per-core meaning:

- `total_rob_occupancy_at_branch_mispredict`: which ROB?
- `cycles_on_wrong_path`, documented as "cycles that produced no useful work": false at core level under SMT;
- DIB counters on a shared DIB.

Stage 1 should classify every `cpu_stats` field as context-owned or core-owned.

**Output format.**

- `[meta].num_cpus`, trace tables keyed `cpu{i}`, and the plain printer's "CPU i runs" would label context 1 as a CPU that does not exist. Nest contexts under their core (`core.cpu0.thread1`), so the CI check that the `ooo_cpu` and `core` key sets match (`.github/workflows/test.yml:329`) still holds.
- `schema_version` 2 already means native (`src/toml_printer.cc:484`), and the loader accepts only 1 and 2 (`src/runtime_config.cc:63-66`). Decide versioning deliberately.
- Every consulted key enters `[config]` and `build_id`, so consulting a `threads` or quota key changes every one-context run's hash (§8.2).
- The whole-run section that would hold the common interval is opt-in in TOML, and the plain printer prints it only when `NUM_CPUS > 1`. Both conditions are wrong for one-core SMT.
- Quotas, arbitration policy and trace mapping should be consulted keys, so `[config]` and `build_id` record them.

### 7.9 There is no per-context cycle accounting

Stage 4 asks for "occupancy/stall/arbitration counters" and Stage 5 for "contention and occupancy explanations" (lines 142, 150), but no accounting model is defined. `cpu_stats` has no stall accounting beyond `cycles_on_wrong_path` (`inc/core_stats.h:10-45`).

Sniper, the roadmap's SMT reference, does have one:

- Every physical cycle, `doDispatch` charges exactly one CPI component per context: `cpiBase`, `cpiSMT`, `cpiBranchPredictor`, `cpiSerialization` or `cpiRSFull` (`snipersim .../rob_smt_timer.cc:153-160, 680-738`).
- `cpiSMT` is charged only when a context could have dispatched but its sibling had the cycle.

The scout warns that `cpiSMT` is not total SMT interference (`2026-09-21-sniper-scout.md:111`). None of this reached the roadmap.

Independently added stall counters at different stages overlap and double-count, so a per-thread slowdown could not be decomposed into PRF, ROB, fetch or sibling effects. Per-thread SMT accounting is itself a research problem (Eyerman and Eeckhout, "Per-Thread Cycle Accounting in SMT Processors", ASPLOS 2009).

**Recommendation.** Define one exclusive per-context accounting over dispatch or retire slots, where every cycle × width slot goes to exactly one category, one of them "held by sibling". Make "categories sum to cycles × width" an exit check. Document that the sibling category is not the SMT slowdown, which still comes from standalone runs.

### 7.10 The trace format is global, so mixed-format pairs are refused or misread

`--trace-version` and `--cloudsuite` are single values applied to every positional trace (`src/main.cc:80-81, 365-366`). Formats are not self-describing. The only guard is a filename convention (`src/tracereader.cc:103-121`, commented "a convention check, not a format check").

The local corpus is split by format: SPEC06/17 and GAP exist only as v1, and SPEC26 only as v2.1. The fork's own protected set mixes formats too: "14 SPEC26 workloads plus the v1 mcf control" (`tools/perf/README.md:186`).

- **Conventionally named mixed pairs are refused.** mcf (v1) with sqlite (v2) fails at startup under either flag value.
- **Misnamed traces are silently misread.** GAP traces are named `*.trace.gz` and Google traces `*.champsim.gz`, matching neither convention. Run under `--trace-version 2`, as a pair with a SPEC26 trace would force, `bc-0.trace.gz` exits 0 with IPC 0.0540, against 0.2627 under the correct v1.
- **The output cannot record per-trace formats.** `[meta].trace_version` is a scalar (`src/main.cc:417`).

The same limitation exists in today's multi-core builds, but SMT makes multi-trace runs the normal case. Make the format and the CloudSuite flag per trace or per context, apply the name checks per trace, record each context's format in the output, and add mixed-format and misnamed-file cases to the Stage 4 exit.

## 8. Parity, validation and process [High for 8.1]

### 8.1 Name the repository's parity gate

Stage 1 asks that "cycles, retired instructions, branch counts, and memory/cache events" match "the recorded baseline" (line 110). Stage 3 says "one-context parity still passes" (line 128). The repository already has a stricter gate:

- `tools/perf/compare_optimization.py` hashes all of `[phase]` and `[config]` plus the warmup and ROI instruction and cycle counts, and refuses any difference (`:16-22, 84-85`). `--regression` adds a 16-case matrix.
- A change to ROB traversal also needs the 5M/50M long gate on 14 SPEC26 workloads (`tools/perf/README.md:184-189`). Stages 1 and 3 are both ROB-traversal changes.

Four gaps remain:

- **Module coverage.** The gate's configurations exercise perceptron or bimodal with `basic_btb`, and no prefetcher except `next_line` in four of the 16 `--regression` cases. Stage 1 re-points the modules that read `input_queue`, so add module overlays through the gate's repeatable `--config`: `ittage_64kb`, `cbp6_tagescl64`, `perfect_btb` and one prefetcher. Also run the payload build (`CHAMPSIM_TRACE_MEMORY_VALUES=1`) suite.
- **Ramulator2.** The gate forces `dram-model = legacy` (`tools/perf/benchmark_ptw.py:67`), so Ramulator2 parity needs a separate check.
- **Stage 2.** It has no parity requirement at all (line 118), although it rewrites translation. vmem frame assignment depends on call order: the PTW allocates CR3 in its constructor (`src/ptw.cc:39`), and later frames go out in walk-completion order. Any change to when vmem is called shifts every later frame. Add bit-exact parity to Stage 2's exit.
- **Multi-core.** One-context-per-core multi-core runs sit outside every parity check. The gate runs one trace and parses only `CPU 0` lines (`tools/perf/benchmark_ptw.py:41-49, 67-73`). The Catch2 suite is compiled with `num_cpus = 1`. The only real two-core run is `buildcheck.yml`'s weekly cron job, which asserts document structure, not numbers (`.github/workflows/buildcheck.yml:3-6, 74, 84-100`). Yet Stages 1 and 4 rewrite code a two-core binary executes: `do_cycle`'s refill, `do_phase`'s completion and livelock handling, `main.cc`'s trace-count check and the printers. Add a two-core, one-context-per-core parity check to the Stage 1 and Stage 4 exits by extending the gate to several traces and `CPU n` lines.

### 8.2 Parity versus new keys and behaviour changes

**New keys and output lines break the gate.** Consulting new keys (`threads`, quotas) changes `[config]` and `build_id` for unchanged one-context runs, and the gate then reports NON-INERT by design. `benchmark_ptw.parse_counts` requires exactly one `Warmup finished CPU 0 …` and one `Simulation finished CPU 0 …` line (`:41-49`). A per-context line reusing that prefix breaks KIPS measurement; a differently worded one lets the harness silently read the wrong line.

- Add a projection mode that hashes `[config]` and `[phase]` minus a declared allowlist, and asserts the allowlisted keys hold neutral values.
- Keep one-context stdout byte-identical, and give per-context lines a distinct prefix.
- Test `tools/perf` by hand; `make pytest` does not reach it.

**"Complete loads through explicit waiters" (Stage 2, line 114) is not timing-neutral for one context.** Core loads carry no waiter list: `execute_load` never fills `instr_depend_on_me` (`src/ooo_cpu.cc:632-644`). The L1D return completes every issued LQ entry in the same virtual block (`:745-756`), which also acts as a same-context merge:

- Load B to block X may still be in the RQ, the translation stash or the tag-check pipeline when load A's response arrives. It completes then.
- B's own later response matches nothing, but still uses a unit of `L1D_BANDWIDTH`.

Per-waiter completion would delay B until its own response and change one-context cycle counts, which Stage 3's parity requirement would then reject.

- The minimal fix is a block match filtered to the owning context, which is bit-identical with one context. Packed ASIDs (§5.3) provide it for free.
- If per-waiter completion is wanted, land it as a separate single-context change with its own re-baseline.
- Match on both `instr_id` and block, as the L1I path does (`:724-725`). An instruction with two memory sources creates two LQ entries with the same `instr_id` (`:525-528`), and merged waiter lists are set-unioned, so neither key works alone.

### 8.3 Characterize before refactoring

Several paths the refactor touches have no test:

- No unit test exercises SQ-to-LQ forwarding or the `ROB.front()` store-drain boundary.
- L1D block-match completion and `LSQ_ENTRY::finish` run only implicitly (in `121-dib-inorder.cc`), and nothing asserts their results.
- LQ slot-order load issue and the PRF-break quirk are unpinned.

The repository's own practice for risky ROB work is to write characterization tests first and verify them on the unchanged code (`docs/research-log/Performance/2026-09-14-performance-optimization.md:967-980`). Add a Stage 1a with those tests. The prefixes 253 and 254 are free. Choose new prefixes that match no existing file: `TEST_NUM` already exits 2 having run nothing for 601 and 703.

### 8.4 Stage 1's touch points are incomplete

The extraction moves members that other code uses directly:

- 17 Catch2 files touch them: 100, 120, 121, 122, 140, 141, 150, 151, 179, 180, 200, 201, 250, 251, 252, 300 and 502.
- 10 module sources read `intern_->input_queue` (30 references). The five `perfect_group` shells reach it through the shared header.
- `src/champsim.cc` is the only core-side writer of `input_queue`.

Split the extraction into two commits:

1. A behaviour-free commit that introduces a context-0 accessor API and migrates the tests and modules. Gate: byte-identical output.
2. The extraction itself, with the tests untouched.

Avoid reference-member aliases on `O3_CPU`: cores live in a `std::vector` (`src/static_environment.cc:421-426`), and a move would leave the aliases dangling.

### 8.5 Two-context harness gaps

- Test helpers leave `instr_id` at 0 (`test/cpp/src/instr.cc`), while L1I returns and LSQ ownership route by `instr_id`.
- `release_MRC` releases by address only (`test/cpp/src/mocks.hpp:165-173`).
- The core never checks ID uniqueness, so two contexts with hand-set IDs would cross-complete or hang.

Drive contexts through tracereader lambdas, as tests 081 and 502 do. Assert relative rather than absolute IDs, because CI runs `--order rand`. Add a mock that releases by (context, address), and consider a debug check that IDs strictly increase within each context.

### 8.6 Always-on identity invariants

The Stage 2 and Stage 4 exit evidence consists only of constructed scenarios. Scarab checks identity at many hand-offs with asserts that stay on in normal runs (for example `scarab src/memory/memory.c:2409-2410`, `src/cmp_model.c:341-344`).

- Add `CHAMPSIM_ASSERT` checks that context or ASID agree at LQ completion, forwarding, ROB memory completion, DIB hits, translation completion, register-producer lookup and the TLB/PTW hand-off.
- Make full two-trace runs on real traces, in a release build, part of Stage 4.
- Never run correctness campaigns on fast builds, which compile these assertions out.

### 8.7 Host speed

Stages 1 and 3 rewrite the loops that take 21–32% of sampled cycles (schedule, execute, complete; `docs/research-log/Performance/2026-09-15-linux-perf-after-cache.md:3-5`). Optimization 12 lost 2.8–8.1% KIPS per pair from a small amount of bookkeeping in the completion scan (`2026-09-14-performance-optimization.md:950-1016`). A runtime context count would put every one-context user on the loop-over-contexts path.

- Consider a compile-time maximum context count, set through a CPPFLAGS policy macro as `CHAMPSIM_TRACE_MEMORY_VALUES` is. An edit to `defs.h` does not enter the build fingerprint (`config/build_config.py:336`).
- Specialize the one-context case with `if constexpr`, and keep a runtime active count no larger than the maximum.
- Set the acceptance rule before measuring: fast builds, the paired 1M/3M campaign, and reject if all 12 pairs slow down or any trace's median loss exceeds a declared tolerance. The roadmap's "without inventing an acceptance percentage before measuring the baseline" defers exactly this.

### 8.8 User-trippable errors must throw and name the key

Quota derivation, minimum-quota checks and module capability checks must throw during `static_environment` construction and name the responsible key, like the `configured_*` helpers (`src/static_environment.cc:146-173`). That way `--knobs` reports them and fast builds keep them. Pin them in the invalid-geometry case of `501-static-environment.cc` and in `test/python/test_operational_config_errors.py`.

### 8.9 The three identities are numerically equal in the milestone shape

With one core and two contexts:

- `main.cc` passes trace index i as each reader's `cpu` (`src/main.cc:364-366`);
- that becomes `asid {i,i}` (`inc/instruction.h:292`);
- `trace_index` is the identity mapping (`src/main.cc:373-375`);
- the physical core is 0;
- the test helpers build everything as cpu 0 (`test/cpp/src/instr.cc`, `inc/core_builder.h:37`).

So context c = trace c = ASID c, and context 0 = physical core 0. Code that uses the context index where the ASID or the trace index is meant would pass every planned test.

Code that uses the context index where the physical core is meant is also invisible: vmem keyed by cpu 1 even yields a distinct address space. Only native Ramulator's range check would catch it; under legacy DRAM the result is out-of-bounds indexing in SHiP and DRRIP.

**Recommendation.** Require non-identity mappings in at least one Stage 2, 3 and 4 test and one end-to-end case:
- a permuted context-to-trace mapping (`502-phase-order.cc:68` already uses `{0, 0}`);
- ASIDs from a range disjoint from context and core indices;
- an SMT core built with a nonzero physical index, as `141-dib-stats.cc:284` does.

Have the identity contract name the one function that derives each identity, so a reviewer can grep for bypasses.

### 8.10 Replace "deterministic reruns" with relabeling and the right sanitizers

The simulator is deterministic by construction today: one host thread, seeded RNGs, no order-affecting unordered or pointer-keyed containers. Rerun equality (Stage 5, line 150) is still a cheap guard against nondeterminism introduced by new code.

The SMT-relevant invariance, though, is relabeling. Rotating priority must start somewhere, `do_cycle` refills contexts in index order, and first-touch order decides frame placement. Run each pair as both (A,B) and (B,A) and report the per-context difference against a stated tolerance.

For modules replicated per context, the likely failure is an uninitialized read. Vendored predictors rely on static zero-initialisation (see the CBP6 adapter notes in CLAUDE.md), and a per-context heap copy loses it. ASan and UBSan do not detect uninitialized reads; use MSan or Valgrind memcheck for the replicated-module sweep, run long enough to reach each module's deep paths.

### 8.11 Contexts must not add operables

`do_cycle` sorts operables by `current_time` with `std::sort` (`src/champsim.cc:44-45`). libstdc++ keeps equal elements in their original order only up to 16. The core, caches and PTW all default to 4000 MHz, so they compare equal on every tick.

The one-core milestone has 10 operables, so approach 1 is safe. Private per-context TLBs or PTWs would add operables, and so would approach 2. Past 16, the core can run after its L1D within a tick, changing request timing in a way that would look like an SMT effect.

State that hardware contexts add no operables and pin the count and order in `501-static-environment.cc`. Before any configuration crosses 16, switch to `std::stable_sort` or a (time, canonical index) key as a separate, documented change, since that alters existing multi-core results.

### 8.12 State an upstream policy

Approach 1 rewrites every stage function in `ooo_cpu.{h,cc}`. The fork is already far from upstream: 372 files changed since the upstream base `51588e1d`. Upstream still touches the core, with 10 commits to the `ooo_cpu` files in the 17 months before that base.

The roadmap weighs divergence only for approach 3 (line 94). Say whether upstream is still tracked:
- If it is, sync before Stage 1 and keep the extraction a mechanical move (the same member names under a context object, formatting in a separate commit), so upstream patches port by rote.
- If it is not, record that the core is permanently diverged.

## 9. Approach, phasing and scope

### 9.1 The approach comparison reaches the right conclusion for the wrong reasons [High]

The roadmap rejects composing several `O3_CPU`s (approach 2) because "each instance already owns bandwidth budgets, resources, modules, and lifecycle. Preventing duplicated capacity and inconsistent clock/phase behavior would require invasive coordination" (line 93). Against the code, most of that does not hold for milestone 1.

**What does not hold:**

- **Clock.** One global clock drives every operable (`src/champsim.cc:96`; `src/operable.cc:26-34`). The only clock hazard is that each instance reads its own `ooo_cpu.cpuN.frequency`, which an equality check closes.
- **Phases and lifecycle.** Trace refill, `phase_complete`, livelock, the `end_phase` broadcast, ROI snapshots and per-CPU statistics are all keyed per `O3_CPU` (`src/champsim.cc:53-59, 94-166`; `src/ooo_cpu.cc:77-89`). Composition inherits them, provided `cpu` means context. Approach 1 has to rebuild them per context in Stage 4.
- **Resources and modules.** The milestone's own policies (fixed ROB/LQ/SQ/PRF quotas, lines 71-74; replicated predictors, line 80) are exactly two instances with halved sizes.

**What composition gets from existing code:**

- vmem separation keyed by `cpu`;
- the PTW computing translations from `mshr.cpu`;
- per-instance SQ and L1D channel, so VA-block load completion never sees the sibling's responses (`src/cache.cc:433` returns each response to the channel the request arrived on);
- per-context cache counters;
- `require_single_core` rejecting cpu 1, although only at initialization;
- the 14 `input_queue`-reading modules staying correct.

**What composition cannot do**, and what actually justifies approach 1:

- a shared PRF and a shared scheduler with per-context watermarks (§1, §2);
- dynamic ROB/LSQ sharing in Stage 5;
- keeping `cpu` physical. Under composition `NUM_CPUS` counts contexts, so Ramulator's per-core counters count contexts too.

Composition also needs more than a wrapper:

- shared width pools, because every stage builds a local `champsim::bandwidth` and does not report what it consumed;
- a shared DIB handle;
- per-channel L1 tag bandwidth, which halves when an L1 gains an upper channel (§3);
- rotation of instance order, or cpu0 claims every shared pool first on every tick;
- TLB qualification anyway, because a shared L1D has one `lower_translate`.

**The consequence.** Under the roadmap's milestone policies, milestone 1 differs from a composition model only in shared widths, the DIB and the scheduler scan. Most of the refactor's payoff would arrive only in Stage 5. Adopting the shared PRF and scheduler of §1 and §2 in milestone 1 removes this problem and makes approach 1 necessary.

**Recommendations.**

- Keep approach 1, but rewrite the comparison to give the real reasons.
- Draw Stage 1's extraction boundary for the shared-resource end state. Per-context state is the RATs, the ROB/LQ/SQ order, the frontend queues and stall state. The core owns the PRF, the free list and the capacity counters, with per-context accounting. Fixed quotas then become a policy over shared storage, not separate storage that Stage 5 must undo.
- Consider a composition build as a cheap early experiment (§9.4). In its fixed-quota mode, approach 1 should reproduce the composition model's `[phase]` statistics under the same arbitration order. That differential oracle is stronger than Stage 3's scenario tests.

### 9.2 Stage 2's exit needs Stage 3's core

Stage 2 keeps a one-context executable (line 114), yet its exit requires showing that two contexts cannot forward stores to each other, complete each other's loads, or share DIB entries (line 118). Those checks need two contexts' instructions inside one `O3_CPU`, with responses routed to the owning context's ROB. That is Stage 3 machinery (line 124).

"Memory and module work can be developed independently" (line 120) holds for the channel, cache, PTW and vmem plumbing, not for the core LSQ and DIB. The DIB change is not in any stage's touch points.

Either move those checks to Stage 3, or require Stage 1 to deliver a test harness that can hold N contexts.

### 9.3 Stage 4 is too big to review, and its failures cannot be attributed

Traces reach a core only through `do_cycle`'s refill (`src/champsim.cc:53-59`), and Stage 3 feeds its harness by hand (line 124). So Stage 2's memory identity and Stage 3's arbitration first meet real TLB, PTW, prefetcher and DRAM traffic in Stage 4. Line 120 says as much.

The same stage also rewrites:
- the CLI and the trace-count check;
- the context-count knob;
- phase, ROI, EOF and repeat semantics;
- the statistics schema and the heartbeat;
- progress and deadlock detection.

Its exit list has about 17 items (line 142). The first two-trace hang or implausible IPC would be diagnosed with a deadlock detector and statistics that are being rewritten in the same change, which makes the cause hard to attribute.

Split it into two stages:

- **4a, integration.** A context-to-trace table in `do_cycle`, and today's per-CPU phase semantics applied per context, with no schema change. Exit: two real traces run for millions of instructions through `static_environment` in a release build, with assertions on and the unchanged deadlock guard. A Catch2 test in the style of 501 that drives `do_phase` would serve.
- **4b, interface and measurement.** The CLI, the knob, the measurement contract, EOF and repeat handling, the output schema and per-context progress detection.

### 9.4 Measure before freezing the decisions that need data

Decisions 1 (quotas, minimum PRF), 2 (arbitration) and 5 (fidelity) cannot be settled by reading code, yet they are to be frozen "before implementation" (line 176). No stage produces real-trace SMT data before Stage 4. Approach 3 is kept "for a throwaway policy experiment" (line 94), but no stage schedules one.

Part of the answer needs no code at all. A one-context run at half resources, set with `--set`, bounds one context under a fixed partition from above. On 605.mcf (1M/3M):

| Setting | IPC | Change |
|---|---|---|
| Baseline | 0.2636 | — |
| `register_file_size=64` | 0.1472 | −44% |
| `rob_size=176` | 0.2639 | none |
| `lq_size=64`, `sq_size=36` | 0.2638 | none |

That is enough to see that the PRF split alone decides the answer. Performance is not monotonic in resources, so this is a bound only for one context beside an idle sibling.

Add a Stage 0.5 measurement step before freezing Decisions 1, 2 and 5:

- half- and quarter-resource `--set` sweeps over the SPEC26 v2.1 set and the v1 controls, covering PRF, ROB, LQ/SQ, the frontend buffers and `scheduler_size`;
- optionally, a throwaway two-context spike (approach 3 with VA-folded ASIDs, §5.3), run on a few real pairs to compare a shared PRF against a fixed split. A composition build (§9.1) cannot model a shared PRF, but it gives the fixed-split reference cheaply.

Record the data with the roadmap.

### 9.5 A suggested restructure

- **Stage 0.5, corrections and measurement.**
  - The defects in §10 each land as a separate commit, run through `compare_optimization.py`. Covered: rejection of empty traces; fatal vmem exhaustion when there is more than one address space.
    - The empty-trace fix changes no run the standard matrix makes, so it must pass it as inert.
    - The running list is `BUGS.md` at the repository root.
  - Rebuild a trustworthy non-native baseline binary, at or after `ed01cca7`.
  - Run the measurement of §9.4.
  - From then on, label every stage item either inert (must pass the exact gate) or non-inert (lands alone with its own re-baseline). Explicit-waiter load completion is non-inert.
- **Stage 1a:** characterization tests on the corrected baseline (§8.3).
- **Stage 1b:** a mechanical context-0 accessor API, migrating tests and modules. Gate: byte-identical output (§8.4).
- **Stage 1c:** extraction into per-context state over core-owned storage (§9.1). Freeze the identity contract here, using non-identity test mappings (§8.9):
  - the ASID mechanism (§5.3);
  - the context index;
  - the dispatch sequence number (§2);
  - the budget-site map (§3);
  - the classification of statistics fields (§7.8);
  - compile-time or runtime context count (§8.7).

  Gate: `compare_optimization.py`, the long gate, module overlays, and a two-core parity run (§8.1).
- **Stage 2:**
  - the address-space mechanism across every translation surface, including fixed PTW;
  - the module capability trait at selection time;
  - a context handle on the hooks;
  - per-context ITTAGE and BLBP;
  - prefetch origin.

  Test with unit-level two-ASID cases (TLB, `finish_translation`, PSCL walk steps, fixed PTW). Gate: bit-exact one-context parity.
- **Stage 3:**
  - two runnable contexts;
  - a shared PRF with reserve (§1) and scheduler occupancy (§2);
  - per-context LQ and SQ;
  - arbitration defined per budget site, including L1 admission (§3);
  - LSQ and DIB isolation.

  Exit checks: the predictor equality test (§6.6), and width, occupancy and utilization invariants.
- **Stage 4a and 4b** as in §9.3, with two-core parity again at the end of 4b.
- **Stage 5:** as planned, plus the methodology in §7.4–§7.6 and §7.9, relabeling runs (§8.10), and the target core of §9.7.

### 9.6 The memory backends are over-scoped

For independent traces, nothing below the LLC needs to change:

- The LLC completes by physical block, and the native adapter by parent ID.
- Legacy DRAM has no `cpu` field at all (`inc/dram_controller.h:98-115`), so "retain physical `cpu` at both memory backends" (line 116) is vacuous for legacy.
- Native passes `cpu` to Ramulator as `source_id`, which indexes per-core row statistics sized by `NUM_CPUS` without a bounds check (`src/ramulator2_driver.cc:38, 428, 439-445`). That is why the existing range check is a memory-safety guard.

**Recommendations.**

- State that the LLC→DRAM edge carries no context identity and never needs to.
- State that DRAM statistics stay per physical core, so per-context row-buffer interference cannot be measured. Either say so, or add a stats-only tag.
- The concrete backend-test work: `test/ramulator2/run_integration.py` counts cores from `--knobs` and passes `[trace] * cores`, so it must count contexts instead. Add one smoke run per backend with one core and two contexts.
- Legacy accepts an unset `cpu` silently, while native throws. A debug `CHAMPSIM_ASSERT(packet.cpu < NUM_CPUS)` at channel admission would make both backends catch it.

### 9.7 Choose a documented SMT target core

The roadmap names no target machine. The only modelled core, `configs/lnc.toml`, is a Lunar Lake Lion Cove. That part ships with the Hyper-Threading hardware removed, and Arrow Lake's Lion Cove also ships without it. So there is no disclosed SMT partitioning to follow and no hardware to compare against. `lnc.toml` also marks its LSQ and register-file sizes as undisclosed and contains hand overrides (`:48-49, 59, 66`).

Add an SMT reference configuration derived from a documented SMT2 core, tagging each value disclosed, derived or default in the `lnc.toml` style. Good candidates are Zen 4 or 5 (AMD's SOG Table 4 lists every resource as statically partitioned, watermarked or competitively shared) or Golden or Redwood Cove. Label any SMT-on-LNC result as counterfactual.

### 9.8 The execution model has no port or unit classes

There is one generic `EXEC_WIDTH` and a uniform `EXEC_LATENCY` (`src/ooo_cpu.cc:479-497`), and `lnc.toml` sets `execute_latency = 0`. Two FP-heavy threads therefore never contend for specific ports, so same-class compute pairs are likely optimistic. Label them as such until Decision 5's port model exists.

`instr_type` exists in `ooo_model_instr` only under `CHAMPSIM_TRACE_MEMORY_VALUES` (`inc/instruction.h:129-146`), and whether v2 traces populate it reliably is unverified.

### 9.9 The reference-simulator evidence is thinner than it looks

**Sniper as a cross-check.** The planned Sniper comparison (line 152) mostly shares the model's simplifications or differs structurally from it:

- the fork's ChampSim reader accepts only 64-byte v1 records, compressed as gz, xz or bz2, so it cannot read this branch's v2 or `.zst` traces (`snipersim common/trace_frontend/sift_reader.cc:138-147, 735-742`);
- it marks every branch non-indirect and counts only direction mispredicts;
- its Nehalem contention model caps issue at 5 uops per cycle even under the 8-wide `meteor_lake_pcore` configuration;
- its standalone model is a different class (`RobTimer`) from its SMT model (`RobSmtTimer`);
- its RS and load/store slots are shared without per-thread caps;
- its TLBs are private per context.

A default Sniper run also uses `--sim-end=first`, a different end policy from the Stage 4 contract. The closest counterpart is `--sim-end=last-restart` with `scripts/stop-by-icount-percore.py`, extended to snapshot per-context statistics.

Sniper misconfiguration is silent. A non-ROB model with `logical_cpus > 1` builds independent pipelines that share the L1, and the fork's `meteor_lake_pcore.cfg` pins `logical_cpus = 1`. Require evidence that `RobSmtTimer` actually ran.

Treat the cross-check as weak evidence. A trend check on real SMT hardware would complement it, if a machine with SMT enabled and aligned trace regions are available; that feasibility was not established.

**Scarab.** Scarab contributes:

- a negative result: no SMT on any of its 41 branches;
- an address-namespace precedent;
- counterexamples for lifecycle and global state: finished cores are skipped, memtrace filters are global, and a global cursor selects each core's state;
- a wrong-path lesson: Scarab fetches and executes the wrong path by default (`scarab src/core.param.def:203`), which is the contrast §4 draws.

It is not a recovery reference, because ChampSim never squashes, so the "useful ... recovery reference" framing (lines 14, 40) overstates it.

**No SMT policy literature was consulted.** gem5's O3 CPU is one example the scouts did not cover. The literature supports the chosen baseline: gem5 O3 defaults to round-robin fetch and commit and partitioned ROB and LSQ, and Intel partitions the ROB and load/store buffers. So the gap is in justification, not a wrong baseline. Cite it before freezing Decisions 1 and 2, and name ICOUNT as a Stage 5 comparator.

## 10. Defects that exist today, independent of SMT

The open ones, and the further defects found while investigating them, are tracked in `BUGS.md` at the repository root. That file lists only open bugs, and an entry is deleted when its fix lands.

### 10.1 Empty or sub-record traces segfault

`bulk_tracereader::operator()` calls `instr_buffer.front()` after a refill that read nothing (`inc/tracereader.h:144-151`). `eof()` is false before the first read, so `do_cycle` calls it.

Reproduced:

- exit 139 for an empty file, with and without `-i`;
- exit 139 for a valid empty `.xz` container (a zero-byte `.xz` file already fails cleanly with `truncated xz stream`);
- exit 139 for a 10-byte file;
- gdb places the fault at `tracereader.h:150`, called from `do_cycle`;
- without repetition, a 5-record trace ends the phase on its first cycle, with 0 instructions retired and exit 0.

Stage 4's "empty/short ... traces behave deterministically" needs these rejected with a named error first.

### 10.2 vmem exhaustion silently aliases pages

When the free list empties, `ppage_pop` refills it with every frame without unmapping anything (`src/vmem.cc:122-130`) and prints one line to stderr. Multi-core runs already share this pool; SMT doubles the footprint per core. The default legacy capacity is 16 GiB, so SPEC pairs are unlikely to exhaust it, but test fixtures use 16 MiB.

Make exhaustion fatal, at least when there is more than one address space, or record it in the output.

### 10.3 Smaller items

- `--hide-heartbeat` sets `O3_CPU::show_heartbeat`, which nothing reads (`src/main.cc:336-339`).
- `matches_id(const T&)` returns `precedes(...)` (`inc/instruction.h:73`): an unused but wrong overload.
- `do_sq_forward_to_lq` is declared and never defined (`inc/ooo_cpu.h:182`).

### 10.4 Local environment

`bin/champsim` and `test/bin/000-test-main` point at native builds whose RUNPATH is a deleted scratch directory. Both exit 127 with `libramulator.so: cannot open shared object file`.

- Because the resolved targets exist, the binary-driven suites in `make pytest` will run and fail instead of skipping.
- The non-native canonical binaries under `bin/x86_64-linux-gnu/…` do start, but they were built minutes before the last commit and record no source commit.

Rebuild a baseline before Stage 1.

## 11. Corrections to factual claims

| Claim (location) | Status | What is actually true |
|---|---|---|
| `channel.h` "carries `cpu` and small ASIDs on requests" (roadmap line 56) | Misleading | The field exists but no core request sets it, and cache constructors drop it (§5.1) |
| "Trace repetition remains explicit" (line 138) | Wrong | `-i` turns repetition on (`src/main.cc:365`), so the any-EOF stop is unreachable on every `-i` run (§7.3) |
| `do_phase` "ends on any EOF" (line 59) | Incomplete | True only without `-i`. `champsim.cc:90` is a comment; the logic is at `:91-167` |
| Composition would need "invasive coordination" to avoid "inconsistent clock/phase behavior" (line 93) | Wrong | One global clock drives every operable, and the phase machinery is per `O3_CPU`, so composition would inherit it (§9.1) |
| Input queue "capacities explicitly partitioned from configured totals" (line 70) | Wrong for the input queue | `IN_QUEUE_SIZE = 2 * FETCH_WIDTH` is derived, not a knob (`inc/ooo_cpu.h:284`). Six adapters need at least `FETCH_WIDTH + 1` entries to read the successor PC |
| Decode "must also consume one aggregate budget" (line 88) | Incomplete | Decode consumes two budgets, and `FETCH_WIDTH` is spent at four sites (§3) |
| Stage 4 touch point "heartbeat/deadlock listeners" (line 136) | Wrong | The only listener is Heartbeat (`inc/event_listeners.h:12`); deadlock and livelock checks are inline in `do_phase` |
| "Retain physical `cpu` at both memory backends" (line 116) | Vacuous for legacy | Legacy DRAM carries no `cpu` (`inc/dram_controller.h:98-115`) |
| Validation row 3: `801-vmem-duplicated.cc`, `250-load-scheduling.cc` | Weak neighbourhood | 801 uses only cpu 0 and `get_pte_pa`; 250 checks issue, not completion. `601-fixed-ptw.cc:166-180` already checks that cpu 0 and cpu 1 get different frames; also see 600, 406, 411, 412, 415 |
| Validation row 4: `251-execution-lsq-readiness.cc` | Off-topic | It tests LSQ timestamp ownership, not waiting on a miss |
| Validation row 5: "tests to add" | Incomplete | `300-retire-from-rob.cc` already covers in-order retirement and bandwidth |
| Validation row 8: 179, 184 | Incomplete | The only guard test is `177-cbp6-host-protocol.cc:168-176`. 184 does exercise `install_branch_module`, the seam `select_modules` uses |
| Validation row 9: 084, 708 | Weak | 084 is a 13-line check. 708 covers native teardown only and is skipped without a native build. The real pins are `502-phase-order.cc:81-105`, `085`, `703-memory-backend.cc:244` and `705-ramulator2-backend.cc:481-579` |
| Scarab README "No SMT" at `README.md:54` (roadmap line 38); scout cites `:49` | Off by one / wrong | `:54` is the "uArch Limitations" heading; "No SMT" is at `:55`, and "No cooperative multithreaded code" at `:52` |
| Scarab as a "recovery reference" (line 14) | Overstated | ChampSim never squashes; Scarab's recovery has no counterpart. `Thread_Data`/`Map_Data` is a fair inventory of per-context dependency state |
| Sniper `rob.cfg` quotation (Sniper scout) | Composite | `logical_cpus = 2` comes from `smt2.cfg:2`. The quote omits `rob.cfg:20-21`, `[perf_model/l1_dcache] outstanding_misses = 10`, which caps L1D misses in every ROB run |
| Sniper's "event-driven coordination" (line 34) | Ambiguous | With more than one context the SMT timer steps every cycle and never skips (`rob_smt_timer.cc:1171-1175`); only host coordination and lifecycle hooks are event-driven |
| Sniper "specialized MMU designs were not exhaustively audited" (Sniper scout) | Moot | The MMU factory accepts only `"default"` (`mmu_factory.h:17-24`); `memory_manager.cc:96` should be `:98` |
| Sniper address-space isolation (both documents) | Missing | Sniper tags every address with an application ID at ingest (§5.3); neither document reports this |

Most other citations in the roadmap's impact table are accurate: `main.cc:155`, `champsim.cc:53`, `ooo_cpu.h:114`, `register_allocator.h:19`, `instruction.h:54`, `tracereader.h:67`, `ooo_cpu.cc:522/561/745/761/904`, `vmem.cc:134`, `cache.cc:683`, `ittage_64kb.cc:38`, `cbp6_host.h:73`, `ramulator2_memory_backend.cc:78`, `ramulator2_driver.cc:425` (the check is at `:428`), and the Sniper dispatch, issue, commit and partition citations.

## 12. Turn "Decisions to freeze" into a register

The roadmap's list (lines 176–184) does not say which items the body has already answered, or when each must be frozen. Parts of Decision 2 (the arbitration shape, line 88) and all of Decision 5 (keep the abstract scheduler, line 77) are already proposed in the body. Decision 4's timing is set twice, "before implementation" (line 176) and "in the stage design" (line 138). Meanwhile the ownership choices that set Stage 1's extraction boundary are made in the table (lines 71–73, 80) without being flagged as decisions.

Make the list a register: one row per decision, marked either decided (quoting the body text that decides it) or open, with the stage whose design must freeze it. The proposed contents, with this review's recommended direction for each:

| # | Decision | Recommended direction | Freeze before |
|---|---|---|---|
| 1 | Resource ownership: core-owned PRF, free list and capacity counters with per-context accounting, or separate per-context storage | Core-owned storage (§9.1) | Stage 1 |
| 2 | PRF policy: shared pool with a dynamic reserve, or fixed split; the undersized-quota diagnostic | Shared, with fixed as a knob (§1) | Stage 1 design, after Stage 0.5 data |
| 3 | Scheduler model: shared occupancy with a watermark, or a static split; the cross-context age key | Shared occupancy plus a dispatch sequence number (§2) | Stage 3 |
| 4 | Arbitration per budget site: rotation, work conservation, retirement policy, eligibility of a frozen context, L1 admission | Site table (§3, §4) | Stage 3 |
| 5 | Address-space mechanism, and the surfaces it must cover | Cost the packed ASID first (§5) | Stage 1 (identity contract) |
| 6 | Load-completion semantics | Context-filtered block match (§8.2) | Stage 2 |
| 7 | Cache-side sharing: shared or per-context core→L1 channels; MSHR, RQ and translation-stash partitioning | Explicit, independent of wiring (§3). One context's burst of TLB misses can fill the L1D translation stash, gated once per cycle against `MSHR_SIZE` (`src/cache.cc:487-489`), and block every untranslated load of its sibling | Stage 3 |
| 8 | TLB and walker policy, and a walker concurrency bound | The detailed PTW never enforces `mshr_size` (`src/ptw.cc:182`); only fixed mode does | Stage 2 |
| 9 | Module contract: capability trait, context handle, capability matrix, storage accounting, milestone-1 module set | §6; excludes `lnc.toml`'s pair | Stage 2 |
| 10 | Measurement: standalone reference, interval endpoints, shared-structure ROI, EOF and laps, pairing and selection, placement noise floor, cycle accounting | §7 | Stage 4a (semantics), 4b (output) |
| 11 | Trace format per context | §7.10 | Stage 4b |
| 12 | Shared code frames for same-binary pairs | Out of scope; state the assumption (§7.6) | Stage 2 (must not be precluded) |
| 13 | Context count: compile-time maximum or runtime, and the host-speed acceptance rule | §8.7 | Stage 1 |
| 14 | Target core for SMT reference parameters | Zen 4/5 or Golden/Redwood Cove (§9.7) | Stage 5 |
| 15 | Intended fidelity (roadmap Decision 5), extended with wrong-path absence and class-less execution | §4, §9.8 | Stage 5 |
| 16 | Upstream policy | §8.12 | Stage 1 |

## 13. What the roadmap gets right

- It chooses approach 1. That is right, though for reasons it does not state (§9.1): only approach 1 can build the shared PRF, the shared scheduler and the shared width pools while keeping `cpu` physical.
- It keeps one ordered ROB per context. Prefix retirement (`src/ooo_cpu.cc:763-764`), `LSQ_ENTRY::finish`'s `partition_point` (`:886`) and the `ROB.front()` store-drain boundary (`:565`) all depend on per-context order.
- Private RATs are required: they are 256-entry arrays indexed by architectural register.
- Keeping the global `instr_id` as a correlation token, and preserving program order only within a context, is correct. IDs are unique across readers (`inc/tracereader.h:34, 70`) but give no age order across contexts.
- It correctly identifies the three LSQ hazards, the `finish_translation` hazard and the VM-key hazard, and that fixing VM allocation alone is not enough.
- It correctly finds that `require_single_core` keys only on the physical core, so two contexts on core 0 evade it.
- It chose synthetic translation over recorded physical addresses. Sniper uses recorded PAs whenever a trace has them (`trace_thread.cc:224-229`), which is exactly the overlap hazard.
- Keeping `cpu` physical at the Ramulator boundary is a memory-safety requirement (§9.6), not a style choice.
- A common warmup barrier is the only workable design, because `warmup` is one flag per operable and a shared backend cannot be warming for one context while measuring the other.
- It warns not to let an early finisher disappear (Scarab does exactly that, `scarab src/sim.c:754`). It refuses to sum IPCs over different intervals, and plans weighted speedup and per-thread slowdown.
- It is honest that Scarab has no SMT, confirmed across all 41 of its branches, and that `NUM_BPS` is not SMT.
- The Sniper summary of dispatch, issue, commit and ROB partitioning matches the code, including the stale "interleaved" comment and the per-context fallback issue and commit budgets.
