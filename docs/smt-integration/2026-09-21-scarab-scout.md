# Scarab scout for ChampSim2 SMT

Date: 2026-09-21. Repository: `/home/rbera/work/alakazam/scarab`. Inspected commit: `47b2c7ae834b9a2edb0bbeb8abc3b4a5b83f224d`. Source inspection only: no compilation, tests, or simulations were run. The repository remained clean. No `.codegraph` index or nested AGENTS.md was present; searches used `rg`.

## Finding: this checkout does not implement SMT

The README explicitly lists [“No cooperative multithreaded code” and “No SMT”](/home/rbera/work/alakazam/scarab/README.md:49). Current executable topology agrees:

- The [model registry](/home/rbera/work/alakazam/scarab/src/model_table.def:34) contains only `cmp` and `dumb` models. `SIM_MODEL` selects the registry entry in [sim.c](/home/rbera/work/alakazam/scarab/src/sim.c:516).
- [Cmp_Model](/home/rbera/work/alakazam/scarab/src/cmp_model.h:51) explicitly says “one thread for each core.” [Allocation](/home/rbera/work/alakazam/scarab/src/cmp_model_support.c:53) creates `NUM_CORES` copies of Thread_Data, recovery data, each pipeline stage, LSQ, and issue queues.
- [cmp_cores()](/home/rbera/work/alakazam/scarab/src/cmp_model.c:227) loops over cores and advances each complete pipeline. There is no sibling-thread selection or one shared backend receiving independently selected architectural contexts.
- [Op identity](/home/rbera/work/alakazam/scarab/src/op.h:168) consists of `proc_id` (documented processor ID), `op_num`, `unique_num`, and `unique_num_per_proc`; it has no separate hardware-thread identity. [McPAT output](/home/rbera/work/alakazam/scarab/src/power/power_scarab_config.cc:192) hardcodes one hardware thread.

Repository-wide searches for SMT, simultaneous multithreading, hardware threads, threads-per-core, thread IDs, primary-thread references, and multithreaded code found documentation, power-model descriptions, host/PIN/checkpoint thread IDs, trace PID/TID handling, and stale comments, but no hidden runnable SMT model. In particular, [the “primary thread” retirement comment](/home/rbera/work/alakazam/scarab/src/node_stage.c:489) accompanies a single-core counter and single ROB retirement sequence, not thread arbitration. The [Thread_Data comment about future MT](/home/rbera/work/alakazam/scarab/src/thread.h:68) is also residue, not an enabled model.

`NUM_BPS` is an especially plausible false positive: [initialization](/home/rbera/work/alakazam/scarab/src/cmp_model.c:132) creates alternate predictors and decoupled frontends for each core, but they explore speculative paths of the same program. The actual pipeline [pops fetch targets only from MAIN_BP](/home/rbera/work/alakazam/scarab/src/decoupled_frontend.cc:120). Alternate frontends [require PT or memtrace](/home/rbera/work/alakazam/scarab/src/decoupled_frontend.cc:241), and [the trace frontend](/home/rbera/work/alakazam/scarab/src/frontend/pt_memtrace/trace_fe.cc:249) has one on-path instruction stream per `proc_id`, with additional off-path state indexed by `bp_id`. Alternate paths must not be presented as SMT threads.

## Configuration and end-to-end dataflow

[`num_cores`](/home/rbera/work/alakazam/scarab/src/core.param.def:52) defaults to one; [`model` and `frontend`](/home/rbera/work/alakazam/scarab/src/general.param.def:54) select topology and instruction source independently. The [frontend registry](/home/rbera/work/alakazam/scarab/src/frontend/frontend_table.def:29) provides execution-driven PIN, PIN traces, and optional PT/memtrace frontends. [Frontend initialization](/home/rbera/work/alakazam/scarab/src/frontend/frontend.c:60) initializes these with core count, and its fetch/recover/retire interface uses `proc_id` plus `bp_id` for speculative alternatives.

The useful trace-driven path is:

