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
  ./perf_gate.py --tier blocking         # a breached ratchet stops the job (see TIERS)
  ./perf_gate.py --tier advisory         # the same comparison, reported, never fails
  ./perf_gate.py --sample-note "..."     # host_guard's per-sample verdict (see TIERS)
  ./perf_gate.py                         # legacy: run + validate (records baseline on first run)
  ./perf_gate.py --update-baseline       # legacy: accept current numbers as the new baseline
  ./perf_gate.py --bench PATH            # time PATH as the CANDIDATE instead of the build output
  ./perf_gate.py --bench-fwd PATH        # probe PATH for the candidate's per-vertex memory
  ./perf_gate.py --baseline-bench-fwd P  # the baseline binary's per-vertex memory (paired)
  ./perf_gate.py --runs N                # legacy: best-of-N bench executions (default 3)

The comparison this file makes is the same on every machine; what differs is whether a
breached ratchet is allowed to STOP the job. That is the caller's TIER, declared on the
command line rather than described in a workflow comment — see `TIERS` below.

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
from typing import Callable

HERE = pathlib.Path(__file__).resolve().parent
BENCH = HERE / "build" / "bench_libtracer"
BENCH_FWD = HERE / "build" / "bench_forward_heap"
# The gated points no longer all come from one binary (#1173). Each POINTS entry names
# the binary that produces its RESULT rows; they all emit bench_common's shared 12-column
# format, so `run_bench_once` reads any of them unchanged.
BENCH_BY_KEY = {
    "main": "bench_libtracer",
    "compact": "bench_compact_delivery",
    "demux": "bench_forward_demux",
}
BASELINE = HERE / "perf_baseline.json"

# `is_contaminated` is host_guard.py's single contamination predicate, and it is IMPORTED
# rather than re-derived here on purpose: "what counts as a suspect sample" has to be
# decided in one place, or the tool that writes the flag and the tool that reads it drift
# apart. host_guard.py sits beside this file, which is on sys.path for both the CLI
# (script directory) and the tests (they insert it), but say so explicitly so a caller
# that runs this file from elsewhere fails on the import instead of on the rule.
sys.path.insert(0, str(HERE))
from host_guard import is_contaminated  # noqa: E402
import bench_conditions as bc  # noqa: E402

# --- MEASUREMENT CONDITIONS (#1676): only a clean run may PASS or FAIL ---------------
# Every TIMED bench execution below goes through `bench_conditions.measure`, which samples
# the bench CPU's foreign time and the host's CPU pressure around it, re-runs a contended
# execution up to `bc.DEFAULT_ATTEMPTS` times, and records the kept attempt in `LEDGER`.
# The gate never reads /proc itself: "were the conditions clean?" is decided in that one
# module, and the only thing this file does with the answer is refuse to render PASS or
# FAIL on a ledger that is not clean (`render_verdict` -> INCONCLUSIVE, exit 3 on the
# blocking tier). The memory probes are NOT routed through it: allocation counts do not
# move with load, so there is nothing for contention to contaminate.
#
# `BENCH_CPU` pins every timed execution (the pinned host sets it); unset — every hosted
# runner — runs unpinned and is classified over the process's whole affinity set.
LEDGER = bc.Ledger()
# Every `CLOCK res_ns sample_ns` line the timed transcripts carried (#1804): the clock's
# resolution and the measured cost of one timed sample, printed under the verdict.
CLOCK_FLOORS: list[tuple[float, float]] = []
CPUS = bc.cpus_from_env()
# Every timed execution that exited non-zero (#1847). A crashed or aborted bench emits a
# partial transcript, and its missing rows used to read as "absent — not gated": a gate
# that measured nothing printed PASS. A non-zero exit is not a verdict on the code either
# way, so it makes the verdict INCONCLUSIVE, the same as a contended run.
BENCH_ERRORS: list[str] = []
EXIT_INCONCLUSIVE = 3


def timed(argv: list[str], timeout: float, score_pressure: bool = True) -> str:
    """@brief Run one timed bench execution under the classifier; its kept stdout.
    @p score_pressure False judges it on foreign time only (a MULTI family set, #1803)."""
    m = LEDGER.add(bc.measure(argv, cpus=CPUS, timeout=timeout, log=print,
                              score_pressure=score_pressure))
    if m.returncode != 0:
        name = pathlib.Path(argv[0]).name + "".join(f" {a}" for a in argv[1:])
        BENCH_ERRORS.append(f"{name} exited {m.returncode}")
        print(f"perf_gate: {name} exited {m.returncode} — the verdict will be INCONCLUSIVE")
    return m.stdout

