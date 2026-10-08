#!/bin/bash
# Stage 1 acceptance: run the three stress points and capture full stdout.
# Usage: run-accept.sh <label>
set -u
LABEL="${1:?usage: run-accept.sh <label>}"
OUT="$HOME/SMT-experiment/stage1/$LABEL"
mkdir -p "$OUT"
cd /home/yyz/ChampSim2 || exit 1
TRACE=$HOME/traces/649.fotonik3d_s-1B.champsimtrace.xz
run_one() {
  local name="$1"; shift
  ./bin/champsim --config configs/sample.toml --trace-version=1 -w 1000000 -i 3000000 "$@" -- "$TRACE" \
    > "$OUT/$name.stdout" 2> "$OUT/$name.stderr"
  echo "  $name exit=$? ipc=$(grep -o 'Simulation finished.*cumulative IPC: [0-9.]*' "$OUT/$name.stdout" | grep -o '[0-9.]*$')"
}
run_one baseline
run_one prf64 --set ooo_cpu.cpu0.register_file_size=64
run_one sched64 --set ooo_cpu.cpu0.scheduler_size=64
# Full normalization: drop environment noise only -- the wall-clock time in
# each progress line, and the trace absolute path. Everything else (every
# cycle/IPC/counter) is kept, so the digest is invariant to machine/user/-
# wherever the trace happens to live.
for f in baseline prf64 sched64; do
  grep -vE 'Simulation time:| runs /|\.champsimtrace' "$OUT/$f.stdout" > "$OUT/$f.norm"
done
sha256sum "$OUT"/*.norm | sed "s|$OUT/||"
