# Sniper SMT architecture scout

Repository: `/home/rbera/work/alakazam/snipersim`; inspected commit `56505e42fd98bca863fac181e769bd3c98d2bb33`. Read-only source investigation; no simulation or tests executed, no repository edits. The checkout was clean at inspection. No `.codegraph` directory and no applicable nested AGENTS.md were found. Findings describe this fork, including its newer MMU and ChampSim trace frontend, not an assumed upstream version.

## Main finding

Sniper implements a shared out-of-order SMT backend, not merely OS time slicing or multiple independent cores. Each logical hardware context remains a `Core` plus `RobSmtPerformanceModel`, while contiguous groups share one `RobSmtTimer`. Dispatch selects one context each cycle; issue can use multiple contexts in one cycle; retirement is independently width-limited per context. The timer has private ROBs and dependency maps, shared RS capacity, shared memory-operation capacity, and shared execution ports. Branch prediction and the inspected standard MMU/TLB hierarchy remain private per logical context.

Selection proof: [common/performance_model/performance_model.cc:35](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_model.cc:35) selects `RobPerformanceModel` for `type=rob, logical_cpus=1`, otherwise `RobSmtPerformanceModel`. `interval` and `oneipc` do not select this SMT backend. [common/performance_model/performance_models/rob_smt_performance_model.cc:6](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_smt_performance_model.cc:6) computes `master = core_id - core_id % logical_cpus`, stores timers in a static map keyed by master, and registers each context on the shared timer at line 41.

## Configuration and topology

The supplied `config/smt2.cfg`, `smt4.cfg`, and `smt8.cfg` only set `[perf_model/core] logical_cpus`; they do not select ROB. See [config/smt2.cfg:1](/home/rbera/work/alakazam/snipersim/config/smt2.cfg:1). `config/rob.cfg:1` selects ROB and defines:

```ini
[perf_model/core]
type = rob
logical_cpus = 2

[perf_model/core/interval_timer]
dispatch_width = 4
window_size = 128

[perf_model/core/rob_timer]
in_order = false
issue_contention = true
issue_memops_at_issue = true
outstanding_loads = 48
outstanding_stores = 32
store_to_load_forwarding = true
address_disambiguation = true
rob_repartition = true
simultaneous_issue = true
commit_width = 128
rs_entries = 36
```

The widths/window values above are supplied by `config/nehalem.cfg:11`; the other values are actual `config/rob.cfg:4` defaults. This is a supported configuration overlay, not a standalone complete configuration: this fork also requires a valid cache/MMU/environment configuration. A command composition is `./run-sniper -n 4 -c <working-machine-config> -c rob -c smt2 ...`: four logical contexts become two physical SMT groups `(0,1)` and `(2,3)`. It is not a tested invocation. `run-sniper:250` parses `-n`; `run-sniper:745` passes it as `general/total_cores`. `common/system/core_manager.cc:28` creates one Core per ID. The explicit config ordering matters: `config/nehalem.cfg:7` resets logical_cpus to 1 and line 8 selects interval, so applying it last would override SMT/ROB selections.

Topology is exposed as SMT index `id % smt_cores`, then physical-core index from `id / smt_cores`; APIC IDs combine package, physical core, and context. See [common/core/topology_info.cc:11](/home/rbera/work/alakazam/snipersim/common/core/topology_info.cc:11). The cache subsystem multiplies cache `shared_cores` by SMT contexts: [common/core/memory_subsystem/parametric_dram_directory_msi/memory_manager.cc:168](/home/rbera/work/alakazam/snipersim/common/core/memory_subsystem/parametric_dram_directory_msi/memory_manager.cc:168). Thus `l1_*/shared_cores=1` still shares L1 across sibling contexts. Cache capacity is instantiated once, not multiplied by sibling count.

Uniform, contiguous groups are the well-supported case. The performance-model selector uses array-valued logical_cpus, but memory hierarchy uses scalar `getInt` at `memory_manager.cc:114`. The timer's shared widths/clock/model come from its first creator. Mixed sibling configurations, partial groups, and nonapplication-context handling deserve separate validation: `rob_smt_performance_model.cc:9` computes master before the nonapplication override to one thread at line 13.

## Sharing matrix

