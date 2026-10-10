# Stage 4 总结（两份带 asid 的完整 trace 端到端喂进同一颗核，跑出隔离结果）

> 代码基线：`~/ChampSim2` 的 `smt-stage4-multi-trace` 分支。
> 姊妹文档：`stage4-plan.md`（设计）、`stage3-summary.md`（上一阶段）、`tools/smt-verify/legA.md` + `legB.md`（4.5 两腿结果）、`tools/smt-verify/verify_stage4.sh`（一键复现）。
> 本文件是 Stage 4 关闭后的沉淀记录，六节照骨架写，不省略。

---

## 1. 目标

一句话：让**两份带 asid 的完整 trace** 端到端喂进**同一颗核**、跑出隔离结果——这正是 stage3-summary §6 诚实边界里那句「Stage 3 没证明的」。Stage 3 只在测试基座手喂流下证明了双 context 架构隔离；asid 在 v1/v2 trace 记录里没有字段，Stage 3 一行都没动。Stage 4 把「地址空间身份来自输入 trace、而非恒为 `{0,0}`」这件事从输入端一路打通到退休统计，并用真 trace 的端到端验证收口。

---

## 2. 分步清单（4.1 → 4.5，每步：做了什么 / commit / 新增测试 / 红转绿）

| 步 | 做了什么 | commit（40 位） | 新增测试 | 红→绿 |
|---|---|---|---|---|
| 4.1 | `num_contexts` 从编译期常量改成运行时旋钮（`ooo_cpu.cpu0.num_contexts`，默认 1），`configured_num_contexts` 校验 1..`max_contexts`，越界 `throw` | `5dc9f30411a337b875a955e849b4e7c56411e6e8` | 无（src-only） | 不适用（N=1 三把锁回归确认） |
| 4.2 | trace→context 接线：每个硬件 context 喂自己的 trace，`trace_index` 按 `[core][context]` 键控，asid 落点来自输入 | `791820e83eb6b2c716457c6d70715fe497890dd2` | 505-context-wiring（新增 80 行）；502/707 联动调整 | 是（505 新增后绿，502/707 改后绿） |
| 死锁修复 | TLB tag 匹配键按**地址空间**而非翻译层级（Stage 3.2 债 a，4.2 首次真双流跑才暴露死锁） | `ed341fe32645c97590ce368c6d6f41364f57c085` | 417-tlb-tag-asid-isolation（重写） | 是（417 重写后绿；修正前 4.2 真双流跑出死锁） |
| 4.3 | 阶段完成 + 统计拆账按 context：per-context 完成判定、per-context 统计归集 | `9daa6655e9fb23bbb21a05d485c96fa65f2c02cc` | 无（src-only） | 不适用（4.5 真 trace 验证） |
| 4.4 | per-context 完成谓词 retire-or-EOF：短 trace 先停、长 trace 续跑，替代旧「任一 EOF 掐断整 phase」 | `f437abbc7c70cd21643b433986e6845224d5a0ee` | 无（src-only） | 不适用（4.5 腿 B EOF/L4 验证） |
| 4.5 腿 A | 端到端验证腿 A（`-i 3M` 固定窗口）：L1 架构隔离 + L3 对称/换序；一键脚本 `verify_stage4.sh` + `legA.md` | `4febf60ee6fc18cdd7b2b28956eca3c098fe2c6e` | tools-only（脚本 + 报告） | 不适用（真 trace 端到端，非 Catch2） |
| 4.5 腿 B | 端到端验证腿 B（不带 `-i` 跑完为止）：EOF 短停长续 + L4 空 sibling 复现单 context；`legB.md` | `9359ddd24845591714bb7951801692943fffc3e3` | tools-only（报告） | 不适用（同上） |

> 测试总数：4.4 关闭时 `make test` 全量绿 `1014 例 / 994 通过 / 20 跳过 / 118599 断言`（较 Stage 3 关闭的 `1012 / 992 / 20 / 118584` 增 2 例 + 15 断言，来自 4.2 的 505 与 417 重写）。4.5 只加 `tools/` 脚本与报告、不碰 `src`，不重跑 `make test`，4.4 的全量绿继续有效。

---

## 3. 三个关键设计决策（+ 为什么）

1. **`num_contexts` 运行时 / `max_contexts` 编译期分层。**
   为什么：`max_contexts = 2` 留在 `inc/defs.h` 编译期——单 context 的 `if constexpr (max_contexts == 1)` 快速路径要能继续折叠（`src/static_environment.cc:177-178` 注释明言这一点）；而 `num_contexts` 作为运行时旋钮（默认 1，`configured_num_contexts` 校验 1..`max_contexts`）让同一份二进制既能跑 N=1 回归、又能跑 dual。运行时层越界 `throw`（`runtime config: … must be between 1 and 2`），编译期层决定结构布局与快速路径——两者各管各的，谁都不越界。

