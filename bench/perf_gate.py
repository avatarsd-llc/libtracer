#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Per-loop performance gate.

Runs the libtracer in-process benchmark and validates the canonical latency /
throughput metric so a regression is caught the moment it lands — not at release.
Tracks two points: `inproc` 64B/fan1/1ep (the standard zero-copy dispatch path) and
`inproc-borrow` (the zero-alloc loaned path). Compares against a recorded baseline
(bench/perf_baseline.json, host-specific, gitignored); on the first run it records
the baseline. Tolerant of run-to-run jitter — fails only on a real regression.

  ./perf_gate.py                 # run + validate (records baseline on first run)
  ./perf_gate.py --update-baseline   # accept current numbers as the new baseline
  ./perf_gate.py --bench PATH        # time PATH instead of the default build output

The --bench override drives the same-runner gate (CI): build main's bench_libtracer
and the PR's bench_libtracer on the SAME runner, record main's numbers as the
baseline (`--bench <main_bin> --update-baseline`), then gate the PR's numbers against
it (`./perf_gate.py`). Both are timed on identical hardware in one job, so absolute-ns
runner speed cancels and only a real relative regression fails — no cross-runner
jitter (which previously false-failed packaging/docs/TS-only PRs).

Exit 0 = PERF: PASS, 1 = PERF: FAIL (latency up >50% or throughput down >34% vs base,
or — with no baseline — past the absolute floors). Stdlib only; no zenoh needed.
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
POINTS = [("inproc", 64, 1, 1), ("inproc-borrow", 64, 1, 1)]
LAT_REGRESS = 1.5    # fail if p50 > baseline * 1.5   (a real ~50% latency regression)
TPUT_REGRESS = 0.66  # fail if deliv_s < baseline * 0.66 (a real ~34% throughput drop)
FLOOR_P50_NS = 1000  # absolute backstop if no baseline (canonical is ~100 ns)
FLOOR_DELIV = 1_000_000


def run_bench(bench: pathlib.Path) -> list[tuple]:
    if not bench.exists():
        print(f"perf_gate: {bench} not built — run: cmake -S {HERE} -B {HERE}/build "
              f"-DCMAKE_BUILD_TYPE=Release && cmake --build {HERE}/build -j", file=sys.stderr)
        sys.exit(2)
    out = subprocess.run([str(bench)], capture_output=True, text=True, timeout=180).stdout
    rows = []
    for line in out.splitlines():
        f = line.split("\t")
        if len(f) == 12 and f[0] == "RESULT":
            rows.append((f[2], int(f[3]), int(f[4]), int(f[5]), float(f[7]), int(f[9])))
    return rows


def metric(rows, mode, size, fan, ep):
    p50 = [r[5] for r in rows if (r[0], r[1], r[2], r[3]) == (mode, size, fan, ep)]
    dv = [r[4] for r in rows if (r[0], r[1], r[2], r[3]) == (mode, size, fan, ep)]
    if not p50:
        return None
    return {"p50_ns": int(statistics.median(p50)), "deliv_s": statistics.median(dv)}


def main() -> int:
    args = sys.argv[1:]
    bench = BENCH
    if "--bench" in args:
        bench = pathlib.Path(args[args.index("--bench") + 1]).resolve()
    rows = run_bench(bench)
    cur = {}
    for (m, s, f, e) in POINTS:
        v = metric(rows, m, s, f, e)
        if v:
            cur[f"{m}/{s}/{f}/{e}"] = v
    base = json.loads(BASELINE.read_text()) if BASELINE.exists() else None
    fails = []
    print("Per-loop perf gate (libtracer in-process):")
    for k, v in cur.items():
        line = f"  {k:<20} p50={v['p50_ns']:>5}ns  deliv/s={v['deliv_s']:>14,.0f}"
        if base and k in base:
            b = base[k]
            if v["p50_ns"] > b["p50_ns"] * LAT_REGRESS:
                fails.append(f"{k} latency regressed: {v['p50_ns']}ns vs base {b['p50_ns']}ns")
            if v["deliv_s"] < b["deliv_s"] * TPUT_REGRESS:
                fails.append(f"{k} throughput regressed: {v['deliv_s']:,.0f} vs base {b['deliv_s']:,.0f}")
            line += f"   (base p50={b['p50_ns']}ns deliv/s={b['deliv_s']:,.0f})"
        else:
            if v["p50_ns"] > FLOOR_P50_NS:
                fails.append(f"{k} p50 {v['p50_ns']}ns over floor {FLOOR_P50_NS}ns")
            if v["deliv_s"] < FLOOR_DELIV:
                fails.append(f"{k} deliv {v['deliv_s']:,.0f} under floor {FLOOR_DELIV}")
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