| State/resource | Ownership and implemented behavior | Evidence |
|---|---|---|
| Input decoding, instruction queue, dynamic instruction/uop preparation | Per logical Core/performance model/frontend thread | `common/trace_frontend/trace_thread.cc:926`; `common/performance_model/performance_model.cc:294`; `common/performance_model/performance_models/micro_op_performance_model.cc:153` |
| Frontend miss/branch stall and resume timestamp | Per context | `common/performance_model/performance_models/rob_performance_model/rob_smt_timer.h:54` |
| ROB and pre-ROB input buffer | Separate circular queue per context; dispatch occupancy limited by common partition size | same header line 62; `rob_smt_timer.cc:29`, `:110`, `:552` |
| Sequence numbers, register producers, memory producers, dependants | Per context; a sequence number alone is meaningful only inside that context | same header line 64; `rob_smt_timer.cc:295`, `:426`, `:468` |
| Physical register file/free list | Not modeled here; register producer tracking substitutes for dependence/renaming timing | `common/performance_model/performance_models/micro_op/register_dependencies.cc:10` |
| Reservation station | Shared occupancy counter, allocated at dispatch and freed at issue | `rob_smt_timer.cc:594`, `:604`, `:811` |
| Issue ports and nonpipelined ALU busy times | One shared RobContention object, initialized once per physical cycle | `rob_smt_timer.cc:69`, `:1017`; `rob_contention_nehalem.cc:21`, `:87` |
| Load/store queues | Shared ContentionModel completion-slot arrays, not full architectural LSQ entries | `rob_smt_timer.h:132`; `rob_smt_timer.cc:773`; `common/performance_model/contention_model.cc:20` |
| Last store completion | Shared across all contexts, including fence checks | `rob_smt_timer.h:131`; `rob_smt_timer.cc:794`, `:909`, `:917` |
| L1I/L1D/cache prefetcher/MSHR state | Shared cache master/controller state; per-context proxy/controller stats | `memory_manager.cc:168`; `cache_cntlr.cc:426`, `:443`, `:462`, `:468`, `:749` |
| Branch predictor | Private predictor instance per PerformanceModel/Core | `common/performance_model/performance_model.cc:69`; `common/performance_model/branch_predictor.cc:31` |
| Standard MMU/PTW/TLB hierarchy | Private instantiated MMU per Core; standard MMU creates its own TLBHierarchy/TLB objects | `memory_manager.cc:96`; `mmu_designs/mmu.cc:125`, `:229`; `translation_components/tlb_subsystem.cc:69` |
| Timing | Shared physical timer plus per-context accounted timestamps; grouped global barrier | `rob_smt_timer.h:130`; `rob_smt_timer.cc:485`; `rob_smt_performance_model.cc:31` |

All shortened paths in the matrix are relative to `/home/rbera/work/alakazam/snipersim`; `rob_smt_timer*`, `smt_timer*`, and `rob_contention_nehalem.cc` are in `common/performance_model/performance_models/rob_performance_model/`; memory manager, cache controller, MMU and translation components are in `common/core/memory_subsystem/parametric_dram_directory_msi/`.

## End-to-end instruction and cycle path

1. The SIFT frontend decodes/caches an instruction, supplies actual branch direction/next PC and memory operands, then calls `queueInstruction` and `iterate`: [common/trace_frontend/trace_thread.cc:926](/home/rbera/work/alakazam/snipersim/common/trace_frontend/trace_thread.cc:926). The current fork also routes ChampSim-format input through `handleChampSimDetailed` at line 975, calls `decodeChampsim` at 984, attaches addresses at 1070, and reaches the same queue/iterate at 1080.
2. `PerformanceModel::iterate` drains its input queue through virtual `handleInstruction`, then calls virtual `synchronize`: [common/performance_model/performance_model.cc:294](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_model.cc:294).
3. `MicroOpPerformanceModel::handleInstruction` constructs dynamic micro-ops at line 168, accesses instruction memory immediately at 210, optionally accesses data before dispatch at 227, predicts/updates branches at 350, and invokes the SMT model's `simulate` at 396. Citation: [common/performance_model/performance_models/micro_op_performance_model.cc:153](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/micro_op_performance_model.cc:153).
4. `RobSmtPerformanceModel::simulate` holds the shared timer lock, pushes uops onto that context's pre-ROB queue, and returns previously simulated committed-instruction/time deltas. It deliberately does not execute timing here; shared execution is deferred to a safe point in `synchronize`. [common/performance_model/performance_models/rob_smt_performance_model.cc:51](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_smt_performance_model.cc:51).
5. `SmtTimer::simulate` enters its local barrier when the current context has more than 128 undispatched uops. An elected host thread executes the shared timing model, synchronizes globally, and releases frontend producers requiring more input. [common/performance_model/performance_models/rob_performance_model/smt_timer.cc:345](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_performance_model/smt_timer.cc:345).
6. `RobSmtTimer::execute` loops while all potentially dispatchable contexts have sufficient input; `canExecute` requires at least `m_num_in_rob + 2*dispatchWidth` queued uops. Thus timing never races past missing input from a runnable sibling. Each cycle is **dispatch → issue → commit**. Newly dispatched uops are made ready no earlier than the next cycle. [common/performance_model/performance_models/rob_performance_model/rob_smt_timer.cc:510](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_performance_model/rob_smt_timer.cc:510), `:611`, `:1116`, `:1149`.

