#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Per-loop performance gate — the strict no-pullback ratchet.

Runs the libtracer in-process benchmark and validates the canonical latency /
throughput points so a regression is caught the moment it lands — not at release.
Compares against a baseline BINARY built on the same runner (paired mode, below), or
— only where no such binary exists — against a recorded bench/perf_baseline.json.

PAIRED mode (`--baseline-bench`, what CI runs) is the primary shape. The two
binaries are executed INTERLEAVED — A B / B A / A B / B A — and compared as a
population, never as two sequential blocks. See `paired_samples` and
`paired_verdict` for the rules; the short version is that a fail needs the
effect to be large (median), separated (the two arms' ranges are disjoint) and
reproducible (a strict majority of the interleaved pairs breach the threshold
on their own). Any sign flip inside the ranges reads as indistinguishable and
can never fail the gate.

Within a run, repeated RESULT rows are medianed (single-iteration jitter), and
comparisons are only ever same-runner, so absolute machine speed cancels and the
thresholds can be tight without false-failing.

LEGACY mode (a recorded `bench/perf_baseline.json`, host-specific, gitignored) is
kept for the no-baseline-binary case (a root commit, or a local `./perf_gate.py`
against yesterday's numbers). It runs the bench --runs times and takes the BEST run
per point. It is NOT what gates a PR any more: best-of-N rejects one bad SAMPLE, and
the failure that actually bit this project three times is a bad WINDOW — a
time-correlated depression of the runner that outlasts every run of whichever arm
happens to hold the machine at the time (#763, #464).

  ./perf_gate.py --baseline-bench PATH   # PAIRED: interleave PATH (base) vs the candidate
  ./perf_gate.py --pairs N               # interleaved A/B pairs (default 4)
  ./perf_gate.py                         # legacy: run + validate (records baseline on first run)
  ./perf_gate.py --update-baseline       # legacy: accept current numbers as the new baseline
  ./perf_gate.py --bench PATH            # time PATH as the CANDIDATE instead of the build output
  ./perf_gate.py --bench-fwd PATH        # probe PATH for the candidate's per-vertex memory
  ./perf_gate.py --baseline-bench-fwd P  # the baseline binary's per-vertex memory (paired)
  ./perf_gate.py --runs N                # legacy: best-of-N bench executions (default 3)

Exit 0 = PERF: PASS, 1 = PERF: FAIL (p50 up >15%, mean up >12%, deliveries/s down
>12%, or per-vertex live bytes up >2%, vs the same-runner baseline, or — with no
baseline — past the absolute floors). The memory points come from bench_forward_heap
(counting allocator, deterministic) and are NOT timed, so they need no interleaving:
they are probed once per binary and ratchet exactly. Supplying that binary for one arm
and not the other is a wiring error and FAILS; supplying it for neither prints an
explicit SKIP and passes — never silence (#792).

Latency percentiles beyond p50/mean (p99, and the p999 the transport benches now emit)
are PUBLISHED but not gated; the measurement behind that decision is recorded beside
the thresholds below. Stdlib only; no zenoh needed.
"""
from __future__ import annotations

import json
import pathlib
import re
import statistics
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
BENCH = HERE / "build" / "bench_libtracer"
BENCH_FWD = HERE / "build" / "bench_forward_heap"
BASELINE = HERE / "perf_baseline.json"

# Canonical points: (mode, size, fanout, endpoints). RESULT cols (collate.py):
# RESULT sys mode size fan ep pub_s deliv_s mb_s p50ns p99ns meanns
# One point per hot-path family, so a regression anywhere on the dispatch
# surface is gated — not just the 1:1 write:
#   inproc / inproc-borrow  — the canonical zero-copy / loaned 1:1 writes
#   fan-out 1024            — the subscriber fan-out loop
#   inproc-path @ 8192 ep   — the resolver canary (registry lookup per write)
#   mixed                   — the composed realistic topology
#   fold-b4                 — the L0 inline-fold codec tier (batch-amortized)
#
# EDITORS: this list is the answer to "how many points does the per-PR gate watch?",
# and `docs/methodology.md` (§What actually stops a regression) states that count and
# names every entry — by its `mode/size/fan/ep` key — in prose. That doc is HAND-WRITTEN
# and is spliced into the published performance page by `gen_results_page.py`, so a
# stale count is not an internal note: it is the PUBLIC description of what the gate
# covers. The same generator's instrument registry states the count a second time, in
# the table at the top of that page. Neither can track this list on its own — the
# sibling MEM_POINTS count rotted exactly that way, silently, until #792. Adding to or
# removing from POINTS means editing docs/methodology.md AND that registry row in the
# same commit; `PointsAreDocumented` in bench/test_perf_gate.py fails until it does
# (#1041).
POINTS = [
    ("inproc", 64, 1, 1),
    ("inproc-borrow", 64, 1, 1),
    ("inproc", 64, 1024, 1),
    ("inproc-path", 64, 1, 8192),
    ("mixed", 0, 6, 128),
    ("fold-b4", 512, 1, 1),
]
# No-baseline absolute-floor backstop, per point: a fan-1024 write's p50 is the
# WHOLE 1024-subscriber fan-out (~13 µs), so the 1 µs 1:1 floor cannot apply.
FLOOR_P50_OVERRIDE = {"inproc/64/1024/1": 100_000}
# The thresholds are the SAME in both modes and unchanged by #763 — what changed is
# the decision taken around them. Same-runner measurement is what makes them tight
# enough to be useful; in paired mode the effect/separation/reproducibility rules are
# what make them safe, and in legacy mode it is still best-of-N. The remaining noise is
# timer quantization (~10 ns ticks on a ~100–300 ns p50): 15% ≥ 1.5 ticks at the
# fastest gated point, so a threshold trip is a real pullback, not clock grain.
LAT_REGRESS = 1.15   # fail if p50 > baseline * 1.15  (latency pullback)
MEAN_REGRESS = 1.12  # fail if mean > baseline * 1.12 — the mean is NOT tick-
                     # quantized (it averages many iterations), so it catches a
                     # one-tick p50 pullback (~10% at 100 ns) the p50 gate cannot
TPUT_REGRESS = 0.88  # fail if deliv_s < baseline * 0.88 (throughput pullback)
LAT_TICK_NS = 25     # sub-100ns baselines: one ~10ns clock tick already exceeds 15%,
                     # so grain alone could fail the gate — such points must ALSO
                     # regress by ~2 ticks in absolute ns before they count.
                     #
                     # NOTE, so nobody over-reads this gate: the guard exists for
                     # CLOCK-QUANTIZED points and it silences the latency legs of any
                     # point measured in single-digit ns, batch-amortized or not.
                     # `fold-b4` sits at ~3 ns, so its p50 and mean legs cannot fire
                     # below ~28 ns and are effectively inert; what actually gates that
                     # point is THROUGHPUT (`TPUT_REGRESS`), which is bulk-timed and has
                     # no tick guard, so it resolves a 12% drop at any magnitude. Its
                     # predecessor `fold-n4` was no better — it published a quantized
                     # p50 of 30 for a real ~11 ns op, so its latency legs needed a
                     # >100% regression to fire. Making the tick guard per-point is the
                     # real fix and is not attempted here.
                     #
                     # What IS done about it, in order of load-bearing-ness:
                     #   1. PAIRED mode's REPRODUCIBILITY rule (`paired_verdict`). A
                     #      throughput-only fail must reproduce across a strict majority
                     #      of interleaved A/B pairs with disjoint ranges. That does not
                     #      care whether the latency legs are inert, which is why it, and
                     #      not the second opinion below, is the fix for #763's defect 1:
                     #      a cross-check that a one-grain p50 step can satisfy is not a
                     #      cross-check.
                     #   2. `TPUT_NEEDS_SECOND_OPINION` below — LEGACY mode only now. The
                     #      inert legs are not re-enabled; the check reads their values
                     #      rather than their verdicts.
FLOOR_P50_NS = 1000  # absolute backstop if no baseline (canonical is ~100 ns)
FLOOR_DELIV = 1_000_000
DEFAULT_RUNS = 3

# --- a throughput pullback on its own needs a second opinion (#464, PR #708) ------
# LEGACY MODE ONLY. Paired mode never consults this: its own reproducibility +
# range-disjointness rules already refuse a one-window throughput excursion, and #763
# established that this check cannot carry the load on a sub-tick point — LAT_TICK_NS
# quantizes `fold-b4`'s p50 to a ~10 ns grain, so a SINGLE grain of downward step in a
# 3 ns operation satisfies "flat-or-better" or breaks it at random, and the corroboration
# it produces carries no information. It is retained below because in legacy mode it is
# strictly better than nothing; it is not the answer to #763.
# Every gated point is measured by TWO instruments over the SAME operation: a per-op
# clock (p50, mean) and a bulk timer (deliveries/s). A change in what the operation
# costs moves both. When only the bulk timer moves and BOTH latency legs came back
# flat-or-better against the same baseline, the two instruments contradict each other,
# and the honest verdict for a contradiction is "inconclusive" — not "fail".
#
# The concrete case. PR #708 touched only L4 (`graph.cpp`, `vertex.hpp`); `fold-b4` is
# the L0 inline-fold codec tier and no call path connects them. The gate failed it:
#
#   fold-b4/512/1/1  p50=8ns  mean=7ns  deliv/s=34,960,881  (base p50=8ns, 52,471,790)
#   ! fold-b4/512/1/1 throughput pullback: -33%
#
# p50 IDENTICAL, mean BETTER, throughput -33%. Nine interleaved same-binary A/B pairs
# put both distributions at ~251 M/s, with the low sample landing on run 3 for BOTH
# binaries — a time-correlated depression of the machine that best-of-N cannot reject
# because it outlasts all N runs. Best-of-N turns such an arm into a CONFIDENT wrong
# answer rather than a noisy one.
#
# Why `fold-b4` in particular had no second opinion: LAT_TICK_NS silences both latency
# GATES below ~28 ns and the point runs at ~3 ns, so throughput was its only live leg.
# This check restores the second opinion without unsilencing anything, because it reads
# the latency legs' VALUES rather than their verdicts. The mean is the load-bearing one:
# averaging clock-quantized samples recovers a sub-tick op cost (a 3 ns op read on a
# ~10 ns grain still averages to ~3 ns), so a genuine 33% slowdown WOULD lift the mean
# even where the mean's own threshold can never fire.
#
# The guard is deliberately narrow, and that narrowness is the safety argument: ANY
# upward move in EITHER latency leg — of any size, threshold or not — leaves the
# throughput failure standing. It fires only on strict contradiction, and it downgrades
# to a loud warning rather than silence, so the disagreement is still on the record.
#
# That the latency VALUES do move when the operation really slows down is measured, not
# assumed. Running the same binary against a quiet baseline while co-tenants were pinned
# to the box, `fold-b4` went p50 3 -> 7 ns and mean 3 -> 7 ns — a 2.3x slowdown, plainly
# visible in both legs — while its throughput fell to 0.49x. The guard did NOT fire
# there, because the latency legs had risen; the failure stood, as it should. And note
# what that same run says about the tick guard: a 2.3x latency regression could not fail
# the LATENCY gate, because 7 - 3 is under LAT_TICK_NS. The values are informative
# exactly where the thresholds are not, which is what this check exploits.
#
# Over 30 base-vs-base comparisons on an idle host the guard fired zero times, so it
# costs no detection there either. Its one demonstrated firing is the #708 shape:
# throughput down, p50 identical, mean flat or better.
TPUT_NEEDS_SECOND_OPINION = True


def tput_contradicted(cur: dict, base: dict) -> bool:
    """@brief Do the latency legs refuse to corroborate a throughput pullback?

    True only when p50 AND mean both came back flat-or-better than the baseline. A
    baseline that predates `mean_ns` cannot corroborate anything, so it returns False
    and the failure stands — the fail-safe direction, since the alternative is a gate
    that gets quieter the older its baseline is.
    """
    if not TPUT_NEEDS_SECOND_OPINION or "mean_ns" not in base:
        return False
    return cur["p50_ns"] <= base["p50_ns"] and cur["mean_ns"] <= base["mean_ns"]


# --- p99 / p999: PUBLISHED, NOT GATED (measured decision, do not re-litigate blind) -
# The RESULT line has carried a p99 since the first bench and `RESULT_TAIL` now adds a
# p999; neither gates, and that is a measurement, not an oversight.
#
# Base-vs-base on one binary, 18 runs on an idle 24-core host, replayed through this
# file's own estimator (per-run median of repeated rows, then best-of-3 across runs),
# worst ratio over all 15 disjoint group pairs:
#
#   leg     worst base-vs-base    threshold that would be false-fail-free
#   p50            1.158x                        +16%
#   mean           1.222x                        +22%
#   deliv          1.355x                        -36%
#   p99            1.667x                        +67%
#
# A p99 gate would have to sit near +67% to stop firing on its own noise, and a +67%
# latency regression already trips the +15% p50 gate several times over. So the p99 leg
# would add no detection the existing legs lack, while adding a new false-fail source
# on every PR.
#
# The two-process net bench's p999 is worse again, and its instability is mostly SAMPLE
# COUNT rather than the transport. Five repeats per configuration, same idle host, only
# the probe count changed:
#
#   probes/point    p50      p99     p999    worst sample
#   4 000          1.09x    5.01x     62x       21x
#   10 000         1.09x    1.40x     17x       13x
#
# The median is indifferent; the tails tighten ~3.6x purely from more samples, which is
# why run_net.sh now pays for 10 000. Even then the p999 stays 17x unstable — a loopback
# deep tail is scheduler wake-up, not either engine's code — so it is charted and never
# gated. The p99, at the published count, IS stable enough to compare engines with, and
# it is charted for that; it still does not gate, because the gate does not run the
# two-process bench at all.
#
# They are published instead — charted per transport on docs/performance.md — because
# an ungated number is still worth having when the alternative is not measuring the
# tail at all. What would change this verdict is a per-point measurement showing some
# specific point's p99 is stable to within a threshold tighter than its own p50 gate.
# Nothing in POINTS is, today.

# --- memory footprint gate (bench_forward_heap counting-allocator probes) --------
# The live usable-size bytes a default leaf vertex holds at rest, plus the increment
# an LKV write / a 5-field app table adds. These are EXACT — the counting allocator
# is deterministic, not timed — so unlike latency they need neither best-of-N nor
# interleaving, and they ratchet tightly: a few bytes per vertex is real on the
# constrained target's ~16 KB budget. Same-runner correctness comes from probing BOTH
# binaries in the same pass — `--baseline-bench-fwd` (paired mode) or `--bench-fwd`
# with `--update-baseline` (legacy); keys are `mem:`-namespaced so they never collide
# with the latency points.
#
# Two independent quantities ride on the same probe rows and BOTH ratchet (#571):
#
#   bytes=  — live usable-size bytes per vertex. Host-allocator-dependent (glibc rounds
#             to 16 with an 8 B header where ESP-IDF TLSF rounds to 4 with 4), so it is
#             comparable only against a baseline measured on the SAME runner, and it
#             carries a tolerance because a lone size-class flip is not a regression.
#   allocs= — the NUMBER of heap blocks a vertex costs. Host-INdependent (it counts
#             operator-new calls, not sizes) and exactly reproducible, so it needs no
#             tolerance at all: one extra block per vertex is a regression, full stop.
#             This is the quantity #546's 7→3 win moved and that nothing was protecting
#             — it was pushed to the history store and charted, but never gated.
#
# `reg_escape` is the odd one out and the reason the alloc ratchet earns its keep: it
# counts the blocks a RUNTIME vertex registration takes that the graph's injected
# memory_resource never sees (ADR-0039's seam, which RFC-0014 turned into a wire-driven
# path — #551). Its target is ZERO, every slice of that work lowers it, and the exact
# ratchet is what stops it drifting back up between slices.
#
# EDITORS: this list is the answer to "how many memory probes does the gate check?",
# and `docs/methodology.md` (§the per-PR hard gate) states that count and names every
# entry in prose. That doc is HAND-WRITTEN, not generated, so it cannot track this list
# on its own — it already went stale once at three points (#792). Adding to or removing
# from MEM_POINTS means editing docs/methodology.md in the same commit.
MEM_POINTS = ["vertex", "vertex_value", "vertex_app5", "vertex_app5_static", "reg_escape"]
MEM_REGRESS = 1.02   # fail if live bytes/vertex > baseline * 1.02 ...
MEM_TICK_B = 1       # ... AND grew by more than one byte (ignore a lone bucket flip)
_MEM_RE = re.compile(r"^RESULT zeroheap (\w+) allocs=(\d+) frees=\d+ bytes=(\d+)")

# --- CHARGED steps: the one thing this ratchet must be able to say yes to --------
# A per-vertex cost a ratified RFC PRICES is not a pullback; it is the step the RFC
# bought, and a ratchet with no way to accept one can only be satisfied by lying to
# it. So a charge is declared HERE, by point, in bytes, naming the clause that
# charges it — and it is spent narrowly:
#
#   * it absorbs AT MOST that many bytes. A step of exactly the charged size passes;
#     charged+1 fails, and the failure names the overshoot rather than the total, so
#     an unpriced byte riding along with a priced one is still caught;
#   * it is PRINTED on every run, charged or not, so a reader of CI output sees the
#     allowance and its clause rather than a gate that quietly moved;
#   * it EXPIRES on its own. The paired baseline is built from `main`, so the moment
#     the step lands there the delta is zero and the charge stops applying to
#     anything. An entry whose step has landed is dead weight, and the review that
#     follows deletes it.
#
# What a charge is NOT: a tolerance. It does not scale, it does not accumulate, and
# it is not a per-point budget to spend later — two charged steps to one point mean
# two reviews, each pricing its own.
#
# EMPTY, and that is the mechanism working rather than the mechanism unused. The one
# entry this table has ever carried — RFC-0024 §6.4's 8 B/vertex index slot, on the four
# points a vertex allocation touches — landed on `main` with the routing car, so the
# paired baseline now contains it, the delta is zero, and the charge would print UNSPENT
# on every run while excusing nothing. Deleted on the expiry rule it was written with: a
# charge outlives its step only as dead weight, and dead weight in a ratchet is the first
# byte of a tolerance. The next priced step declares its own.
MEM_CHARGED: dict[str, tuple[int, str]] = {}

# --- ADR-0060 LKV copy-store gate (same-run pool-vs-heap ratio, NOT vs-baseline) --
# The pooled value_backend vs the default heap on the write-path alloc/free op. A
# same-run ratio cancels absolute machine speed, so it needs no baseline: it proves
# the pool routing is live (a heap fallback would collapse the ratio toward 1x) and
# stays deterministically cheaper. The multiple is host-allocator-dependent (glibc's
# tcache serves a hot same-size malloc/free in ~15 ns → ~2.5x here; ESP-IDF
# multi_heap is hundreds of ns → the ADR's ≳10x, validated on-device), so the floor
# is conservative. Skips cleanly when the rows are absent (an older bench binary),
# keeping the gate backward-compatible with a main baseline that predates the rows.
LKV_MIN_RATIO = 2.0


def lkv_ratio_gate(bench: pathlib.Path) -> list[str]:
    """Fail if the pooled alloc/free is not >= LKV_MIN_RATIO x the default heap. Runs
    the bench's isolated `lkv` sweep (fast); best-of-3 max ops/s per size."""
    best: dict[int, dict[str, float]] = {}
    for _ in range(3):
        out = subprocess.run([str(bench), "lkv"], capture_output=True, text=True,
                             timeout=120).stdout
        for line in out.splitlines():
            f = line.split("\t")
            if len(f) == 12 and f[0] == "RESULT" and f[2] in ("lkv-alloc-heap", "lkv-alloc-pool"):
                s = best.setdefault(int(f[3]), {})
                s[f[2]] = max(s.get(f[2], 0.0), float(f[6]))  # f[6] = deliveries/s (ops/s)
    fails = []
    for size, s in sorted(best.items()):
        h, p = s.get("lkv-alloc-heap"), s.get("lkv-alloc-pool")
        if not h or not p:
            continue
        ratio = p / h
        print(f"  lkv-alloc S={size:<6} pool/heap alloc/free = {ratio:>4.1f}x"
              + ("" if ratio >= LKV_MIN_RATIO else f"  << under {LKV_MIN_RATIO}x floor"))
        if ratio < LKV_MIN_RATIO:
            fails.append(f"lkv-alloc S={size} pool only {ratio:.1f}x heap alloc/free "
                         f"(< {LKV_MIN_RATIO}x — the ADR-0060 value_backend routing may have "
                         f"broken / fallen back to the heap)")
    return fails


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


# --- PAIRED (interleaved) comparison — the fix for #763 --------------------------
# Defect: baseline and candidate were measured as two SEQUENTIAL BLOCKS on one runner
# (record main's three runs, then time the PR's three runs). Any drift in the machine
# between the blocks lands entirely on one side of the comparison, and best-of-N does
# not help: it rejects a bad SAMPLE, not a bad WINDOW. Measured on #708 and again on
# #758 — nine same-binary A/B pairs where the low sample landed on run 3 for BOTH
# binaries, a machine depression of 0.64x median that CI reported as a 0.67x "candidate
# regression" against an untouched baseline arm that itself swung 2.8x across runners.
#
# Fix: run the two binaries INTERLEAVED, alternating which one starts each pair, so a
# drift window is shared by both arms instead of being donated to one. Then decide on
# the resulting population rather than on one order statistic. Three conditions must
# ALL hold to fail, and each answers a different question the old gate could not ask:
#
#   effect         — median(cand) vs median(base) breaches the threshold  (is it big?)
#   separation     — the two arms' [min..max] ranges are DISJOINT         (is it real?)
#   reproducibility— a strict majority of the interleaved pairs breach    (does it recur?)
#
# The separation rule is the "never fail on a sign flip inside the ranges" clause: if the
# candidate's best sample is no worse than the baseline's worst, the two populations are
# not distinguishable by this instrument and the honest verdict is PASS, whatever the
# medians say. It is also what supplies the run-drift readout #763 asked for: each arm's
# own [min..max] spread IS the control leg. No extra bench point is needed — the baseline
# arm is by construction invariant under the change being gated, so its spread across the
# interleaved pairs is exactly the "did this run drift?" number.
#
# Cost. The old shape was 3 baseline + 3 candidate = 6 bench executions per gate. The new
# one is PAIRS x 2 = 8 at the default, i.e. ~1.33x wall-clock — inside the ~2x budget.
PAIRS_DEFAULT = 4


def paired_samples(cand: pathlib.Path, base: pathlib.Path,
                   pairs: int) -> dict[str, dict[str, list[dict]]]:
    """@brief Run candidate and baseline interleaved, alternating which one starts.

    Emits A B / B A / A B / … so neither arm systematically owns the early (cold, or
    boost-ramping) part of the job and neither systematically owns a late drift window.
    Returns {"cand"|"base": {point_key: [per-pair metric dict, …]}} with index `i` of
    both arms drawn from the SAME pair, so the lists can be compared element-wise.
    """
    out: dict[str, dict[str, list[dict]]] = {"cand": {}, "base": {}}
    for i in range(max(1, pairs)):
        order = [("base", base), ("cand", cand)]
        if i % 2:
            order.reverse()
        for arm, binary in order:
            rows = run_bench_once(binary)
            for (m, s, f, e) in POINTS:
                v = metric(rows, m, s, f, e)
                if v:
                    out[arm].setdefault(f"{m}/{s}/{f}/{e}", []).append(v)
    return out


def _tick_ok(cur: int, ref: int) -> bool:
    """@brief The sub-100ns clock-grain guard, unchanged from the legacy latency legs."""
    return ref >= 100 or cur - ref > LAT_TICK_NS


def paired_verdict(cand: list[float], base: list[float], factor: float,
                   lower_is_worse: bool, tick_guard: bool = False) -> dict:
    """@brief Decide one metric of one point from its interleaved A/B pairs.

    `factor` is the existing relative threshold (1.15 for p50, 0.88 for deliv/s);
    `lower_is_worse` selects the throughput sense. Returns the three conditions plus the
    numbers a reader needs to audit the call, and fails only when all three hold. With
    fewer than three pairs "a strict majority" degrades to "every pair", because two
    samples cannot vote.
    """
    n = min(len(cand), len(base))
    cs, bs = cand[:n], base[:n]

    def breach(c: float, b: float) -> bool:
        if lower_is_worse:
            return c < b * factor
        return c > b * factor and (not tick_guard or _tick_ok(int(c), int(b)))

    cm, bm = statistics.median(cs), statistics.median(bs)
    pairs_breached = sum(1 for c, b in zip(cs, bs) if breach(c, b))
    # Disjoint = the candidate's BEST sample is still worse than the baseline's WORST.
    disjoint = (max(cs) < min(bs)) if lower_is_worse else (min(cs) > max(bs))
    majority = pairs_breached * 2 > n if n >= 3 else pairs_breached == n
    return {"n": n, "cand_med": cm, "base_med": bm,
            "cand_range": (min(cs), max(cs)), "base_range": (min(bs), max(bs)),
            "effect": breach(cm, bm), "disjoint": disjoint,
            "pairs_breached": pairs_breached, "majority": majority,
            "fail": breach(cm, bm) and disjoint and majority}


def _spread(rng: tuple[float, float]) -> float:
    """@brief Worst-to-best ratio of an arm's own samples — its drift readout."""
    lo, hi = rng
    return (hi / lo) if lo else float("inf")


def paired_report(v: dict, label: str, unit: str, fmt: str) -> str:
    """@brief One human-auditable line per metric: medians, both ranges, and the verdict
    path (which of effect / separation / reproducibility held)."""
    cr, br = v["cand_range"], v["base_range"]
    ratio = (v["cand_med"] / v["base_med"]) if v["base_med"] else float("nan")
    line = (f"      {label:<9} base {v['base_med']:>{fmt}}{unit} "
            f"[{br[0]:>{fmt}}..{br[1]:>{fmt}}, spread {_spread(br):.2f}x]  "
            f"cand {v['cand_med']:>{fmt}}{unit} "
            f"[{cr[0]:>{fmt}}..{cr[1]:>{fmt}}, spread {_spread(cr):.2f}x]  x{ratio:.2f}")
    if v["effect"]:
        line += (f"  | effect YES, pairs {v['pairs_breached']}/{v['n']}, "
                 f"ranges {'DISJOINT' if v['disjoint'] else 'OVERLAP'}")
        if not v["fail"]:
            line += " -> INDISTINGUISHABLE, not failed"
    return line


def gate_paired(cand: pathlib.Path, base: pathlib.Path, pairs: int) -> list[str]:
    """@brief The interleaved per-PR gate. Prints the full population and returns fails."""
    samples = paired_samples(cand, base, pairs)
    fails: list[str] = []
    print(f"Interleaved A/B perf gate ({pairs} pairs, alternating start; fail needs "
          f"effect + disjoint ranges + a majority of pairs):")
    if pairs < 3:
        # Below three pairs "reproduces across a majority" degrades to "happened twice",
        # which is the property #763 showed a runner can manufacture. Usable for a quick
        # local look; not a verdict, and CI never runs it this way.
        print(f"  WARNING: {pairs} pair(s) — the reproducibility rule cannot discriminate "
              f"below 3. Treat a FAIL here as a prompt to re-run at the default.")
    worst_drift = 0.0
    for (m, s, f, e) in POINTS:
        k = f"{m}/{s}/{f}/{e}"
        cs, bs = samples["cand"].get(k), samples["base"].get(k)
        if not cs or not bs:
            print(f"  {k:<22} (absent from one arm — not gated)")
            continue
        print(f"  {k}")
        legs = [
            ("p50_ns", "p50", "ns", "9,.0f", LAT_REGRESS, False, True),
            ("mean_ns", "mean", "ns", "9,.0f", MEAN_REGRESS, False, True),
            ("deliv_s", "deliv/s", "", "15,.0f", TPUT_REGRESS, True, False),
        ]
        for key, label, unit, fmt, factor, lower_worse, tick in legs:
            c = [float(x[key]) for x in cs]
            b = [float(x[key]) for x in bs]
            if lower_worse and (min(c) <= 0 or min(b) <= 0):
                continue  # latency-only row (#553): deliv_s = 0 means "not measured here"
            v = paired_verdict(c, b, factor, lower_worse, tick)
            worst_drift = max(worst_drift, _spread(v["base_range"]))
            print(paired_report(v, label, unit, fmt))
            if v["fail"]:
                fails.append(
                    f"{k} {label} pullback: {v['cand_med']:,.0f}{unit} vs base "
                    f"{v['base_med']:,.0f}{unit} "
                    f"({(v['cand_med'] / v['base_med'] - 1) * 100:+.0f}%), reproduced in "
                    f"{v['pairs_breached']}/{v['n']} interleaved pairs with disjoint ranges")
    # The baseline arm cannot be moved by the candidate's code, so its worst spread is
    # this run's drift figure. It does not gate — it tells a reader whether the run was
    # worth believing at all, which is what a 2.8x baseline swing needed and never got.
    print(f"  run drift (worst baseline-arm spread across pairs): {worst_drift:.2f}x")
    return fails


def mem_probe(bench_fwd: pathlib.Path) -> dict[str, dict]:
    """Live usable-size bytes AND heap-block count per vertex from the counting-allocator
    probes — one run (deterministic). Returns {"mem:<what>": {"bytes": N, "allocs": M}};
    empty when the binary is absent or emits no probe rows, so memory gating degrades to
    a note, never a crash."""
    if not bench_fwd.exists():
        return {}
    out = subprocess.run([str(bench_fwd)], capture_output=True, text=True, timeout=180).stdout
    got: dict[str, dict] = {}
    for line in out.splitlines():
        m = _MEM_RE.match(line)
        if m and m.group(1) in MEM_POINTS:
            got[f"mem:{m.group(1)}"] = {"bytes": int(m.group(3)), "allocs": int(m.group(2))}
    return got


def mem_gate(cur: dict, base: dict | None) -> list[str]:
    """@brief The exact per-vertex memory ratchet — bytes with a tolerance, heap-block
    count with none. Deterministic (a counting allocator, not a clock), so it is probed
    once per binary and never interleaved."""
    fails: list[str] = []
    for k, v in cur.items():
        if not k.startswith("mem:") or "bytes" not in v:
            continue
        line = f"  {k:<22} live={v['bytes']:>6} B/vertex  blocks={v['allocs']:>2}"
        b = base.get(k) if base else None
        charge, why = MEM_CHARGED.get(k, (0, ""))
        if b and "bytes" in b:
            grew = v["bytes"] - b["bytes"]
            # The charge is subtracted from the GROWTH, never from the measurement: the
            # printed live figure stays the real one, and only the amount a ratified
            # clause paid for is excused.
            over = grew - charge
            if v["bytes"] - charge > b["bytes"] * MEM_REGRESS and over > MEM_TICK_B:
                fails.append(f"{k} memory pullback: {v['bytes']}B vs base {b['bytes']}B "
                             f"(+{(v['bytes'] / b['bytes'] - 1) * 100:.1f}%)"
                             + (f"; {charge}B of that is charged to {why}, "
                                f"the other {over}B is not" if charge else ""))
            if charge:
                # Said out loud on every run, spent or not: an allowance nobody can see in
                # the log is a moved gate.
                state = f"charged {min(max(grew, 0), charge)}/{charge}B" if grew > 0 \
                    else f"charge {charge}B UNSPENT (step already on main — delete the entry)"
                print(f"  {'':<22} {state} — {why}")
            line += f"   (base {b['bytes']}B"
            # Block COUNT ratchets exactly — no tolerance, no tick guard. It is a
            # count of operator-new calls, identical on every host, so any increase
            # is the code allocating more per vertex. Guarded on presence so a
            # baseline recorded before this key existed degrades to bytes-only
            # gating rather than crashing (the checked-in perf_baseline.json
            # fallback deliberately carries no `allocs`: it was captured on a
            # different toolchain, and an exact ratchet is only honest against a
            # same-runner baseline binary, which is what CI supplies).
            if "allocs" in b:
                if v["allocs"] > b["allocs"]:
                    fails.append(f"{k} allocation pullback: {v['allocs']} heap blocks per "
                                 f"vertex vs base {b['allocs']} (+{v['allocs'] - b['allocs']})")
                line += f", {b['allocs']} blocks"
            line += ")"
        print(line)
    return fails


def _mem_arm(label: str, p: pathlib.Path | None, flag: str) -> str:
    """@brief One arm's supply state, worded for a human reading CI output."""
    if p is None:
        return f"{label}: not supplied ({flag})"
    return f"{label}: {'present' if p.exists() else 'MISSING'} at {p}"


def mem_ratchet(bench_fwd: pathlib.Path | None, base_fwd: pathlib.Path | None) -> list[str]:
    """@brief Decide whether the paired per-vertex memory ratchet runs, skips, or fails.

    `mem_probe` returns `{}` for an absent binary and `mem_gate` iterates the candidate
    dict, so an absent CANDIDATE used to print nothing and pass — a skipped gate that is
    indistinguishable from a gate that passed, which is exactly the blind spot this file
    warns about two lines below (#792, same class as #464). The rule now:

      * both arms present  -> probe both and ratchet (unchanged behaviour);
      * NEITHER arm present -> print an explicit SKIP and pass. There is nothing to
        compare and no claim is being hidden, but it is said out loud;
      * exactly ONE arm present -> FAIL. An asymmetric supply is a wiring error: one side
        of a same-runner comparison was built and handed over and the other was not, so
        whatever the present arm measures cannot be ratcheted against anything. Failing
        here is what keeps a dropped `--baseline-bench-fwd` (or an unbuilt
        `bench_forward_heap` target) from reading as a clean gate.
    """
    cand_ok = bench_fwd is not None and bench_fwd.exists()
    base_ok = base_fwd is not None and base_fwd.exists()
    cand = _mem_arm("candidate", bench_fwd, "--bench-fwd")
    base = _mem_arm("baseline", base_fwd, "--baseline-bench-fwd")
    if cand_ok and base_ok:
        return mem_gate(mem_probe(bench_fwd), mem_probe(base_fwd))
    if not cand_ok and not base_ok:
        # Say so. A skipped gate that prints nothing is indistinguishable from a
        # gate that passed, which is how a guard becomes a blind spot.
        print(f"  SKIP: the per-vertex memory ratchet is NOT run — neither arm supplied a "
              f"bench_forward_heap ({cand}; {base}). Build the `bench_forward_heap` target "
              f"on both arms and pass --baseline-bench-fwd to gate memory.")
        return []
    missing, present = (cand, base) if base_ok else (base, cand)
    return [f"memory ratchet wiring error: bench_forward_heap was supplied for one arm but "
            f"not the other, so the per-vertex memory points cannot be ratcheted — "
            f"{present}, but {missing}. This is not a skip: an asymmetric supply hides the "
            f"gate rather than running it (#792)."]


def mem_ratchet_legacy(cur: dict, base: dict | None, bench_fwd: pathlib.Path | None) -> list[str]:
    """@brief The same decision for the legacy (recorded-baseline) arm, which had the
    identical hole: `cur` simply carries no `mem:` keys when the candidate binary is
    absent, so `mem_gate` printed nothing and passed.

    Here the "baseline side was supplied" question is answered by the recorded JSON: if
    it carries `mem:` points and this run produced none, the ratchet that guarded those
    points has silently disappeared — a wiring error, not a skip. With neither side
    carrying them there is nothing to compare, which is announced and passes. Note the
    asymmetry also poisons `--update-baseline`, which would drop the recorded `mem:`
    points on the floor.
    """
    if any(k.startswith("mem:") for k in cur):
        return mem_gate(cur, base)
    where = _mem_arm("candidate", bench_fwd, "--bench-fwd")
    if base and any(k.startswith("mem:") for k in base):
        return [f"memory ratchet wiring error: the recorded baseline carries memory points "
                f"but this run produced none — {where}. This is not a skip: it would drop "
                f"the per-vertex memory gate (and, under --update-baseline, the recorded "
                f"points themselves) without saying so (#792)."]
    print(f"  SKIP: the per-vertex memory ratchet is NOT run — no memory probe rows were "
          f"produced ({where}) and the recorded baseline carries none. Build the "
          f"`bench_forward_heap` target to gate memory.")
    return []


def _opt(args: list[str], name: str, default: pathlib.Path | None) -> pathlib.Path | None:
    """@brief Resolve an optional path-valued flag."""
    return pathlib.Path(args[args.index(name) + 1]).resolve() if name in args else default


def main() -> int:
    args = sys.argv[1:]
    bench = _opt(args, "--bench", BENCH)
    bench_fwd = _opt(args, "--bench-fwd", BENCH_FWD)
    base_bench = _opt(args, "--baseline-bench", None)
    base_fwd = _opt(args, "--baseline-bench-fwd", None)

    # PAIRED mode — the per-PR gate. Both binaries in hand, so nothing is recorded to
    # or read from perf_baseline.json: the comparison is made against samples drawn in
    # the same interleaved rotation, which is the whole point of #763's fix.
    if base_bench is not None:
        pairs = int(args[args.index("--pairs") + 1]) if "--pairs" in args else PAIRS_DEFAULT
        print(f"Per-loop perf gate (libtracer in-process, INTERLEAVED baseline/candidate, "
              f"fail: p50 +{(LAT_REGRESS - 1) * 100:.0f}% / "
              f"mean +{(MEAN_REGRESS - 1) * 100:.0f}% / "
              f"deliv -{(1 - TPUT_REGRESS) * 100:.0f}%):")
        fails = gate_paired(bench, base_bench, pairs)
        fails += mem_ratchet(bench_fwd, base_fwd)
        fails += lkv_ratio_gate(bench)  # ADR-0060 same-run ratio (no baseline needed)
        print("PERF: PASS" if not fails else "PERF: FAIL")
        for x in fails:
            print("  ! " + x)
        return 0 if not fails else 1

    runs = int(args[args.index("--runs") + 1]) if "--runs" in args else DEFAULT_RUNS
    cur = best_of(bench, runs)
    cur.update(mem_probe(bench_fwd))  # fold the mem:* points into the same baseline dict
    base = json.loads(BASELINE.read_text()) if BASELINE.exists() else None
    fails = []
    warns: list[str] = []  # measured, reported, does not fail the gate
    mem_hdr = (f" / mem +{(MEM_REGRESS - 1) * 100:.0f}% / blocks +0"
               if any(k.startswith("mem:") for k in cur) else "")
    print(f"Per-loop perf gate (libtracer in-process, LEGACY best of {runs} run(s), "
          f"fail: p50 +{(LAT_REGRESS - 1) * 100:.0f}% / mean +{(MEAN_REGRESS - 1) * 100:.0f}% / "
          f"deliv -{(1 - TPUT_REGRESS) * 100:.0f}%{mem_hdr}):")
    fails += mem_ratchet_legacy(cur, base, bench_fwd)
    for k, v in cur.items():
        if k.startswith("mem:"):
            continue  # handled by mem_gate above
        line = (f"  {k:<22} p50={v['p50_ns']:>7}ns mean={v['mean_ns']:>7}ns "
                f"deliv/s={v['deliv_s']:>14,.0f}")
        if base and k in base:
            b = base[k]

            def lat_fails(cur: int, ref: int, factor: float) -> bool:
                """Relative pullback, tick-guarded for sub-100ns points."""
                return cur > ref * factor and (ref >= 100 or cur - ref > LAT_TICK_NS)

            if lat_fails(v["p50_ns"], b["p50_ns"], LAT_REGRESS):
                fails.append(f"{k} latency pullback: {v['p50_ns']}ns vs base {b['p50_ns']}ns "
                             f"(+{(v['p50_ns'] / b['p50_ns'] - 1) * 100:.0f}%)")
            if "mean_ns" in b and lat_fails(v["mean_ns"], b["mean_ns"], MEAN_REGRESS):
                fails.append(f"{k} mean-latency pullback: {v['mean_ns']}ns vs base "
                             f"{b['mean_ns']}ns (+{(v['mean_ns'] / b['mean_ns'] - 1) * 100:.0f}%)")
            # A latency-only row (the `-batch` arms, #553) publishes deliv_s = 0 meaning
            # "this row does not measure throughput" — its bulk-timed twin does. Gating a
            # zero against a zero is vacuous; gating a zero against a real baseline would
            # fail every run for a metric the row never claimed to produce.
            if b["deliv_s"] > 0 and v["deliv_s"] > 0 and v["deliv_s"] < b["deliv_s"] * TPUT_REGRESS:
                msg = (f"{k} throughput pullback: {v['deliv_s']:,.0f} vs base "
                       f"{b['deliv_s']:,.0f} ({(v['deliv_s'] / b['deliv_s'] - 1) * 100:.0f}%)")
                if tput_contradicted(v, b):
                    warns.append(
                        f"{msg} — NOT FAILED: the same point's latency legs contradict it "
                        f"(p50 {v['p50_ns']} vs {b['p50_ns']} ns, mean {v['mean_ns']} vs "
                        f"{b['mean_ns']} ns — both flat or better). Two instruments over one "
                        f"operation disagree, which is a machine-state signature, not a code "
                        f"one. If this repeats on a quiet runner, it is real: re-run.")
                else:
                    fails.append(msg)
            line += f"   (base p50={b['p50_ns']}ns deliv/s={b['deliv_s']:,.0f})"
        else:
            floor = FLOOR_P50_OVERRIDE.get(k, FLOOR_P50_NS)
            if v["p50_ns"] > floor:
                fails.append(f"{k} p50 {v['p50_ns']}ns over floor {floor}ns")
            if v["deliv_s"] > 0 and v["deliv_s"] < FLOOR_DELIV:
                fails.append(f"{k} deliv {v['deliv_s']:,.0f} under floor {FLOOR_DELIV:,}")
        print(line)
    fails += lkv_ratio_gate(bench)  # ADR-0060 same-run ratio (no baseline; skips if absent)
    if base is None or "--update-baseline" in args:
        BASELINE.write_text(json.dumps(cur, indent=2) + "\n")
        print(f"  ({'recorded' if base is None else 'updated'} baseline -> {BASELINE.name})")
    print("PERF: PASS" if not fails else "PERF: FAIL")
    for x in fails:
        print("  ! " + x)
    # Warnings print AFTER the verdict and under their own marker, never merged into
    # the fail list. A downgraded failure that printed nothing would be indistinguishable
    # from a point that never regressed, which is how a guard turns into a blind spot.
    for x in warns:
        print("  ~ " + x)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
