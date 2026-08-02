#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Render UNIFIED per-family benchmark-history charts for the Performance page.

The benchmark-action store (gh-pages ``dev/bench/data.js``) tracks >130 flat
series; the stock /dev/bench page draws one tiny chart per series, which makes
comparing related series (fan-out sweep, payload sweep, MT scaling, ...) nearly
impossible. This module groups the series into FAMILIES and emits one chart per
family with every series as a line on shared axes — plus, for families whose
series differ by a *numeric* parameter (fan-out, payload bytes, topic count,
fold width, threads), the full three-axis view set drawn client-side by
``docs/_static/perf_history.js``: trend (value vs commit, one line per
parameter), sweep (value vs parameter, one line per commit, recency-faded),
heatmap (commit x parameter, color = value), and an isometric 3D surface
(commit x parameter x value).

Release tags (``v*``) are resolved to recorded commits via git and drawn as
labeled vertical markers on every history chart: an exact marker when the tag's
commit is itself a recorded point, else a '≈' marker at the nearest FOLLOWING
recorded commit (ancestor test). Everything degrades to a note, never a crash:
no store -> the caller skips the block; no git/tags -> charts without markers.

The emitted markup is a self-contained MyST ``:::{raw} html`` block with the
committed ``docs/_static/perf_history.{css,js}`` assets INLINED (same rationale
as render_compare: path-independent, no CDN, self-contained page). Stdlib only.
"""
from __future__ import annotations

import json
import pathlib
import re
import subprocess

REPO = pathlib.Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# family specs
# ---------------------------------------------------------------------------
# Each family: a regex over series names in ONE suite ("latency"/"throughput"),
# a label + numeric-parameter extractor for the capture, and axis metadata.
# `px` (the parameter axis) is present only when the captured key is numeric —
# it unlocks the sweep/heatmap/3D views; label-keyed families stay trend-only.


def _num(m: re.Match) -> float:
    return float(m.group(1))


# Every metric the emitter records per measured point, in the order a card offers
# them. A family below names a POINT pattern only — `^inproc 64B/fan(\d+)/1ep` —
# and each metric here appends its own suffix to make a series name.
#
# This exists because the families used to hardcode one metric each, which quietly
# made most of the recorded data undrawable: of 323 emitted series, 160 appeared on
# no chart at all, and `p99 latency` was 70-for-70 dark. Nothing had decided p99 was
# uninteresting — no family had ever been written for it. Deriving the metric axis
# instead of spelling it out per family means a new mode charts every metric it
# emits, without anyone remembering to add three more entries.
#
# `suite` matters: the emitter splits smaller-is-better from bigger-is-better into
# two stores, so a card's x-axis comes from the ACTIVE metric's suite, not the card's.
METRICS: list[dict] = [
    dict(name="p50 latency", suite="latency", fmt="ns", ylabel="p50 latency",
         blurb="The MEDIAN operation: half are faster. The typical cost."),
    dict(name="p99 latency", suite="latency", fmt="ns", ylabel="p99 latency",
         blurb="The 99th percentile: 1 op in 100 is slower. This is the tail a "
               "latency claim lives or dies on — a good p50 with a bad p99 means "
               "occasional stalls users actually feel."),
    dict(name="ns/delivery", suite="latency", fmt="ns", ylabel="ns / delivery",
         blurb="Throughput re-expressed as time per delivery (1e9 / deliveries-per-second). "
               "Unlike p50 it is an AVERAGE over the bulk run, so it includes every cost "
               "the percentile loop excludes."),
    dict(name="throughput", suite="throughput", fmt="rate", ylabel="deliveries / second",
         blurb="Deliveries completed per second during the bulk phase — the same "
               "measurement as ns/delivery, in its natural direction (higher is better)."),
]


FAMILIES: list[dict] = [
    # -- latency suite ------------------------------------------------------
    dict(id="fan", section="dispatch", title="In-process write — by fan-out",
         cond="inproc · 64 B payload · 1 topic — one line per fan-out",
         pat=r"^inproc 64B/fan(\d+)/1ep",
         label=lambda m: f"fan {m.group(1)}", key=_num, log=True,
         px=dict(label="fan-out (subscribers)", log=True, fmt="count")),
    dict(id="payload", section="dispatch", title="In-process write — by payload size",
         cond="inproc · fan-out 1 · 1 topic — one line per payload",
         pat=r"^inproc (\d+)B/fan1/1ep",
         label=lambda m: f"{m.group(1)} B", key=_num, log=False,
         px=dict(label="payload size", log=True, fmt="bytes")),
    dict(id="borrow-payload", section="dispatch",
         title="Loaned (zero-copy) write — by payload size",
         cond="inproc-borrow · fan-out 1 · 1 topic — one line per payload",
         pat=r"^inproc-borrow (\d+)B/fan1/1ep",
         label=lambda m: f"borrow {m.group(1)} B", key=_num, log=False,
         px=dict(label="payload size", log=True, fmt="bytes")),
    dict(id="topics", section="dispatch", title="Write-by-path — by topic count",
         cond="inproc-path · 64 B · fan-out 1 — one line per registry size (resolver canary)",
         pat=r"^inproc-path 64B/fan1/(\d+)ep",
         label=lambda m: f"{m.group(1)} topics", key=_num, log=False,
         px=dict(label="topic count", log=True, fmt="count")),
    # `mixed` is ONE point (the composed realistic topology), so it can never form a
    # multi-line comparison — which is why it charted nowhere at all until now, despite
    # being one of perf_gate.py's canonical gated points. A gated number with no visible
    # history is the exact blind spot these charts exist to remove, so it opts in to a
    # single-line trend.
    # libtracer AND Zenoh on one axes, over commits. Zenoh's version is PINNED, so its
    # line is expected to be flat and is read as a CONTROL: libtracer moving while Zenoh
    # holds still is the code; both moving together is the runner, which on shared CI
    # varies about 2x. Until the emitter was keyed by system these two could not coexist
    # as series at all — a `zenoh` row and a libtracer row with the same mode were
    # medianed into one number.
    dict(id="vs-zenoh-fan", section="dispatch",
         title="libtracer vs Zenoh — by fan-out, over commits",
         cond="inproc · 64 B · 1 topic · same runner, same pass — Zenoh (zenoh-c 1.9.0) is "
              "version-pinned, so its line is the runner's own drift",
         pat=r"^(zenoh )?inproc 64B/fan(\d+)/1ep",
         label=lambda m: ("zenoh" if m.group(1) else "libtracer") + " fan " + m.group(2),
         key=lambda m: ("z" if m.group(1) else "l") + f"-{int(m.group(2)):05d}", log=True,),
    dict(id="mixed", section="dispatch", title="Mixed workload — the composed topology",
         cond="mixed · 128 topics · varied fan-out and payloads — one gated point, tracked "
              "over time rather than compared against a sibling",
         pat=r"^mixed 0B/fan6/128ep", min_series=1,
         label=lambda m: "mixed", key=lambda m: "mixed", log=False,),
    dict(id="dispatch", section="dispatch", title="Dispatch modes compared",
         cond="64 B · fan-out 1 · 1 topic — the four dispatch paths on one axes",
         pat=r"^(inproc|inproc-borrow|inproc-path|inproc-mt1) 64B/fan1/1ep",
         label=lambda m: m.group(1), key=lambda m: m.group(1), log=False,),
    # The #553 pair. Each `-batch` series measures the SAME operation as the series
    # beside it, timed over a calibrated batch instead of one at a time — so the gap
    # between a mode and its `-batch` twin IS the clock's contribution, read directly off
    # the chart. Both are kept because they answer different questions: the quantized
    # series is the unbroken long-run history and the only one with a real p99, the batch
    # series is the one that can resolve a few nanoseconds.
    dict(id="quantization", section="dispatch",
         title="Clock quantization — per-op vs batch-amortized",
         cond="64 B · fan-out 1 · 1 topic — each mode against its own batch-amortized twin; "
              "the gap is what the clock costs, not what the code costs",
         pat=r"^(inproc|inproc-borrow|inproc-path)(-batch)? 64B/fan1/1ep",
         label=lambda m: m.group(1) + (" (batch)" if m.group(2) else " (per-op)"),
         key=lambda m: m.group(1) + (m.group(2) or ""), log=False,),
    dict(id="batch-payload", section="dispatch",
         title="Batch-amortized write — by payload size",
         cond="inproc-batch · fan-out 1 · 1 topic — the clock-free twin of the payload sweep",
         pat=r"^inproc-batch (\d+)B/fan1/1ep",
         label=lambda m: f"{m.group(1)} B", key=_num, log=False,
         px=dict(label="payload size", log=True, fmt="bytes")),
    dict(id="batch-fan", section="dispatch",
         title="Batch-amortized write — by fan-out",
         cond="inproc-batch · 64 B payload · 1 topic — the clock-free twin of the fan-out sweep; "
              "converges on its per-op twin as the operation outgrows the clock",
         pat=r"^inproc-batch 64B/fan(\d+)/1ep",
         label=lambda m: f"fan {m.group(1)}", key=_num, log=True,
         px=dict(label="fan-out (subscribers)", log=True, fmt="count")),
    dict(id="batch-borrow-payload", section="dispatch",
         title="Batch-amortized loaned write — by payload size",
         cond="inproc-borrow-batch · fan-out 1 · 1 topic — the clock-free twin of the loaned "
              "payload sweep, where the per-op reading is flat at the clock's own floor",
         pat=r"^inproc-borrow-batch (\d+)B/fan1/1ep",
         label=lambda m: f"borrow {m.group(1)} B", key=_num, log=False,
         px=dict(label="payload size", log=True, fmt="bytes")),
    dict(id="batch-topics", section="dispatch",
         title="Batch-amortized write-by-path — by topic count",
         cond="inproc-path-batch · 64 B · fan-out 1 — the clock-free twin of the resolver canary",
         pat=r"^inproc-path-batch 64B/fan1/(\d+)ep",
         label=lambda m: f"{m.group(1)} topics", key=_num, log=False,
         px=dict(label="topic count", log=True, fmt="count")),
    dict(id="mt", section="dispatch", title="Multi-thread scaling — by thread count",
         cond="64 B · fan-out 1 — ns/delivery per worker count (lower = better scaling)",
         pat=r"^inproc-mt(\d+) 64B/fan1/\d+ep",
         label=lambda m: f"mt{m.group(1)}", key=_num, log=False,
         px=dict(label="worker threads", log=True, fmt="count")),
    dict(id="eptype", section="dispatch", title="Endpoint-type family",
         cond="eptype-* · 64 B · fan-out 1 · 1 topic",
         # The `(?<!-batch)` is load-bearing, not defensive clutter: without it a future
         # `eptype-lean-batch` row would land here labelled as a THIRD endpoint type
         # rather than as the same type measured with a different instrument (#553).
         pat=r"^eptype-([\w-]+)(?<!-batch) 64B/fan1/1ep",
         label=lambda m: f"eptype-{m.group(1)}", key=lambda m: m.group(1), log=False,),
    # Re-pointed from `fold-n*` to `fold-b*`: the old rows timed ONE ~11 ns op between two
    # clock reads, so every width published p50=30 and this chart was four identical flat
    # lines. The batch-amortized `fold-b*` rows resolve the widths (~1/2/3/6 ns), so the
    # chart shows the fold-width story it was drawn for. A re-pointed family charts a NEW
    # series set with no history — the old `fold-n*` series stop here rather than being
    # silently continued under a name whose meaning changed.
    dict(id="fold", section="routing", title="Fold family — by width",
         cond="fold-b* · 512 B · fan-out 1 — batch-amortized ns/delivery per fold width",
         pat=r"^fold-b(\d+) 512B/fan1/1ep",
         label=lambda m: f"fold n{m.group(1)}", key=_num, log=False,
         px=dict(label="fold width n", log=True, fmt="count")),
    dict(id="acl", section="dispatch", title="ACL-inherit family",
         cond="acl-inherit depth 4 · 64 B — single-thread vs mt4",
         pat=r"^(acl-inherit-d4(?:-mt4)?) 64B/fan\d+/\d+ep",
         label=lambda m: m.group(1), key=lambda m: m.group(1), log=False,),
    # The per-vertex probes: LIVE usable-size bytes a resident object holds, as opposed
    # to the transient per-op churn in `lat-heap`. These are the series the memory
    # chapter's prose argues from (the vertex diet, #361, and the ADR-0058 borrowed
    # app-field erratum that took a leaf from 592 to 392 B) — they were recorded for
    # months and charted nowhere, so the argument had no picture under it.
    dict(id="mem-vertex-bytes", section="memory", suite="latency",
         title="Resident bytes per vertex — what a live object holds",
         cond="allocator probe · LIVE usable-size bytes, not per-op churn — a bare leaf, "
              "the increment one small LKV write adds, and both app-field installs (ADR-0058)",
         names=[("heap bytes per vertex (probe)", "bare leaf"),
                ("heap bytes per vertex_value (probe)", "+ one LKV write"),
                ("heap bytes per vertex_app5 (probe)", "+ owning 5-field table"),
                ("heap bytes per vertex_app5_static (probe)", "+ borrowed 5-field table"),
                ("heap bytes per fanout_wide (probe)", "wide fan-out publish")],
         log=True, fmt="bytes", ylabel="live bytes"),
    dict(id="mem-vertex-allocs", section="memory", suite="latency",
         title="Allocations per vertex — how many blocks that footprint costs",
         cond="allocator probe · block COUNT for the same five probes — fragmentation "
              "pressure on an MCU allocator, which the byte total alone does not show",
         names=[("heap allocs per vertex (probe)", "bare leaf"),
                ("heap allocs per vertex_value (probe)", "+ one LKV write"),
                ("heap allocs per vertex_app5 (probe)", "+ owning 5-field table"),
                ("heap allocs per vertex_app5_static (probe)", "+ borrowed 5-field table"),
                ("heap allocs per fanout_wide (probe)", "wide fan-out publish")],
         log=False, fmt="num", ylabel="allocations"),
    # Kept OUT of the two per-vertex families above on purpose. Those measure a leaf on a
    # DEFAULT graph; this measures a connection-shaped vertex (long peer-chosen name, one
    # handler) on an INJECTED one. Drawing them on shared axes would invite reading "4 vs
    # 3 blocks" as a regression when it is two different fixtures. Both series here head
    # for ZERO — that is the whole point of the chart (#551).
    dict(id="mem-seam-escape", section="memory", suite="latency",
         title="ADR-0039 seam escapes — what a runtime registration allocates OUTSIDE the resource",
         cond="allocator probe · a wire-driven `/net/<module>/<name>` registration on a graph with "
              "an injected memory_resource — blocks and live bytes that bypass it. Target: zero",
         names=[("heap allocs per reg_escape (probe)", "blocks escaping the seam"),
                ("heap bytes per reg_escape (probe)", "live bytes escaping the seam")],
         log=True, fmt="num", ylabel="value (per-series units)"),
    dict(id="mem-heap", section="memory", suite="latency", title="Heap & memory footprint",
         cond="allocator probe (allocs / bytes per hop) + whole-run max RSS — mixed units, log axis",
         names=[("heap allocs per forward (probe)", "allocs/forward"),
                ("heap allocs per terminus (probe)", "allocs/terminus"),
                ("heap bytes per forward (probe)", "bytes/forward"),
                ("heap bytes per terminus (probe)", "bytes/terminus"),
                ("bench_libtracer max RSS", "max RSS (KB)")],
         log=True, fmt="num", ylabel="value (per-series units)"),
    # -- throughput suite ---------------------------------------------------
    dict(id="deliver-fan", section="dispatch",
         title="Deliver-only (propagate) — by fan-out",
         cond="inproc-deliver · 64 B · 1 topic — the value is stored once and each op only "
              "delivers (Zenoh `put` semantics)",
         pat=r"^inproc-deliver 64B/fan(\d+)/1ep",
         label=lambda m: f"fan {m.group(1)}", key=_num, log=True,
         px=dict(label="fan-out (subscribers)", log=True, fmt="count")),
    dict(id="target-fan", section="dispatch",
         title="Path-target dispatch — by fan-out",
         cond="inproc-target-* · 64 B · 1 topic — edges carrying a `target_key` (the wire "
              "SUBSCRIBER form) rather than a callback; `stored` lands in the target's LKV, "
              "`handler` in its on_write. Read against the `inproc` fan-out chart, which is "
              "the callback leg.",
         pat=r"^inproc-target-(\w+) 64B/fan(\d+)/1ep",
         label=lambda m: f"{m.group(1)} fan {m.group(2)}",
         key=lambda m: f"{m.group(1)}-{int(m.group(2)):05d}", log=True,),
    dict(id="pool-payload", section="dispatch",
         title="Pooled value backend — by payload size",
         cond="inproc-pool · fan-out 1 · 1 topic — the value backend is a `sync_pool_t` "
              "(ADR-0060) instead of the default heap",
         pat=r"^inproc-pool (\d+)B/fan1/1ep",
         label=lambda m: f"pool {m.group(1)} B", key=_num, log=False,
         px=dict(label="payload size", log=True, fmt="bytes")),
    dict(id="pool-borrow-payload", section="dispatch",
         title="Pooled loaned path — by payload size",
         cond="inproc-pool-borrow · fan-out 1 · 1 topic — pooled backend on the borrowed view",
         pat=r"^inproc-pool-borrow (\d+)B/fan1/1ep",
         label=lambda m: f"pool-borrow {m.group(1)} B", key=_num, log=False,
         px=dict(label="payload size", log=True, fmt="bytes")),
    # -- routing / wire (latency suite) -------------------------------------
    # The two demux arms are separate families rather than one chart: `fixed` sweeps
    # fan(N)/1ep and `scan` sweeps fan(N)/Nep, so a single family would need two
    # different patterns, and each arm's parameter sweep is what carries the story
    # (flat for `fixed`, rising for `scan`).
    dict(id="demux-fixed", section="routing",
         title="Forward demux — target registered FIRST",
         cond="fwd-demux-fixed · one line per registry size — the size-independent part of a hop",
         pat=r"^fwd-demux-fixed 79B/fan(\d+)/1ep",
         label=lambda m: f"{m.group(1)} links", key=_num, log=False,
         px=dict(label="registered links", log=True, fmt="count")),
    dict(id="demux-scan", section="routing",
         title="Forward demux — target registered LAST",
         cond="fwd-demux-scan · one line per registry size — the lookup walks the whole table, "
              "so the rise over the chart above is the scan's marginal cost",
         pat=r"^fwd-demux-scan 79B/fan(\d+)/\d+ep",
         label=lambda m: f"{m.group(1)} links", key=_num, log=False,
         px=dict(label="registered links", log=True, fmt="count")),
    dict(id="compact-terminus", section="routing",
         title="COMPACT terminus — by payload size",
         cond="compact-terminus · a framed COMPACT delivery resolved at its terminus",
         pat=r"^compact-terminus (\d+)B/fan1/1ep",
         label=lambda m: f"terminus {m.group(1)} B", key=_num, log=False,
         px=dict(label="payload size", log=True, fmt="bytes")),
    dict(id="compact-forward", section="routing",
         title="COMPACT forward hop — by payload size",
         cond="compact-forward · the zero-allocation forward path (ADR-0038 §3) — flat with "
              "payload is the property this chart exists to show",
         pat=r"^compact-forward (\d+)B/fan1/1ep",
         label=lambda m: f"forward {m.group(1)} B", key=_num, log=False,
         px=dict(label="payload size", log=True, fmt="bytes")),
    dict(id="path-parse", section="routing",
         title="`path_t::parse` — by segment count",
         cond="path-parse · one line per segment count — the address parse every by-path "
              "write pays before it reaches the registry",
         pat=r"^path-parse \d+B/fan(\d+)/1ep",
         label=lambda m: f"{m.group(1)} segments", key=_num, log=False,
         px=dict(label="segments", log=True, fmt="count")),
    dict(id="lkv", section="memory",
         title="LKV publish — pooled vs heap value backend",
         cond="lkv-{alloc,store}-{heap,pool} · fan-out 1 · 1 topic — `alloc` isolates the "
              "value allocation, `store` the full publish",
         pat=r"^lkv-(alloc|store)-(heap|pool) (\d+)B/fan1/1ep",
         label=lambda m: f"{m.group(1)} {m.group(2)} {m.group(3)} B",
         key=lambda m: f"{m.group(1)} {m.group(2)} {int(m.group(3)):06d}", log=False,),
    dict(id="alloc-mt", section="memory",
         title="Allocator under contention — pooled vs heap, by thread count",
         cond="{pool,heap}alloc-mt* · 64 B — alloc+free of one value block per op, every "
              "thread instrumented. This is the chart behind the ADR-0060 erratum: the pool "
              "does not stay ahead of the heap as threads are added.",
         pat=r"^(pool|heap)alloc-mt(\d+) 64B/fan1/1ep",
         label=lambda m: f"{m.group(1)} mt{m.group(2)}",
         key=lambda m: f"{m.group(1)} {int(m.group(2)):03d}", log=False,),]


# ---------------------------------------------------------------------------
# release-tag resolution (directive: mark releases on graph history)
# ---------------------------------------------------------------------------
def _release_tags() -> list[tuple[str, str]]:
    """@brief All ``v*`` tags as (name, peeled commit sha), version-sorted oldest first
    (so the newest release is always last — lexicographic name order would misplace
    ``v0.10.0`` before ``v0.2.0``); [] when git/tags absent."""
    try:
        p = subprocess.run(
            ["git", "for-each-ref", "refs/tags/v*", "--sort=version:refname",
             "--format=%(refname:short) %(objectname) %(*objectname)"],
            capture_output=True, text=True, cwd=REPO, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return []
    tags = []
    for ln in p.stdout.splitlines():
        f = ln.split()
        if len(f) >= 2:
            tags.append((f[0], f[2] if len(f) >= 3 else f[1]))  # peeled sha for annotated tags
    return tags


def _is_ancestor(a: str, b: str) -> bool:
    try:
        return subprocess.run(["git", "merge-base", "--is-ancestor", a, b],
                              capture_output=True, cwd=REPO, timeout=30).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


# ---------------------------------------------------------------------------
# instrument-change markers (directive: "create markers if test has been changed")
# ---------------------------------------------------------------------------
# A release marker says "the CODE changed here". These say "the INSTRUMENT changed here",
# which is a different and more urgent statement: points either side of one are not
# comparable, because the thing doing the measuring is not the same thing.
#
# This is not hypothetical. Within one week this repo found a bench whose filler link names
# were rejected on length before ever reaching the comparison it claimed to time; a history
# emitter that silently recorded nothing; a latency column quantized so coarsely that a 6%
# improvement read as zero; and fold rows that published one constant for four different
# widths. Every one of those changed a published number without changing the code under
# test, and nothing on the chart said so.
#
# `core/**` is deliberately NOT a source here. A core change moving the line is the SIGNAL
# the chart exists to show. Mixing the two would make the marker mean "something happened",
# which is worth nothing. Presentation-only files (gen_results_page.py, render_*.py, docs/**)
# are excluded for the same reason inverted: they cannot move a number.
INSTRUMENT_SOURCES: list[tuple[str, list[str]]] = [
    (r"^(inproc|inproc-borrow|inproc-path|inproc-deliver|inproc-pool|inproc-pool-borrow"
     r"|inproc-target-\w+"
     r"|inproc-mt\d+|eptype-[\w-]+|fold-b\d+|acl-\S+|mixed|path-parse|lkv-\S+"
     r"|poolalloc-mt\d+|heapalloc-mt\d+)\b", ["bench/bench_libtracer.cpp"]),
    (r"^zenoh ", ["bench/bench_zenoh.cpp"]),
    (r"^fwd-demux-", ["bench/bench_forward_demux.cpp"]),
    (r"^compact-", ["bench/bench_compact_delivery.cpp"]),
    (r"^heap (allocs|bytes) per ", ["bench/bench_forward_heap.cpp"]),
    (r"max RSS$", ["bench/bench_libtracer.cpp"]),
]
# Shared harness: a change here can move EVERY series, so it marks all of them.
HARNESS_SOURCES = ["bench/bench_common.hpp", "bench/CMakeLists.txt",
                   "bench/perf_emit_benchmark.py", ".github/workflows/perf.yml"]


def _touching_commits(path: str, first: str, last: str) -> list[str]:
    """@brief Commits in (first, last] that touched @p path, newest first.

    `--first-parent` is MANDATORY, not an optimization. gh-pages records the merge commits
    on main, so a plain `git log` returns the topic-branch commits that are not recorded
    points and the markers would land nowhere. Measured on this repo for
    bench_libtracer.cpp: plain log = 1 of 20 are merges; --first-parent = 17 of 20.
    """
    try:
        p = subprocess.run(["git", "log", "--first-parent", "--format=%H",
                            f"{first}..{last}", "--", path],
                           capture_output=True, text=True, cwd=REPO, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return []
    return [ln.strip() for ln in p.stdout.splitlines() if ln.strip()]


def _substantive(sha: str, path: str) -> bool:
    """@brief True if @p sha changed @p path's CODE, not just comments/whitespace.

    A comment-only edit cannot move a number, and marking it would train readers to ignore
    the markers — which is the failure mode that makes an alerting mechanism worthless. On
    any error we return True: over-marking is a visible annotation, under-marking is a
    silent discontinuity, and the whole point is to stop those.
    """
    try:
        par = subprocess.run(["git", "rev-parse", f"{sha}^1"], capture_output=True,
                             text=True, cwd=REPO, timeout=30).stdout.strip()
        if not par:
            return True
        before = subprocess.run(["git", "show", f"{par}:{path}"], capture_output=True,
                                text=True, cwd=REPO, timeout=30).stdout
        after = subprocess.run(["git", "show", f"{sha}:{path}"], capture_output=True,
                               text=True, cwd=REPO, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return True
    return _strip_noise(before, path) != _strip_noise(after, path)


def _strip_noise(text: str, path: str) -> str:
    """@brief Drop comments and collapse whitespace, so formatting is not a 'change'."""
    if path.endswith((".cpp", ".hpp")):
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
    else:
        text = re.sub(r"(?m)^\s*#[^\n]*", "", text)
    return re.sub(r"\s+", " ", text).strip()


def instrument_annotations(series_names: list[str], entries: list[dict]) -> list[dict]:
    """@brief Commits where the INSTRUMENT behind @p series_names changed.

    Returned in the same shape as @ref release_annotations so the renderer draws both from
    one code path: ``{"i": <index>, "label": str, "approx": bool}``. A marked commit that is
    not itself a recorded point resolves to the nearest FOLLOWING one, exactly as a release
    tag does — the discontinuity is real either way and the reader needs to see it somewhere.
    """
    shas = [e.get("commit", {}).get("id", "") for e in entries]
    if len(shas) < 2:
        return []
    paths: set[str] = set(HARNESS_SOURCES)
    for pat, srcs in INSTRUMENT_SOURCES:
        if any(re.match(pat, n) for n in series_names):
            paths.update(srcs)
    hits: dict[int, set[str]] = {}
    for path in sorted(paths):
        for sha in _touching_commits(path, shas[0], shas[-1]):
            if not _substantive(sha, path):
                continue
            idx = next((i for i, s in enumerate(shas) if s == sha), None)
            approx = False
            if idx is None:
                idx = next((i for i, s in enumerate(shas) if s and _is_ancestor(sha, s)), None)
                approx = True
            if idx is not None:
                hits.setdefault(idx, set()).add((pathlib.Path(path).name, approx)[0])
    return [{"i": i, "label": "instrument: " + ", ".join(sorted(names)), "approx": False}
            for i, names in sorted(hits.items())]


def release_annotations(entries: list[dict]) -> list[dict]:
    """@brief Map each ``v*`` tag onto this suite's recorded-commit axis.

    Exact when the tag's commit is itself a recorded point; else ``approx``
    (drawn with '≈') at the nearest FOLLOWING recorded commit — the first entry
    that has the tag commit as an ancestor. A tag reachable from no recorded
    commit (or unresolvable shas, e.g. a shallow clone) is silently skipped.

    At most ONE marker per index. Every tag older than a store's first recorded point
    resolves approximately to that same first point, so a store that started recently
    stacks its whole release history on entry 0 — the bench-local store opened after
    v0.7.0 and drew five overprinted '≈' labels on its first commit, which is unreadable
    and reads as five events. Where several land together the NEWEST wins (tags come out
    of `_release_tags` in version order, oldest first, so the newest is the last one
    to land on an index), and an exact hit always beats an approximate one: the older
    releases at that index are strictly implied by it, and neither was measured on
    that point anyway.
    """
    shas = [e.get("commit", {}).get("id", "") for e in entries]
    at: dict[int, dict] = {}
    for name, sha in _release_tags():
        idx, approx = None, False
        for i, s in enumerate(shas):
            if s == sha:
                idx = i
                break
        if idx is None:
            for i, s in enumerate(shas):
                if s and _is_ancestor(sha, s):
                    idx, approx = i, True
                    break
        if idx is None:
            continue
        prev = at.get(idx)
        if prev is None or prev["approx"] or not approx:
            at[idx] = {"i": idx, "label": name, "approx": approx}
    return [at[i] for i in sorted(at)]


# ---------------------------------------------------------------------------
# family assembly
# ---------------------------------------------------------------------------
def _suite_key(suite_name: str) -> str:
    if "latency" in suite_name:
        return "latency"
    if "throughput" in suite_name:
        return "throughput"
    return suite_name


def _series_by_name(entries: list[dict]) -> dict[str, list[list[float]]]:
    """@brief name -> [[entry_idx, value], ...] (sparse; a series may start late)."""
    out: dict[str, list[list[float]]] = {}
    for i, e in enumerate(entries):
        for b in e.get("benches", []):
            try:
                out.setdefault(b["name"], []).append([i, float(b["value"])])
            except (KeyError, TypeError, ValueError):
                continue
    return out


def _first_line(msg: str, limit: int = 72) -> str:
    ln = (msg or "").splitlines()[0] if msg else ""
    ln = ln.replace("<", "&lt;").replace(">", "&gt;")
    return ln[: limit - 1] + "…" if len(ln) > limit else ln


def _family_sources(samples: list[str]) -> list[str]:
    """@brief The bench source a family's series come from, as a repo-relative path.

    Resolved through INSTRUMENT_SOURCES rather than a second per-family field. That
    table already maps series-name patterns to the files that produce them — it is what
    puts the "instrument changed" markers on these charts — so a separate `src=` per
    family would be a duplicate free to disagree with it. One list, two uses.

    Takes REAL matched series names, never the family's own pattern: two families begin
    with a regex construct rather than a literal (`^(pool|heap)alloc-mt`, and an
    `eptype-` pattern carrying a negative lookbehind), so pattern-probing silently
    resolved 20 of 22 and would have quietly dropped the link on exactly the two
    families whose names are hardest to guess from the chart.

    Returns EVERY distinct source, not the first: the libtracer-vs-Zenoh family draws two
    engines from two different harnesses, and naming only one would credit a chart to code
    that produced half of it.
    """
    out: list[str] = []
    for sample in samples:
        for pat, srcs in INSTRUMENT_SOURCES:
            if re.match(pat, sample) and srcs[0] not in out:
                out.append(srcs[0])
                break
    return out


def _host_meta(entries: list[dict]) -> dict:
    """@brief The host descriptor a store's points were measured on, if it records one.

    benchmark-action carries a free-form `extra` string per bench. The bench-local
    store puts the HOST descriptor there (`studio · pinned cpu2 · <cpu> · …`) on every
    point, because that store's whole claim is "same silicon every point" and a reader
    must be able to check it. The hosted store records no `extra` at all, so this
    returns {} there and the tooltip gains nothing.

    Emitted as ONE string per suite when every point agrees (`host`), and only as a
    per-entry list (`hosts`) when they do not — a per-POINT copy would repeat a ~110-char
    descriptor 368 times per entry and dominate the blob for zero added information.
    """
    def esc(s: str) -> str:
        # The descriptor is free-form store text rendered into the tooltip's innerHTML —
        # same escaping the commit subject already gets in _first_line.
        return s.replace("<", "&lt;").replace(">", "&gt;")

    per_entry: list[str] = []
    for e in entries:
        vals = {esc(b["extra"]) for b in e.get("benches", []) if b.get("extra")}
        # More than one descriptor in one entry is not a fault to hide: the runner
        # changed mid-run, and the reader should see both rather than one of them.
        per_entry.append(" / ".join(sorted(vals)))
    if not any(per_entry):
        return {}
    uniq = {h for h in per_entry if h}
    if len(uniq) == 1:
        return {"host": next(iter(uniq))}
    return {"hosts": per_entry}


def build(data: dict, colors: dict[str, int] | None = None) -> dict:
    """@brief Assemble the chart payload perf_history.js draws.

    Returns {"suites": {key: {shas, msgs, releases}}, "charts": [...]} — each
    chart carries its family's series as sparse [entry_idx, value] point lists,
    a global per-label color index (same label == same color on every chart),
    and, for numeric-parameter families, the parameter axis meta + per-series
    numeric parameter value (pv) that unlocks the sweep/heatmap/3D views.

    `colors` is the label -> color-index map, passed IN so two stores charted on the
    same page (the hosted store and the bench-local one, switched by the page's source
    selector) agree on it. Built independently, "fan 8" would be one color before the
    switch and another after it, and the switch would read as a data change.
    """
    suites: dict[str, dict] = {}
    suite_series: dict[str, dict[str, list[list[float]]]] = {}
    for suite_name, entries in data.get("entries", {}).items():
        if not entries:
            continue
        k = _suite_key(suite_name)
        suites[k] = {
            "shas": [e.get("commit", {}).get("id", "")[:7] for e in entries],
            "msgs": [_first_line(e.get("commit", {}).get("message", "")) for e in entries],
            "releases": release_annotations(entries),
            # Markers for commits where the INSTRUMENT changed. Computed per suite over
            # the series that suite actually carries, so a bench_forward_demux edit does
            # not annotate the in-process charts. See instrument_annotations.
            "instruments": instrument_annotations(
                sorted({b.get("name", "") for e in entries for b in e.get("benches", [])}),
                entries),
            **_host_meta(entries),
        }
        suite_series[k] = _series_by_name(entries)

    if colors is None:
        colors = {}
    charts: list[dict] = []
    seen: list[str] = []  # one real series name per family, for source resolution

    def collect(fam: dict, names: dict, pat: str | None) -> list[dict]:
        """@brief The family's series within one metric's suite, or [] if it has none there.

        Returns fewer than two entries as [] on purpose: a single line is not a
        comparison, and a card offering a metric that draws one line is worse than a
        card that does not offer it.
        """
        picked: list[tuple] = []  # (sort_key, label, pv, pts)
        if "names" in fam:  # explicit fixed list (heap/memory probes, not point-swept)
            for i, (name, label) in enumerate(fam["names"]):
                if name in names:
                    seen.append(name)
                    picked.append((i, label, None, names[name]))
        else:
            for name in names:
                m = re.match(pat, name)
                if not m:
                    continue
                seen.append(name)
                key = fam["key"](m)
                pv = key if isinstance(key, float) else None
                picked.append((key, fam["label"](m), pv, names[name]))
        picked.sort(key=lambda t: (t[0],) if not isinstance(t[0], str) else (float("inf"), t[0]))
        if len(picked) < fam.get("min_series", 2):
            return []
        out = []
        for _, label, pv, pts in picked:
            # Color is global BY LABEL across every chart and every metric, so "fan 8"
            # is one color everywhere — switching metric must not reshuffle the legend.
            ci = colors.setdefault(label, len(colors))
            s = {"label": label, "ci": ci, "pts": pts}
            if pv is not None:
                s["pv"] = pv
            out.append(s)
        return out

    for fam in FAMILIES:
        variants: list[dict] = []
        del seen[:]
        if "names" in fam:
            # A fixed-name probe family names whole series itself, so there is no metric
            # suffix to vary — it carries exactly the one it declares.
            series = collect(fam, suite_series.get(fam["suite"], {}), None)
            if series:
                variants.append({"name": fam.get("metric", fam["ylabel"]), "suite": fam["suite"],
                                 "fmt": fam["fmt"], "ylabel": fam["ylabel"], "series": series})
        else:
            for met in METRICS:
                series = collect(fam, suite_series.get(met["suite"], {}),
                                 fam["pat"] + " " + met["name"] + "$")
                if series:
                    variants.append({"name": met["name"], "suite": met["suite"],
                                     "fmt": met["fmt"], "ylabel": met["ylabel"],
                                     "blurb": met["blurb"], "series": series})
        if not variants:
            continue
        first = variants[0]
        src = _family_sources(seen)
        chart = {"id": fam["id"], "section": fam.get("section", "other"),
                 "title": fam["title"], "cond": fam["cond"], "log": fam["log"],
                 "src": src or None,
                 "metrics": variants,
                 # The active-metric fields are mirrored at the top level so a renderer
                 # that never switches metric reads exactly what it read before.
                 "suite": first["suite"], "fmt": first["fmt"], "ylabel": first["ylabel"],
                 "series": first["series"]}
        if "px" in fam and all(all("pv" in s for s in v["series"]) for v in variants):
            chart["px"] = fam["px"]
        charts.append(chart)
    return {"suites": suites, "charts": charts}


# ---------------------------------------------------------------------------
# emission
# ---------------------------------------------------------------------------
def _assets() -> tuple[str, str]:
    """@brief The committed chart CSS + JS, read at generate time to be INLINED
    (same reasoning as render_compare._assets: performance.md renders one
    directory deep, so a relative _static href would 404; inlining is
    path-independent and keeps the page self-contained — no CDN)."""
    static = REPO / "docs" / "_static"
    return (static / "perf_history.css").read_text(), (static / "perf_history.js").read_text()


def _source_selector(local_ok: bool, why: str) -> str:
    """@brief The two-way store selector that heads every chart block.

    The page charts TWO stores that answer different questions — the GitHub-hosted
    series (portability envelope, best-across-three-runners) and the bench-local series
    (absolute trend, one pinned host). They must never be drawn on the same axes, so the
    control is a selector rather than an overlay: one store at a time, switched in place.

    `hosted` is the default for continuity — it is the store every existing reader and
    every existing link has been reading. When the bench-local store is unreachable at
    generate time (a fork with no `gh-pages`, an offline build) the second button renders
    DISABLED and carries `why` as its title, rather than vanishing: a control that
    silently disappears reads as "this page has one store", which is false.
    """
    dis = "" if local_ok else f' disabled title="{why}"'
    return (
        '<div class="ph-srcsel" role="group" aria-label="benchmark data source">'
        '<span class="ph-srclab">source</span>'
        '<button type="button" class="ph-srcbtn on" data-src="hosted"'
        ' title="GitHub-hosted runners, best of three per point — a portability envelope">'
        'GitHub-hosted</button>'
        f'<button type="button" class="ph-srcbtn" data-src="local"{dis}'
        ' title="one pinned self-hosted CPU, same silicon every point — the absolute-trend instrument">'
        'bench-local</button>'
        '</div>')


def html_blocks(data: dict, local: dict | None = None) -> dict[str, str]:
    """@brief One MyST raw-html chart block per SECTION, keyed by section name.

    The page places each block under the chapter it belongs to, so a chart sits
    next to the prose that explains it instead of all thirty-odd landing in one
    undifferentiated grid. Every block is the SAME renderer with the SAME payload
    schema — that uniformity is the point: a reader who learns to read one chart
    can read all of them.

    Two things must stay whole across the split:

    - the **color index** is global (assigned in `build`), so "fan 8" is one
      color on every chart in every section;
    - the **suite metadata** (commit shas, release markers, instrument markers)
      is repeated into each block, because a block must be independently
      renderable — a section whose axis data was emitted elsewhere would draw
      no axes at all.

    These blocks carry NO assets. The renderer is emitted once by
    `assets_block()`, which the page emits unconditionally — because these blocks
    are conditional (no store, malformed store, no chartable family all yield
    `{}`) and the comparison block is not, so hanging the only copy of the engine
    off one of them left the comparison charts undrawn whenever the store was
    unreachable. An offline build, a fork PR with no `gh-pages`, and a malformed
    store are all documented-normal conditions, so that is not a corner.

    `local` is the SECOND store (bench-local, one pinned self-hosted CPU). Both payloads
    are embedded in every block and the page's source selector switches between them
    client-side, because the alternative \u2014 refetching on switch \u2014 would need a network
    call from a page whose whole design is to be self-contained. Both are built through
    the same `build()` with a SHARED color map, so switching store keeps every legend
    color put. A section the second store has no chart for still gets an (empty) payload,
    so the selector can say "no bench-local data for this chapter yet" instead of drawing
    an empty axis.
    """
    colors: dict[str, int] = {}
    payload = build(data, colors) if data else {"suites": {}, "charts": []}
    lpayload = build(local, colors) if local else None
    if not payload["charts"] and not (lpayload and lpayload["charts"]):
        return {}
    # Section ORDER follows the hosted store (the default view, and the store whose
    # families the surrounding prose was written against); a section only the
    # bench-local store carries is appended rather than dropped.
    sections = list(dict.fromkeys([c["section"] for c in payload["charts"]]
                                  + [c["section"] for c in (lpayload or {"charts": []})["charts"]]))
    out: dict[str, str] = {}
    for n, sec in enumerate(sections):
        charts = [c for c in payload["charts"] if c["section"] == sec]
        blob = json.dumps({"suites": payload["suites"], "charts": charts},
                          separators=(",", ":"))
        if lpayload is None:
            lblob = "null"
        else:
            lcharts = [c for c in lpayload["charts"] if c["section"] == sec]
            lblob = json.dumps({"suites": lpayload["suites"], "charts": lcharts},
                               separators=(",", ":"))
        # Count every metric variant, not just the active one: the card carries all
        # four, and reporting only the default understates the block by ~4x.
        nser = sum(len(v["series"]) for c in charts for v in c["metrics"])
        sel = _source_selector(
            lpayload is not None,
            "the bench-local store was not reachable when this page was generated")
        out[sec] = f""":::{{raw}} html