# --- VERDICT TIERS (#1251): who a breached ratchet is allowed to stop --------------
# The two-tier policy used to live in a `perf.yml` comment, which meant the gate could
# neither obey nor violate it: `perf_gate.py` had no tier concept at all, so "the pinned
# host blocks, the GitHub runners warn" was a claim about a mechanism that did not exist.
# It is a flag now, and the workflow DECLARES its tier on the command line.
#
#   blocking  — a breached ratchet fails the job. The comparison stops a merge.
#   advisory  — the SAME comparison, on the same thresholds, printed with the same
#               numbers; the breach is announced (verdict line + a `::warning::`
#               annotation) and the process still exits 0.
#
# The tier changes NOTHING about how the comparison is made. It is not a second, looser
# threshold set and it must never become one: an advisory run that quietly measured
# something weaker than a blocking run would make the two tiers incomparable, and the
# whole value of the advisory tier is that its output can be read as "this is what the
# blocking tier would have said here".
#
# DEFAULT = advisory, and the reasoning is about the callers rather than about taste.
# Every CI caller passes `--tier` explicitly (test_perf_gate.py's `WorkflowsDeclareTheirTier`
# fails the build if one does not), so the default is never what gates a PR — it is what a
# maintainer gets from `./perf_gate.py` on an unpinned laptop against yesterday's numbers.
# On that machine a blocking default fails on the machine rather than on the code, and the
# lesson a reader takes from a red run they cannot reproduce is to stop reading the gate.
# A forgotten flag therefore under-enforces LOUDLY (the breach is still printed, still
# annotated, and the workflow lint reds) rather than false-failing quietly.
TIERS = ("blocking", "advisory")
DEFAULT_TIER = "advisory"

