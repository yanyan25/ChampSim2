#!/usr/bin/env python3
"""Stage 5.3 (C): SMT fairness metrics from statistics documents.

Reads one co-phase statistics document (two hardware contexts) and two
standalone single-context documents (one per thread), and reports weighted
speedup (WS), average normalized turnaround time (ANTT), and per-thread
slowdown.

Two hard requirements (critique §7.4 / §7.6) are enforced, never optional:

1. The standalone denominator is a *single-context* run of the SAME binary /
   config / region -- never an idle sibling on a 2-context machine (critique
   §7.4). Enforced by requiring every standalone document to carry
   `num_contexts == 1` with exactly one `roi.core.cpu*` section, and by
   requiring its effective `[config]`, warmup and simulation counts to match
   the co-phase document exactly. (The `[meta].build_id` hash is NOT usable
   here: it folds in `num_contexts`, which is 2 in co-phase and 1 in
   standalone, so it necessarily differs and would reject every valid pair.)

2. Co-phase and standalone must replay the SAME measured region (critique
   §7.6's "region 不对齐 speedup 就是假数"). Because the phase ends only when
   BOTH contexts retire `-i`, the faster context retires past `-i` while its
   sibling catches up (the overshoot documented in `tools/smt-verify/`). The
   co-phase `cycles` field is therefore full-phase cycles, not the fast
   thread's own completion, and `instructions` carries the overshoot on top of
   `-i`; neither is that thread's own ROI IPC. The script reads the per-thread
   own-completion field `roi.core.cpu{i}.roi_cycles` instead (cycles from ROI
   begin to *this* context retiring its `-i`-th instruction), and REFUSES to
   compute when that field is absent. (The overshoot in `instructions` and in
   `cycles` cancels to first order while a thread's IPC is steady, so a naive
   `ipc` division often lands near the exact speedup -- but it is exact only
   under that assumption, and breaks when the fast thread stalls or its trace
   EOFs during the catch-up window, which is why the exact field is required
   rather than the tainted `ipc`.)

Metric definitions (stage5-plan.md §4.1): `speedup_i = IPC_i^co / IPC_i^standalone`,
`slowdown_i = 1 / speedup_i`, `WS = Σ speedup_i`, `ANTT = mean(slowdown_i)`.
With a fixed instruction count `M` per thread, `M` cancels and speedup reduces
to a cycles ratio: `speedup_i = cycles_standalone_i / roi_cycles_co_i`.
"""

from __future__ import annotations

import argparse
import sys
import tomllib
from typing import Any, Dict, Iterable, List, Sequence, Tuple


def load(path: str) -> Dict[str, Any]:
    with open(path, "rb") as fh:
        return tomllib.load(fh)


def _get(doc: Dict[str, Any], *path: str, default: Any = None) -> Any:
    for key in path:
        if not isinstance(doc, dict) or key not in doc:
            return default
        doc = doc[key]
    return doc


def trace_version(doc: Dict[str, Any]) -> Any:
    return _get(doc, "meta", "trace_version")


def warmup_instructions(doc: Dict[str, Any]) -> Any:
    return _get(doc, "meta", "warmup_instructions")


def simulation_instructions(doc: Dict[str, Any]) -> Any:
    return _get(doc, "meta", "simulation_instructions")


def num_contexts(doc: Dict[str, Any]) -> Any:
    return _get(doc, "config", "ooo_cpu", "cpu0", "num_contexts")


def randomization_seed(doc: Dict[str, Any]) -> Any:
    return _get(doc, "config", "vmem", "randomization")


def _flatten(table: Dict[str, Any], prefix: str = "") -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    for key, value in table.items():
        dotted = f"{prefix}.{key}" if prefix else key
        if isinstance(value, dict):
            out.update(_flatten(value, dotted))
        else:
            out[dotted] = value
    return out


