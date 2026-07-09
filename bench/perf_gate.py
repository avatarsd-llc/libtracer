#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Per-loop performance gate — the strict no-pullback ratchet.

Runs the libtracer in-process benchmark and validates the canonical latency /
throughput points so a regression is caught the moment it lands — not at release.
Compares against a recorded baseline (bench/perf_baseline.json, host-specific,
gitignored); on the first run it records the baseline.

Noise handling (what makes STRICT thresholds viable):
  * the bench is run --runs times (default 3) and each point takes the BEST run
    (min p50, max deliveries/s) — co-tenancy spikes on a shared runner hit one
    run, not all, so best-of-N converges on the machine's real capability;
  * within a run, repeated RESULT rows are medianed (single-iteration jitter);
  * comparisons are only ever same-runner (see below), so absolute machine
    speed cancels and the thresholds can be tight without false-failing.

The --bench override drives the same-runner comparisons (CI):
  * PR gate: build main's bench and the PR's bench on the SAME runner, record
    main's numbers (`--bench <main_bin> --update-baseline`), gate the PR's.
  * main-push ratchet: build HEAD^'s bench and HEAD's bench on the SAME runner
    and gate HEAD against its parent — a merge that pulls latency up or
    throughput down past the thresholds turns main red immediately.

  ./perf_gate.py                     # run + validate (records baseline on first run)
  ./perf_gate.py --update-baseline   # accept current numbers as the new baseline
  ./perf_gate.py --bench PATH        # time PATH instead of the default build output
  ./perf_gate.py --runs N            # best-of-N bench executions (default 3)

Exit 0 = PERF: PASS, 1 = PERF: FAIL (p50 up >15%, mean up >12%, or deliveries/s
down >12% vs the
same-runner baseline, or — with no baseline — past the absolute floors).
Stdlib only; no zenoh needed.
"""
from __future__ import annotations

import json
import pathlib
import statistics
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
BENCH = HERE / "build" / "bench_libtracer"
BASELINE = HERE / "perf_baseline.json"

# Canonical points: (mode, size, fanout, endpoints). RESULT cols (collate.py):
# RESULT sys mode size fan ep pub_s deliv_s mb_s p50ns p99ns meanns
# One point per hot-path family, so a regression anywhere on the dispatch
# surface is gated — not just the 1:1 write:
#   inproc / inproc-borrow  — the canonical zero-copy / loaned 1:1 writes
#   fan-out 1024            — the subscriber fan-out loop
#   inproc-path @ 8192 ep   — the resolver canary (registry lookup per write)
#   mixed                   — the composed realistic topology
#   fold-n4                 — the L0 inline-fold codec tier
POINTS = [
    ("inproc", 64, 1, 1),
    ("inproc-borrow", 64, 1, 1),
    ("inproc", 64, 1024, 1),
    ("inproc-path", 64, 1, 8192),
    ("mixed", 0, 6, 128),
    ("fold-n4", 512, 1, 1),
]
# No-baseline absolute-floor backstop, per point: a fan-1024 write's p50 is the
# WHOLE 1024-subscriber fan-out (~13 µs), so the 1 µs 1:1 floor cannot apply.
FLOOR_P50_OVERRIDE = {"inproc/64/1024/1": 100_000}
# Same-runner + best-of-N makes tight thresholds safe. The remaining noise is
# timer quantization (~10 ns ticks on a ~100–300 ns p50): 15% ≥ 1.5 ticks at the
# fastest gated point, so a threshold trip is a real pullback, not clock grain.
LAT_REGRESS = 1.15   # fail if p50 > baseline * 1.15  (latency pullback)
MEAN_REGRESS = 1.12  # fail if mean > baseline * 1.12 — the mean is NOT tick-
                     # quantized (it averages many iterations), so it catches a
                     # one-tick p50 pullback (~10% at 100 ns) the p50 gate cannot
TPUT_REGRESS = 0.88  # fail if deliv_s < baseline * 0.88 (throughput pullback)
FLOOR_P50_NS = 1000  # absolute backstop if no baseline (canonical is ~100 ns)
FLOOR_DELIV = 1_000_000
DEFAULT_RUNS = 3


def run_bench_once(bench: pathlib.Path) -> list[tuple]:
    if not bench.exists():
        print(f"perf_gate: {bench} not built — run: cmake -S {HERE} -B {HERE}/build "
              f"-DCMAKE_BUILD_TYPE=Release && cmake --build {HERE}/build -j", file=sys.stderr)
        sys.exit(2)
    out = subprocess.run([str(bench)], capture_output=True, text=True, timeout=180).stdout
    rows = []
    for line in out.splitlines():
        f = line.split("\t")
        if len(f) == 12 and f[0] == "RESULT":
            rows.append((f[2], int(f[3]), int(f[4]), int(f[5]), float(f[7]), int(f[9]),
                         int(f[11])))
    return rows


def metric(rows, mode, size, fan, ep):
    """Median across one run's repeated RESULT rows for the point."""
    p50 = [r[5] for r in rows if (r[0], r[1], r[2], r[3]) == (mode, size, fan, ep)]
    dv = [r[4] for r in rows if (r[0], r[1], r[2], r[3]) == (mode, size, fan, ep)]
    mean = [r[6] for r in rows if (r[0], r[1], r[2], r[3]) == (mode, size, fan, ep)]
    if not p50:
        return None
    return {"p50_ns": int(statistics.median(p50)), "deliv_s": statistics.median(dv),
            "mean_ns": int(statistics.median(mean))}