# Canonical points: (mode, size, fanout, endpoints). RESULT cols (collate.py):
# RESULT sys mode size fan ep pub_s deliv_s mb_s p50ns p99ns meanns
# One point per GATED family, so a regression on any of these legs is caught —
# not just the 1:1 write. This is NOT the whole dispatch surface: inproc-deliver, the
# -batch twins and the eptype-* sweep's two LEAN arms are charted and published but
# ungated. Do not restate this list as "anywhere on the dispatch surface" —
# docs/methodology.md did, and that sentence reaches the public performance page (#1041):
#   inproc / inproc-borrow  — the canonical zero-copy / loaned 1:1 writes
#   fan-out 1024            — the subscriber fan-out loop
#   inproc-path @ 8192 ep   — the resolver canary (registry lookup per write)
#   mixed                   — the composed realistic topology
#   fold-b4                 — the L0 inline-fold codec tier (batch-amortized)
#   lkv-store-{heap,pool}   — the L1 rope->contiguous copy (`rope_t::materialize`)
#   lkv-{store,alloc}-heap @ 1024 B — the heap backend's large-segment layout (#1768)
#   inproc-target-{handler,stored} @ fan 8 — the path-target dispatch legs
#   eptype-stream           — the STREAM role's bounded-history retention leg
#
# `eptype-stream` is gated and its two siblings are NOT, and the asymmetry is the whole
# reason it is here. `eptype-lean` and `eptype-lean-cached` are `run_inproc` re-emitted
# under an endpoint-type name (`run_eptype`), so they are the SAME code as `inproc/64/1/1`
# and `inproc-borrow/64/1/1` — already gated, twice over, and gating them again would buy
# correlated evidence rather than coverage. `eptype-stream` is not a re-emission: it is
# the only point on this list that runs `vertex_policy_t::retention` and therefore the only one
# downstream of the STREAM role's bounded-ring RETENTION work, which every write to a
# STREAM vertex pays before fan-out. Nothing else here touches that path, so a pullback
# confined to retention was invisible to all fourteen predecessors — the same guard-gap
# shape as #1250 (`rope_t::materialize`) and #1173 (`compact-forward`), and a live one
# while RFC-0025's stream work (#1204) is reshaping exactly that ring.
#
# It costs NO extra wall-clock: `bench_libtracer`'s default sweep already emits this row
# (`run_eptype_stream`), so the gate reads one more key out of output it was already
# collecting — the same deal as the `lkv-store-*` and `inproc-target-*` pairs.
#
# All three legs gate at the nominal +15% / +12% / -12%: measured on a (busy) host at
# 190 ns p50 / 190 ns mean over 5 rounds, best-of-rounds, which is far above the 100 ns
# band where `LAT_TICK_NS` would demand an extra +25 ns absolute and blunt the latency
# legs the way it blunts `fold-b4`. The p50 read 190 ns in every one of the five rounds.
#
# The two `inproc-target-*` legs are here because of #1077, and fan 8 rather than any
# other width for two measured reasons. That issue was opened on a ~+5.5% step at
# `inproc-target-handler` fan 8 that no gate could see; the step was later absorbed by
# ordinary fan-out/store work (#1129, #1174) and HEAD now runs ~11% FASTER than v0.8.0
# there — but the guard gap it exposed is what kept the issue open, and #1235 is a live
# instance of the same gap costing a sustained +10.5% / -10.4% step that walked under
# these thresholds unwatched. Fan 8 is the width to watch because it sits exactly on
# `vertex_t::kInlineFanout`, the no-heap small-fan-out boundary, so the narrow-fan
# snapshot path is what it prices; and because the layout control run of 2026-08-14
# measured it as the most layout-STABLE point of the whole sweep (0.62% spread across
# three `-falign-functions` placements, against 15% for `fold-b4`), so the false-red
# risk from code placement here is near zero. BOTH legs are gated: `stored` carries the
# same shape as `handler`, and it is only readable at all now that `bench/host_guard.py`
# (#1236) rejects the contaminated windows that once put its own A/A null at -14.7%.
# Like the `lkv-store-*` pair these cost NO extra wall-clock — `bench_libtracer`'s
# default sweep already emits both rows at every width in `kFanouts`; the gate simply
# reads two more keys out of output it was already collecting.
#
# These two rows are NOT tick-quantized, unlike `fold-b4`. The p50/mean columns of a
# fan-8 row time the whole 8-subscriber publish, not one delivery: measured ~640 ns p50 /
# ~680 ns mean (`handler`) and ~800 / ~1030 (`stored`), against per-DELIVERY costs of
# ~62-80 ns. Both baselines are far over the 100 ns line at which `LAT_TICK_NS` starts
# demanding an absolute +25 ns, so all three legs gate at their nominal +15% / +12% /
# -12% and the ~5-8% class of step this issue was opened on is inside their reach on the
# mean leg. Do not read the ns figures here as per-delivery numbers; the published
# charts derive those from deliv/s.
#
# The two `lkv-store-*` legs are here because of what happened without them (#1250):
# reshaping `rope_t::flatten`'s wrapper cost 25-48% on EVERY `materialize` path — branch
# and field writes, `op_resolve` reads, FWD COMPACT emission, the RX span sink — and not
# one of the then-ten points is downstream of that call, so every gate stayed green
# through a release. They cost NOTHING extra to run: `bench_libtracer`'s default sweep
# already emits these rows (`run_lkv_store_rows`), so this is two more keys read out of
# output the gate was already collecting, not two more measurements — the added
# wall-clock is zero. They were taken at 64 B only, on the argument that the sweep's
# 1024 B twins move with them (measured, #1250) and a second size would buy correlated
# evidence. #1768 disproved that for the HEAP rows: RFC-0028 slice 10 put the segment header
# and payload in one block, which made 64 B ~30% faster and 1024 B ~2x slower (a 1072 B
# request misses glibc's 1032 B tcache), and v0.17.0 shipped with the gate green because both
# directions met on one size. So the heap backend is also gated at 1024 B, on both rows:
# `lkv-store-heap` (alloc + copy) and `lkv-alloc-heap` (the alloc/free alone; the ADR-0060
# pool/heap ratio below is report-only, so this row is its only gate). The pool
# rows stay at 64 B: a pool slot's layout does not depend on the payload size. These two
# rows also come from output the default sweep already emits, so they too add no wall-clock.
# Both run in the sub-100 ns band, so `LAT_TICK_NS` tick-guards their latency legs and the
# throughput leg (bulk-timed) carries them; the #1768 step (17.6 -> 47 ns, 27 -> 54 ns)
# clears every leg.
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
    ("main", "inproc", 64, 1, 1),
    ("main", "inproc-borrow", 64, 1, 1),
    ("main", "inproc", 64, 1024, 1),
    ("main", "inproc-path", 64, 1, 8192),
    ("main", "mixed", 0, 6, 128),
    ("main", "fold-b4", 512, 1, 1),
    ("main", "lkv-store-heap", 64, 1, 1),
    ("main", "lkv-store-pool", 64, 1, 1),
    ("main", "lkv-store-heap", 1024, 1, 1),
    ("main", "lkv-alloc-heap", 1024, 1, 1),
    ("main", "inproc-target-handler", 64, 8, 1),
    ("main", "inproc-target-stored", 64, 8, 1),
    ("main", "eptype-stream", 64, 1, 1),
    ("compact", "compact-forward", 64, 1, 1),
    ("compact", "compact-terminus", 64, 1, 1),
    # Keyed by the frame the bench EMITS (`frame.size()`): 61 B since RFC-0018's packed
    # PATH records (1fe92124). They were keyed 79 for seven weeks after that and silently
    # matched nothing, which is why a key the candidate does not emit now fails (#1847).
    ("demux", "fwd-demux-fixed", 61, 1, 1),
    ("demux", "fwd-demux-scan", 61, 64, 64),
    # MULTI-threaded rows (#1803): timed in their own `--family-set multi` invocation and
    # judged on foreign CPU time only (see GATE_FAMILY_SET_MULTI). T=4 because the gate's
    # bench CPU set is four CPUs; a host with fewer reports them absent, not failed.
    ("main", "inproc-mt4", 64, 1, 4),
    ("main", "acl-inherit-d4-mt4", 64, 1, 4),
    ("main", "poolalloc-mt4", 64, 1, 1),
]
# The only POINTS a correct candidate may legitimately not emit: the MULTI rows above run
# only on a host with at least four usable CPUs (`bench::usable_cpus() >= 4`). Every other
# gated key the candidate does not emit is a FAIL (#1847) — a renamed mode, a changed frame
# size or a sibling binary that was not built must never read as "not gated" again.
MAY_BE_ABSENT = frozenset({"inproc-mt4/64/1/4", "acl-inherit-d4-mt4/64/1/4",
                           "poolalloc-mt4/64/1/1"})


def missing_point(k: str) -> str:
    """@brief The fail line for a gated key the candidate's run did not emit (#1847)."""
    print(f"::error::perf gate: gated point {k} was not emitted by the candidate — "
          f"re-key POINTS to the row the bench emits, or build its binary")
    return (f"{k} not measured: the candidate emitted no row for this gated point "
            f"(re-key POINTS or build the binary — a missing key is never 'not gated')")
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

