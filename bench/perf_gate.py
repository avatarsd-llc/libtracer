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
  ./perf_gate.py --bench-fwd PATH    # gate PATH's per-vertex memory bytes (baseline binary)
  ./perf_gate.py --runs N            # best-of-N bench executions (default 3)

Exit 0 = PERF: PASS, 1 = PERF: FAIL (p50 up >15%, mean up >12%, deliveries/s down
>12%, or per-vertex live bytes up >2%, vs the same-runner baseline, or — with no
baseline — past the absolute floors). The memory points come from bench_forward_heap
(counting allocator, deterministic); pass --bench-fwd to record the baseline binary's
bytes same-runner, mirroring --bench for latency.

One failure is downgraded to a `~` warning rather than a fail: a THROUGHPUT-only
pullback whose own p50 and mean both came back flat-or-better. Two instruments over one
operation cannot honestly disagree, so that combination is a machine-state signature,
not a code one — see `TPUT_NEEDS_SECOND_OPINION`. Latency percentiles beyond p50/mean
(p99, and the p999 the transport benches now emit) are PUBLISHED but not gated; the
measurement behind that decision is recorded beside the thresholds below.
Stdlib only; no zenoh needed.
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
# Same-runner + best-of-N makes tight thresholds safe. The remaining noise is
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
                     # What IS done about it: `TPUT_NEEDS_SECOND_OPINION` below. The
                     # inert legs are not re-enabled — a silenced GATE is still a live
                     # MEASUREMENT, and the check reads their values rather than their
                     # verdicts.
FLOOR_P50_NS = 1000  # absolute backstop if no baseline (canonical is ~100 ns)
FLOOR_DELIV = 1_000_000
DEFAULT_RUNS = 3

# --- a throughput pullback on its own needs a second opinion (#464, PR #708) ------
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
# is deterministic, not timed — so unlike latency they need no best-of-N and ratchet
# tightly: a few bytes per vertex is real on the constrained target's ~16 KB budget.
# Same-runner correctness comes from --bench-fwd (record the baseline binary's bytes),
# mirroring --bench for the latency binary; keys are `mem:`-namespaced so they never
# collide with the latency points.
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
MEM_POINTS = ["vertex", "vertex_value", "vertex_app5", "vertex_app5_static", "reg_escape"]
MEM_REGRESS = 1.02   # fail if live bytes/vertex > baseline * 1.02 ...
MEM_TICK_B = 1       # ... AND grew by more than one byte (ignore a lone bucket flip)
_MEM_RE = re.compile(r"^RESULT zeroheap (\w+) allocs=(\d+) frees=\d+ bytes=(\d+)")

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


def main() -> int:
    args = sys.argv[1:]
    bench = BENCH
    if "--bench" in args:
        bench = pathlib.Path(args[args.index("--bench") + 1]).resolve()
    runs = int(args[args.index("--runs") + 1]) if "--runs" in args else DEFAULT_RUNS
    cur = best_of(bench, runs)
    bench_fwd = BENCH_FWD
    if "--bench-fwd" in args:
        bench_fwd = pathlib.Path(args[args.index("--bench-fwd") + 1]).resolve()
    cur.update(mem_probe(bench_fwd))  # fold the mem:* points into the same baseline dict
    base = json.loads(BASELINE.read_text()) if BASELINE.exists() else None
    fails = []
    warns: list[str] = []  # measured, reported, does not fail the gate
    mem_hdr = (f" / mem +{(MEM_REGRESS - 1) * 100:.0f}% / blocks +0"
               if any(k.startswith("mem:") for k in cur) else "")
    print(f"Per-loop perf gate (libtracer in-process, best of {runs} run(s), "
          f"fail: p50 +{(LAT_REGRESS - 1) * 100:.0f}% / mean +{(MEAN_REGRESS - 1) * 100:.0f}% / "
          f"deliv -{(1 - TPUT_REGRESS) * 100:.0f}%{mem_hdr}):")
    for k, v in cur.items():
        if "bytes" in v:  # memory footprint point (mem:*) — exact, deterministic
            line = f"  {k:<22} live={v['bytes']:>6} B/vertex  blocks={v['allocs']:>2}"
            b = base.get(k) if base else None
            if b and "bytes" in b:
                if v["bytes"] > b["bytes"] * MEM_REGRESS and v["bytes"] - b["bytes"] > MEM_TICK_B:
                    fails.append(f"{k} memory pullback: {v['bytes']}B vs base {b['bytes']}B "
                                 f"(+{(v['bytes'] / b['bytes'] - 1) * 100:.1f}%)")
                line += f"   (base {b['bytes']}B"
                # Block COUNT ratchets exactly — no tolerance, no tick guard. It is a
                # count of operator-new calls, identical on every host, so any increase
                # is the code allocating more per vertex. Guarded on presence so a
                # baseline recorded before this key existed degrades to bytes-only
                # gating rather than crashing (the checked-in perf_baseline.json
                # fallback deliberately carries no `allocs`: it was captured on a
                # different toolchain, and an exact ratchet is only honest against a
                # same-runner baseline binary, which is what CI's --bench-fwd supplies).
                if "allocs" in b:
                    if v["allocs"] > b["allocs"]:
                        fails.append(f"{k} allocation pullback: {v['allocs']} heap blocks per "
                                     f"vertex vs base {b['allocs']} (+{v['allocs'] - b['allocs']})")
                    line += f", {b['allocs']} blocks"
                line += ")"
            print(line)
            continue
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
