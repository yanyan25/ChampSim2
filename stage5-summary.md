# Stage 5 总结（真正版：共享策略收口：A 水位线 → B/C/D 记账）

> 代码基线：`~/ChampSim2` 的 `smt-stage5-shared-policy` 分支（从 `fork/smt-stage5-singletons` 开，HEAD=`e2897bce`，src 树与 Stage 4 顶端一致）。
> 姊妹文档：`~/SMT-experiment/stage5-plan.md`（设计）、`~/SMT-experiment/stage5-plan-singletons.md`（旧「债 b 核销」计划，仅存档）、`~/SMT-experiment/prf-deadlock-issue.md`（A 的单核反面教材）、`~/SMT-experiment/stage6-plan.md`（后继债去向）、`stage4-summary.md`（上一阶段，本仓库根）。
> 本文件是 Stage 5 的终版沉淀记录，覆盖 5.1 → 5.4 全链（A 水位线实装 + B/C/D 记账）。5.3 的公平性数字已随 5.2b ROB 均分重跑订正；最终 N=1 三把锁锁定见文末。

---

## A. 每线程动态水位线（5.1 实装 + 5.1b 真实短缺报错）

> 共享 PRF 原为 FCFS（plan §1.1）：一个线程的 committed 集合懒增长、永不缩，FCFS 下可吃光整池饿死兄弟。5.1 在 scheduler 分配门加「预留量」：context i 要 rename 一条指令时，只有当 `count_free_registers() >= 该指令的 need + Σ_{k≠i} need_k` 才放行（`need_k` = 兄弟 k 的 ROB 里最老未调度指令的 rename 需求 = 未映射源 + 目标，∈ [0,6]）。共享池结构不变（Stage 1 方向不动），只在消费端加闸（`src/ooo_cpu.cc` schedule gate + `inc/ooo_cpu.h` helper）。单核快路径不碰（`sibling_need ≡ 0`，逐字节等于 FCFS），N=1 三把锁复跑不变。

> 死锁自由（plan §2.4）：门保证每有待调度指令的 context 恒有 `free ≥ need_k`（不饿死）；永久停滞 ⇒ `N < C + need_i + need_j`（真实容量不足，非水位线造成）。预留量只有 `Σ need_k ≤ 2×6 = 12`，不会制造死锁——死锁只来自 `N < C + Σ need_k` 的真实短缺。

> 真实短缺响亮报错（5.1b，plan §2.4 决策 D-B）：把 `register_file_size` 过小从「仅单核」扩到多 context——某 context ROB 头被门挡、且全 core 无在飞可 retire 指令时抛 `runtime config: ooo_cpu.cpuN.register_file_size = X is too small`，报错写明多线程需求（`N >= C + Σ need_k`），不静默挂。

> 单测钉死：`204-prf-waterline.cc`（"A context cannot schedule past its sibling's rename reservation"：8 取 7 留 1 给兄弟；"A sibling with room to spare still schedules its oldest writer"）、`205-prf-waterline-undersized.cc`（"Two blocked heads with nothing in flight are a named undersized error"：抛 "register_file_size = 4 is too small"；"A blocked head waits while a sibling has an instruction in flight"：兄弟在飞则不误报）。

---

## 诚实边界（B/C/D 逐条记账，只写边界不改码）

### B. ROB 总容量守恒（5.2b 订正；5.2 的「只记账」已作废）

> ROB 原为 per-context 全量复制：每个 hardware context 各拿一整份 `ROB_SIZE`（`inc/core_context.h:87`），2 线程下核心级 ROB 容量实为 `2×ROB_SIZE`——违反「总容量守恒」，落在物理造不出来的配置上，方向性错误。**5.2b 已订正**：dispatch 门槛从「每 context 比 `ROB_SIZE`」改为「每 context 比 `rob_capacity()`」（`src/ooo_cpu.cc` dispatch gate），核心级总预算仍一个 `ROB_SIZE`，两线程聚合占用 ≤ `ROB_SIZE`。默认分法「均分」：每 context 固定 `ROB_SIZE / num_contexts`。N=1 时 `ROB_SIZE/1 == ROB_SIZE`，三把锁逐字节不变（8dcc/9bb1/8dcc 复验一致，IPC 2.34/1.778/2.34）；2 context 时聚合占用 ≤ `ROB_SIZE`（单测 `254-rob-capacity-conservation.cc` 钉住：每 context ≤ `ROB_SIZE/2`、聚合 == `ROB_SIZE`）。