def best_of(bench: pathlib.Path, runs: int) -> dict[str, dict]:
    """Best run per point: min p50, max deliveries/s across `runs` executions."""
    cur: dict[str, dict] = {}
    for _ in range(max(1, runs)):
        rows = run_bench_once(bench)
        for (m, s, f, e) in POINTS:
            v = metric(rows, m, s, f, e)
            if not v:
                continue
            k = f"{m}/{s}/{f}/{e}"
            if k not in cur:
                cur[k] = v
            else:
                cur[k]["p50_ns"] = min(cur[k]["p50_ns"], v["p50_ns"])
                cur[k]["deliv_s"] = max(cur[k]["deliv_s"], v["deliv_s"])
                cur[k]["mean_ns"] = min(cur[k]["mean_ns"], v["mean_ns"])
    return cur


def main() -> int:
    args = sys.argv[1:]
    bench = BENCH
    if "--bench" in args:
        bench = pathlib.Path(args[args.index("--bench") + 1]).resolve()
    runs = int(args[args.index("--runs") + 1]) if "--runs" in args else DEFAULT_RUNS
    cur = best_of(bench, runs)
    base = json.loads(BASELINE.read_text()) if BASELINE.exists() else None
    fails = []
    print(f"Per-loop perf gate (libtracer in-process, best of {runs} run(s), "
          f"fail: p50 +{(LAT_REGRESS - 1) * 100:.0f}% / mean +{(MEAN_REGRESS - 1) * 100:.0f}% / "
          f"deliv -{(1 - TPUT_REGRESS) * 100:.0f}%):")
    for k, v in cur.items():
        line = (f"  {k:<22} p50={v['p50_ns']:>7}ns mean={v['mean_ns']:>7}ns "
                f"deliv/s={v['deliv_s']:>14,.0f}")
        if base and k in base:
            b = base[k]
            if v["p50_ns"] > b["p50_ns"] * LAT_REGRESS:
                fails.append(f"{k} latency pullback: {v['p50_ns']}ns vs base {b['p50_ns']}ns "
                             f"(+{(v['p50_ns'] / b['p50_ns'] - 1) * 100:.0f}%)")
            if "mean_ns" in b and v["mean_ns"] > b["mean_ns"] * MEAN_REGRESS:
                fails.append(f"{k} mean-latency pullback: {v['mean_ns']}ns vs base "
                             f"{b['mean_ns']}ns (+{(v['mean_ns'] / b['mean_ns'] - 1) * 100:.0f}%)")
            if v["deliv_s"] < b["deliv_s"] * TPUT_REGRESS:
                fails.append(f"{k} throughput pullback: {v['deliv_s']:,.0f} vs base "
                             f"{b['deliv_s']:,.0f} ({(v['deliv_s'] / b['deliv_s'] - 1) * 100:.0f}%)")
            line += f"   (base p50={b['p50_ns']}ns deliv/s={b['deliv_s']:,.0f})"
        else:
            floor = FLOOR_P50_OVERRIDE.get(k, FLOOR_P50_NS)
            if v["p50_ns"] > floor:
                fails.append(f"{k} p50 {v['p50_ns']}ns over floor {floor}ns")
            if v["deliv_s"] < FLOOR_DELIV:
                fails.append(f"{k} deliv {v['deliv_s']:,.0f} under floor {FLOOR_DELIV:,}")
        print(line)
    if base is None or "--update-baseline" in args:
        BASELINE.write_text(json.dumps(cur, indent=2) + "\n")
        print(f"  ({'recorded' if base is None else 'updated'} baseline -> {BASELINE.name})")
    print("PERF: PASS" if not fails else "PERF: FAIL")
    for x in fails:
        print("  ! " + x)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
