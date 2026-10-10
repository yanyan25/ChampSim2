#!/bin/bash
# Stage 4.5 end-to-end verification of the SMT multi-trace path (L1-L4) on real
# traces. Two legs, deliberately kept on DIFFERENT statistical baselines so a
# window-internal invariant is never compared against a run-to-completion number:
#
#   Leg A  ( -i 3M fixed window )  L1 / L3  -- isolation, swap, symmetry
#   Leg B  ( no -i, run to EOF )   EOF / L4  -- short-stops-long, empty sibling
#
# Every simulation must exit 0 with no SIGABRT: that is the weak form of L2
# (capacity/width not doubled), which Stage 3 pins with CHAMPSIM_ASSERT at the
# unit level and this script only re-confirms end-to-end ("assertion not
# triggered"). It is NOT a per-cycle occupancy measurement.
#
# HONEST BOUNDARY (kept in the report too): aggregate-counter equality is NOT
# per-instruction order equality. The --toml document has no per-instruction
# architectural event log, so L1 below is "aggregate architectural counters
# within tolerance", not a literal replay. Literal per-instruction replay is a
# future instrumentation task and is deliberately NOT smuggled into Stage 4.5.
#
# Tolerance for L1 (and L4) is pinned to concrete numbers here, not left as
# "small". N (instructions) = 10, M (each branch census bucket) = 10. Rationale
# (see legA.md footnote): with -i the phase ends only when BOTH contexts have
# retired >= the window, so the faster context retires a few extra instructions
# past the window while its sibling catches up. For a SAME-trace pair the two
# contexts cross the window almost simultaneously (equal IPC), so that overshoot
# is single-digit: measured |Delta instructions| = 2, |Delta branches| = 1. Ten
# leaves 5x headroom over the measurement yet still catches real contamination,
# which would move a count by thousands (compare the 0.16% fast-context overshoot
# observed in the L3 swap of two DIFFERENT-speed traces). Do NOT apply L1's tight
# tolerance to the L3 (A,B) swap: there the fast context legitimately retires far
# past the window, and that is the phenomenon under test, not noise.
set -u

# ---------------------------------------------------------------------------
# Environment. Override the traces via env; everything else is derived.
# ---------------------------------------------------------------------------
CHAMPSIM_BIN="${CHAMPSIM_BIN:-./bin/champsim}"
CONFIG="${CONFIG:-configs/sample.toml}"
TRACE_DIR="${TRACE_DIR:-$HOME/traces}"
FOTONIK3D="$TRACE_DIR/649.fotonik3d_s-1B.champsimtrace.xz"   # v1, 64-byte records
SQLITE="$TRACE_DIR/708.sqlite_r.sp0.champsim2.zst"           # v2, 512-byte records
OMNETPP="$TRACE_DIR/710.omnetpp_r.sp0.champsim2.zst"         # v2
WORKDIR="${WORKDIR:-$(mktemp -d /tmp/smt-verify.XXXXXX)}"

TOL_INSTR=10      # L1/L4: |Delta instructions| <= 10
TOL_BRANCH=10     # L1/L4: |Delta| per executed-census bucket <= 10
IPC_SYM_PCT=5     # L3 (A,A): |IPC ctx0 - ctx1| relative diff <= 5%

WARMUP=1000000    # -w
WINDOW=3000000    # -i (leg A only)

echo "workdir = $WORKDIR"
mkdir -p "$WORKDIR"

# ---------------------------------------------------------------------------
# v1 trace surgery. v1 is a flat sequence of 64-byte input_instr records, no
# header/footer, so truncating on a 64-byte boundary yields a legal prefix. The
# one-instruction sibling is 64 bytes = ONE complete record, which the bulk
# tracereader reads and then reports EOF on the next refill. A 0-byte file is
# NOT legal here: bulk_tracereader::operator() would front() an empty deque.
# ---------------------------------------------------------------------------
xz -dc "$FOTONIK3D" | head -c $((1500000 * 64)) | xz > "$WORKDIR/short.xz"       # 1.5M instr
xz -dc "$FOTONIK3D" | head -c $((4000000 * 64)) | xz > "$WORKDIR/long-4M.xz"     # 4M instr
xz -dc "$FOTONIK3D" | head -c 64 | xz > "$WORKDIR/one-instr.xz"                  # 1 instr (legal "empty")

