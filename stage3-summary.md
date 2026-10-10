# Stage 3 总结（真·双 context 并发，隔离 + 仲裁 + 无饿死）

> 代码基线：`~/ChampSim2` 的 `smt-stage3-dual-context` 分支。
> 姊妹文档：`stage3-plan.md`（设计）、`stage3-debt.md`（欠账清单，本阶段的必答题来源）、`stage2-summary.md`。
> 本文件是 Stage 3 关闭后的沉淀记录，六节照骨架写，不省略。

---

## 1. 目标

一句话：在**不动物理核/缓存布线、不新增 operable、容量与宽度不翻倍**的前提下，让单个 `O3_CPU` 同时推进两个 context，两者架构事件彼此隔离、共享资源受逐预算点仲裁，并以「全局 oldest-first」而非轮转作为跨 context 年龄键，保证不饿死。

---

## 2. 分步清单（3.1 → 3.7，每步：做了什么 / commit / 新增测试 / 红转绿）

| 步 | 做了什么 | commit（40 位） | 新增测试 | 红→绿 |
|---|---|---|---|---|
| 3.1 | 双 context 测试基座 + 恒等/非恒等不变量（`instr_id` 递增断言、非恒等映射、交接处 `CHAMPSIM_ASSERT`） | `06d2864c92e55b40664f549b15ac74928900bd53` | 311-context-identity | 是（本步与 3.2–3.5 同落此 commit，逐条红转绿在 3.5 报告内） |
| 3.2 | TLB tag 按 `(asid, vpage)` 键控，`matches_address` 拆成物理匹配 / asid-qualified 匹配（债 a） | `06d2864c92e55b40664f549b15ac74928900bd53` | 417-tlb-tag-asid-isolation | 是（两地址空间同虚页命中不互串） |
| 3.3 | 模块 SMT 能力矩阵（unsupported / per-context / shared）+ per-context 实例复制，复制不了的继续配置期拒绝（债 b） | `06d2864c92e55b40664f549b15ac74928900bd53` | 187-module-smt-capability（+186 升级） | 是 |
| 3.4 | 前端仲裁：fetch/decode/dispatch 逐预算点共享 budget + 轮转 + 工作保存 | `06d2864c92e55b40664f549b15ac74928900bd53` | 312-frontend-arbitration | 是 |
| 3.5 | **后端仲裁 + Option A**：execute/complete 全局 oldest-first，按 `dispatch_sequence`（非 `instr_id`）作跨 context 年龄键；调度共享占用 ≤ `SCHEDULER_SIZE` + per-context 水位 | `06d2864c92e55b40664f549b15ac74928900bd53` | 313-backend-arbitration | 是（A 的 miss/mispredict 不挡 B，无饿死） |
| 3.6 Item 1 | L1D 准入显式：每周期 core→L1D 1 load + 1 store（布线决定的隐藏预算写死） | `3381739ddc9c371898e770f84fded93839b2dd15` | 418-l1d-admission | 是 |
| 3.6 Item 2 | 翻译暂存（translation stash）按地址空间分区：一个 context 的 TLB miss 突发不填满 stash 挡 sibling | `6c5ecc40d5f11082b898f2418fe1e8481efc84c4` | 419-l1d-translation-stash-partition | 是 |
| 3.6 Item 3 | `prefetch_line` 来源显式：`ip_stride` 的延迟发射不再拿「cache 最后处理的 context」，改带触发者的 asid+cpu | `8100bd8d0828d3ba3476c16c1137c29537527683` | 455-ip-stride-origin | 是 |
| 3.7 Item 1 | 419 完整闭环：sibling 未翻译 load 走完整 walk→回填→下层 fill→带自身 asid 的 response 返回 | `ed19dbe34eaea17c58908b9657f46a6db3173e45` | 456-l1d-translation-completion | 是（移除 mock_translator 后 3 条断言全红，恢复后全绿） |
| 3.7 Item 2 | 3 参兼容 `prefetch_line` 零生产调用者的 grep 证据（无代码改动） | 无 commit | — | 不适用 |
| 3.7 Item 3 | 五条真·双 context 判据（同 reg 独立 / miss 不挡 sibling / 未退休 store 不误认 ROB 前沿 / 无饿死 / oldest-first 顺序） | `29171b75ae91508c02ce91bb7e8fb75ecf6f5c04` | 314-dual-context-backend-criteria | 是（临时把 `min(dispatch_sequence)` 换成 fixed-priority，恰好 (4)(5) 红、(1)(2)(3) 绿，恢复后全绿） |

