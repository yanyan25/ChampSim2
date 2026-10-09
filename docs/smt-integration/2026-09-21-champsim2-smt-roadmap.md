# SMT scouting and initial ChampSim2 roadmap

Date: 2026-09-21. Status: source-based reconnaissance and proposed development stages; no simulator implementation is included.

The user selected **independent traces sharing one physical core** as the first milestone. Each trace initially represents a distinct address space. Cooperating threads, shared-memory synchronization, migration, and OS scheduling belong to a later project.

## Evidence and scope

Two independent scouts examined Sniper and Scarab while the coordinating agent mapped the relevant ChampSim2 interfaces and cross-checked key findings. All three repositories lacked a `.codegraph/` directory, so investigation used direct source searches and reads. The checkouts were clean at the start.

| Checkout | Revision examined | Result |
|---|---|---|
| `snipersim` | `56505e42fd98bca863fac181e769bd3c98d2bb33` | Real SMT in the ROB performance model; useful policy and ownership reference |
| `scarab` | `47b2c7ae834b9a2edb0bbeb8abc3b4a5b83f224d` | This checkout does **not** implement SMT; useful single-context pipeline and recovery reference |
| `champsim-ramulator` | `81275433816490e911e650936f06b8365870be50` | One instruction stream per `O3_CPU`; several identities and ownership assumptions must be separated |

Detailed source inventories, call paths, policies, and limitations are in the [Sniper scout](2026-09-21-sniper-scout.md) and [Scarab scout](2026-09-21-scarab-scout.md). Source links point to the local checkouts at these revisions. Findings describe inspected code, not simulator runs or validation against hardware. No builds or simulations were run for this investigation.

## What the reference simulators actually provide

### Sniper: borrow the ownership model and explicit arbitration

Sniper models a physical core with multiple logical contexts sharing a `RobSmtTimer`. Each `RobThread` owns its ROB queue, dependency trackers, frontend stall state, and counters; the timer owns the core clock, issue contention model, RS occupancy, and outstanding load/store contention models. See [RobThread and shared timer state](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_performance_model/rob_smt_timer.h:54).

The implementation has several independently chosen policies:

- Dispatch selects one eligible context per cycle using rotating priority. It does not give every context a fresh dispatch budget. See [doDispatch](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_performance_model/rob_smt_timer.cc:674).
- Issue can visit multiple contexts in a cycle with `simultaneous_issue=true`; execution contention is shared. A stale comment describing interleaved multithreading is insufficient to characterize the executable behavior. See [doIssue](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_performance_model/rob_smt_timer.cc:1012).
- With issue contention disabled, the fallback issue-width counter is local to each context's scan, so aggregate issue can grow with context count. This is another reason to make ChampSim2's aggregate budgets explicit. See [tryIssue](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_performance_model/rob_smt_timer.cc:925).
- ROB storage uses per-context queues with a total-window partitioning policy; repartitioning among running contexts is optional. A separate deque per context does not imply a separate full-capacity physical ROB.
- Commit is independently in order within each context. Sniper's configured commit width is **per context**, and its loop counts uops. ChampSim2 should explicitly choose its own core-wide instruction retirement budget. See [doCommit](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_performance_model/rob_smt_timer.cc:1049).
- `smt2.cfg` only sets `logical_cpus=2`; the ROB model must also be selected. The knobs are documented in [rob.cfg](/home/rbera/work/alakazam/snipersim/config/rob.cfg:1) and [smt2.cfg](/home/rbera/work/alakazam/snipersim/config/smt2.cfg:1).

Sniper is a timing-model reference, not a specification to reproduce literally. Its event-driven coordination, host-thread locks, per-context commit budget, per-logical-context MMU/TLB ownership in this fork, and core-wide `last_store_done` have consequences that differ from the proposed ChampSim2 design. Its dependency tracking also does not substitute for ChampSim2's finite physical register allocator. The scout identifies these boundaries and statically inferred lifecycle hazards.

### Scarab: useful pipeline decomposition, not an SMT implementation

