#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Emit the in-process bench as github-action-benchmark JSON (two suites).

Companion to `perf_gate.py` (the hard per-PR regression gate). Where the gate answers
"did THIS PR regress vs its own same-runner main baseline", this emitter feeds a
persisted, **build-to-build history** on the `gh-pages` branch so slow drift that
stays under the gate's 50% threshold is still visible as a trend — and a tighter
soft-alert (see perf.yml) auto-comments on the offending commit.

Every (mode, size, fanout, endpoints) point the default bench run produces is
tracked — not a hand-picked subset — with **latency, throughput and memory footprint
as separate values per test**, split across two suites because the chart store keys
one direction per suite:

  * `--out-smaller` (customSmallerIsBetter): per-point p50 / p99 latency (ns) and
    ns/delivery (throughput re-expressed in the legacy ns unit so the pre-existing
    series continue unbroken), plus the memory-footprint metrics — heap bytes per
    forward hop / per terminus resolve parsed from bench_forward_heap's probe
    output, and the whole-run max RSS from `/usr/bin/time -v` when provided.
  * `--out-bigger` (customBiggerIsBetter): per-point throughput in natural
    deliveries/s, so throughput trends read in their own direction and unit.

Medians the repeated RESULT rows, exactly as the gate does, so run-to-run jitter
does not move the recorded point. `--raw` consumes a pre-captured bench transcript
(CI runs the bench once, archives the transcript as the per-commit artifact, and
feeds this emitter from it) instead of re-running the binary.

  ./perf_emit_benchmark.py --raw raw.txt --zeroheap-raw zh.txt --time-v time.txt \\
      --out-smaller benchmark_ns.json --out-bigger benchmark_throughput.json

Stdlib only. The RESULT columns (tab-separated, from bench_libtracer):
  RESULT sys mode size fan ep pub_s deliv_s mb_s p50ns p99ns meanns
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import statistics
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent


def run_bench(bench: pathlib.Path) -> str:
    if not bench.exists():
        print(f"perf_emit_benchmark: {bench} not built", file=sys.stderr)
        sys.exit(2)
    return subprocess.run([str(bench)], capture_output=True, text=True, timeout=300).stdout


def parse_rows(out: str) -> list[tuple]:
    """RESULT lines -> (mode, size, fan, ep, deliv_s, p50ns, p99ns) tuples."""
    rows = []
    for line in out.splitlines():
        f = line.split("\t")
        if len(f) == 12 and f[0] == "RESULT":
            rows.append((f[2], int(f[3]), int(f[4]), int(f[5]),
                         float(f[7]), int(f[9]), int(f[10])))
    return rows


def points(rows: list[tuple]) -> list[tuple]:
    """Every distinct (mode, size, fan, ep) the run produced, in first-seen order —
    ALL bench rows feed the tracker, not a hand-picked subset."""
    seen: dict[tuple, None] = {}
    for r in rows:
        seen.setdefault((r[0], r[1], r[2], r[3]))
    return list(seen)


def median_point(rows: list[tuple], key: tuple):
    sel = [r for r in rows if (r[0], r[1], r[2], r[3]) == key]
    if not sel:
        return None
    return {
        "p50_ns": int(statistics.median(r[5] for r in sel)),
        "p99_ns": int(statistics.median(r[6] for r in sel)),
        "deliv_s": statistics.median(r[4] for r in sel),
    }


def zeroheap_metrics(text: str) -> list[dict]:
    """Memory footprint from bench_forward_heap's probe lines: heap bytes (and alloc
    count) armed around one steady-state forward hop / terminus resolve. Space-
    separated `RESULT zeroheap <what> allocs=N frees=N bytes=N ...` lines."""
    series = []
    for m in re.finditer(r"^RESULT(?:\s+zeroheap)?\s+(\w+)\s+allocs=(\d+)\s+frees=\d+\s+bytes=(\d+)",
                         text, re.MULTILINE):
        what, allocs, nbytes = m.group(1), int(m.group(2)), int(m.group(3))
        series.append({"name": f"heap bytes per {what} (probe)", "unit": "bytes",
                       "value": nbytes})
        series.append({"name": f"heap allocs per {what} (probe)", "unit": "allocs",
                       "value": allocs})
    return series


