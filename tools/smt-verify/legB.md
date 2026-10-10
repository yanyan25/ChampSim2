# Stage 4.5 — Leg B ( 跑完为止 ): EOF / L4 / L2

`tools/smt-verify/verify_stage4.sh` 一键复现本腿。所有跑法 **不带 `-i`**、`-w 1000000`
（腿 B 口径：trace 只读一遍，跑到自然 EOF，短 trace 先停、sibling 继续）。stderr 只有
`[VMEM] WARNING`，无 panic / critical / SIGABRT。

## 口径纪律（腿 B 的 IPC/cycles 不与腿 A 比）

腿 B 的 IPC/cycles 是「跑到 trace 完」的数，腿 A 是「固定窗口」的数，两者永不直接
比大小。腿 B 只做定性判据（短停长续、空 sibling 复现单 context），不产 IPC 数字去
和腿 A 对齐。

## EOF — 短停长续（4.4 已收，这里复现锁死）

`num_contexts=2`，短 1.5M + 长 4M，不带 `-i`，`-w 1M`：

| context        | instructions | 判据                       |
|----------------|--------------|----------------------------|
| ctx0（短 1.5M）| 499996       | > 0 且 < 1M（短 trace 跑完）|
| ctx1（长 4M）  | 2993511      | 跑完自己 4M trace ≈ 3M（非被掐断）|

长 context 的 2993511 = 4M 记录 − ~1M warmup − warmup overshoot（dual 跑法里长 context
先到 1M warmup、等短 context 追平时多退 6489 条），即**跑完自己整条 trace 才 EOF**，
而不是被短 trace 的 EOF 掐断。exit 0，无 SIGABRT。旧规则 A（任一 trace EOF 掐断整
phase）会把长 context 停在 ~0.5M；4.4 的 per-context 谓词让长 context 跑到 ~3M——
EOF 分支端到端真跑通，非单测。

## L4 — 空 sibling 复现单 context

`num_contexts=2`，4M 主 trace（ctx0）+ 1 条指令的合法 sibling（ctx1），不带 `-i`。

**「空 trace」的合法造法**：v1 是 64 字节 `input_instr` 的平铺序列、无头无尾，所以
「内容为空」的合法最小 trace 是 **1 条记录 = 64 字节**（`head -c 64`）。tracereader
读进这 1 条、下次 refill 即 EOF——是「立即 EOF」的合法 trace。**0 字节文件不合法**：
`bulk_tracereader::operator()` 会对空 deque 调 `front()`，行为不定，所以刻意避开。

| 计数器（ctx0 架构）   | 单跑 4M    | 双跑 ctx0（+1 指令 sibling）| Δ |
|-----------------------|------------|------------------------------|---|
| `instructions`        | 2999401    | 2999401                      | 0 |
| `total_branches`      | 713212     | 713212                       | 0 |
| `.executed`[6]        | 85528/18050/472426/68501/102/68605 | 逐位同 | 0 |
| sibling ctx1 `instructions` | —    | 0                            | — |

ctx0 架构计数器**逐字节相等（Δ=0）**，sibling 退休 0 条 ROI 指令（它的 1 条指令在
warmup 就消费掉）。cycles 单跑 1281765 vs 双跑 1281793（Δ28 = 0.002%，sibling 那 1
条指令的仲裁扰动，可忽略）。空 sibling 不参与仲裁、完整复现单 context，**通过**。

## L2 — 弱形式（诚实声明）

L2（容量/宽度不翻倍）在 Stage 3 由 `CHAMPSIM_ASSERT`（单测 312/313/418）锁死。4.5
真 trace 的等价是「腿 A/腿 B 所有跑法 exit 0、无 SIGABRT」= 断言未触发。本腿 8 个
进程（含腿 A）全部 exit 0，**L2 弱形式通过**。这不是逐 cycle occupancy 测量（那又是
一次 instrumentation，不在 4.5 范围）。