> 说明：3.1–3.5 的**代码改动**落在同一个 commit `06d2864c`（“Stage 3: dual-context SMT front-end and back-end arbitration”），前端与后端仲裁一并交付，未按「每步一 commit」拆开——这是本分支历史的既成事实，此处如实列出，各步的新增测试号仍可对上。

---

## 3. 三个关键设计决策（+ 为什么）

1. **Option A：execute/complete 用全局 oldest-first（`min(dispatch_sequence)`），不是轮转。**
   为什么：跨 context 的年龄不能再用 `instr_id`（各 context 自增、会撞），必须引入 core-wide `dispatch_sequence`。若用轮转/per-context 固定优先级，年轻指令可能排到更老 sibling 前面——test 314 的判据 (4)(5) 直接锁死：`{seq 0,3; 1,2}` 交错时，全局年龄顺序是 `{10,20,21,11}`，轮转会给 `{10,11,20,21}`。这是「正确性问题」，不是性能偏好。

2. **Decision 7：翻译暂存 / MSHR / RQ 按 asid 分区。**
   为什么：一个 context 的 TLB miss 突发若占满 L1D 的 translation stash，会挡住 sibling 的未翻译 load——共享结构里的一个 context 就能饿死另一个，等价于隔离失效。分区把「互挡」从可能变成不可能。

3. **`prefetch_line` 来源显式（asid + cpu 线程化），绝不用「last-processor」隐式状态。**
   为什么：默认 `ip_stride` 的延迟发射在 tag check 之后才走，若从 `prefetcher_cycle_operate` 拿「cache 最后处理的 context」，拿到的不是触发者的 asid，虚拟 prefetch 会落进错误地址空间。改成显式传参后，bug 没有「换个门溜回来」的可能（3.7 Item 2 的 grep 证据证明 3 参兼容版本已零生产调用）。

---

## 4. N=1 安全锁（三把，逐字节锁定）+ 每步归因

三把锁（`~/SMT-experiment/stage1/run-accept.sh stage37` 实测，与锁定值逐字节一致）：

```
baseline.norm  8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6
prf64.norm     9bb10576f07f5e12983d7711a01ad2811be871b9f76b9f155ce46986a659ea56
sched64.norm   8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6
```

逐步归因：

- **3.1–3.5（commit `06d2864c`）**：改动路径**被 parity gate 跑到**——单 context 模式继续走 `compare_optimization.py` 逐位 gate，三配置 sha256 回到 8dcc/9bb1/8dcc，证明拆 context 没有改坏单 context 的数值路径。
- **3.6 三件套（`3381739d` / `6c5ecc40` / `8100bd8d`）**：同样改动路径被 parity gate 跑到（缓存侧改动，N=1 下 asid 恒 `{0,0}`，物理/翻译匹配等价，逐位一致仍须成立）。
- **3.7 三件套（`ed19dbe3` / grep 无 commit / `29171b75`）**：三条 commit **全是 test-only**，改动路径 N=1 不活动——所以这是**回归确认**（锁还在），**不是改动验证**（没有生产代码改动可验）。

---

## 5. 测试对账

`make test` 最终总数（Stage 3 关闭时）：

```
test cases: 1012 | 992 passed | 20 skipped
assertions: 118584 | 118584 passed | 0 skipped
```

较 Stage 3.6 的 `1006 例 / 118537 断言` 增加 **6 例 + 47 断言**，正好等于 3.7 两个 test-only commit：456 的 `1+23` 与 314 的 `5+24`。

---

## 6. 诚实边界（Stage 3 证明了什么，没证明什么）

**证明了（单元级）：** 双 context 架构隔离（同 reg/同虚页/翻译命中不互串）、共享资源逐预算点仲裁不翻倍、无饿死、Option A 的 oldest-first 发射顺序。这些由 311–314 / 416–419 / 455–456 在**测试基座手喂流**下层下覆盖。

**没证明（留给 Stage 4 的入口）：** 「两份**带 asid 的完整 trace** 端到端喂进同一颗核、跑出隔离结果」。当前 trace→core 只填 `contexts.front().input_queue`（context 0），asid 在 v1/v2 trace 记录里**没有字段**、由 tracereader 边界按 `{cpu,cpu}` 盖章——所以「地址空间身份来自输入、而非恒为 `{0,0}`」这件事，Stage 3 一行都没动。这正是 Stage 4 的活。