# --- ADR-0060 LKV copy-store ratio (pool-vs-heap), REPORTED, never gated -------------
# The pooled value_backend vs the default heap on the write-path alloc/free op, as a ratio
# within one run. The pool's acceptance is NOT a speed ratio (maintainer, #1695, 2026-10-03):
# its only goal is never to take an allocation from the system heap. That claim is gated
# structurally, by `bench_forward_heap`'s LKV-ROUTE window: the pool arm's alloc/free loop
# must make ZERO global operator new/delete calls, with the heap arm as its control, and CI
# runs it again with LKV_ROUTE_BREAK=1 and requires it to fail. That is the ONLY blocking
# pool check. A slower pool path is caught as a row, `lkv-store-pool/64/1/1` in POINTS.
#
# The ratio is printed for the record and fails nothing. It was gated before (a 2.0x floor,
# paired against main by #1745, with a 1.25x fallback threshold), and it measured the runner:
# over 254 per-arm best-of-3 CI readings on healthy code (2026-09-29..10-03) S=64 ran
# 1.4x-6.5x, median 3.0x, bimodal, with main and an identical candidate at 2.1x/1.6x in the
# SAME session. The paired form stays so the report sets main's same-session ratio beside
# the candidate's: the arms run interleaved, alternating which starts, and each keeps its
# BEST observation of each row (best-of-rounds: contamination is one-sided).
LKV_ROUNDS = 3             # legacy best-of rounds, and the paired form's pair count
LKV_ROWS = ("lkv-alloc-heap", "lkv-alloc-pool")


def lkv_parse(out: str) -> dict[int, dict[str, float]]:
    """@brief The lkv-alloc rows of one `bench_libtracer lkv` run: {size: {row: ops/s}}."""
    rows: dict[int, dict[str, float]] = {}
    for line in out.splitlines():
        f = line.split("\t")
        if len(f) == 12 and f[0] == "RESULT" and f[2] in LKV_ROWS:
            rows.setdefault(int(f[3]), {})[f[2]] = float(f[6])  # f[6] = deliveries/s (ops/s)
    return rows


def lkv_best(runs: list[dict[int, dict[str, float]]]) -> dict[int, float]:
    """@brief Best-of-rounds per row (max ops/s), then the pool/heap ratio per size.

    Each row keeps its own best: the heap and the pool rows are separate executions, and a
    neighbour that slowed one of them in a round says nothing about the other."""
    best: dict[int, dict[str, float]] = {}
    for run in runs:
        for size, r in run.items():
            b = best.setdefault(size, {})
            for row, ops in r.items():
                b[row] = max(b.get(row, 0.0), ops)
    out = {}
    for size, b in best.items():
        h, p = b.get("lkv-alloc-heap"), b.get("lkv-alloc-pool")
        if h and p:
            out[size] = p / h
    return out


def lkv_line(size: int, cand: float, base: float | None) -> str:
    """@brief One size's report line: the candidate's best-of ratio, and main's beside it.

    @param cand The candidate's best-of-rounds pool/heap ratio.
    @param base Main's ratio from the SAME interleaved session, or None (legacy form, or a
                main binary without the rows).
    """
    line = f"  lkv-alloc S={size:<6} pool/heap alloc/free = {cand:>4.1f}x"
    if base is not None:
        line += f"  (main {base:.1f}x, same session)"
    return line + "  (reported, not gated — LKV-ROUTE is the pool gate, #1695)"


def lkv_ratio_report(bench: pathlib.Path) -> None:
    """@brief The legacy form: the candidate alone, best of LKV_ROUNDS. Fails nothing."""
    cand = lkv_best([lkv_parse(timed([str(bench), "lkv"], timeout=120))
                     for _ in range(LKV_ROUNDS)])
    for size in sorted(cand):
        print(lkv_line(size, cand[size], None))


def lkv_ratio_report_paired(bench: pathlib.Path, base_bench: pathlib.Path,
                            pairs: int = LKV_ROUNDS) -> None:
    """@brief The per-PR form (#1745): main and the candidate interleaved, best-of-pairs.
    Fails nothing."""
    runs: dict[str, list] = {"cand": [], "base": []}
    for i in range(max(1, pairs)):
        order = [("base", base_bench), ("cand", bench)]
        if i % 2:
            order.reverse()
        for arm, path in order:
            runs[arm].append(lkv_parse(timed([str(path), "lkv"], timeout=120)))
    cand, base = lkv_best(runs["cand"]), lkv_best(runs["base"])
    for size in sorted(cand):
        print(lkv_line(size, cand[size], base.get(size)))


# --- HOW THE GATE TIMES THE FAMILIES (#1803) ------------------------------------------
# `bench_libtracer`'s default sweep is a list of families, each tagged SINGLE- or MULTI-
# threaded (`bench_libtracer --families`). A MULTI family (inproc-mt*, acl-…-mt4, the
# *alloc-mt* rows) runs T workers on the pinned CPUs while its main thread spins waiting for
# them, so the bench's OWN threads queue behind each other and its own cgroup's CPU
# pressure climbs. The condition check scored that pressure and, sampled at the next
# launch, the residue too: about one gate in three came back INCONCLUSIVE with 0% foreign
# load and own-cgroup psi 84 -> 23 -> 5.7 across its re-runs.
#
# So the gate times the two sets as SEPARATE invocations, and both stay compared A/B:
#   - `--family-set single`, judged on foreign time AND own-cgroup pressure, as before;
#   - `--family-set multi`, judged on foreign time ONLY. Its own threads' pressure is
#     recorded, not scored. A real intruder still shows as foreign CPU time on the bench
#     CPUs, so it still makes the verdict INCONCLUSIVE.
# Every SINGLE invocation runs before every MULTI one (`paired_samples` takes all pairs and
# `best_of` all runs of the single set first, and the lkv ratio report runs
# ahead of both), so no pressure-scored launch follows a MULTI run's residue.
#
# Both arms must speak it or neither uses it: a baseline built before family sets refuses
# `--families` (exit 2), and then both arms sweep everything in one invocation, pressure
# scored, exactly as before this change.
GATE_FAMILY_SET = ("--family-set", "single")
GATE_FAMILY_SET_MULTI = ("--family-set", "multi")


