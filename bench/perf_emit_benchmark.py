#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Emit the in-process bench as github-action-benchmark `customSmallerIsBetter` JSON.

Companion to `perf_gate.py` (the hard per-PR regression gate). Where the gate answers
"did THIS PR regress vs its own same-runner main baseline", this emitter feeds a
persisted, **build-to-build ns history** on the `gh-pages` branch so slow drift that
stays under the gate's 50% threshold is still visible as a trend — and a tighter
soft-alert (see perf.yml) auto-comments on the offending commit.

Every tracked metric is a **nanosecond latency**, smaller-is-better: p50 and p99 for
each canonical point, plus throughput re-expressed as `ns/delivery` (1e9 / deliveries-
per-second) so the whole series shares one unit and one direction (a single
`customSmallerIsBetter` chart set). Medians the repeated RESULT rows, exactly as the
gate does, so run-to-run jitter does not move the recorded point.

  ./perf_emit_benchmark.py --bench build/bench_libtracer --out benchmark_output.json

Stdlib only. The RESULT columns (tab-separated, from bench_libtracer):
  RESULT sys mode size fan ep pub_s deliv_s mb_s p50ns p99ns meanns
"""
from __future__ import annotations

import argparse
import json
import pathlib
import statistics
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent

# The canonical points the gate also tracks: (mode, size, fanout, endpoints).
POINTS = [("inproc", 64, 1, 1), ("inproc-borrow", 64, 1, 1)]


def run_bench(bench: pathlib.Path) -> list[tuple]:
    if not bench.exists():
        print(f"perf_emit_benchmark: {bench} not built", file=sys.stderr)
        sys.exit(2)
    out = subprocess.run([str(bench)], capture_output=True, text=True, timeout=180).stdout
    rows = []
    for line in out.splitlines():
        f = line.split("\t")
        if len(f) == 12 and f[0] == "RESULT":
            # (mode, size, fan, ep, deliv_s, p50ns, p99ns)
            rows.append((f[2], int(f[3]), int(f[4]), int(f[5]),
                         float(f[7]), int(f[9]), int(f[10])))
    return rows


def median_point(rows: list[tuple], mode: str, size: int, fan: int, ep: int):
    sel = [r for r in rows if (r[0], r[1], r[2], r[3]) == (mode, size, fan, ep)]
    if not sel:
        return None
    return {
        "p50_ns": int(statistics.median(r[5] for r in sel)),
        "p99_ns": int(statistics.median(r[6] for r in sel)),
        "deliv_s": statistics.median(r[4] for r in sel),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", default=str(HERE / "build" / "bench_libtracer"),
                    help="path to the bench_libtracer binary")
    ap.add_argument("--out", default=str(HERE / "benchmark_output.json"),
                    help="where to write the github-action-benchmark JSON array")
    args = ap.parse_args()

    rows = run_bench(pathlib.Path(args.bench).resolve())
    series: list[dict] = []
    for (mode, size, fan, ep) in POINTS:
        v = median_point(rows, mode, size, fan, ep)
        if not v:
            continue
        tag = f"{mode} {size}B/fan{fan}/{ep}ep"
        series.append({"name": f"{tag} p50 latency", "unit": "ns", "value": v["p50_ns"]})
        series.append({"name": f"{tag} p99 latency", "unit": "ns", "value": v["p99_ns"]})
        # Throughput re-expressed as ns/delivery so it shares the ns, smaller-is-better
        # axis: a slower path delivers fewer/s -> more ns/delivery -> charts as a rise.
        if v["deliv_s"] > 0:
            series.append({"name": f"{tag} ns/delivery",
                           "unit": "ns", "value": round(1e9 / v["deliv_s"], 1)})

    if not series:
        print("perf_emit_benchmark: no RESULT rows parsed from the bench", file=sys.stderr)
        return 1
    pathlib.Path(args.out).write_text(json.dumps(series, indent=2) + "\n")
    print(f"perf_emit_benchmark: wrote {len(series)} ns metrics -> {args.out}")
    for m in series:
        print(f"  {m['name']:<34} {m['value']:>10} {m['unit']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