There is no modeled decoder throughput stage here (`rob_smt_timer.cc:1146`). Instruction-cache accesses happen during preparation ahead of dispatch; their reported miss latency is then charged as a per-context frontend stall. This is a buffered, timing-directed committed-path model, not execution of speculative wrong paths.

## Arbitration and capacity algorithms

**ROB allocation:** `computeCurrentWindowsize` divides total window size by the number of running contexts when repartition is enabled, otherwise by configured hardware-context count. With no running contexts it gives the full window. Integer division can leave unused entries. Every context allocates a queue of `windowSize + 255` entries, mixing dispatched ROB entries and undispatched lookahead. Partition enforcement checks only that context's dispatched occupancy; there is no aggregate ROB occupancy gate. [common/performance_model/performance_models/rob_performance_model/rob_smt_timer.cc:29](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_performance_model/rob_smt_timer.cc:29), `:110`, `:552`.

**Dispatch:** round-robin priority; blocked contexts are skipped (I-cache/branch, inactive/future timestamp, full partition). The first context that dispatches any uop owns the entire cycle's dispatch opportunity, up to `dispatchWidth` **uops**. Unused width is not filled from a second context. RS capacity is globally checked. If all fail, starting priority still rotates. `rob_smt_timer.cc:543`, `:671`, `:713`, `:742`.

**Issue:** reset shared execution-port availability once each cycle; scan contexts starting at rotating priority; scan each context's ROB oldest first, skipping already issued/completed entries and unready dependencies. With `simultaneous_issue=true`, continue scanning other contexts using the same shared ports. With false, stop after the first context issues any uop. This actual branch at `rob_smt_timer.cc:1036` supersedes the stale “single-thread only/interleaved” comment at 1020. There is no cross-context oldest-age competition and no ICOUNT fetch policy. With the contention model disabled, the fallback `num_issued == dispatchWidth` budget is local to each `tryIssue`, so simultaneous mode can issue up to `contexts * dispatchWidth` uops/cycle in aggregate. Evidence: `rob_smt_timer.cc:875`, `:925`, `:1001`, `:1012`.

**Commit:** visit every context each cycle, pop ready entries from each ROB head in order, and reset `num_committed` to zero for each context. The width increments once per popped micro-op; only `uop->isLast()` increments architectural instruction count. Therefore `commit_width` is a **per-context uop width**, despite `config/rob.cfg:16` calling it instructions/cycle. Aggregate peak is contexts × commit_width, with no shared retirement bandwidth arbitration. Exact proof: [common/performance_model/performance_models/rob_performance_model/rob_smt_timer.cc:1052](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_performance_model/rob_smt_timer.cc:1052), `:1073`, `:1084`.

## Dependencies, memory, and branch behavior

Register producer tracking records latest producer sequence per architectural register. It does not allocate physical registers or model register-file capacity. Dependencies, readiness, forwarding and ROB lookups are scoped to the selected `RobThread`, preventing sibling register numbers from creating false dependencies. `rob_smt_timer.cc:295`, `:405`, `:426`; [common/performance_model/performance_models/micro_op/register_dependencies.cc:10](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/micro_op/register_dependencies.cc:10).

MemoryDependencies tracks exact address equality against preceding stores within that context. A load depends on its latest matching store; barriers are tracked per context. There is no byte-range overlap check here. Store-to-load forwarding replaces the store dependency with the store's own producer dependencies. This is dependence/latency modeling, not transfer of actual data values. [common/performance_model/performance_models/micro_op/memory_dependencies.cc:13](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/micro_op/memory_dependencies.cc:13), `:65`; `rob_smt_timer.cc:309`.

Issue blocks loads if the shared load capacity is full; disabling address disambiguation also blocks loads behind older unresolved-address stores. Stores require `head_of_queue` and store capacity. That variable means no earlier not-yet-issued operation blocked this scan, not literally only ROB index zero: entries with known done timestamps are skipped regardless of whether completion is in the future. Memory barriers block later loads/stores while waiting; L/SFENCE is explicitly FIXME. `rob_smt_timer.cc:888`, `:907`, `:925`, `:934`, `:983`.