**分法参数（第二层，只留接口不实装）**：分法做成参数 `ooo_cpu.cpu0.rob_partition`，当前唯一取值 `equal`（均分），非 `equal` 配置期响亮拒绝（`configured_rob_partition` → `runtime config: ... must be 'equal' (dynamic ROB sharing is not implemented)`）。动态共享（闲置配额回收、水位线分法）是未来扩展，需数据支撑后再加，不现在拍脑袋；它只在 `O3_CPU::rob_capacity()` 这一个入口点新增一个 `rob_partition` 取值和一个 case，不散落别处。

### C. 公平性指标从零建（WS / ANTT / per-thread slowdown，`tools/smt/fairness.py`）

> 公平性指标原为空白（plan §1.3），本阶段从零建：`tools/smt/fairness.py`（纯 stdlib，`tomllib`）读一份 co-phase 统计 doc（2 context）+ 两份 standalone 单 context doc，报 weighted speedup、ANTT、harmonic speedup 与 per-thread slowdown，公式 `speedup_i = cycles_standalone_i / roi_cycles_co_i`（每线程指令数固定 M，M 约掉）。两个硬口径（critique §7.4/§7.6）为硬性校验、失败拒绝计算：① standalone 必须是同一机器（有效 `[config]` 除 `num_contexts` 与 `vmem.randomization` 外 178 键逐位相等，**弃 `build_id`**——它把 `num_contexts` 折进 hash，co/standalone 必然不同，会拒掉每对合法输入）+ 同一 region（warmup/sim/trace_version 一致）的单 context 单跑；② 读 per-thread own 完成周期 `roi.core.cpu{i}.roi_cycles`（新增纯增量字段，N=1 恒等 `cycles`、三把锁 8dcc/9bb1/8dcc 不变），缺则拒绝计算。

> 实测（release，`-w 1M -i 3M`，**5.2b ROB 均分后重跑**）：同速对 fotonik3d×2 WS=0.9087、ANTT=2.2010（对称到 0.49%）；异速对 sqlite+omnetpp WS=1.2122、ANTT=1.8004（sqlite speedup 0.7814 / omnetpp 0.4308，sqlite overshoot +53%）。单核对照：standalone fotonik3d ipc=2.34 == N=1 锁 2.34（N=1 分母逐字节不变）。

> **ROB 全量 vs 均分的实测差（订正后重跑，同一把尺子 fairness.py）**：5.2b 把每线程 ROB 从一整份 `ROB_SIZE` 减半到 `ROB_SIZE/2` 后，WS 只轻微下降——同速对 fotonik3d×2 从 0.9093 → 0.9087（−0.07%），异速对 sqlite+omnetpp 从 1.2175 → 1.2122（−0.44%）。即 2×ROB 乐观模型的「偏乐观」程度实测 <0.5% WS：这批低 IPC 负载（standalone 0.80–2.34）下 ROB 容量不是主要瓶颈，均分几乎不伤公平性。这是 critique 二手结论（「ROB 全量 vs 均分差多少」）变成我们自己实测的结果。

> **对称性从 0.05% 变到 0.49%（差 13999 cycles / 2.82M）**：均分后两「同 trace」线程不再几乎逐位一致（5.2b 前差 1469 cycles，之后 13999 cycles）。只证了「差仍很小（<0.5%）」，**未证**差从哪来——之前 0.05% 归因「asid 键控预测器 + 轮询仲裁」是合理推断、非已证；均分后差值放大到 0.49%，来源同样未证，不作铁结论转述。