The README explicitly lists [“No SMT”](/home/rbera/work/alakazam/scarab/README.md:54). The executable structure agrees: [Cmp_Model](/home/rbera/work/alakazam/scarab/src/cmp_model.h:51) says one thread per core; [allocation](/home/rbera/work/alakazam/scarab/src/cmp_model_support.c:53) creates thread, frontend, backend, LSQ, and issue-queue state for each `NUM_CORES`; [model registration](/home/rbera/work/alakazam/scarab/src/model_table.def:34) exposes `cmp` and `dumb`.

Scarab's `Thread_Data`, per-operation ownership, dependency wakeup, stage-local recovery, and resource accounting are useful examples for defining ChampSim2 context boundaries. They are not evidence of hardware-thread arbitration. `NUM_BPS` represents alternative predictor/frontend paths, not SMT contexts. The scout also distinguishes frontend software-thread selection from simultaneous hardware execution.

Scarab's address separation illustrates the need for namespaces, but its [core-ID address encoding](/home/rbera/work/alakazam/scarab/src/globals/utils.c:673) should not be copied into ChampSim2 as an implicit thread/address-space mapping. An explicit address-space identifier is easier to reason about and preserves a path to cooperating threads later.

## ChampSim2 changes exposed by the scouting

| Area | Current evidence | SMT consequence |
|---|---|---|
| Topology and input | [main.cc](/home/rbera/work/alakazam/champsim-ramulator/src/main.cc:155) expects `NUM_CPUS` traces; [do_cycle](/home/rbera/work/alakazam/champsim-ramulator/src/champsim.cc:53) fills one input queue per CPU | Trace count and trace mapping must use hardware contexts, while physical cores and cache wiring remain separate |
| Pipeline ownership | [ooo_cpu.h](/home/rbera/work/alakazam/champsim-ramulator/inc/ooo_cpu.h:114) owns one set of frontend queues, ROB, LSQ, allocator, and fetch stall state | Extract context state while keeping one physical-core `operate()` and clock |
| Dependencies and registers | [RegisterAllocator](/home/rbera/work/alakazam/champsim-ramulator/inc/register_allocator.h:19) combines one frontend/backend RAT pair, PRF, and free list | Architectural register names require context-local RATs; total physical capacity must be bounded explicitly |
| Program order | [program_ordered](/home/rbera/work/alakazam/champsim-ramulator/inc/instruction.h:54) compares scalar IDs; [tracereader](/home/rbera/work/alakazam/champsim-ramulator/inc/tracereader.h:67) already assigns globally unique IDs | Preserve unique correlation IDs; distinguish ordering within a context from arbitration between contexts |
| Retirement and stores | [retire_rob](/home/rbera/work/alakazam/champsim-ramulator/src/ooo_cpu.cc:761) retires a single completed prefix; [operate_lsq](/home/rbera/work/alakazam/champsim-ramulator/src/ooo_cpu.cc:561) uses `ROB.front()` as a store retirement boundary | Each context needs its own in-order frontier; a stalled sibling must not impose global program order |
| Store forwarding | [do_memory_scheduling](/home/rbera/work/alakazam/champsim-ramulator/src/ooo_cpu.cc:522) matches stores by virtual address and instruction order | Independent contexts with the same VA must never forward to each other |
| Load completion | [handle_memory_return](/home/rbera/work/alakazam/champsim-ramulator/src/ooo_cpu.cc:745) completes issued loads matching a returned virtual block | Responses must identify their actual waiters; VA equality cannot identify a context |
| Address translation | [vmem.cc](/home/rbera/work/alakazam/champsim-ramulator/src/vmem.cc:134) keys mappings by `cpu_num`; [finish_translation](/home/rbera/work/alakazam/champsim-ramulator/src/cache.cc:683) matches untranslated entries by virtual page | Replace accidental CPU/address-space equivalence throughout VM, TLB, PTW, merging, and translation responses |
| Requests and responses | [channel.h](/home/rbera/work/alakazam/champsim-ramulator/inc/channel.h:48) carries `cpu` and small ASIDs on requests; responses lack context/address-space fields | Adding a request-side thread tag alone cannot make the return path safe |
| Predictor and BTB state | [ooo_cpu.h](/home/rbera/work/alakazam/champsim-ramulator/inc/ooo_cpu.h:195) has context-free hooks; [ITTAGE](/home/rbera/work/alakazam/champsim-ramulator/btb/ittage_64kb/ittage_64kb.cc:38) has static state and reads `input_queue` directly | Isolate histories/RAS and pending updates; audit adapters and pass context-local successor information |
| Existing module guards | [require_single_core](/home/rbera/work/alakazam/champsim-ramulator/inc/cbp6/cbp6_host.h:73) rejects only nonzero core IDs | Two contexts on core zero evade that guard; unsupported modules need explicit SMT capability validation |
| Phases and output | [do_phase](/home/rbera/work/alakazam/champsim-ramulator/src/champsim.cc:90) tracks completion by CPU and ends on any EOF; [phase_stats](/home/rbera/work/alakazam/champsim-ramulator/inc/phase_info.h:42) and [TOML output](/home/rbera/work/alakazam/champsim-ramulator/src/toml_printer.cc:275) index CPUs | Define per-context progress, a common core interval, EOF behavior, and separate context/core statistics |
| Ramulator boundary | [adapter validation](/home/rbera/work/alakazam/champsim-ramulator/src/ramulator2_memory_backend.cc:78) and [driver](/home/rbera/work/alakazam/champsim-ramulator/src/ramulator2_driver.cc:425) require `cpu < num_cpus` | Keep physical core identity at this boundary; do not encode logical contexts by expanding `cpu` |