def has_family_sets(bench: pathlib.Path) -> bool:
    """@brief Whether @p bench understands `--family-set` (its `--families` probe exits 0)."""
    try:
        p = subprocess.run([str(bench), "--families"], capture_output=True, text=True,
                           timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return p.returncode == 0 and "\tsingle" in p.stdout


def gate_sweep_args(*binary_sets: dict[str, pathlib.Path],
                    probe: Callable[[pathlib.Path], bool] | None = None) -> tuple[str, ...]:
    """@brief The extra argv the gate passes to every `main` binary for its FIRST pass: the
    SINGLE family set when every arm's `bench_libtracer` supports family sets (a second,
    foreign-only pass then times the MULTI set), otherwise nothing (one whole sweep).
    @p probe defaults to @ref has_family_sets, looked up at call time so tests can patch it."""
    probe = probe or has_family_sets
    mains = [bins["main"] for bins in binary_sets if bins and "main" in bins]
    if mains and all(probe(m) for m in mains):
        return GATE_FAMILY_SET
    return ()


def _passes(extra: tuple[str, ...]) -> list[tuple[tuple[str, ...], bool, bool]]:
    """@brief The gate's timing passes: (main argv, run the sibling binaries?, score psi?)."""
    if not extra:
        return [((), True, True)]
    return [(extra, True, True), (GATE_FAMILY_SET_MULTI, False, False)]


def run_bench_once(bench: pathlib.Path, extra: tuple[str, ...] = (),
                   score_pressure: bool = True) -> list[tuple]:
    if not bench.exists():
        print(f"perf_gate: {bench} not built — run: cmake -S {HERE} -B {HERE}/build "
              f"-DCMAKE_BUILD_TYPE=Release && cmake --build {HERE}/build -j", file=sys.stderr)
        sys.exit(2)
    out = timed([str(bench), *extra], timeout=180, score_pressure=score_pressure)
    rows = []
    for line in out.splitlines():
        f = line.split("\t")
        if len(f) == 12 and f[0] == "RESULT":
            # Latency columns are read as floats: a batch row prints them to the picosecond
            # (#1804), and int() here would re-quantize exactly what that change removed.
            rows.append((f[2], int(f[3]), int(f[4]), int(f[5]), float(f[7]), float(f[9]),
                         float(f[11])))
        elif f[0] == "CLOCK" and len(f) == 3:
            CLOCK_FLOORS.append((float(f[1]), float(f[2])))
    return rows


def metric(rows, mode, size, fan, ep):
    """Median across one run's repeated RESULT rows for the point."""
    p50 = [r[5] for r in rows if (r[0], r[1], r[2], r[3]) == (mode, size, fan, ep)]
    dv = [r[4] for r in rows if (r[0], r[1], r[2], r[3]) == (mode, size, fan, ep)]
    mean = [r[6] for r in rows if (r[0], r[1], r[2], r[3]) == (mode, size, fan, ep)]
    if not p50:
        return None
    return {"p50_ns": statistics.median(p50), "deliv_s": statistics.median(dv),
            "mean_ns": statistics.median(mean)}


def best_of(binaries: dict[str, pathlib.Path], runs: int) -> dict[str, dict]:
    """Best run per point: min p50, max deliveries/s across `runs` executions.

    Each BINARY is executed once per run and its rows are matched only against the points
    that declare it, so adding a point from a new binary costs one extra process per run
    rather than one per point.

    The pass loop is outermost, as in @ref paired_samples: every run of the pressure-scored
    SINGLE pass launches before any MULTI run, so none starts into a MULTI run's residue.
    The fold is a per-point min/max, so the order does not change the result.
    """
    cur: dict[str, dict] = {}
    for args, siblings, psi in _passes(gate_sweep_args(binaries)):
        for _ in range(max(1, runs)):
            rows_by_bin: dict[str, list] = {}
            for b, path in binaries.items():
                if b == "main":
                    rows_by_bin[b] = run_bench_once(path, args, psi)
                elif siblings:
                    rows_by_bin[b] = run_bench_once(path)
            for (b, m, s, f, e) in POINTS:
                if b not in rows_by_bin:
                    continue
                v = metric(rows_by_bin[b], m, s, f, e)
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


def paired_samples(cand: dict[str, pathlib.Path], base: dict[str, pathlib.Path],
                   pairs: int) -> dict[str, dict[str, list[dict]]]:
    """@brief Run candidate and baseline interleaved, alternating which one starts.

    Emits A B / B A / A B / … so neither arm systematically owns the early (cold, or
    boost-ramping) part of the job and neither systematically owns a late drift window.
    Returns {"cand"|"base": {point_key: [per-pair metric dict, …]}} with index `i` of
    both arms drawn from the SAME pair, so the lists can be compared element-wise.
    """
    out: dict[str, dict[str, list[dict]]] = {"cand": {}, "base": {}}
    passes = _passes(gate_sweep_args(cand, base))
    print("  bench_libtracer sweep: " + (
        "all families, one invocation" if len(passes) == 1 else
        "single set (foreign + pressure), then multi set (foreign only)"))
    # Pass by pass, each one interleaved over every pair: all pressure-scored SINGLE
    # invocations run before any MULTI one, so none launches into a MULTI run's residue.
    for args, siblings, psi in passes:
        for i in range(max(1, pairs)):
            order = [("base", base), ("cand", cand)]
            if i % 2:
                order.reverse()
            for arm, binaries in order:
                rows_by_bin = {b: (run_bench_once(path, args, psi) if b == "main"
                                   else run_bench_once(path))
                               for b, path in binaries.items() if b == "main" or siblings}
                for (b, m, s, f, e) in POINTS:
                    if b not in rows_by_bin:
                        continue
                    v = metric(rows_by_bin[b], m, s, f, e)
                    if v:
                        out[arm].setdefault(f"{m}/{s}/{f}/{e}", []).append(v)
    return out


def _tick_ok(cur: float, ref: float) -> bool:
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
        return c > b * factor and (not tick_guard or _tick_ok(c, b))

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


def gate_paired(cand: dict[str, pathlib.Path], base: dict[str, pathlib.Path],
                pairs: int) -> list[str]:
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
    for (_b, m, s, f, e) in POINTS:
        k = f"{m}/{s}/{f}/{e}"
        cs, bs = samples["cand"].get(k), samples["base"].get(k)
        if not cs and k not in MAY_BE_ABSENT:
            # Absent from the candidate (and so from both arms, or a candidate that stopped
            # emitting a row the baseline still has): nothing was gated, and that FAILS.
            print(f"  {k:<22} (absent from the candidate — FAIL, see below)")
            fails.append(missing_point(k))
            continue
        if not cs or not bs:
            # Baseline-only absence is a new point the baseline build predates.
            print(f"  {k:<22} (absent from one arm — not gated)")
            continue
        print(f"  {k}")
        legs = [
            ("p50_ns", "p50", "ns", "11,.3f", LAT_REGRESS, False, True),
            ("mean_ns", "mean", "ns", "11,.3f", MEAN_REGRESS, False, True),
            ("deliv_s", "deliv/s", "", "15,.0f", TPUT_REGRESS, True, False),
        ]
        for key, label, unit, fmt, factor, lower_worse, tick in legs:
            c = [float(x[key]) for x in cs]
            b = [float(x[key]) for x in bs]
            if min(c) <= 0 or min(b) <= 0:
                # 0 means "this row does not measure that": deliv_s on a latency-only row
                # (#553), p50/mean on a bulk-only row such as `lkv-*` (#1804).
                continue
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
            # gating rather than crashing (a LOCAL perf_baseline.json fallback
            # carries no `allocs` if it predates the key, and an exact ratchet is
            # only honest against a same-runner baseline binary, which is what CI
            # supplies). "Checked-in" it is not, and never was: the file is
            # gitignored (bench/.gitignore), tracked by nothing, and no workflow
            # reads it — CI always passes `--baseline-bench`. See the LEGACY
            # paragraph in this module's docstring (#1495).
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


def enforces(tier: str, sample_note: str | None = None) -> tuple[bool, str]:
    """@brief May THIS run's fails stop the job, and if not, what is the reason?

    Two things can take the teeth out of a comparison, and they are different claims:

      * the caller's tier is `advisory` — this runner is not one whose noise floor the
        blocking bar was calibrated against, so its verdict is reported, not enforced;
      * the SAMPLE is flagged. `host_guard.py` brackets the measured run with an A/A
        null pair and flags the sample when the machine moved underneath it. Such a
        sample is FLAGGED, never deleted — and the rule that makes flagging worth
        anything is that a gating consumer stops believing it. A contaminated sample
        that could still fail a PR would be the guard measuring the host and then
        billing the code for it.

    Returns (enforced, reason). `reason` is empty when the run enforces.
    """
    if tier != "blocking":
        return False, "this caller's tier is advisory — reported, not enforced"
    if is_contaminated(sample_note):
        return False, (f"the sample is FLAGGED contaminated ({sample_note}) — the "
                       f"blocking tier does not render a verdict on a sample taken "
                       f"while the machine was moving")
    return True, ""


def render_verdict(fails: list[str], warns: list[str], tier: str,
                   sample_note: str | None = None,
                   conditions: bc.Ledger | None = None,
                   bench_errors: list[str] | None = None) -> int:
    """@brief Print the verdict under its tier and return the process exit code.

    Same numbers, same lines, same markers in both tiers — `!` for a breached ratchet,
    `~` for the pre-existing soft-warn list (#792/#464), which is a DIFFERENT mechanism
    and is untouched by the tier: it is a comparison the gate declines to call a
    failure at all, in any tier, and it prints under its own marker either way.

    The only thing the tier changes is the verdict line and the exit code, and an
    unenforced breach is never quiet: it names itself in the verdict line AND raises a
    `::warning::` annotation, because a downgraded failure that printed nothing would
    be indistinguishable from a point that never regressed.

    INCONCLUSIVE (#1676) comes first and overrides both: when @p conditions says a timed
    execution stayed contended through every re-run, the comparison was made on a
    machine that was not ours, so neither PASS nor FAIL is true. The numbers still print
    (under `?`, never `!`); the blocking tier exits `EXIT_INCONCLUSIVE` so the job is
    re-run rather than merged on an unverified green, and the advisory tier exits 0.

    @p bench_errors (#1847) makes the verdict INCONCLUSIVE the same way: a bench process
    that exited non-zero left a partial transcript, so the comparison is not complete.
    """
    contended = conditions is not None and not conditions.clean
    if contended or bench_errors:
        mark = "::error::" if tier == "blocking" else "::warning::"
        if contended:
            kept = conditions.kept()
            bad = sum(not c.clean for c in kept)
            print(f"PERF: INCONCLUSIVE  [tier={tier}] — {bad} of {len(kept)} timed "
                  f"execution(s) ran on a contended bench CPU through every re-run; this is "
                  f"not a verdict on the code. Re-run the job.")
            print(f"{mark}perf gate INCONCLUSIVE — {conditions.note()}; "
                  f"re-run on a quiet runner (a PASS or FAIL needs clean conditions)")
        if bench_errors:
            print(f"PERF: INCONCLUSIVE  [tier={tier}] — {len(bench_errors)} bench "
                  f"execution(s) exited non-zero; the comparison is incomplete, not passed.")
            print(f"{mark}perf gate INCONCLUSIVE — {bench_errors[0]}"
                  + (f" +{len(bench_errors) - 1} more" if len(bench_errors) > 1 else "")
                  + "; fix the bench, then re-run")
            for x in bench_errors:
                print("  ? " + x)
        why = "contended" if contended else "a bench exited non-zero"
        for x in fails:
            print("  ? " + x + f"  (unverified — {why})")
        for x in warns:
            print("  ~ " + x)
        return EXIT_INCONCLUSIVE if tier == "blocking" else 0
    enforced, why = enforces(tier, sample_note)
    if not fails:
        print(f"PERF: PASS  [tier={tier}]")
    elif enforced:
        print("PERF: FAIL  [tier=blocking — this comparison stops the job]")
    else:
        print(f"PERF: FAIL (ADVISORY — {why}; this job is NOT failed) [tier={tier}]")
        print(f"::warning::perf gate: {len(fails)} comparison(s) past the ratchet, "
              f"reported but not failing this job — {why}")
    for x in fails:
        print("  ! " + x)
    # Warnings print AFTER the verdict and under their own marker, never merged into
    # the fail list. A downgraded failure that printed nothing would be indistinguishable
    # from a point that never regressed, which is how a guard turns into a blind spot.
    for x in warns:
        print("  ~ " + x)
    return 1 if (fails and enforced) else 0


def _tier(args: list[str]) -> str:
    """@brief The declared tier, or `DEFAULT_TIER`. An unknown tier REFUSES to run.

    Exit 2 (a wiring error), not a silent fall-back to either tier: falling back to
    blocking would fail a job on a typo, and falling back to advisory would turn one
    into a gate that measures and never enforces.
    """
    if "--tier" not in args:
        return DEFAULT_TIER
    tier = args[args.index("--tier") + 1]
    if tier not in TIERS:
        print(f"perf_gate: unknown --tier {tier!r}; expected one of {', '.join(TIERS)}",
              file=sys.stderr)
        sys.exit(2)
    return tier


def _opt(args: list[str], name: str, default: pathlib.Path | None) -> pathlib.Path | None:
    """@brief Resolve an optional path-valued flag."""
    return pathlib.Path(args[args.index(name) + 1]).resolve() if name in args else default


def _siblings(bench: pathlib.Path) -> dict[str, pathlib.Path]:
    """@brief The gated binaries in @p bench's build directory, keyed as POINTS names them.

    A binary that is absent is OMITTED rather than defaulted, so its points are never
    compared against a stale build: absent from the baseline they are "not gated" (a point
    the baseline predates), absent from the candidate they FAIL (#1847).
    """
    out = {}
    for key, name in BENCH_BY_KEY.items():
        path = bench if name == bench.name else bench.parent / name
        if path.exists():
            out[key] = path
    return out


def main() -> int:
    args = sys.argv[1:]
    tier = _tier(args)
    # The A/A-bracket verdict for the sample this run is about, when the caller has one
    # (`host_guard.py bracket` emits it as a step output). Absent = nothing is known
    # about the machine, which is the state every GitHub runner is in.
    sample_note = args[args.index("--sample-note") + 1] if "--sample-note" in args else None
    bench = _opt(args, "--bench", BENCH)
    bench_fwd = _opt(args, "--bench-fwd", BENCH_FWD)
    base_bench = _opt(args, "--baseline-bench", None)
    base_fwd = _opt(args, "--baseline-bench-fwd", None)
    # The sibling binaries live beside the one named by --bench, so a caller that points
    # the gate at a baseline BUILD DIRECTORY gets that build's compact/demux arms too —
    # rather than silently comparing a candidate's compact binary against the candidate's.
    cand_bins = _siblings(bench)
    base_bins = _siblings(base_bench) if base_bench is not None else None

    # PAIRED mode — the per-PR gate. Both binaries in hand, so nothing is recorded to
    # or read from perf_baseline.json: the comparison is made against samples drawn in
    # the same interleaved rotation, which is the whole point of #763's fix.
    if base_bench is not None:
        pairs = int(args[args.index("--pairs") + 1]) if "--pairs" in args else PAIRS_DEFAULT
        print(f"Per-loop perf gate (libtracer in-process, INTERLEAVED baseline/candidate, "
              f"tier {tier}, "
              f"fail: p50 +{(LAT_REGRESS - 1) * 100:.0f}% / "
              f"mean +{(MEAN_REGRESS - 1) * 100:.0f}% / "
              f"deliv -{(1 - TPUT_REGRESS) * 100:.0f}%):")
        # The lkv ratio report runs FIRST: it is single-threaded, and the gate's
        # last pass is the MULTI family set, whose own-pressure residue it must not inherit.
        lkv_ratio_report_paired(bench, base_bench)  # ADR-0060 ratio: reported (#1695)
        fails = gate_paired(cand_bins, base_bins, pairs)
        fails += mem_ratchet(bench_fwd, base_fwd)
        print_conditions()
        return render_verdict(fails, [], tier, sample_note, LEDGER, BENCH_ERRORS)

    runs = int(args[args.index("--runs") + 1]) if "--runs" in args else DEFAULT_RUNS
    # The lkv ratio report runs FIRST here too: it is single-threaded, and best_of's last pass is
    # the MULTI family set, whose own-pressure residue it must not inherit.
    lkv_ratio_report(bench)  # ADR-0060 same-run ratio: reported, not gated (#1695)
    cur = best_of(cand_bins, runs)
    cur.update(mem_probe(bench_fwd))  # fold the mem:* points into the same baseline dict
    base = json.loads(BASELINE.read_text()) if BASELINE.exists() else None
    fails = []
    warns: list[str] = []  # measured, reported, does not fail the gate
    mem_hdr = (f" / mem +{(MEM_REGRESS - 1) * 100:.0f}% / blocks +0"
               if any(k.startswith("mem:") for k in cur) else "")
    print(f"Per-loop perf gate (libtracer in-process, LEGACY best of {runs} run(s), "
          f"tier {tier}, fail: p50 +{(LAT_REGRESS - 1) * 100:.0f}% / "
          f"mean +{(MEAN_REGRESS - 1) * 100:.0f}% / "
          f"deliv -{(1 - TPUT_REGRESS) * 100:.0f}%{mem_hdr}):")
    fails += mem_ratchet_legacy(cur, base, bench_fwd)
    for (_b, m, s, f, e) in POINTS:
        k = f"{m}/{s}/{f}/{e}"
        if k not in cur and k not in MAY_BE_ABSENT:
            print(f"  {k:<22} (not emitted — FAIL, see below)")
            fails.append(missing_point(k))
    for k, v in cur.items():
        if k.startswith("mem:"):
            continue  # handled by mem_gate above
        line = (f"  {k:<22} p50={v['p50_ns']:>9.3f}ns mean={v['mean_ns']:>9.3f}ns "
                f"deliv/s={v['deliv_s']:>14,.0f}")
        if base and k in base:
            b = base[k]

            def lat_fails(cur: float, ref: float, factor: float) -> bool:
                """Relative pullback, tick-guarded for sub-100ns points; a 0 is "not
                measured" (a bulk-only row, #1804), never a pullback."""
                return (ref > 0 and cur > ref * factor
                        and (ref >= 100 or cur - ref > LAT_TICK_NS))

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
    print_conditions()
    if not LEDGER.clean:
        # A contended sample must not become the recorded baseline either.
        print("  (baseline NOT recorded — the measurement conditions were contended)")
    elif base is None or "--update-baseline" in args:
        BASELINE.write_text(json.dumps(cur, indent=2) + "\n")
        print(f"  ({'recorded' if base is None else 'updated'} baseline -> {BASELINE.name})")
    return render_verdict(fails, warns, tier, sample_note, LEDGER, BENCH_ERRORS)


def print_conditions() -> None:
    """@brief The measurement-conditions block every verdict is printed under (#1676)."""
    print(f"Measurement conditions ({'CLEAN' if LEDGER.clean else 'CONTENDED'}; "
          f"contended = foreign > {bc.FOREIGN_MAX_PCT:g}% on the bench CPU or "
          f"CPU pressure > {bc.PRESSURE_MAX:g} (own cgroup if pinned, host if not), re-run up to {bc.DEFAULT_ATTEMPTS}x):")
    print(f"  {LEDGER.line()}")
    for x in LEDGER.report(notable_only=True):
        print(x)
    # The clock floor every transcript recorded (#1804): resolution and per-sample cost, ns.
    if CLOCK_FLOORS:
        res = sorted({r for r, _ in CLOCK_FLOORS})
        cost = [c for _, c in CLOCK_FLOORS]
        print(f"  clock floor: resolution {'/'.join(f'{r:g}' for r in res)} ns, "
              f"{min(cost):.1f}..{max(cost):.1f} ns per timed sample "
              f"({len(cost)} transcript(s))")


if __name__ == "__main__":
    sys.exit(main())