1. [memtrace_init()](/home/rbera/work/alakazam/scarab/src/frontend/pt_memtrace/memtrace_fe.cc:232) maps `CBP_TRACE_R0..R63` to readers, initializes one reader per core, and calls the uop generator with `NUM_CORES`.
2. [memtrace_trace_read()](/home/rbera/work/alakazam/scarab/src/frontend/pt_memtrace/memtrace_fe.cc:172) selects a PID/TID and skips other PID/TID records. This is not scheduling multiple software threads. Moreover, the selected [`prior_tid` and `prior_pid` are globals](/home/rbera/work/alakazam/scarab/src/frontend/pt_memtrace/memtrace_fe.cc:73), not arrays per reader: the per-core reader array should not be mistaken for demonstrated support for arbitrary multi-process memtrace combinations. No runtime test of that combination was performed.
3. [ext_trace_fetch_op()](/home/rbera/work/alakazam/scarab/src/frontend/pt_memtrace/trace_fe.cc:249) supplies `next_onpath_pi[proc_id]` or replay-generated off-path instructions to the uop generator. On end of trace, it sets core-indexed `trace_read_done`, `reached_exit`, and `op->exit`.
4. [Decoupled_FE update](/home/rbera/work/alakazam/scarab/src/decoupled_frontend.cc:514) fills a fetch-target queue subject to capacity, taken-branch, fetch-target-per-cycle, and predictor constraints; [fetch callbacks](/home/rbera/work/alakazam/scarab/src/decoupled_frontend.cc:594) call `frontend_fetch_op`.
5. [Icache FT arbitration](/home/rbera/work/alakazam/scarab/src/icache_stage.c:483) pops from the primary frontend. Instruction cache/uop cache and decode feed IDQ; [cmp_cores()](/home/rbera/work/alakazam/scarab/src/cmp_model.c:241) calls dcache, execute, ROB, map, IDQ, decode, icache, then decoupled frontend updates.
6. [Map processing](/home/rbera/work/alakazam/scarab/src/map_stage.c:251) adds each op to the context's sequential list, constructs register/memory dependencies, allocates renamed registers, and attaches wakeups.
7. [ROB insertion](/home/rbera/work/alakazam/scarab/src/node_stage.c:341) checks ROB and LSQ capacity, asserts op/core identity, and appends to one program-order linked list. Issue queues fill and schedule; execute consumes operands and delivers results/wakeups.
8. [Retirement](/home/rbera/work/alakazam/scarab/src/node_stage.c:430) walks that ROB's head in order, stops at an unready head, updates counts/predictors, frees register and LSQ resources, and notifies the frontend at selected instruction boundaries.

This is useful single-context OoO plumbing. Neither changing `NUM_CORES` nor changing `NUM_BPS` turns it into an SMT pipeline.

## Actual ownership matrix

There is no SMT sharing matrix to extract; the following records actual current multicore ownership.

| Resource | Ownership / executable evidence |
| --- | --- |
| Program-order state, architectural dependency map | One `Thread_Data` per core; contains `Map_Data` and sequential op list ([thread.h](/home/rbera/work/alakazam/scarab/src/thread.h:55), [selection](/home/rbera/work/alakazam/scarab/src/cmp_model_support.c:94)). |
| Rename maps and physical register accounting | `Map_Data` contains register maps, last-store map, memory dependency hash, wakeup allocator, and INT/FP register-file pointers ([map.h](/home/rbera/work/alakazam/scarab/src/map.h:47)). Separate per core; not shared with siblings. |
| ROB, decode, rename, IDQ, execute/FUs | One full set per core ([allocation](/home/rbera/work/alakazam/scarab/src/cmp_model_support.c:64)). Single ROB head/tail and one retirement order. |
| RS / issue queues | `per_core_issue_queues`, selected by `proc_id` ([issue_queue.cc](/home/rbera/work/alakazam/scarab/src/issue_queue.cc:999)). |
| Load and store queues | `per_core_lsq_unit` ([lsq.cc](/home/rbera/work/alakazam/scarab/src/lsq.cc:240)); queues retain program-order entries and free from front ([LSQ::free](/home/rbera/work/alakazam/scarab/src/lsq.cc:104)). |
| Branch prediction / recovery | Predictors per core and per speculative predictor ID; one recovery record per core ([cmp_model.c](/home/rbera/work/alakazam/scarab/src/cmp_model.c:132)). |
| Instruction and data L1 caches | Embedded in each core's icache/dcache stage ([icache init](/home/rbera/work/alakazam/scarab/src/icache_stage.c:153), [dcache init](/home/rbera/work/alakazam/scarab/src/dcache_stage.c:127)). |
| MLC | Current initialization creates **one shared object** and points all cores at it ([memory.c](/home/rbera/work/alakazam/scarab/src/memory/memory.c:388)). This disagrees with README's private-MLC description; current code is the stronger evidence. |
| LLC | Called `L1` internally; selectable private/shared. Private mode divides configured total capacity/banks across cores; shared mode assigns one object to every core ([memory.c](/home/rbera/work/alakazam/scarab/src/memory/memory.c:402)). Do not confuse this `L1` with the core's L1 data cache. |
| Memory requests / DRAM | Shared `Memory`, queue and MSHR policies; request-buffer capacity may scale by core with `PRIVATE_MSHR_ON`, and completion queues are per core ([memory.c](/home/rbera/work/alakazam/scarab/src/memory/memory.c:302)). |
| TLB | Perfect; no finite TLB/PTW sharing model to reuse ([power config](/home/rbera/work/alakazam/scarab/src/power/power_scarab_config.cc:443), [DTLB](/home/rbera/work/alakazam/scarab/src/power/power_scarab_config.cc:492)). |
| Clock, completion, statistics | Core-indexed clock domain, done flags, and stat arrays ([core loop](/home/rbera/work/alakazam/scarab/src/cmp_model.c:228), [stats](/home/rbera/work/alakazam/scarab/src/statistics.c:111)). |