The v2 trace format carries physical-address payloads, but the current core [CacheBus](/home/rbera/work/alakazam/champsim-ramulator/src/ooo_cpu.cc:904) sends virtual addresses for translation. Adding SMT must not silently switch to recorded PAs, which may overlap across independently recorded applications. The first milestone should retain synthetic translation with distinct address spaces.

## Recommended initial architecture

Keep one `O3_CPU` per physical core and introduce an explicit collection of hardware contexts within it. Retain compile-time physical core count and the current cache topology; context count can be a validated runtime property. The initial supported SMT shape is one core with two contexts, alongside the existing one-context mode.

| State/resource | Proposed first-milestone ownership and policy |
|---|---|
| Input, fetch/decode/DIB-hit/dispatch queues | Context-local ordering; capacities explicitly partitioned from configured totals |
| ROB | One ordered queue per context, fixed quotas summing to the core ROB capacity |
| Rename maps and architectural dependencies | Private to each context |
| Physical registers | Fixed capacity per context initially, summing to the configured core PRF budget; account for first-use/live-in allocations |
| LQ/SQ | Context-local order/forwarding, fixed quotas summing to core totals; shared memory-issue bandwidth |
| Frontend stall/misprediction state | Private to each context; sibling progress continues |
| Fetch/decode/dispatch/execute/complete/retire budgets | Core-wide budgets created once per cycle; rotating eligibility-based arbitration |
| Scheduler inspection budget | Core-wide, with a defined fair scan across contexts; it remains ChampSim's search-window abstraction |
| DIB storage, L1I, L1D, lower caches | Shared core resources; DIB/virtual accesses need address-space-aware identity |
| Translation | Distinct address spaces, shared modeled hierarchy with qualified tags, merges, responses, PTW roots, and walk-cache keys |
| Branch predictors/BTBs | Separate context instances of audited modules initially; history, RAS and pending protocol state remain context-local; report replicated predictor storage explicitly |
| Clock and memory buses | One physical-core clock and existing shared buses |
| Statistics | Per-context progress and stalls, plus core aggregate work and resource utilization |

Fixed quotas provide a clear first implementation and prevent capacity from multiplying with thread count. They do mean a single active context in a two-context configuration cannot automatically consume the sibling's quota. Dynamic sharing and reclamation are later policies, not hidden changes to the baseline. Predictor replication is a separate, explicit modeling choice rather than an implied constant-area comparison.

