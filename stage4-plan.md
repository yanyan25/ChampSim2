# Stage 4 设计（trace 携带 asid / 多 trace，先文档，未写代码）

> 代码基线：`~/ChampSim2` 的 `smt-stage3-dual-context` 分支（Stage 3 已关闭于 `29171b75`）。
> 姊妹文档：`stage3-summary.md`（Stage 3 沉淀）、`stage3-plan.md`（Stage 3 设计，其中 §0 已划死：trace 携带 asid / 多 trace = Stage 4）。
> 本文件是 Stage 4 的**分步计划**，不写代码；每步给「改行为」判据 + 两个专门小节回答「trace 层怎么带 asid」「怎么不改 trace 格式」。等你拍板后再动手。

---

## 0. 目标与范围（一句话）

让**两份带 asid 的完整 trace 端到端喂进同一颗核、跑出真·双 asid 并发**——把 Stage 3 已经建好的双 context 隔离/仲裁/无饿死机制，从「测试基座手喂流」接到「真实 trace 输入」，且**不改 trace 记录格式本身**。

---

## 1. 中心设计洞察（先立住，全计划的地基）

侦察结论（三处关键事实，都是当前代码的硬约束）：

1. **v1/v2 trace 记录里没有 asid 字段。** `input_instr`（v1，64B）和 `input_instr_v2`（v2，512B）都没有 asid；只有 `cloudsuite_instr`（SPARC）自带 `asid[2]`。
2. **asid 是在 tracereader 边界按 `{cpu,cpu}` 盖章的，不是从 trace 读的。** `ooo_model_instr(uint8_t cpu, const input_instr&)` / `(…input_instr_v2&)` 都是 `{cpu, cpu}`（`inc/instruction.h:298/301`），而这个 `cpu` 参数来自 `bulk_tracereader` 构造时传入的 `cpu_idx`（`main.cc:366` 的 `i++` = 位置参数序号）。**所以 `cpu_idx` 现在就是一个纯 asid 章，跟物理核号无关。**
3. **只喂 context 0。** `src/champsim.cc:54-59` 只填 `cpu.ctx().input_queue`（=`contexts.front()`）；`phase_info.trace_index` 是「每个物理核一个 trace」，`main.cc` 位置参数与核号一一对应。contexts[1..] 的 `input_queue` 端到端**从不被喂**。

由此得出那个不用改 trace 格式的结论：

> **asid 由「哪条 trace 喂给哪个 context」这份接线决定，不藏在 trace 字节里。**
> 因为 tracereader 已经按位置序号盖章 `{i,i}`，两条 trace 天然就是 `{0,0}` 和 `{1,1}` 两个不同地址空间——缺的不是 asid 字段，缺的是「把第二条 trace 接到 context 1 的 input_queue」这一步接线，以及「让机器能构建 2 个 context」这一件事。

---

## 2. 分步计划（每步：改什么 / 动哪些文件 / 改行为判据）

### 4.1 把 context 数做成运行时 knob，让生产机器能构建 2 个 context

- **改什么**：`src/static_environment.cc` 的 core 构建链（`:440-488`）目前**从没调 `.num_contexts(...)`**，`core_builder::m_num_contexts` 默认 1（`inc/core_builder.h:48`）——即生产机器永远是单 context，Stage 3 的双 context 机制在真机器上还开不了。本步接上：`.num_contexts(cfg.positive_value<std::size_t>(key + ".num_contexts", 1))`，并让 `select_modules` 里已有的 `> max_contexts` 拒绝（`:515-518`）继续把关。
- **文件**：`src/static_environment.cc`（可能 `inc/static_environment.h`）。
- **改行为判据**：
  - `--knobs` 列出 `ooo_cpu.cpu0.num_contexts`，默认 1。
  - `--set ooo_cpu.cpu0.num_contexts=2` 构造出 `contexts.size() == 2`；`=3` 被 `> max_contexts(=2)` 拒绝并命名 key。
  - **N=1 不变量**：`num_contexts=1` 时三配置 sha256 仍 `8dcc/9bb1/8dcc`（纯接线步，不改数值路径）。
  - 注意：本步单独不足以跑 SMT2——context 1 仍没人喂（4.2 才是喂流），所以本步与 4.2 必须一起验收。

### 4.2 trace→context 接线：每个 context 喂自己的 trace（核心）

