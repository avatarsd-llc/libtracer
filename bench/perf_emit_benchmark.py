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
    output (plus its #1808 exact rows: RAM per edge / link / 1 KiB value, blocks per write,
    STREAM stripe sections), and each bench_libtracer family's RSS delta (`RSS family=`
    lines in the transcript, #1808), which replaced the whole-run max RSS.
  * `--out-bigger` (customBiggerIsBetter): per-point throughput in natural
    deliveries/s, so throughput trends read in their own direction and unit.

Medians the repeated RESULT rows, exactly as the gate does, so run-to-run jitter
does not move the recorded point. `--raw` consumes a pre-captured bench transcript
instead of re-running the binary, and **may repeat**: CI runs the bench on THREE
independently-drawn runners and feeds all transcripts here; per point the emitter
records the BEST RUNNER's whole tuple (`best_tuple`: the runner with the lowest p50, or the
highest throughput on a bulk-only row), so the recorded history point approximates the
code's capability, not the machine lottery (GitHub-hosted runners vary ~2x in absolute
speed). It never mixes runners inside one point (#1807): a min of p99s across runners
recorded a tail no single machine had, and a p50 from one runner beside a throughput from
another described no run at all.

  ./perf_emit_benchmark.py --raw r1.txt --raw r2.txt --raw r3.txt \\
      --zeroheap-raw zh.txt \\
      --out-smaller benchmark_ns.json --out-bigger benchmark_throughput.json

Stdlib only. The RESULT columns (tab-separated, from bench_libtracer):
  RESULT sys mode size fan ep pub_s deliv_s mb_s p50ns p99ns meanns
The latency columns are ns and may carry three decimals (a batch row's picoseconds, #1804).
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
    """RESULT lines -> (system, mode, size, fan, ep, deliv_s, p50ns, p99ns) tuples.

    `system` (RESULT column 1) is part of the key, and that is not cosmetic. It used to
    be DROPPED, so a point was identified by mode alone — fine while only libtracer
    transcripts were ever fed in, and silently destructive the moment one was not:
    Zenoh's `inproc` rows and libtracer's `inproc` rows would have been medianed
    together into a single series blending two engines, under a name that claims to be
    one of them. Keying by system is what makes recording Zenoh history possible at all.
    """
    rows = []
    for line in out.splitlines():
        f = line.split("\t")
        if len(f) == 12 and f[0] == "RESULT":
            rows.append((f[1], f[2], int(f[3]), int(f[4]), int(f[5]),
                         float(f[7]), float(f[9]), float(f[10])))
    return rows


def points(rows: list[tuple]) -> list[tuple]:
    """Every distinct (system, mode, size, fan, ep) the run produced, in first-seen
    order — ALL bench rows feed the tracker, not a hand-picked subset."""
    seen: dict[tuple, None] = {}
    for r in rows:
        seen.setdefault(r[:5])
    return list(seen)


def series_tag(system: str, mode: str, size: int, fan: int, ep: int) -> str:
    """@brief The recorded series name for one point.

    libtracer keeps its historical bare-`mode` naming, byte-identical, because these
    names key a persisted gh-pages store: renaming them would orphan every point
    recorded so far and restart the history at zero. Any OTHER system is prefixed, so
    its series can never collide with libtracer's.
    """
    head = f"{mode}" if system == "libtracer" else f"{system} {mode}"
    return f"{head} {size}B/fan{fan}/{ep}ep"


def median_point(rows: list[tuple], key: tuple):
    sel = [r for r in rows if r[:5] == key]
    if not sel:
        return None
    return {
        # Kept to the picosecond, never truncated to whole ns (#1804): a batch row's latency
        # is a fraction of a nanosecond-grained quantity, and int() would re-quantize it.
        "p50_ns": round(statistics.median(r[6] for r in sel), 3),
        "p99_ns": round(statistics.median(r[7] for r in sel), 3),
        "deliv_s": statistics.median(r[5] for r in sel),
    }


def best_tuple(cands: list[dict]) -> dict:
    """@brief The one runner's tuple a point records (#1807): lowest p50, or highest
    throughput when the row has no latency (a bulk-only row, #1804). Ties keep the first.
    Never a per-metric min/max across runners, and never a min of p99s."""
    if any(m["p50_ns"] > 0 for m in cands):
        return min((m for m in cands if m["p50_ns"] > 0), key=lambda m: m["p50_ns"])
    return max(cands, key=lambda m: m["deliv_s"])


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


def exact_metrics(text: str) -> list[dict]:
    """@brief bench_forward_heap's #1808 exact rows as series, each `_x1000` field divided
    back to its unit so a fraction (1.035 blocks per edge) is charted as one."""
    series = []
    for m in re.finditer(r"^RESULT ramprobe (\w+) blocks_x1000=(\d+) bytes_x1000=(\d+)",
                         text, re.MULTILINE):
        series.append({"name": f"live bytes per {m.group(1)} (ramprobe)", "unit": "bytes",
                       "value": int(m.group(3)) / 1000})
        series.append({"name": f"heap blocks per {m.group(1)} (ramprobe)", "unit": "blocks",
                       "value": int(m.group(2)) / 1000})
    for m in re.finditer(r"^RESULT writeblocks (\w+) S=(\d+) seam_x1000=(\d+) "
                         r"seam_bytes_x1000=(\d+) heap_x1000=(\d+)", text, re.MULTILINE):
        tag = f"{m.group(1)} write {m.group(2)}B"
        series.append({"name": f"seam blocks per {tag}", "unit": "blocks",
                       "value": int(m.group(3)) / 1000})
        series.append({"name": f"heap blocks per {tag}", "unit": "blocks",
                       "value": int(m.group(5)) / 1000})
    for m in re.finditer(r"^RESULT streamlock (\w+) sections_x1000=(\d+)", text, re.MULTILINE):
        series.append({"name": f"stripe sections per STREAM write ({m.group(1)})",
                       "unit": "sections", "value": int(m.group(2)) / 1000})
    return series


_RSS_RE = re.compile(r"^RSS family=(\S+) start_kb=\d+ peak_kb=\d+ delta_kb=(\d+)", re.MULTILINE)


def rss_metrics(texts: list[str]) -> list[dict]:
    """@brief Each bench_libtracer family's RSS delta (#1808): what the family's own rows
    added to its fresh process, not the harness peak the old whole-run max RSS reported.
    Smallest across runners, like every other smaller-is-better metric here."""
    best: dict[str, int] = {}
    for text in texts:
        for m in _RSS_RE.finditer(text):
            best[m.group(1)] = min(best.get(m.group(1), int(m.group(2))), int(m.group(2)))
    return [{"name": f"{fam} RSS delta", "unit": "KB", "value": kb} for fam, kb in best.items()]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", default=str(HERE / "build" / "bench_libtracer"),
                    help="path to the bench_libtracer binary (used when --raw is absent)")
    ap.add_argument("--raw", action="append",
                    help="pre-captured bench_libtracer stdout to parse instead of "
                         "re-running the binary; repeatable — one per runner, the "
                         "emitter records the best runner's whole tuple per point")
    ap.add_argument("--zeroheap-raw", help="pre-captured bench_forward_heap stdout: "
                                           "heap-probe bytes become memory-footprint metrics")
    ap.add_argument("--out-smaller", "--out", dest="out_smaller",
                    default=str(HERE / "benchmark_output.json"),
                    help="customSmallerIsBetter JSON: latency ns + ns/delivery + memory")
    ap.add_argument("--out-bigger", dest="out_bigger", default=None,
                    help="customBiggerIsBetter JSON: throughput in deliveries/s "
                         "(omitted if not given)")
    args = ap.parse_args()

    texts = [pathlib.Path(r).read_text() for r in (args.raw or [])
             if pathlib.Path(r).exists()]
    if not texts:
        texts = [run_bench(pathlib.Path(args.bench).resolve())]
    per_runner = [parse_rows(t) for t in texts]
    all_rows = [r for rows in per_runner for r in rows]
    smaller: list[dict] = []
    bigger: list[dict] = []
    for key in points(all_rows):
        system, mode, size, fan, ep = key
        # Best runner: each runner contributes its own median for the point, and the
        # best one's WHOLE tuple is recorded — p50, p99 and throughput from one machine.
        v = best_tuple([m for m in (median_point(rows, key) for rows in per_runner) if m])
        tag = series_tag(system, mode, size, fan, ep)
        # A zero p50 means a bulk-only row (`lkv-*`, #1804): its one metric is throughput,
        # recorded below, and a constant-zero latency series would not be a measurement.
        if v["p50_ns"] > 0:
            smaller.append({"name": f"{tag} p50 latency", "unit": "ns", "value": v["p50_ns"]})
        # A zero p99 means the row is batch-amortized and has no distribution to take a
        # percentile of, not that its tail latency is zero nanoseconds. Recording it
        # anyway gave the `lkv-*` rows eight constant-zero series, one point per commit,
        # that looked like measurements and were not. Same shape as the throughput guard
        # below: a metric a row does not produce is not a series.
        if v["p99_ns"] > 0:
            smaller.append({"name": f"{tag} p99 latency", "unit": "ns", "value": v["p99_ns"]})
        if v["deliv_s"] > 0:
            # Throughput twice, deliberately: natural deliveries/s in the bigger-is-
            # better suite, and the legacy ns/delivery inversion (1e9 / deliv_s) so the
            # pre-existing smaller-is-better series continue without a break.
            bigger.append({"name": f"{tag} throughput", "unit": "deliveries/s",
                           "value": round(v["deliv_s"], 1)})
            smaller.append({"name": f"{tag} ns/delivery",
                            "unit": "ns", "value": round(1e9 / v["deliv_s"], 3)})

    if args.zeroheap_raw and pathlib.Path(args.zeroheap_raw).exists():
        zh = pathlib.Path(args.zeroheap_raw).read_text()
        smaller += zeroheap_metrics(zh) + exact_metrics(zh)
    smaller += rss_metrics(texts)

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