> **已知未解（钩子，不现在追）**：ROB 均分是否引入新的不对称来源（如奇数 `ROB_SIZE` 的 floor 余数 1 个格子的归属）——未证、不影响本阶段结论，留待单独一轮实验。

**诚实纠正（防止后人对 roi_cycles 的目的误判）**：设计阶段曾报警「读 TOML `ipc` 相除对异速负载就是假数」，实跑证伪了该强度——overshoot 在 speedup 里自相抵消（快线程在 catch-up 窗口继续以 ≈ M/own 的 IPC retire，`(1+overshoot/M)·(own/full)=1` 精确成立），naive 相除在 IPC 稳定时给近似正确 speedup（sqlite tainted 4,571,290/7,286,841≈0.6273 vs exact 3,000,000/4,781,495≈0.6274，speedup 都是 0.784）。roi_cycles 的价值修正为三条（都仍成立）：① plan §4.1 前提仍为假——TOML `cycles` 对快线程是全程周期（7.29M）而非 own（4.78M），按 plan 的 `instr=M / cycles=full` 算会得 0.514 而非 0.784（−35%）；② exact 不需要「IPC 稳定」假设，快线程在 catch-up 窗口 stall 或 trace EOF 时 naive 公式才真错；③ per-thread IPC 数字本身要诚实（M/own ≠ (M+overshoot)/full）。

**边界**：单核 2 context、多程序物理地址不重叠（不同 asid 不共享物理页）负载才有效；`vmem.randomization` 只固定 seed=1 单点，未扫 seed（§4.3 的 mean±range 未做，属后继）。

### D. 预测器（混合，只记账不改）

> 方向预测器（bimodal/gshare/perceptron/hashed_perceptron）已 ASID-keyed `shared`——无别名串扰、但共享表容量，介于完全共享与复制之间；BTB（blbp_64kb/blbp_64kb_tuned/ittage_32kb/ittage_64kb）已 `per_context` 复制（= no-interference 上界）；CBP6 ×4（cbp6_ddtage/cbp6_runlts_norv/cbp6_tagescl192/cbp6_tagescl64）`unsupported`，SMT（`num_contexts>1`）下被 `check_smt_capability`（`inc/modules.h:316`）配置期拒绝。本阶段不改，只记账。

---

## 分步清单（commit）

| 步 | 做了什么 | commit | 测试 |
|---|---|---|---|
| 5.1 A 水位线 | 共享 PRF 预留每 context 下一条 rename | `d4628bfb` | `204-prf-waterline.cc`（`93064ac9`） |
| 5.1b 真实短缺报错 | 多 context PRF 过小 → 命名 config error | `d0a7c5fb` | `205-prf-waterline-undersized.cc`（`0e48a1b3`） |
| 5.3a roi_cycles | per-thread own-ROI 完成周期字段 | `ef03a4cf` | 099 金样（`9e282c53`）+ 三把锁 |
| 5.3b fairness | `tools/smt/fairness.py`（WS/ANTT/slowdown） | `2e9c79a8` | 内嵌 `--self-test` |
| 5.2b B 订正 | ROB 总容量守恒（equal 均分） | `6aba46c4` | `254-rob-capacity-conservation.cc` + python 拒绝用例 |
| 5.2 B 记账 / 5.4 D 记账 | 纯文档（无 `src/` 改动） | — | — |

（5.2「只记账」已作废、升为 5.2b「订正」；5.4 D 一句话落上文 D 节。）

---

## 最终锁定（N=1 三把锁，`run-accept.sh stage5-final`）

- baseline  `8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6`  ipc 2.34
- prf64     `9bb10576f07f5e12983d7711a01ad2811be871b9f76b9f155ce46986a659ea56`  ipc 1.778
- sched64   `8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6`  ipc 2.34

与 5.2/5.3a/5.2b 锁值逐字节一致（8dcc/9bb1/8dcc）。N=1 下 `ROB_SIZE/1 == ROB_SIZE`，均分 inert，坐实。