Introduce separate identities for **physical core**, **hardware context**, and **address space**. Retain globally unique instruction tokens for callbacks; preserve program order only within a context. Initial address-space IDs are assigned by trace mapping, never inferred from the thread's position in a shared queue or from the physical core ID. Request merging must preserve all waiter identities, even when a later shared-address-space mode permits physical requests to coalesce.

Adapt Sniper's rotating, one-context-per-cycle dispatch policy to ChampSim2 fetch and dispatch. Sniper itself prepares instruction-cache accesses ahead of dispatch and omits decoder throughput; it does not supply a complete fetch/decode arbitration model to copy. Eligible contexts can use shared execution bandwidth in the same cycle. Decode, completion, and retirement arbitration must also consume one aggregate budget, and retirement must independently inspect each context's oldest instruction. Exact eligibility and same-cycle scheduling order should be frozen in the first stage's design; retain today's `operate()` stage order for one-context compatibility.

Three approaches were considered:

1. **Extend `O3_CPU` with explicit context ownership and common arbiters — recommended.** Fits existing cache wiring, clocking, physical-core IDs, and finite resources. Requires deliberate extraction of single-context state.
2. **Compose multiple existing `O3_CPU` instances under a sharing wrapper.** Reuses more code at first, but each instance already owns bandwidth budgets, resources, modules, and lifecycle. Preventing duplicated capacity and inconsistent clock/phase behavior would require invasive coordination.
3. **Create a separate SMT core implementation.** Allows an isolated prototype, but duplicates pipeline behavior and increases the risk that future ChampSim2 fixes diverge. Reserve this for a throwaway policy experiment, not the main development path.

## Multi-stage development roadmap

These are reviewable development stages, not an approved implementation specification or a line-by-line coding plan. The first end-to-end SMT milestone is complete only after Stages 1–4. Each stage should get its own concrete design and implementation plan when reached.

### Stage 0 — reconnaissance (this deliverable)

Deliver the source-linked scout reports, ownership comparison, ChampSim2 impact map, and this roadmap. Record the absence of Scarab SMT rather than treating its per-core loops as an SMT implementation. Scope is fixed to independent traces by the user's answer.

### Stage 1 — identity and context extraction with one-context parity

**Deliverable:** `O3_CPU` still runs one context, with architectural state explicitly owned by that context. Separate physical-core/context/address-space identities and the trace-to-context mapping contract. Extract context state into a focused unit (candidate `inc/core_context.h`) while preserving stage order and the one-context timing behavior.

**Touch points:** `inc/ooo_cpu.h`, `src/ooo_cpu.cc`, `inc/instruction.h`, `inc/register_allocator.h`, `src/register_allocator.cc`, `inc/core_builder.h`, and trace-reader interfaces as needed. Preserve the existing global instruction ID allocator; audit `std::merge`, `partition_point`, and all ROB/SQ ordering assumptions.

**Exit evidence:** existing Catch2/Python suites pass; deterministic one-context traces reproduce cycles, retired instructions, branch counts, and memory/cache events against the recorded baseline. Unit tests prove that identical architectural register numbers in distinct test contexts cannot acquire cross-context producers. Record performance overhead on a fixed workload, without inventing an acceptance percentage before measuring the baseline.

### Stage 2 — memory namespaces and module isolation

**Deliverable:** the infrastructure can safely represent two independent address spaces, even while the supported executable remains in one-context mode. Carry context/address-space or equivalent waiter identity through requests, cache merges, translations, and replies. VM keys and PTW roots/walk-cache entries must be qualified; fixing only VM allocation is insufficient. Restrict store forwarding to the originating context and complete loads through explicit waiters.

**Touch points:** `inc/channel.h`, `src/channel.cc`, `inc/cache.h`, `src/cache.cc`, `inc/ptw.h`, `src/ptw.cc`, `inc/vmem.h`, `src/vmem.cc`, core LSQ handling, module interfaces/registry, and audited branch/BTB implementations. Check prefetch-origin metadata and context-dependent module state as part of this audit. Retain physical `cpu` at both memory backends.