def rss_metric(text: str) -> list[dict]:
    """Whole-run memory footprint from a `/usr/bin/time -v` transcript."""
    m = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", text)
    return ([{"name": "bench_libtracer max RSS", "unit": "KB", "value": int(m.group(1))}]
            if m else [])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", default=str(HERE / "build" / "bench_libtracer"),
                    help="path to the bench_libtracer binary (used when --raw is absent)")
    ap.add_argument("--raw", help="pre-captured bench_libtracer stdout to parse "
                                  "instead of re-running the binary")
    ap.add_argument("--zeroheap-raw", help="pre-captured bench_forward_heap stdout: "
                                           "heap-probe bytes become memory-footprint metrics")
    ap.add_argument("--time-v", help="pre-captured `/usr/bin/time -v` stderr of the bench "
                                     "run: max RSS becomes a memory-footprint metric")
    ap.add_argument("--out-smaller", "--out", dest="out_smaller",
                    default=str(HERE / "benchmark_output.json"),
                    help="customSmallerIsBetter JSON: latency ns + ns/delivery + memory")
    ap.add_argument("--out-bigger", dest="out_bigger", default=None,
                    help="customBiggerIsBetter JSON: throughput in deliveries/s "
                         "(omitted if not given)")
    args = ap.parse_args()

    out = (pathlib.Path(args.raw).read_text() if args.raw
           else run_bench(pathlib.Path(args.bench).resolve()))
    rows = parse_rows(out)
    smaller: list[dict] = []
    bigger: list[dict] = []
    for key in points(rows):
        mode, size, fan, ep = key
        v = median_point(rows, key)
        tag = f"{mode} {size}B/fan{fan}/{ep}ep"
        smaller.append({"name": f"{tag} p50 latency", "unit": "ns", "value": v["p50_ns"]})
        smaller.append({"name": f"{tag} p99 latency", "unit": "ns", "value": v["p99_ns"]})
        if v["deliv_s"] > 0:
            # Throughput twice, deliberately: natural deliveries/s in the bigger-is-
            # better suite, and the legacy ns/delivery inversion (1e9 / deliv_s) so the
            # pre-existing smaller-is-better series continue without a break.
            bigger.append({"name": f"{tag} throughput", "unit": "deliveries/s",
                           "value": round(v["deliv_s"], 1)})
            smaller.append({"name": f"{tag} ns/delivery",
                            "unit": "ns", "value": round(1e9 / v["deliv_s"], 1)})

    if args.zeroheap_raw and pathlib.Path(args.zeroheap_raw).exists():
        smaller += zeroheap_metrics(pathlib.Path(args.zeroheap_raw).read_text())
    if args.time_v and pathlib.Path(args.time_v).exists():
        smaller += rss_metric(pathlib.Path(args.time_v).read_text())

    if not smaller:
        print("perf_emit_benchmark: no RESULT rows parsed from the bench", file=sys.stderr)
        return 1
    pathlib.Path(args.out_smaller).write_text(json.dumps(smaller, indent=2) + "\n")
    print(f"perf_emit_benchmark: wrote {len(smaller)} smaller-is-better metrics -> "
          f"{args.out_smaller}")
    if args.out_bigger:
        pathlib.Path(args.out_bigger).write_text(json.dumps(bigger, indent=2) + "\n")
        print(f"perf_emit_benchmark: wrote {len(bigger)} throughput metrics -> "
              f"{args.out_bigger}")
    for m in smaller + (bigger if args.out_bigger else []):
        print(f"  {m['name']:<44} {m['value']:>14} {m['unit']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
