# Stage 5 总结（债 b 核销确认：8 个全局态单例全部定案，验证收口）

> 代码基线：`~/ChampSim2` 的 `smt-stage5-singletons` 分支。
> 姊妹文档：`~/SMT-experiment/stage5-plan.md`（设计，含清货表）、`~/SMT-experiment/stage3-debt.md`（债 b 原文）、`stage3-summary.md`（3.3 核销记录）、`stage4-summary.md`（上一阶段）。
> 本文件是 Stage 5 关闭后的沉淀记录，六节照骨架写，不省略。

---

## 1. 目标

一句话：核销 stage3-debt.md 债 b 的 8 个全局态单例。清货取证（grep）显示它们在 **Stage 3.3（commit `06d2864c`）已全部定案**——4 个 cbp6 复制不了、已配置期拒绝；4 个 BTB 能复制、已 per-context 化——所以 Stage 5 的实质从「改行为」降级为「验证收口」：补一条端到端拒绝测试，把债 b 的「拒绝」半边钉进 `static_environment` 构建链；复跑 N=1 三把锁确认无回归；并如实记录一个 out-of-scope 的多核隐患，显式落一条 stage6 债。**本阶段无任何 `src/` 改动。**

---

## 2. 分步清单（每步：做了什么 / commit / 新增测试 / 红转绿）

| 步 | 做了什么 | commit（40 位） | 新增测试 | 红→绿 |
|---|---|---|---|---|
| 清货 | grep 取证 8 个单例的真实声明/使用，结论「债 b 已在 3.3 核销、无一裸共享」 | 无 commit（纯侦察） | 无 | 不适用 |
| 设计 | `stage5-plan.md`（清货表 + 逐单例定案 + 三个决策点） | 无 commit（文档在 `~/SMT-experiment/`） | 无 | 不适用 |
| 补强 A | 端到端拒绝用例（`num_contexts=2` + 4 个 cbp6 → `static_environment` 构造抛错）+ 正面对照（`num_contexts=2` + ittage_32kb 不抛） | `9cfbc9890ecb51cf1e5baace4790d1d96389d18f` | +2 用例（2 个 TEST_CASE）+5 断言（4×`REQUIRE_THROWS_WITH` + 1×`REQUIRE_NOTHROW`） | 是（红：临时拔掉两条 `check_smt_capability` 后「refuses」用例失败；恢复后全绿） |
| N=1 复跑 | 三把锁复跑（`tools/run-accept.sh stage5-final`） | 无 commit（验证） | 无 | 不适用 |
| 收口 | `stage5-summary.md`（本文件） | 本 commit | 无 | 不适用 |

> 测试总数：补强 A 后 `make test` 全量绿 `1016 例 / 996 通过 / 20 跳过 / 118604 断言`（较 Stage 4 关闭的 `1014 / 994 / 20 / 118599` 增 2 例 + 2 通过 + 5 断言，20 skipped 不变）。增数正好等于补强 A 的 2 个 TEST_CASE + 5 条断言，20 跳过（native/ramulator2 禁用那批）全程不动。

---

## 3. 三个关键设计决策（+ 为什么）

Stage 5 没有代码设计，只有三条「定位」决策（对应 `stage5-plan.md` §5 的 D-A / D-B / D-C）：

1. **接受「债 b 已核销」，本阶段 = 验证收口，不改 `src`。**
   为什么：清货证据显示 8 个单例无一裸共享——cbp6×4 未声明 capability → 默认 `unsupported` → 配置期拒绝；BTB×4 已 per-context 复制。债 b 原文（stage3-debt.md）记的是 Stage 2 收尾时状态；Stage 3.3 已把 `require_single_core` 换成 `smt_capability` 能力矩阵并落地 + 测试（`06d2864c`）。没有需要「改行为」的单例，硬改反而引入 N=1 数值风险。

2. **补强 A 从「可选」升级为「必做」。**
   为什么：187 只单测了 `check_smt_capability` 函数和 `capability()` 标签，没有一条用例走完整 `static_environment` 构建链验证 `num_contexts=2 + cbp6 → 抛错`。补强 A 把「cbp6 被拒」从单元级钉到构建链级，是债 b「拒绝」半边的收口，不是锦上添花。

3. **隐患 B 只记录、不改，显式落 stage6 债。**
   为什么：ittage 用函数局部 static（进程级一份、跨核共享数组），blbp 用成员数组（每核一份）。单核 SMT（本阶段范围）两者都正确；多核下 ittage 会跨核别名。多核是独立 binary、非本阶段范围，改存储方式是行为改动、动 N=1 风险，需另评估。