2. **trace→context 结构键控：`trace_index` 嵌套 `[core][context]`，不是扁平步进。**
   为什么：`phase_info::trace_index` 是 `vector<vector<size_t>>`，`trace_index[k][c]` 是喂给物理核 k 的硬件 context c 的 trace 的**命令行位置序号**；每个 slot 的 asid 落点为 `{trace_index[k][c], trace_index[k][c]}`（`inc/phase_info.h:39-43`）。用 `[core][context]` 而非扁平的 `num_cpus × num_contexts` 步进，让「第几份 trace 喂第几个 context」成为**结构声明**而非算术约定，主循环按 `trace_index.at(cpu.cpu).at(c)` 取值，喂错 context 变成一眼可读的越界，而不是静默错位。

3. **per-context 完成谓词：retire-or-EOF。**
   为什么：旧规则 A「任一 trace EOF 就掐断整个 phase」会把长 context 停在短 context 的 ~0.5M 处（4.5 腿 B 实测证明旧规则会这么干）。新谓词 `core_complete = AND_c (sim_instr(c) >= length || eof(c))` 让每个 context 各自判定「退满窗口 或 自己 trace 读完」，短停长续。带 `-i` 时 `repeatable::eof()` 恒 false（`main.cc:373` `repeat = simulation_given`、`inc/repeatable.h:47`），谓词退化成 `AND_c (retired)`，与 4.3 逐字节一致——这正是 N=1 不变的原因。

---

## 4. 三个诚实发现（都不是 4.5 失败，但要写进报告）

1. **预测器分化（shared `hashed_perceptron`）：架构隔离 vs 预测器行为分线。**
   默认 `hashed_perceptron` 是 `smt_capability::shared`（`branch/hashed_perceptron/hashed_perceptron.h:52-54`，单 per-core 实例按 ASID 键控），全局历史被对偶线程的分支交错污染 → mispredict 单跑 **7602** vs 双跑 ctx0 **67203**（mpki 2.53 vs 22.40、acc 98.93% vs 90.58%）。这是预期分化，不算 L1 失败。架构计数器（退休 + `.executed` 分支普查）逐位不变，正说明隔离成立、分化只在预测器 speculative 态。所以 L1 只比架构计数器，明确排除 mispredict/mpki/accuracy。

2. **快 context overshoot（fast-waits-for-slow）。**
   `-i` 固定窗口的语义是「两个 context **都**退满 3M 才结束 phase」，于是快 context 会一直退休到慢 context 也过线为止。sqlite（快）在换序前后多退 7297 条（4572550 vs 4565253，0.16%）。这是 fast-waits-for-slow 的时序量，不是隔离失败；其前 3M 退休前缀由 in-order 退休保证逐位一致（Stage 3 已锁），只是聚合值含 overshoot 尾。

3. **L4 从腿 A 挪到腿 B（`-i` 造不出空 sibling）。**
   草图原把 L4 放腿 A（带 `-i`）。实测 `-i` 下 1 条指令的 sibling 会无限回卷重放（`repeat = simulation_given`、`repeatable::eof()` 恒 false），空 sibling 根本造不出来。所以 L4 必须在不带 `-i` 的腿 B 做，用 1 条指令（立即 EOF）+ 有限 4M 主 trace。空 trace 用 **1 条记录 = 64 字节**（`head -c 64`）造，刻意避开 0 字节文件——`bulk_tracereader::operator()` 会对空 deque 调 `front()`，行为不定。

---

## 5. N=1 三把锁（全程 8dcc/9bb1/8dcc 逐字节不变）

三把锁（`tools/run-accept.sh stage45-final` 实测，与锁定值逐字节一致）：

```
baseline.norm  8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6
prf64.norm     9bb10576f07f5e12983d7711a01ad2811be871b9f76b9f155ce46986a659ea56
sched64.norm   8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6
```

逐步归因：

- **4.1–4.4（含死锁修复）**：全是生产代码改动，但 N=1 下 `num_contexts = 1` 恒走单 context 路径、asid 恒 `{0,0}`，逐位一致**必须**成立——三把锁全程不变，证明 multi-trace 重构没有改坏单 context 的数值路径。这是**回归确认**（锁还在），不是改动验证（改动路径 N=1 不活动）。
- **4.5 两腿（`4febf60e` / `9359ddd2`）**：只加 `tools/` 脚本与报告、不碰 `src`，本轮复跑再次锁定 8dcc/9bb1/8dcc——纯粹是收口时的回归确认。

---

## 6. 诚实边界（Stage 4 证明了什么，没证明什么）

**证明了（端到端、真 trace）：** 两份带 asid 的完整 trace 喂进同一颗核后，架构计数器隔离（L1 Δ≤2）、换序隔离（L3 慢 context 逐位、快 context overshoot 已解释）、对称（L3 (A,A) IPC 相对差 0.03%）、空 sibling 复现单 context（L4 Δ=0）、EOF 短停长续、L2 断言未触发——全部由 `tools/smt-verify/verify_stage4.sh` 一键复现，实测 `RESULT: PASS`（exit 0）。

**没证明（留给未来 instrumentation）：** L1 是**聚合架构计数器指纹**（紧容差 ≤10），**不是逐指令 replay**。`--toml` 文档没有逐指令架构事件日志，所以「退休序列逐位相同」只能由 in-order 退休 + 聚合计数器在紧容差内相等间接支撑，不能逐条对账。逐指令 replay 是未来的 instrumentation 工作，不在此步宣称（已写进 `legA.md` 与 `verify_stage4.sh` 注释）。