- **改什么**：
  - `phase_info.trace_index` 从「每物理核一个」泛化为「每 (核, context) 一个」：改成 `std::vector<std::vector<std::size_t>>`（外层核、内层 context），或展平成长度 `num_cpus * num_contexts` 的 `std::vector<std::size_t>`。
  - `main.cc:369-375` 的位置参数映射从「i = 核号」改成「i = (核, context) 展平号，行优先」：trace 0 → core0/ctx0，trace 1 → core0/ctx1，trace 2 → core1/ctx0，…
  - `src/champsim.cc:54-59` 的读 trace 循环，从只填 `cpu.ctx().input_queue` 改成对 `cpu.contexts[c]` 逐个填各自 `input_queue`，每 context 从 `traces[trace_index[cpu][c]]` 读。
- **文件**：`inc/phase_info.h`、`src/champsim.cc`、`src/main.cc`。
- **改行为判据**（这一步就是「trace 层带 asid」的落点，见 §3）：
  - 两条 trace 跑一个 SMT2 核：context 0 的 `input_queue` 来自 trace 0、context 1 来自 trace 1；trace 0 每条指令 `asid=={0,0}`、trace 1 `asid=={1,1}`（断言在 `do_init_instruction`/进 IFETCH_BUFFER 处）。
  - 两条 trace 的同 VA 访问，经 Stage 2/3 的 asid 键控结构（TLB tag / VM / DIB / load 完成）被分到不同物理页 / 不同命中，不串台。
  - **N=1 不变量**：单 trace、`num_contexts=1` 时 `trace_index` 退化成 `[0]`，路径逐字等价，三配置 sha256 仍 `8dcc/9bb1/8dcc`。

### 4.3 阶段完成 + 统计按 context 记账

- **改什么**：`do_phase`（`src/champsim.cc:90-167`）里三处只认 context 0 的逻辑要认全 context：
  - `cpu.sim_instr()`/`sim_cycle()` 只读 `ctx()`（context 0，`inc/ooo_cpu.h:185-186`）→ 阶段完成判定 `cpu.sim_instr() >= length` 得改成「每个 context 都退够」。
  - livelock 检测的 `cpu.sim_instr()` 同理 → 用聚合退休数。
  - `stats.sim/roi_cpu_stats` 只 push `cpu.ctx()`（`champsim.cc:181-182`）→ 每 context 各 push 一份，统计文档按 (核, context) 出表。
- **文件**：`src/champsim.cc`、`inc/ooo_cpu.h`（加聚合/逐 context 访问器）、`src/toml_printer.cc`、`src/plain_printer.cc`。
- **改行为判据**：
  - SMT2 跑两条 trace：阶段**直到两个 context 都退够 `length`** 才结束，不是只看 context 0。
  - 统计文档里能看到 per-context 退休数 / IPC / MPKI，两个 context 的加总 == 聚合。
  - 单 context 模式输出与现在逐字一致（这是 `compare_optimization.py` 的硬门槛，不能因为加了 per-context 循环就漂）。

### 4.4 EOF / 死锁 / livelock 的多 context 语义

- **改什么**：现在 `champsim.cc:145-147`「任何一条 trace 到 EOF 就终止全部 phase」。SMT2 下两条 trace 通常不等长，得定语义；以及 `deadlock_cycle`/livelock 的进度口径（见决策 D-C）。
- **文件**：`src/champsim.cc`。
- **改行为判据**：
  - 一条 trace 先到 EOF：其 context 停止退休（不再产生新指令），**sibling 继续跑**；阶段在「所有 context 退够 length **或** 所有 context 都无输入且无在飞」时结束。
  - 一个 context 卡死（trace 没完但退休停摆）不能掩盖另一个 context 也卡死：死锁/无进展检测用**聚合进度**，不是 context 0 的进度。
  - 单 trace / 单 context 时 EOF 语义与现在一致（terminate all，因为只有一个 context）。

### 4.5 端到端验证：两份完整 trace 跑双 asid（不变量收口）

- **改什么**：不写生产代码，把 Stage 3 的 L1–L4 分层不变量（`stage3-plan.md` §4）在真 trace 上落成可重复脚本（`tools/` 下），作为 Stage 4 的 exit gate。
- **文件**：`tools/` 验证脚本（不碰 src）。
- **改行为判据**（就是 `stage3-summary.md` §6 诚实边界那条）：
  - **L1**：每条 trace 在 SMT2 双跑里的 per-context 架构事件（退休顺序 / 改名 / 分支 / load 完成路由 / store 转发）与它**单跑**逐位一致。
  - **L2**：聚合 fetch/decode/dispatch/schedule/execute/retire ≤ 各宽度，LQ/SQ/ROB/scheduler 占用 ≤ 容量（不翻倍）。
  - **L3**：无饿死；A 的长 miss 不挡 B；`(A,B)`/`(B,A)` 换序 per-context 差在容差内；`(A,A)` 对称对 IPC 近似相等。
  - **L4**：SMT2 配一个**空 sibling** 复现单 context 的 `[phase]` 统计（N=1 parity 续命）；单 context 模式继续走 `compare_optimization.py` 逐位 gate。