With issue-time memory enabled, uops whose cache result is unknown invoke their **originating context's Core** accessMemory with the shared timer's current timestamp. The call returns latency/hit location synchronously to the timing model; completion and dependent readiness are scheduled by timestamps, not a later ROB callback lookup. Shared load/store slots reserve until modeled completion. Stores leave ROB one cycle after issue even if memory completion is later, with `last_store_done` tracking the later time. `rob_smt_timer.cc:747`, `:773`, `:792`.

Cross-context memory dependencies are not created by register/memory dependence tables. Cache/coherence sharing provides memory interference. The shared `last_store_done` does nevertheless couple sibling fences: a store from context A advances it at `rob_smt_timer.cc:794`; serializing/fence issue in context B checks the same scalar at `:909`/`:917`.

Cache sharing is real pointer sharing: master allocates the cache/prefetcher; siblings reuse the master's state. L1 memory accesses take the shared SMT lock. [common/core/memory_subsystem/parametric_dram_directory_msi/cache_cntlr.cc:426](/home/rbera/work/alakazam/snipersim/common/core/memory_subsystem/parametric_dram_directory_msi/cache_cntlr.cc:426), `:443`, `:462`, `:635`. The fork's standard MMU is independently allocated per logical context and independently creates its TLB hierarchy; do not infer that shared L1 means shared TLB. Other specialized MMU designs were not exhaustively audited.

Each context predicts and immediately updates its branch predictor during frontend processing ([common/core/core.cc:199](/home/rbera/work/alakazam/snipersim/common/core/core.cc:199)). A known mispredict blocks that context's further dispatch; issue of the branch releases the frontend after `misprediction_penalty - 2`. There is no wrong-path fetch, checkpoint restoration, or ROB suffix squash in this timer. `rob_smt_timer.cc:624`, `:865`. `pushInstructions` does delete uops already marked squashed, but that is upstream instruction/uop preparation, not branch recovery (`:460`).

## Synchronization, lifecycle and statistics

The local SMT barrier waits for all running contexts to arrive, but requires at least one waiter; `in_sync` prevents another executor while the timer lock is released around the global clock barrier. Barrier release selects contexts lacking lookahead and has a progress escape that selects approximately smallest surplus if everyone has enough. [common/performance_model/performance_models/rob_performance_model/smt_timer.cc:76](/home/rbera/work/alakazam/snipersim/common/performance_model/performance_models/rob_performance_model/smt_timer.cc:76), `:101`, `:150`, `:190`, `:359`.

Sibling clock clients are assigned to one global barrier group at `rob_smt_performance_model.cc:31`. Global barrier uses group master in detailed mode and individual logical IDs during fast-forward: [common/system/barrier_sync_server.cc:69](/home/rbera/work/alakazam/snipersim/common/system/barrier_sync_server.cc:69). SMT cycle skipping is disabled whenever more than one context exists, including when only one is active (`rob_smt_timer.cc:1171`).

Start/exit/stall/resume/migration hooks update the running-context set and ROB partition. Exit/stall re-evaluate barrier progress. Migration detaches the old thread identity, releases any old barrier waiter, and binds the new software thread to its logical context. ROI begin clears wakeup flags and synchronizes timestamps. Disable wakes all barrier waiters. `smt_timer.cc:230`, `:242`, `:254`, `:270`, `:286`, `:309`, `:379`.

There is no flush/drain implementation in these lifecycle paths. `RobSmtTimer::synchronize` updates context time, clears wakeup flag, and may advance shared time to the earliest context time; its own comment says queues might need flushing, but does not do it (`rob_smt_timer.cc:520`). Trace EOF gets elapsed time and calls onThreadExit (`common/trace_frontend/trace_thread.cc:1699`); `common/system/thread_manager.cc:148` marks idle and fires exit hooks without draining the timing ROB. Undispatched input for a nonrunning context cannot dispatch, while already dispatched entries can continue issuing/committing if a sibling drives timing. Final-tail and migration correctness therefore require dedicated validation.

Per-context stats include uop counts, committed instructions, CPI base/SMT/branch/RS/cache categories, load/store counts and latency, producer distance and optional MLP histograms. `rob_smt_timer.cc:127`. Shared contention queue statistics are registered under the creating/master Core (`:77`; `common/performance_model/contention_model.cc:33`). Every physical cycle attributes one CPI category per context during dispatch (`rob_smt_timer.cc:733`); `cpiSMT` specifically counts frontend opportunities lost because another context dispatched, not all SMT interference. Back-end head classification is heuristic (`:642`). Do not sum per-context elapsed time as physical elapsed time or assume CPI-SMT is total slowdown.