def machine_config(doc: Dict[str, Any]) -> Dict[str, Any]:
    """The effective [config] as flat dotted keys, minus the two that legitimately
    vary between a co-phase and a standalone run.

    `[meta].build_id` cannot serve here: it hashes the effective config, which
    includes `ooo_cpu.cpu0.num_contexts` -- 2 in co-phase, 1 in standalone --
    so it necessarily differs and would reject every valid pair. Comparing the
    config directly, dropping only the controlled variable (`num_contexts`) and
    the seed (`vmem.randomization`, compared separately as a warning), is the
    honest "same machine" test the metric needs.
    """
    flat = _flatten(_get(doc, "config", default={}))
    flat.pop("ooo_cpu.cpu0.num_contexts", None)
    flat.pop("vmem.randomization", None)
    return flat


def core_stats(doc: Dict[str, Any]) -> Dict[int, Dict[str, Any]]:
    """Return {context_index: {instructions, cycles, roi_cycles}} for the ROI.

    `roi.core.cpu{i}` is flat-indexed over [core][context] (toml_printer.cc
    §325-328), so for the single-core 2-context machine `cpu0`/`cpu1` are core 0's
    two contexts, not two cores.
    """
    core = _get(doc, "phase", "simulation", "roi", "core", default={})
    out: Dict[int, Dict[str, Any]] = {}
    for key, table in core.items():
        if not (isinstance(key, str) and key.startswith("cpu")):
            continue
        idx = int(key[3:])
        out[idx] = {
            "instructions": table.get("instructions"),
            "cycles": table.get("cycles"),
            "roi_cycles": table.get("roi_cycles"),
        }
    return out


def validate(co: Dict[str, Any], standalones: Sequence[Tuple[str, Dict[str, Any]]]) -> Tuple[List[str], List[str]]:
    """Enforce the two hard requirements. Returns (errors, warnings)."""
    errors: List[str] = []
    warnings: List[str] = []

    co_machine = machine_config(co)
    co_warm = warmup_instructions(co)
    co_sim = simulation_instructions(co)
    co_tv = trace_version(co)

    # Requirement 1: every standalone is a same-machine single-context run.
    co_nc = num_contexts(co)
    co_nctx = len(core_stats(co))
    if co_nc != 2 or co_nctx != 2:
        errors.append(
            f"co-phase: expected num_contexts=2 with 2 roi.core.cpu* sections, got num_contexts={co_nc!r} sections={co_nctx}"
        )

    for name, sa in standalones:
        if machine_config(sa) != co_machine:
            errors.append(
                f"{name}: effective [config] differs from co-phase (different machine); "
                f"only num_contexts and vmem.randomization may vary"
            )
        if trace_version(sa) != co_tv:
            errors.append(f"{name}: trace_version {trace_version(sa)!r} != co-phase {co_tv!r}")
        if warmup_instructions(sa) != co_warm or simulation_instructions(sa) != co_sim:
            errors.append(
                f"{name}: region mismatch (warmup {warmup_instructions(sa)!r}/{co_warm!r}, sim {simulation_instructions(sa)!r}/{co_sim!r})"
            )
        sa_nc = num_contexts(sa)
        sa_nctx = len(core_stats(sa))
        if sa_nc != 1 or sa_nctx != 1:
            errors.append(
                f"{name}: standalone must be num_contexts=1 with 1 roi.core.cpu* section "
                f"(got num_contexts={sa_nc!r} sections={sa_nctx}); an idle sibling on a 2-context machine is not a valid reference"
            )

    # Requirement 2: per-thread own-completion cycles, not the overshoot-tainted ipc.
    for idx, stats in core_stats(co).items():
        rc = stats["roi_cycles"]
        if rc is None:
            errors.append(
                f"co-phase ctx{idx}: missing roi.core.cpu{idx}.roi_cycles (per-thread own ROI completion); "
                f"cannot guarantee region alignment, refusing to compute from tainted ipc"
            )
        elif stats["cycles"] is not None and rc > stats["cycles"]:
            errors.append(f"co-phase ctx{idx}: roi_cycles {rc} > full-phase cycles {stats['cycles']} (impossible)")

    seeds = {randomization_seed(d) for d in [co, *(s for _, s in standalones)]}
    if len(seeds) > 1:
        warnings.append(f"vmem.randomization differs across documents: {sorted(map(str, seeds))}")

    return errors, warnings