---

## 3. trace 层怎么带 asid（不改 trace 格式）

三句话：

1. **asid 不写进 trace。** v1/v2 记录一个字节都不动——`input_instr`/`input_instr_v2` 照旧，没有 asid 字段照样能双 asid，因为 asid 本来就是「喂给哪个地址空间」这件事，不是「记录里写了什么」。

2. **asid = trace 的位置槽，已在 `{i,i}` 盖章。** `bulk_tracereader` 已经拿位置序号当 `cpu_idx` 盖章 `{i,i}`。两条 trace 就是 `{0,0}`/`{1,1}` 两个天然不同的地址空间。Stage 4 只是让这个「位置槽」继续当 asid，同时把它和「物理核号」解耦——核号是 `O3_CPU::cpu`，asid 是 trace 槽，两者从此分开（现在单核下它们都等于 0，所以分不清；SMT2 下一核两 trace，asid=0/1 而核号恒 0，第一次真正分家）。

3. **context 身份（`ctx_index`）由接线决定，asid 身份由 trace 槽决定，各管各。** 指令进了 `contexts[c].input_queue` 就天然是 context c（ROB/RAT 按 `ctx_index` 分），它的 `asid` 是 trace 槽（TLB/VM/DIB 按 asid 分）。两者是 Stage 3.5 就定下的两种不同键，Stage 4 不合并它们。

> 一个可选的更明确写法（要不要做，见决策 D-A）：给每条 trace 一个显式 asid 覆盖（比如 CLI 上 `trace0=asid:7`），默认仍用位置槽。这只是把「隐式 `{i,i}`」显式化，为将来「两条 trace 共享一个地址空间」留口子；本阶段默认不需要。

---

## 4. 需要你拍板的决策

| # | 决策 | 我的建议 | 影响 |
|---|---|---|---|
| D-A | asid 取「全局 trace 槽」还是「核内 context 序号」 | **全局 trace 槽**（=现有 `{i,i}`，单核 SMT2 下两者同值） | 决定 asid 是否随「trace 落到哪颗核」而变；全局槽更稳，多核 SMT2 也不撞 |
| D-B | `-w`/`-i` 的 `length` 是「每 context」还是「每核聚合」 | **每 context**（每个线程退够 length 才结束） | 决定 SMT2 的 `-i 50M` 是两个线程各 50M 还是合计 50M；我倾向各 50M，语义最接近「两条独立 trace 各跑各的」 |
| D-C | 一条 trace 先 EOF 时：该 context 停摆 sibling 继续，还是整 phase 停 | **context 停摆、sibling 继续**（阶段在所有 context 都无输入且无在飞时结束） | 与现「any eof → terminate all」不同，但那是单核假设；SMT 下不等长 trace 是常态 |
| D-D | `num_contexts` 的 knob 名与校验 | `ooo_cpu.cpuN.num_contexts`，`positive_value` + `> max_contexts` 拒绝 | 与现有配置 key 命名一致；trace 数必须 == `num_cpus * num_contexts`（新增校验） |

---

## 5. 风险 / 遗留

- **`do_phase` 的完成判定与统计改完，单 context 必须逐字复现**（`compare_optimization.py` 硬门槛）。4.3/4.4 是热路径旁边的控制逻辑，但要防「为了 SMT 加 per-context 循环、单 context 也背上循环或改了结束时机」。
- **`stats`/`[config]` schema 变化**：per-context 统计表是新增 key，会动 `501-static-environment.cc` 和 `toml_printer` 的 key 集断言——4.3 里一起对。
- **trace 数校验**：现在隐式「trace 数 = 核数」，SMT2 后必须显式 `trace 数 == num_cpus * num_contexts`，否则多喂/少喂会静默错喂（这是 D-D 的必答题）。
- **编译期 `max_contexts=2`** 是上限不是目标；本阶段只做 2 context，不碰更高。
- **不改 trace 格式是硬约束**：任何「给 v1/v2 加 asid 字段」的方案都是错的（会破坏 `bulk_tracereader` 的 `is_trivial/is_standard_layout` 与 64/512B 布局，且 v1 是公开 DPC-3 格式）。asid 只从接线来。