**Exit evidence:** two synthetic contexts using identical VAs/IPs translate distinctly and cannot incorrectly merge TLB requests, share DIB entries, forward stores, or complete one another's loads. Verify out-of-order returns, two loads to one block, repeated requests, backpressure, and merged waiter delivery. Confirm baseline predictors have separate histories and return stacks. Reject unsupported global-state adapters at configuration time whenever more than one context is requested, including on physical core zero. Run existing VM/cache/PTW and module-hook regressions.

Memory and module work can be developed independently after Stage 1 defines identities and interfaces, then integrated before runnable SMT.

### Stage 3 — one shared core, two runnable contexts

**Deliverable:** two synthetic or explicitly supplied instruction streams execute in the core test harness with fixed ROB/LQ/SQ/PRF quotas, independent dependency chains, and shared stage budgets. Implement rotating frontend selection, fair shared scheduler traversal, simultaneous ready execution, context-local branch stalls, and independent in-order retirement.

**Touch points:** context state, `O3_CPU` stage functions, allocator partitioning, `inc/core_builder.h`, and `src/static_environment.cc`. Use small focused arbitration helpers if they make budget ownership explicit; do not introduce a second simulator clock or host execution threads.

**Exit evidence:** per-cycle aggregate activity never exceeds each configured width; aggregate occupancy never exceeds each configured capacity; same-register instruction streams stay independent; a long miss or misprediction in context A allows context B to issue and retire. Context A's unretired store cannot be treated as retired by context B's ROB frontier. A continuously eligible context receives service within the bound implied by round-robin arbitration when the required downstream resource is available. One-context parity still passes.

This stage proves SMT scheduling mechanics, not yet complete workload/ROI semantics.

### Stage 4 — trace execution, phases, statistics, and first supported milestone

**Deliverable:** run two independent traces on one physical core through the normal CLI, with reproducible mapping/configuration and documented measurement semantics. Add a runtime context-count option (candidate `ooo_cpu.cpu0.threads = 2`, not an existing knob). Validate trace count against total configured contexts after runtime configuration is known. Keep physical-core numbering and cache names stable.

**Touch points:** `src/main.cc`, `src/champsim.cc`, `inc/phase_info.h`, `inc/core_stats.h`, statistics printers, heartbeat/deadlock listeners, `src/static_environment.cc`, configuration documentation, and both memory-backend integration tests.

**Proposed initial measurement contract to settle in the stage design:** warm each context to its instruction quota while faster contexts continue generating interference, then start measurement at a common barrier. During a fixed-instruction experiment, snapshot each context's requested ROI when it finishes, but continue its background execution until every context finishes. Record a separate common core measurement interval so core IPC uses the sum of all instructions retired during that same interval divided by core cycles. Do not sum IPC values measured over different intervals. Trace repetition remains explicit; non-repeating EOF uses a documented stop-all or drain policy with exact counters and termination reason. Do not quietly let an early finisher disappear from the competition.

Record each context's actual retired-instruction offset and trace repetition index at the common start and its ROI end. Faster contexts can pass their nominal warmup quota before the barrier; standalone comparisons must replay the same measured regions. Specify how instructions and memory requests crossing a phase boundary contribute to each event counter while preserving one-context compatibility.

**Exit evidence:** one-/two-context CLI cases, invalid trace counts, invalid quotas, supported/unsupported modules, empty/short/unequal-length traces, early EOF, trace repetition, warmup transition, and outstanding requests at phase boundaries all behave deterministically. Show per-context retire counts and IPC, aggregate core IPC over a common interval, occupancy/stall/arbitration counters, mapping and policies in TOML output. Preserve meaningful physical-core IDs in legacy and Ramulator2 runs; never duplicate completions or add undocumented drain cycles. Detect lack of progress per context so a healthy sibling cannot hide a stuck one; distinguish valid downstream waits from deadlock.

**Milestone completion:** an end-to-end independent-trace SMT baseline with correctness evidence and stated resource/measurement policies. This does not yet claim calibrated hardware accuracy.

### Stage 5 — sharing policies and validation for research use