def compute(co: Dict[str, Any], standalones: Sequence[Tuple[str, Dict[str, Any]]]) -> Dict[str, Any]:
    """Compute WS / ANTT / per-thread slowdown. `standalones` maps positionally to ctx 0..N-1."""
    m = simulation_instructions(co)
    co_stats = core_stats(co)
    rows: List[Dict[str, Any]] = []
    for i, (name, sa) in enumerate(standalones):
        cyc_sa = core_stats(sa)[0]["cycles"]
        cyc_co = co_stats[i]["roi_cycles"]
        rows.append(
            {
                "thread": name,
                "ipc_standalone": m / cyc_sa,
                "ipc_co": m / cyc_co,
                "cycles_standalone": cyc_sa,
                "cycles_co": cyc_co,
                "speedup": cyc_sa / cyc_co,
                "slowdown": cyc_co / cyc_sa,
            }
        )
    ws = sum(r["speedup"] for r in rows)
    antt = sum(r["slowdown"] for r in rows) / len(rows)
    return {"m": m, "rows": rows, "ws": ws, "antt": antt, "hsu": 1.0 / antt}


def render(result: Dict[str, Any]) -> str:
    lines = [
        f"{'thread':<10}{'IPC standalone':>16}{'IPC co':>10}{'stand cycles':>14}{'co cycles':>12}{'speedup':>10}{'slowdown':>10}"
    ]
    for r in result["rows"]:
        lines.append(
            f"{r['thread']:<10}{r['ipc_standalone']:>16.4f}{r['ipc_co']:>10.4f}{r['cycles_standalone']:>14}{r['cycles_co']:>12}"
            f"{r['speedup']:>10.4f}{r['slowdown']:>10.4f}"
        )
    lines.append(f"weighted speedup (WS) = {result['ws']:.4f}")
    lines.append(f"ANTT = {result['antt']:.4f}   harmonic speedup (HSU) = {result['hsu']:.4f}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Self-test: synthetic statistics documents (the dict shape tomllib produces)
# that exercise the computation and the two hard-requirement guards without a
# simulation run. `python3 tools/smt/fairness.py --self-test` must print PASS.
# ---------------------------------------------------------------------------
def _doc(build: str, warm: int, sim: int, nctx: int, entries: Sequence[Dict[str, Any]], seed: Any = 1) -> Dict[str, Any]:
    return {
        "meta": {"build_id": build, "warmup_instructions": warm, "simulation_instructions": sim},
        "config": {"ooo_cpu": {"cpu0": {"num_contexts": nctx}}, "vmem": {"randomization": seed}},
        "phase": {
            "simulation": {
                "roi": {"core": {f"cpu{i}": entry for i, entry in enumerate(entries)}}
            }
        },
    }


def _approx(a: float, b: float, tol: float = 1e-6) -> bool:
    return abs(a - b) <= tol


def self_test() -> int:
    build, warm, sim, seed = "0xAAAA", 1_000_000, 3_000_000, 1

    # Standalone single-context runs (own-completion == full-phase cycles).
    sa = _doc(build, warm, sim, 1, [{"instructions": 3_000_002, "cycles": 1_200_000, "roi_cycles": 1_200_000}], seed)
    sb = _doc(build, warm, sim, 1, [{"instructions": 3_000_001, "cycles": 1_500_000, "roi_cycles": 1_500_000}], seed)

    # Co-phase: ctx0 (A) finishes at 1.5M cycles, ctx1 (B, slowest) at 2.0M = full.
    # ctx0 overshoots to 3.6M instructions while B catches up; its tainted ipc
    # would be 3.6M/2.0M, which is NOT the per-thread IPC. roi_cycles is correct.
    co = _doc(
        build,
        warm,
        sim,
        2,
        [
            {"instructions": 3_600_000, "cycles": 2_000_000, "roi_cycles": 1_500_000},
            {"instructions": 3_000_004, "cycles": 2_000_000, "roi_cycles": 2_000_000},
        ],
        seed,
    )

    std = [("A", sa), ("B", sb)]
    errors, _ = validate(co, std)
    assert not errors, errors

    res = compute(co, std)
    # speedup_A = 1.2M / 1.5M = 0.8; speedup_B = 1.5M / 2.0M = 0.75.
    assert _approx(res["rows"][0]["speedup"], 0.8), res
    assert _approx(res["rows"][1]["speedup"], 0.75), res
    assert _approx(res["ws"], 1.55), res
    assert _approx(res["antt"], (1.25 + 1.0 / 0.75) / 2), res
    assert _approx(res["hsu"], 1.0 / res["antt"]), res

    # Guard 1a: a machine-config difference beyond num_contexts/seed is rejected.
    sa_other = _doc(build, warm, sim, 1, [{"instructions": 3_000_002, "cycles": 1_200_000, "roi_cycles": 1_200_000}], seed)
    sa_other["config"]["ooo_cpu"]["cpu0"]["rob_size"] = 256
    errors, _ = validate(co, [("A", sa_other), ("B", sb)])
    assert any("config" in e for e in errors), errors

    # Guard 1b: a 2-context doc fed as a standalone reference is rejected.
    errors, _ = validate(co, [("A", co), ("B", sb)])
    assert any("single-context" in e or "num_contexts" in e for e in errors), errors

    # Guard 2a: a co-phase doc missing roi_cycles is rejected (no fake speedup).
    co_no_rc = _doc(build, warm, sim, 2, [{"instructions": 3_000_000, "cycles": 2_000_000}] * 2, seed)
    errors, _ = validate(co_no_rc, std)
    assert any("roi_cycles" in e for e in errors), errors

    # Guard 2b: roi_cycles > full cycles is impossible and rejected.
    co_bad = _doc(
        build,
        warm,
        sim,
        2,
        [
            {"instructions": 3_000_000, "cycles": 2_000_000, "roi_cycles": 2_100_000},
            {"instructions": 3_000_000, "cycles": 2_000_000, "roi_cycles": 2_000_000},
        ],
        seed,
    )
    errors, _ = validate(co_bad, std)
    assert any("roi_cycles" in e and ">" in e for e in errors), errors

    # Parsing path: a minimal TOML string round-trips through load().
    toml = (
        '[meta]\nbuild_id = "0xAAAA"\ntrace_version = 1\nwarmup_instructions = 1000000\nsimulation_instructions = 3000000\n\n'
        "[config.ooo_cpu.cpu0]\nnum_contexts = 2\n\n"
        "[phase.simulation.roi.core.cpu0]\ninstructions = 3000000\ncycles = 2000000\nroi_cycles = 1500000\n"
    )
    import io

    doc = tomllib.loads(toml)
    assert trace_version(doc) == 1, doc
    assert num_contexts(doc) == 2, doc
    assert core_stats(doc)[0]["roi_cycles"] == 1_500_000, doc

    print("PASS: fairness self-test (computation + two hard-requirement guards + TOML parse)")
    return 0


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description="SMT fairness metrics (WS / ANTT / per-thread slowdown)")
    parser.add_argument("--co", help="co-phase statistics document (2 contexts)")
    parser.add_argument("--standalone", nargs="+", help="standalone single-context documents, ctx0..ctxN-1 in order")
    parser.add_argument("--self-test", action="store_true", help="run the embedded synthetic self-test and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    if not args.co or not args.standalone:
        parser.error("--co and --standalone are required (or pass --self-test)")

    co = load(args.co)
    standalones = [(f"ctx{i}", load(p)) for i, p in enumerate(args.standalone)]

    errors, warnings = validate(co, standalones)
    for w in warnings:
        print(f"WARNING: {w}", file=sys.stderr)
    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        print("refusing to compute: fix the inputs above", file=sys.stderr)
        return 2

    print(render(compute(co, standalones)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