---

## 4. 诚实发现

1. **债 b 已在 Stage 3.3（`06d2864c`）核销，Stage 5 实为验证收口、无 src 改动。**
   清货表（`stage5-plan.md` §1）逐单例给出 grep 证据：8 个单例无一裸共享——cbp6×4 复制不了、已配置期拒绝（未声明 capability → 默认 `unsupported` + `select_modules` 对 branch/btb 各调一次 `check_smt_capability`）；BTB×4 已 per-context 复制（ittage 用函数局部 static 数组、blbp 用成员数组）。债 b 原文记的是 Stage 2 收尾时状态，Stage 3.3 已核销并落地测试（187/186）。所以本阶段从头到尾没有任何 `src/` 改动，唯一的代码产出是补强 A 一条 test-only 用例。

2. **补强 A 的变量隔离：throw 是「unsupported 模块被拒」，不是「`num_contexts=2` 本身会抛」。**
   补强 A 的第二个用例（`num_contexts=2` + ittage_32kb → `REQUIRE_NOTHROW`）作正面对照，证明第一个用例的 throw 只来自 unsupported 的 cbp6，而非 `num_contexts=2` 本身。红演示（临时注释掉 `select_modules` 里两条 `check_smt_capability`，rebuild）让「refuses」用例失败（`no exception was thrown where one was expected: name := cbp6_tagescl64`）、还原后全绿，证明测试非空转、真能抓住「拒绝被拔掉」。

3. **N=1 三把锁全程不变（8dcc/9bb1/8dcc），因为没有生产代码改动可验。**
   本阶段唯一代码改动是 test-only（补强 A），改动路径 N=1 不活动。所以三把锁复跑是「回归确认」（锁还在），不是「改动验证」。

---

## 5. N=1 三把锁（全程 8dcc/9bb1/8dcc 逐字节不变）

三把锁（`tools/run-accept.sh stage5-final` 实测，与锁定值逐字节一致）：

```
baseline.norm  8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6
prf64.norm     9bb10576f07f5e12983d7711a01ad2811be871b9f76b9f155ce46986a659ea56
sched64.norm   8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6
```

IPC 2.34 / 1.778 / 2.34。逐步归因：

- **补强 A（`9cfbc989`）**：test-only，改动路径 N=1 不活动，三把锁必须不变——这是**回归确认**，不是改动验证。
- 本阶段其余步骤（清货 / 设计 / 收口）均为文档，不碰 `src`，N=1 数值路径全程未动。

---

## 6. 诚实边界（Stage 5 证明了什么，没证明什么）

**证明了：** 债 b 的 8 个单例已全部定案（cbp6×4 配置期拒绝、BTB×4 per-context 复制），「拒绝」半边现在有端到端构建链级的测试收口（补强 A，`9cfbc989`），「复制」半边的隔离行为由 186 钉死。全套 `make test` 全量绿（`1016 / 996 / 20 / 118604`），N=1 三把锁 8dcc/9bb1/8dcc 逐字节不变。

**没证明 / 留下的隐患 B（→ 显式落一条 stage6 债）：**

- **隐患 B：ittage 的函数局部 static 跨核共享。** ittage_32kb / ittage_64kb 的 per-context 实例是函数局部 static `std::array<impl_type,max_contexts>`（进程级一份、跨核共享数组），而 blbp_64kb / blbp_64kb_tuned 是成员数组（每核一份）。单核 SMT（本阶段范围）两者都正确；多核 SMT 下 ittage 两个核的 context 0 会共用同一槽位（跨核别名）。多核是独立 binary、非本阶段范围，本阶段不改。

- **Stage 6 债（显式落账）：ittage 的 per-context 实例存储需 per-core 化。** 债项：`btb/ittage_32kb` 与 `btb/ittage_64kb` 的 per-context 实例用函数局部 static 数组（进程级、跨核共享），单核 SMT 正确，多核 SMT 下会跨核别名。Stage 6 若开多核，需把 ittage 的实例存储从「函数局部 static 数组」改为「per-core per-context 键」（成员数组，或 `std::array<std::array<impl_type,max_contexts>, num_cpus>`），属行为改动，需另评估 N=1 影响；blbp 已经是成员数组，无需改。此债只写进本文件与 `stage5-plan.md` §4/§5，不写进代码，留待 Stage 6 决策。