**Deliverable:** add policies one at a time behind stable interfaces: dynamically shared ROB or register capacity with reservation guarantees, active-context quota reclamation, alternative frontend selection, and explicit predictor-table sharing if the research requires it. Keep fixed partitioning as a reproducible reference.

**Exit evidence:** compute/compute, compute/memory, branch-heavy/compute, and memory/memory pairs; bounded aggregate throughput; contention and occupancy explanations; no starvation under the stated eligibility assumptions; deterministic reruns. Compare each thread's SMT IPC against its standalone IPC using the same trace region and measurement contract. Report weighted speedup and per-thread slowdown with configurations, rather than treating aggregate IPC alone as fairness.

Use matched Sniper experiments as a qualitative cross-check of trends, after accounting for instruction/uop units, commit budgets, frontend abstraction, memory configuration, and predictor policy. A Sniper match is not a hardware validation. Hardware or independently validated reference measurements are a separate step if calibrated SMT performance is the goal.

### Stage 6 — broader topology, then a separate cooperating-thread project

Generalize to multiple physical cores and more than two contexts after the one-core invariants hold. Audit all `NUM_CPUS`-sized counters, request-source checks, thread-ID widths, configuration replay, and uneven context counts. Verify both memory backends with multiple shared-core groups.

Cooperating threads require an additional design covering trace synchronization events, shared address-space mappings, atomics/fences, memory ordering, and limitations of replaying a fixed interleaving. A context identifier and shared cache do not supply those semantics. Preserve the identity separation now so this extension does not require redefining physical-core IDs later.

## Validation priorities

| Scenario | Required observation | Existing test neighborhood |
|---|---|---|
| One context, unchanged configuration | Same simulated cycles/events and architectural counts | `100-core-latency.cc`, `150-fetch-bandwidth.cc`, `200-rob-scheduling.cc`, `252-rob-execution-prefix.cc` |
| Two contexts use the same register names | No false RAW/WAW dependencies or incorrect reclamation | `201-register-rename.cc` |
| Two address spaces use identical VA/IP | Distinct translation identity and correctly routed completions | `801-vmem-duplicated.cc`, cache/PTW tests, `250-load-scheduling.cc` |
| One context waits on a branch or cache miss | Sibling can proceed; all aggregate widths remain bounded | `180-wrong-path-cycles.cc`, `251-execution-lsq-readiness.cc` |
| One context's ROB head is blocked | Other context retires its own ordered prefix | Core retirement tests to add |
| One context's store is unretired | Sibling progress cannot authorize its store drain or forwarding | LSQ/store tests to add |
| Resource saturation or an inactive sibling | Declared quota and arbitration policy holds; no accidental doubled resources | Core occupancy/arbitration tests to add |
| Global-state predictor selected on core 0 with two contexts | Explicit rejection until made context-safe | `179-branch-module-hooks.cc`, `184-module-configure.cc`, CLI tests |
| Warmup, early finish, EOF, and pending memory | Documented measurement boundaries and termination; no lost/duplicate callback | `084-tracereader-eof.cc`, `502-phase-order.cc`, `708-ramulator2-lifecycle.cc` |

The standard suite entry points are `make test` and `make pytest`. Stage-specific microtraces and memory-backend checks supplement them. These are proposed future checks; this scouting pass did not execute them.

## Decisions to freeze before implementation

The workload scope is confirmed. The remaining design choices are explicit rather than inherited accidentally from either reference simulator:

1. Exact fixed quotas, remainder handling, and minimum viable PRF capacity for two live contexts.
2. Eligibility, rotating priority, and which stages may serve multiple contexts in a cycle.
3. Predictor capacity accounting and the initial supported module set.
4. Exact warmup/ROI, repeat, EOF, and final outstanding-request policy, with a compatible one-context mode.
5. The intended fidelity: retain ChampSim2's existing abstract scheduler/execution model first, or separately budget a port/FU model later.

The next useful design unit is Stage 1 plus the cross-stage identity contract. That provides a small compatibility-preserving foundation before memory identity and shared scheduling change behavior.