## Scheduling, dependencies, memory ordering

There is no fetch-thread policy, sibling fair-share quota, sibling-aware dispatch, SMT issue selection, or per-thread commit arbitration.

Widths are independent controls: [`DECODE_WIDTH`, `DISPATCH_WIDTH`, `RENAME_WIDTH`, `RS_FILL_WIDTH`, `NODE_RET_WIDTH`](/home/rbera/work/alakazam/scarab/src/core.param.def:124). [Map backpressure](/home/rbera/work/alakazam/scarab/src/map_stage.c:193) checks rename resource availability. [IssueQueues::dispatch()](/home/rbera/work/alakazam/scarab/src/issue_queue.cc:861) walks ROB order into compatible available queues, stops when no queue is available, and honors `RS_FILL_WIDTH` (`0` means unlimited). [OldestFirstSchedulePolicy](/home/rbera/work/alakazam/scarab/src/issue_queue.cc:438) compares `op_num`; other policies prioritize ROB-head or physical queue order. [Scheduling](/home/rbera/work/alakazam/scarab/src/issue_queue.cc:908) bids/grants across queues and FUs. [Execution](/home/rbera/work/alakazam/scarab/src/exec_stage.c:274) latches into available FUs. These are policies among ops of one architectural thread, not sibling arbitration.

[Retirement](/home/rbera/work/alakazam/scarab/src/node_stage.c:443) is capped by `NODE_RET_WIDTH`, stops at the single unready head, and [asserts the next expected op number](/home/rbera/work/alakazam/scarab/src/node_stage.c:493). Reusing this globally ordered ROB across SMT contexts would impose cross-thread retirement dependencies unless its semantics were deliberately changed.

[Dependency wrappers](/home/rbera/work/alakazam/scarab/src/thread.c:141) select the current context map and assert the op has the same `proc_id`. [Wakeups](/home/rbera/work/alakazam/scarab/src/cmp_model.c:335) also assert producer and consumer are on the same core. Two map slots per architectural register are selected by `off_path`; they are on/off-path versions, **not two threads** ([map.c](/home/rbera/work/alakazam/scarab/src/map.c:318)).

Memory dependencies reside outside the LSQ occupancy structure: [read_store_map()](/home/rbera/work/alakazam/scarab/src/map.c:290) can constrain operations behind the last store, and [map_mem_dep()](/home/rbera/work/alakazam/scarab/src/map.c:359) uses store-address hashes to add load dependencies. The LSQ itself records ops and capacity, commits at the front, and recovers from the back using an op-number threshold ([lsq.cc](/home/rbera/work/alakazam/scarab/src/lsq.cc:71)). There is no independent model of sibling memory consistency or synchronization.

Address isolation is explicit: [`convert_to_cmp_addr()`](/home/rbera/work/alakazam/scarab/src/globals/utils.c:673) packs `proc_id` in the top six address bits. Optional [`addr_translate()`](/home/rbera/work/alakazam/scarab/src/addr_trans.c:50) scrambles address bits while preserving that identity; it is not OS translation. Consequently, identical virtual addresses from separate simulated processes remain separate memory identities even when caches are shared. This is directly relevant to the user's first milestone—independent traces on one physical core—but the identity should be an explicit address-space ID when adapted to ChampSim2, independent of both core and hardware-thread IDs, so later shared memory remains possible.

## Recovery, time, exit, and attribution

[cmp_istreams()](/home/rbera/work/alakazam/scarab/src/cmp_model.c:201) selects core context and checks scheduled recovery against that core's clock before normal pipeline work. [cmp_recover()](/home/rbera/work/alakazam/scarab/src/cmp_model.c:365) restores frontend/predictor state, register state, sequential list and dependency map, then each pipeline stage, LSQ, issue queues, memory bookkeeping, and ROB. [recover_seq_op_list()](/home/rbera/work/alakazam/scarab/src/thread.c:113) clips younger operations by the recovering context's sequence number. This relies on one thread per selected core; it is not a selective SMT squash implementation.

[recover_memory()](/home/rbera/work/alakazam/scarab/src/memory/memory.c:740) marks relevant off-path requests but does not cancel them. Request consumers use op pointer plus unique generation number and validity checks to reject stale ops after recycling ([request merging](/home/rbera/work/alakazam/scarab/src/memory/memory.c:2301)). This is a reusable invariant for outstanding completions across thread flush/exit, provided future ownership includes context identity.