<div class="ph-hist ph-sourced">
  {sel}
  <p class="ph-note"><span class="ph-count">{len(charts)} family charts \u00b7 {nser} series</span> \u00b7 x-axis = recorded
  <code>main</code> commits (oldest \u2192 newest) \u00b7 \U0001f3f7 dashed verticals mark release
  tags (<b>\u2248</b> = tag commit itself is not a recorded point; marker sits at the nearest
  following recorded commit) \u00b7 \U0001f527 dotted verticals mark commits where the BENCH
  changed \u2014 points either side of one are not comparable. Each card carries every METRIC that point\n  recorded \u2014 <b>p50</b> / <b>p99</b> / <b>ns per delivery</b> / <b>throughput</b> \u2014 pick one under the title. Families with a numeric parameter\n  axis also offer <b>trend</b> / <b>sweep</b> / <b>heatmap</b> / <b>3D</b> views \u2014 same data,\n  three axes (commit \u00d7 parameter \u00d7 value). Hover any chart for exact per-commit values.</p>
  <div class="ph-grid ph-charts"></div>
  <script type="application/json" class="ph-data">{blob}</script>
  <script type="application/json" class="ph-data-local">{lblob}</script>
</div>
:::"""
    return out


def assets_block() -> str:
    """@brief The chart CSS + JS, inlined once for the whole page.

    Emit this UNCONDITIONALLY and exactly once. It is separate from the chart
    blocks because those are conditional and the comparison block is not: when
    the history store is unreachable there are no history blocks, and if the
    engine rode along with one of them the comparison charts would silently
    render as an empty grid.

    Inlined rather than linked because `docs/conf.py` registers neither file in
    `html_css_files` / `html_js_files`, and `performance.md` renders one
    directory deep, so a relative `_static/` href would 404. Placement in the
    document does not matter: `perf_history.js` defers its bootstrap to
    `DOMContentLoaded` when the document is still parsing.
    """
    css, js = _assets()
    return f""":::{{raw}} html
<style>{css}</style>
<script>{js}</script>
:::"""
