# Stage 2 总结（地址空间身份贯通内存系统 + 模块隔离）

> 代码基线：`~/ChampSim2` 的 `smt-stage1-context-extraction` 分支（Stage 2 关闭于 `1af168ed`，Stage 3 在此基线上继续）。
> 姊妹文档：`stage2-plan.md`（侦察 + 拆解计划）、`stage2-implementation.md`（逐步执行记录，含结论 1/2）、`stage3-summary.md`。
> 本文件是补写的 Stage 2 沉淀记录，六节骨架与 `stage3-summary.md` 一致。

---

## 1. 目标

一句话：把 **address-space identity（asid）** 从「请求发起处」贯通到内存系统的每一段（channel/cache/PTW/DRAM）、把 VM 映射与关键前端结构的键从「物理核号」换成「asid」，并给 8 个进程级共享单例模块装上按 context 数拒绝的守卫——**全程不改变 N=1 的逐位结果**。

---

## 2. 分步清单（2.1 → 2.7，每步：做了什么 / commit / 新增测试 / 红转绿）

| 步 | 做了什么 | commit（40 位） | 新增测试 | 红→绿 |
|---|---|---|---|---|
| 2.1 + 2.4 | 补全请求链路 asid 字段（纯搬家）+ store 转发显式 asid 断言 | `0c72172db125ce3cca281d9babb8ff07655be0dc` | （仅改 431） | 是（三配置逐位一致 8dcc/9bb1/8dcc） |
| 2.2 | VM 映射键 cpu→asid；修正 601/447 用显式 asid={1,1} 表达第二地址空间 | `176b2d05735567a28a3171813544ac51186d652a` | 801-vmem-duplicated（非恒等：asid 250/251 同 VA 分物理页） | 是 |
| 2.3 | load 完成键改 `(block, asid)`；`load_waiters` 字段端到端铺好但不参与收窄完成 | `53ce950e2488e72720270fb32c7117ebe8d4977b` | 253-load-completion-by-waiter | 是 |
| 2.5 | DIB / 分支预测器 / BTB 按 asid 隔离 | `bfa6ec164356d80a7dd3d7487c6592606ece001b` | 185-asid-isolation | 是 |
| 2.7（含 2.6） | 模块守卫按 context 数拒绝（配置期）+ 翻译**完成侧**按 asid 键控 | `1af168edd7914cc73a203ee348aa16ecb74db2f5` | 186-global-state-module-guard、416-translation-asid-isolation | 是 |

> 说明：2.4 与 2.1 合并在 `0c72172d`；2.6（模块守卫）折进了 2.7 的 `1af168ed`（其 subject 即 “context-count module guard”）。

测试总数逐步：2.1+2.4 → `972/952/20`；2.2 → `973/953/20`；2.3 → `976/956/20`；2.7 关闭 → `982/962/20`。

---

## 3. 关键设计决策（+ 为什么）

1. **asid 贯通走「纯搬家」先行，N=1 逐位一致作安全带。**
   为什么：先只把 asid 字段端到端铺进 channel/cache/PTW/DRAM、不改变任何完成判断键，N=1 下 asid 恒 `{0,0}`，三配置 sha256 逐字节回到 8dcc/9bb1/8dcc——证明「没改坏 N=1」，才敢在 2.2/2.3 做真正的「改行为」。

2. **load 完成键 = `(block, asid)`，不是 `load_waiter_id` 精确匹配。**
   为什么：`stage2-implementation.md` 结论 1 实测——原代码是广播语义，一个响应会完成所有同 block 的 load（含自己请求还在飞的）。精确 waiter 完成在 N=1 下**必然漂移**（fotonik3d 跑出 196113 处差异）。所以完成判断用 `(block, asid)` 保逐位一致，`load_waiters` 字段留作显式身份给 Stage 3 用，但**不参与**完成判断。

3. **8 个全局态单例（cbp6×4 + ittage×2 + blbp×2）配置期拒绝，不静默扩到多 context。**
   为什么：这些预测器本体 / BTB / RAS 活在函数局部 static 单例里，进程级共享，无法按 context 复制。守卫查 `(cpu_index != 0 || num_contexts > 1)` 就拒绝——宁可不支持，也不给一个「跑起来但串台」的 SMT 基线。

---

## 4. N=1 安全锁（三把，逐字节锁定）

全程每一步（含纯搬家步）都验三配置逐位一致，锁定值自 Stage 1 起不变：

```
baseline.norm  8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6
prf64.norm     9bb10576f07f5e12983d7711a01ad2811be871b9f76b9f155ce46986a659ea56
sched64.norm   8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6
```

这是 Stage 2 的主判据：所有改动路径都被 parity gate 跑到，N=1 下 asid 恒 `{0,0}`，物理/翻译匹配等价，逐位一致必须成立。

---

## 5. 测试对账

`make test` Stage 2 关闭时：

```
test cases: 982 | 962 passed | 20 skipped
```

新增/升级测试：253（load 完成按 waiter）、801（同 VA 不同 asid 分物理页，非恒等）、185（asid 隔离）、186（全局态模块守卫）、416（翻译 asid 隔离）。

---

## 6. 诚实边界（Stage 2 证明了什么，没证明什么）

**证明了：** asid 身份已端到端贯通、VM/load 完成/DIB/分支/BTB 的关键键从核号换成 asid、8 个共享单例被守卫拦住；N=1 逐位一致全程未破。翻译**完成侧**已按 asid 匹配（`matches_vpage`）。

**没证明 / 留给 Stage 3：**
- **翻译命中/查找侧仍按地址别名**（`try_hit`/`matches_address` 无 asid，`cache_block` 无 asid 字段）——这是欠账 a。
- **「四个身份数值相等」的地雷**：N=1 下 asid ≡ trace ≡ context ≡ 核号 ≡ 0，逐位一致只能证明「没改坏 N=1」，证明不了「身份换对了」。身份换对的证据只能靠跨 asid 单测（253/801/416）与非恒等端到端。
- **真·双 context 并发**本身不在 Stage 2 范围（欠账 d），那是 Stage 3。