[sim.c's loop](/home/rbera/work/alakazam/scarab/src/sim.c:710) advances the frequency model once and calls the model cycle. Each core executes only when its frequency domain is ready. There is no extra simulated time per architectural thread.

EOF is attached to an op; [retirement sets `retired_exit[proc_id]`](/home/rbera/work/alakazam/scarab/src/node_stage.c:510). The [top-level loop](/home/rbera/work/alakazam/scarab/src/sim.c:737) marks done at retired exit or instruction limit and supports first/last-done stopping. Both [core loops skip `sim_done` cores](/home/rbera/work/alakazam/scarab/src/cmp_model.c:205). This is core lifecycle, not thread lifecycle. Instruction-limit stopping does not establish full backend/memory drain. Legacy FE_TRACE “bogus restart” code/comments exist ([cmp_model_support.c](/home/rbera/work/alakazam/scarab/src/cmp_model_support.c:122), [sim.c](/home/rbera/work/alakazam/scarab/src/sim.c:758)), but resetting trace state does not clear `sim_done`, and current core loops skip done cores. Do not claim working continued-interference behavior from those comments without a runtime check. PT/memtrace explicitly do not restart in bogus mode.

[Stats](/home/rbera/work/alakazam/scarab/src/statistics.c:111) are allocated per core, with separate alternate-BP stats; retirement counts micro-ops and instructions by `node->proc_id` ([node_stage.c](/home/rbera/work/alakazam/scarab/src/node_stage.c:496)). This does not provide SMT throughput, per-thread IPC, fairness, or normalized slowdown measurements.

## Configurations, tests, and limits of the evidence

A documented single-context trace configuration is:

```sh
scarab --frontend memtrace --fetch_off_path_ops 0 --cbp_trace_r0=/path/to/trace
```

This comes from [docs/memtrace.md](/home/rbera/work/alakazam/scarab/docs/memtrace.md:26); compiling PT/memtrace requires its documented optional dependencies/build flag. Use a suitable PARAMS.in as described in [running-scarab.md](/home/rbera/work/alakazam/scarab/docs/running-scarab.md:25). There is **no supported SMT configuration example** to provide. `--num_cores 2` means two full cores. Source exposes numbered trace slots, but this scout does not certify multicore memtrace combinations, particularly given the global PID/TID filter above.

[PARAMS.sunny_cove](/home/rbera/work/alakazam/scarab/src/PARAMS.sunny_cove:80) supplies decode 5, dispatch/rename 6, register sizes, split reservation stations, retire 6, and ROB 352. These are single-context pipeline parameters, not per-thread shares or evidence of modeled Intel SMT.

Existing [test targets](/home/rbera/work/alakazam/scarab/src/test/Makefile:48) exercise frontend/socket/message infrastructure, and [dummy client tests](/home/rbera/work/alakazam/scarab/src/test/scarab_dummy_client_test.cc:107) cover fetch/retire interfaces. [Verification documentation](/home/rbera/work/alakazam/scarab/docs/verification.md:9) describes qsort sanity checks against reference statistics. Broad searches found no SMT policy or sibling-isolation tests. Nothing was executed, so this report establishes code structure and observed limitations, not runtime correctness or performance.

## Reusable patterns and adaptation hazards for ChampSim2

These are engineering implications, not claims that Scarab already implements them:

1. Preserve the separation of context program-order/dependency state from pipeline resources. Scarab's `Thread_Data` is a helpful inventory, but its current one-context-per-core allocation must not be copied as a shared-resource SMT design.
2. Use explicit physical-core ID, hardware-thread/context ID, address-space ID, per-thread sequence number, and globally unique/generation ID. Scarab shows why both order and lifetime identities matter; its packed core-address convention cannot express independent address spaces sharing a core or shared memory across cores cleanly.
3. Allocate physical pipeline widths and capacities once per core, then specify how contexts compete. Running the full single-thread pipeline loop once per sibling would multiply modeled bandwidth and capacity. Scarab has no answer for ICOUNT versus round-robin fetch, partitioned versus dynamic ROB/LSQ allocation, or sibling commit fairness.
4. Keep recovery and architectural dependency tracking per context; filter shared queues and callbacks by context plus age/lifetime. Scarab's core-wide recovery walk and single ROB-head assumptions are precisely the points that would need redesign under sharing.
5. Separate source EOF, fetch disable, architectural completion, pipeline drain, outstanding memory work, and measurement completion per thread. Choose continued-interference versus stop behavior deliberately. Scarab's current stop/restart inconsistencies are a caution against inferring lifecycle from comments.
6. Retain core-clock scheduling once per physical cycle and add both thread and core statistics. Shared-cache occupancy/port accounting can remain physical while request attribution follows the issuing context and address space.

Scarab should therefore be used as a detailed single-context OoO implementation reference and a source of identity/recovery/lifecycle lessons. It cannot serve as the working SMT reference for ChampSim2.