## Untested hazards and limitations worth retaining in the comparison

These are static inferences, not reproduced failures:

* Wakeup implementation/comment mismatch: resume/migration set `in_wakeup` and comments claim latency suppression (`smt_timer.cc:290`), but `returnLatency` unconditionally returns shared-now minus context-now (`rob_smt_timer.cc:485`). The flag is cleared on synchronize and displayed in debug output; no timing check reads it. Stale wakeup timestamps could be incorrectly charged.
* Dynamic ROB repartition is not strict global capacity enforcement. A sleeping context can retain dispatched entries while the running sibling receives the full window; on wakeup, already allocated entries exceeding the new share are not removed. The code only gates future dispatch per context (`rob_smt_timer.cc:110`, `:552`).
* Per-context fallback issue budgets and per-context commit budgets can exceed a desired physical-core width (`:925`, `:1052`). These are actual semantics, whether intentional approximation or unsuitable for a new model.
* Shared store-drain timestamp can over-serialize sibling fences (`:794`, `:909`, `:917`). Per-context dependency tracking does not undo that cross-context coupling.
* End-of-trace, short inputs below the >128 launch threshold, context replacement with residual pre-ROB/ROB/dependency state, and all-contexts-idle progress are not covered by an explicit drain protocol in inspected paths. Do not borrow this lifecycle unchanged.
* Issue-port helper mutates port occupancy before testing nonpipelined ALU availability; a failed ALU issue can leave ports consumed for the cycle (`rob_contention_nehalem.cc:39`, `:77`). This is pre-existing approximate resource modeling, not specifically an SMT mechanism.
* Branch recovery models only timing stalls. Memory dependence matches exact starting address, not byte ranges; address-disambiguation=true does not introduce memory-order violation replay. This fits known-path traces but is not a full speculative execution model.
* Maximum lookahead and buffering constants are intertwined: context queue `windowSize+255`, simulation launch surplus >128, refill threshold `2*dispatchWidth` (`rob_smt_timer.cc:34`, `:446`; `smt_timer.cc:349`). Large widths or large batches need explicit capacity tests.

## Existing examples, validation status, transferable ideas

SMT examples are the small `config/smt{1,2,4,8}.cfg` overlays plus `config/rob.cfg` knobs. Repository-wide searches for SMT settings under `test/` and `testing/` found no dedicated SMT regression suite. [testing/test_config.yaml:1](/home/rbera/work/alakazam/snipersim/testing/test_config.yaml:1) describes MMU/memory smoke tests and sweeps. No run is claimed here.

The confirmed first ChampSim2 milestone is independent traces sharing one physical core. Treat each trace as a distinct architectural register context and address space; virtual addresses that happen to match must not create cross-trace forwarding, translation reuse, or false completion matches. Cooperating shared-memory threads and software synchronization can remain later milestones.

For a trace-driven OoO ChampSim2 implementation, the strongest reusable ideas are structural: explicit physical-core timer owning shared resources; private context queues/producer maps/sequence spaces; context identity propagated to memory access; separate dispatch and issue arbiters; explicit active-context lifecycle; per-context performance attribution alongside physical-resource totals. The shared RS counter plus private ROBs is a simple first ownership model, and single-context-per-cycle round-robin dispatch is an understandable policy to validate before adding more policies.

Do not mechanically import Sniper's threading machinery into a single host event loop: its local barrier solves parallel functional producer synchronization. A ChampSim trace-reader adapter can instead provide deterministic per-context queues and refill backpressure, while maintaining one cycle advancement per physical core. Likewise, make aggregate widths, ROB capacity policy, address-space identity, physical-versus-context clocks, memory request identity, completion routing, EOF drain, predictor sharing, and TLB sharing explicit; Sniper makes distinct choices for each, sometimes with approximations described above.

Useful later tests (not executed or implemented): two independent ALU streams proving shared width, same architectural register IDs across contexts proving dependency isolation, one context stalled on cache/branch while the other progresses, same VA in different address spaces proving request isolation, sibling store plus fence proving intended ordering, one context ending early proving drain/progress, wakeup/repartition with outstanding uops proving capacity/timing invariants, and measurement separating per-context throughput from aggregate core throughput.

Open uncertainties: runtime behavior of all-context sleep/exit corner cases; migration with residual uops; compatibility of heterogeneous SMT sizes with cache grouping; specialized MMU sharing policies beyond the standard hierarchy; complete interprocess physical-address mapping in every frontend; and quantitative accuracy against real SMT hardware. These require targeted runs or additional scope, not assumptions from the class names.