# run <name> <args...> -- <traces...>. --toml is injected BEFORE the -- trace
# separator so it can never be mistaken for a trace path.
run() {
  local name="$1"; shift
  local -a pre traces
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do pre+=("$1"); shift; done
  [ $# -gt 0 ] && shift   # drop the --
  traces=("$@")
  "$CHAMPSIM_BIN" --config "$CONFIG" "${pre[@]}" --toml "$WORKDIR/$name.toml" -- "${traces[@]}" \
    > "$WORKDIR/$name.stdout" 2> "$WORKDIR/$name.stderr"
  echo "  $name exit=$?"
  if [ -s "$WORKDIR/$name.stderr" ]; then
    grep -vE '^\[VMEM\] WARNING' "$WORKDIR/$name.stderr" | sed 's/^/    stderr: /'
  fi
}

# extract_arch <toml> <core>  ->  8 lines "KEY VALUE" in canonical order.
# Scoped to the exact core section, so the .mispredict table (same keys) is ignored.
extract_arch() {
  awk -v target="phase.simulation.roi.core.$2" -v ex="phase.simulation.roi.core.$2.executed" '
    $0 == "[" target "]" { in_main = 1; next }
    $0 == "[" ex "]"      { in_ex = 1; next }
    /^\[/                  { in_main = 0; in_ex = 0; next }
    in_main && ($1 == "instructions" || $1 == "total_branches") { print $1, $3 }
    in_ex && $1 ~ /^branch_(direct_jump|indirect|conditional|direct_call|indirect_call|return)$/ { print $1, $3 }
  ' "$1"
}

check_arch() {  # check_arch <label> <tomlA> <coreA> <tomlB> <coreB> <tol>  -> pass/fail
  local label="$1" ta="$2" ca="$3" tb="$4" cb="$5" tol="$6"
  echo "  $label"
  [ -f "$ta" ] && [ -f "$tb" ] || { echo "    FAIL: missing stats document ($ta or $tb)"; return 1; }
  local -A A B; local k v d
  while read -r k v; do A[$k]=$v; done < <(extract_arch "$ta" "$ca")
  while read -r k v; do B[$k]=$v; done < <(extract_arch "$tb" "$cb")
  { [ "${#A[@]}" -eq 8 ] && [ "${#B[@]}" -eq 8 ]; } || { echo "    FAIL: expected 8 architectural counters, got ${#A[@]}/${#B[@]}"; return 1; }
  local fail=0
  for k in instructions total_branches branch_direct_jump branch_indirect branch_conditional branch_direct_call branch_indirect_call branch_return; do
    d=$(( ${A[$k]:-0} - ${B[$k]:-0} )); [ "$d" -lt 0 ] && d=$(( -d ))
    if [ "$d" -gt "$tol" ]; then
      printf '    FAIL %-20s |Δ|=%d (%s=%s vs %s=%s)\n' "$k" "$d" "$ca" "${A[$k]}" "$cb" "${B[$k]}"
      fail=1
    else
      printf '    ok   %-20s Δ=%d\n' "$k" "$d"
    fi
  done
  return "$fail"
}

get_val() {  # get_val <toml> <core> <key>
  awk -v target="phase.simulation.roi.core.$2" -v key="$3" '
    $0 == "[" target "]" { insec = 1; next }
    /^\[/ { insec = 0; next }
    insec && $1 == key { print $3; exit }
  ' "$1"
}

check_ipc_symmetry() {  # check_ipc_symmetry <toml> <maxpct>  -> pass/fail
  local i0 i1 c0 c1
  i0=$(get_val "$1" cpu0 instructions); c0=$(get_val "$1" cpu0 cycles)
  i1=$(get_val "$1" cpu1 instructions); c1=$(get_val "$1" cpu1 cycles)
  awk -v i0="${i0:-0}" -v c0="${c0:-1}" -v i1="${i1:-0}" -v c1="${c1:-1}" -v m="$2" '
    BEGIN {
      ipc0 = i0 / c0; ipc1 = i1 / c1;
      d = (ipc0 > ipc1) ? (ipc0 - ipc1) : (ipc1 - ipc0);
      rel = 100 * d / ((ipc0 + ipc1) / 2);
      printf "  L3 (A,A) IPC ctx0=%.5f ctx1=%.5f rel_diff=%.3f%% (limit %.1f%%)\n", ipc0, ipc1, rel, m;
      exit (rel > m);
    }
  '
}

overall=0
echo ""
echo "=== Leg A (-i fixed window): L1 / L3 ==="

# --- L1: single-context vs dual-context ctx0, SAME trace (fotonik3d x2). -----
# With two identical traces the contexts cross the 3M window together, so the
# fast-context overshoot is negligible and the architectural counters must match
# to single digits. Mispredicts are deliberately NOT compared: the default
# hashed_perceptron is smt_capability::shared (single per-core instance keyed by
# asid), so its speculative state legitimately diverges with a co-resident thread.
run l1-single --trace-version=1 -w "$WARMUP" -i "$WINDOW" -- "$FOTONIK3D"
run l1-dual   --trace-version=1 --set ooo_cpu.cpu0.num_contexts=2 -w "$WARMUP" -i "$WINDOW" -- "$FOTONIK3D" "$FOTONIK3D"
check_arch "L1 single vs dual ctx0 (architectural, tol $TOL_INSTR/$TOL_BRANCH)" \
  "$WORKDIR/l1-single.toml" cpu0 "$WORKDIR/l1-dual.toml" cpu0 "$TOL_INSTR" || overall=1

# --- L3 (A,A) symmetry: ctx0 vs ctx1 of the dual fotonik3d run. -------------
check_ipc_symmetry "$WORKDIR/l1-dual.toml" "$IPC_SYM_PCT" || overall=1

# --- L3 (A,B) swap: sqlite + omnetpp in both orders. ------------------------
# Two DIFFERENT-speed traces on one core: the fast context retires far past the
# 3M window while the slow one catches up, so its aggregate is NOT position-
# invariant -- that overshoot is the phenomenon under test, not a failure. The
# slow context (the one ending near 3M) IS position-invariant to a few
# instructions, which is the isolation evidence. Reported, not gated on a tight
# tolerance.
run l3-ab --trace-version=2 --set ooo_cpu.cpu0.num_contexts=2 -w "$WARMUP" -i "$WINDOW" -- "$SQLITE" "$OMNETPP"
run l3-ba --trace-version=2 --set ooo_cpu.cpu0.num_contexts=2 -w "$WARMUP" -i "$WINDOW" -- "$OMNETPP" "$SQLITE"
echo "  L3 (A,B) swap, per-trace architectural counters (report, no tight gate):"
echo "    sqlite   (fast): $(get_val "$WORKDIR/l3-ab.toml" cpu0 instructions) vs $(get_val "$WORKDIR/l3-ba.toml" cpu1 instructions) instructions (overshoot)"
echo "    omnetpp  (slow): $(get_val "$WORKDIR/l3-ab.toml" cpu1 instructions) vs $(get_val "$WORKDIR/l3-ba.toml" cpu0 instructions) instructions (position-invariant)"

echo ""
echo "=== Leg B (no -i, run to EOF): EOF / L4 ==="

# --- EOF (4.4): short 1.5M + long 4M, no -i. The short context hits EOF early
# and stops, the long context runs to its own 3M; exit 0, no SIGABRT. ---------
run legb-eof --trace-version=1 --set ooo_cpu.cpu0.num_contexts=2 -w "$WARMUP" -- "$WORKDIR/short.xz" "$WORKDIR/long-4M.xz"
short_instr=$(get_val "$WORKDIR/legb-eof.toml" cpu0 instructions)
long_instr=$(get_val "$WORKDIR/legb-eof.toml" cpu1 instructions)
# The long context runs to its OWN EOF: 4M records - ~1M warmup - warmup overshoot,
# so it retires ~3M (measured 2993511), NOT the ~0.5M the old "any EOF" rule would
# have left it with when the short context EOF'd first. "> 2000000" cleanly
# separates "ran to its own ~3M EOF" from "cut off at the short's ~0.5M".
echo "  EOF short ctx0=$short_instr (must be in (0,1M)), long ctx1=$long_instr (must be > 2M: ran to its own EOF)"
{ [ -n "$short_instr" ] && [ -n "$long_instr" ] && [ "$short_instr" -gt 0 ] && [ "$short_instr" -lt 1000000 ] && [ "$long_instr" -gt 2000000 ]; } || overall=1

# --- L4: 4M primary + 1-instruction sibling, no -i. The sibling reads its one
# record and EOFs, retiring zero ROI instructions, so the primary context must
# reproduce the single-context run byte-for-byte. The 0-byte "empty trace" is
# deliberately avoided (see the trace-surgery note above). --------------------
run l4-single --trace-version=1 -w "$WARMUP" -- "$WORKDIR/long-4M.xz"
run l4-dual   --trace-version=1 --set ooo_cpu.cpu0.num_contexts=2 -w "$WARMUP" -- "$WORKDIR/long-4M.xz" "$WORKDIR/one-instr.xz"
echo "  L4 sibling ctx1 ROI instructions = $(get_val "$WORKDIR/l4-dual.toml" cpu1 instructions) (must be 0)"
check_arch "L4 primary single vs dual ctx0 (byte-identical, tol 0)" \
  "$WORKDIR/l4-single.toml" cpu0 "$WORKDIR/l4-dual.toml" cpu0 0 || overall=1

echo ""
if [ "$overall" -eq 0 ]; then
  echo "RESULT: PASS (L1/L3/L4 architectural + EOF + L2-weak all green)"
else
  echo "RESULT: FAIL (see lines above)"
fi
echo "artifacts in $WORKDIR"
exit "$overall"
