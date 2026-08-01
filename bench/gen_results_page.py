#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
"""Generate docs/performance.md from LIVE harness output — so the published page is
auto-generated test+perf results, not a hand-maintained snapshot (ADR-0032 §auto-published).

Runs the cross-core conformance driver (run-all.py), the in-process perf bench
(bench_libtracer), the cross-core codec benches (cpp-core bench_codec + ts-core
perf.mjs + rust-core `cargo run --example perf`), and the coverage audit, and
renders a MyST page: the cross-match matrix, the per-family trend charts (one chart
idiom throughout, distributed into the chapter each family belongs to), the
cross-core codec comparison, and the zenoh comparison. CI regenerates this in-place before
sphinx-build (docs.yml); the committed copy is the last snapshot. Each codec
runner degrades gracefully if its toolchain is missing (a note, not a crash).

  python3 bench/gen_results_page.py            # write docs/performance.md
Env: LIBTRACER_CXX_HARNESS (conformance_runner path); falls back to build dirs.
Stdlib only.
"""
from __future__ import annotations

import dataclasses
import datetime
import os
import pathlib
import re
import shutil
import statistics
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import render_compare  # noqa: E402  (sibling module in bench/)
import render_history  # noqa: E402  (sibling module in bench/)

REPO = pathlib.Path(__file__).resolve().parent.parent
OUT = REPO / "docs" / "performance.md"
BENCH = REPO / "bench" / "build" / "bench_libtracer"
BENCH_ZENOH = REPO / "bench" / "build" / "bench_zenoh"
CODEC = REPO / "bench" / "build" / "bench_codec"
VECTORS = REPO / "tests" / "conformance" / "vectors" / "v1"
CXX = os.environ.get("LIBTRACER_CXX_HARNESS") or str(REPO / "build" / "tests" / "conformance_runner")



def _missing(binary: str, hint: str) -> str:
    """A bench binary is absent: degrade locally, FAIL the published build.

    A placeholder is fine in a local preview. On the live site it is not: the prose around
    these blocks asserts measured figures ("87 ns", "-36%"), so a reader meets an assertion
    with a "not built" note where its evidence should be and reasonably reads the assertion
    as measured. That is exactly what §2b did — `bench_forward_demux` was never in the docs
    workflow's build target list, so the published page carried a placeholder under live
    numbers. `LIBTRACER_DOCS_STRICT=1` (set only in docs.yml) turns that into a build
    failure, so the page can never again publish prose whose evidence is missing.
    """
    if os.environ.get("LIBTRACER_DOCS_STRICT") == "1":
        sys.exit(f"gen_results_page: {binary} not built, and LIBTRACER_DOCS_STRICT=1. "
                 f"Build it ({hint}) or drop the section that cites it.")
    return f"_({binary} not built — `{hint}`)_"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, cwd=REPO, **kw).stdout


def cross_core_block() -> str:
    env = dict(os.environ, LIBTRACER_CXX_HARNESS=CXX)
    out = subprocess.run([sys.executable, "tests/conformance/run-all.py"],
                         capture_output=True, text=True, cwd=REPO, env=env).stdout
    # Pull the per-core summary + the gate verdict for a compact table.
    summary = [ln.strip() for ln in out.splitlines()
               if re.match(r"\s*(cpp|ts|rust):", ln) or ln.startswith(("Type codes:", "Opt bits:", "COVERAGE:", "CONFORMANCE:"))]
    body = "\n".join(f"- {s}" for s in summary) or "- (driver produced no summary)"
    return body, ("CONFORMANCE: PASS" in out)


# ---------------------------------------------------------------------------
# the page's own chapter list — numbering and cross-references are DERIVED
# ---------------------------------------------------------------------------
# Chapter numbers used to be typed into the heading AND into every sentence that
# referenced one ("§4's counted bytes", "see #8-raw-data-provenance"). They drifted:
# the surface table pointed at §1-§6 while the chapters it described were §5-§10, and
# the raw-browser link in `charts_for` pointed at an anchor no heading produced. One
# ordered list, numbered here, is what stops that.
CHAPTERS: tuple[tuple[str, str], ...] = (
    ("metrics", "What the metrics mean"),
    ("gates", "What actually stops a regression"),
    ("noise", "Noise, variance, and what one number is worth"),
    ("surfaces", "The measurement surfaces, and the instruments behind them"),
    ("conformance", "Cross-core conformance (every native core must agree byte-for-byte)"),
    ("dispatch", "Dispatch — in-process latency & throughput"),
    ("routing", "Wire & routing — what a framed hop costs"),
    ("memory", "Memory & allocation"),
    ("zenoh", "libtracer vs Zenoh — measured, absolute"),
    ("codec", "Cross-core codec performance (decode→encode roundtrip, same v1 vectors)"),
    ("tests", "Test rollup (live ctest, unified with the perf surface)"),
    ("repro", "Reproducing this page locally"),
    ("raw", "Raw data & provenance"),
)
_CH_NUM = {cid: n for n, (cid, _) in enumerate(CHAPTERS, start=1)}
_CH_TITLE = dict(CHAPTERS)


def ch(cid: str) -> str:
    """@brief A chapter cross-reference (`§7`), numbered from CHAPTERS."""
    return f"§{_CH_NUM[cid]}"


def head(cid: str) -> str:
    """@brief A chapter's `##` heading line, numbered from CHAPTERS."""
    return f"## {_CH_NUM[cid]} · {_CH_TITLE[cid]}"


def anchor(cid: str) -> str:
    """@brief The MyST heading anchor a chapter's heading produces."""
    slug = re.sub(r"[^a-z0-9]+", "-", f"{_CH_NUM[cid]} {_CH_TITLE[cid]}".lower())
    return "#" + slug.strip("-")


# ---------------------------------------------------------------------------
# the instrument registry — every table on this page is derived from it
# ---------------------------------------------------------------------------
@dataclasses.dataclass(frozen=True)
class surface_t:
    """@brief One measurement surface: the unit of comparability on this page."""

    id: str
    title: str
    measures: str
    rule: str
    extra: tuple[str, ...] = ()     # harnesses that are not a `bench/bench_*.cpp`
    extra_at: tuple[str, ...] = ()  # chapters those extra harnesses appear in


SURFACES: tuple[surface_t, ...] = (
    surface_t("conformance", "Cross-core conformance", "byte-exactness across cores, not speed",
              "any DISAGREE fails CI", ("tests/conformance/run-all.py",), ("conformance",)),
    surface_t("inproc", "In-process timing", "per-operation latency and throughput in one process",
              "gated per PR and per `main` push, same-runner"),
    surface_t("framed", "Framed-hop timing", "what one wire frame costs, before and after the payload",
              "batch-amortized against the host clock, self-calibrating"),
    surface_t("counted", "Allocation counting", "exact allocations and bytes around ONE operation — counted, never timed",
              "forward hop hard-gated at zero; per-vertex block count ratcheted exactly"),
    surface_t("net", "Network timing", "one-way latency over a real socket, two processes",
              "same topology for both engines; p50, the p99 tail and the p999 deep tail — "
              "published, never gated"),
    surface_t("compare", "Engine comparison", "libtracer and Zenoh side by side, absolute",
              "same runner, same pass — no ratios"),
    surface_t("codec", "Cross-core codec", "decode→encode roundtrip per implementation",
              "the same v1 vectors for every core",
              ("bindings/typescript/…/bench/perf.mjs", "bindings/rust/examples/perf.rs"),
              ("codec",)),
    surface_t("scaling", "Concurrency & scaling", "how one shared object behaves as threads are added",
              "many-core host, run on demand — not part of the published sweep"),
    surface_t("meta", "Instrument self-check", "that the harness's own estimator reproduces a distribution it was given",
              "run before trusting a published percentile; touches neither clock nor library"),
)


@dataclasses.dataclass(frozen=True)
class instrument_t:
    """@brief One harness: what it drives, what it reports, and where that lands.

    `what` is the whole description a reader gets — one or two sentences, present
    tense, no history. If a bench needs more than that to be understood, the place
    for it is the bench's own file header, an ADR, or the CHANGELOG.
    """

    src: str
    surface: str
    sections: tuple[str, ...]  # chart sections this bench feeds; () = not charted here
    what: str
    units: str
    gate: str = "—"


INSTRUMENTS: tuple[instrument_t, ...] = (
    # -- the published sweep ------------------------------------------------
    instrument_t(
        "bench_libtracer.cpp", "inproc", ("dispatch", "routing", "memory"),
        "Drives the in-process hot path — resolve a vertex, write a value, notify, deliver — "
        "swept over fan-out, payload size, topic count, thread count, endpoint type, value "
        "backend and dispatch mode. Every `<mode>` row has a `<mode>-batch` twin timed over a "
        "calibrated batch instead of one operation at a time. Its `fold-*`, `lkv-*` and "
        "`*alloc-mt*` rows are what the routing and memory chapters chart.",
        "ns p50 / p99 / mean · deliveries/s",
        "per-PR + per-push gate, six canonical points"),
    instrument_t(
        "bench_forward_demux.cpp", "framed", ("routing",),
        "Drives one FWD forward hop through the inbound link's own receiver against a registry "
        "of N links, with the target child registered first (`fixed`) or last (`scan`). It "
        "never resolves a vertex, so it measures routing alone.",
        "ns p50, batch-amortized",
        "recorded per `main` push"),
    instrument_t(
        "bench_compact_delivery.cpp", "framed", ("routing",),
        "Drives the Nth `COMPACT` frame on an already-advertised binding — the steady state of "
        "an established flow — in both its forms: the label resolves locally (`terminus`) or "
        "swaps and re-emits downstream (`forward`).",
        "ns p50 · allocations per frame",
        "recorded per `main` push"),
    instrument_t(
        "bench_forward_heap.cpp", "counted", ("memory",),
        "Replaces the global allocator with a counting wrapper and arms it around exactly one "
        "operation, so every number is an exact count rather than a sample. Eight probes: the "
        "forward hop, a terminus resolve, five resident-vertex shapes, and a wire-driven "
        "registration's escape from the injected memory seam.",
        "allocations · live usable-size bytes",
        "forward hop = 0 allocations; per-vertex bytes +2%, block counts exact"),
    instrument_t(
        "bench_codec.cpp", "codec", ("codec",),
        "Decodes and re-encodes every shared v1 conformance vector through the C++ core — the "
        "`lang` axis of the cross-core codec surface, run identically by the TypeScript and "
        "Rust cores over the same inputs.",
        "ns per roundtrip · roundtrips/s",
        "—"),
    instrument_t(
        "bench_zenoh.cpp", "compare", ("zenoh",),
        "The Zenoh side of the in-process comparison: intra-session peer pub/sub over zenoh-c, "
        "sweeping the same fan-out / payload / endpoint matrix and emitting the same RESULT rows.",
        "ns p50 / p99 · deliveries/s",
        "—"),
    instrument_t(
        "bench_transports.cpp", "net", ("zenoh",),
        "libtracer's network latency: a publisher and a subscriber process on one host, one "
        "transport each (UDP / TCP / WebSocket), payload carrying a send timestamp so one-way "
        "latency is valid across the two processes. Every paced probe is kept, so the same run "
        "yields the deep tail and the sample count that backs it.",
        "ns p50 / p99 / p999 / max, one-way · samples per point",
        "—"),
    instrument_t(
        "bench_zenoh_net.cpp", "net", ("zenoh",),
        "The Zenoh side of the same two-process network measurement, over a configured UDP "
        "endpoint with multicast scouting disabled so the pair talks only over that socket. It "
        "shares the subscriber-side accumulator with the libtracer arm, so both engines' tails "
        "are estimated by the same code from the same number of samples.",
        "ns p50 / p99 / p999 / max, one-way · samples per point",
        "—"),
    # -- the wire plane, run on demand --------------------------------------
    instrument_t(
        "bench_forward_rope.cpp", "framed", (),
        "Forwards a frame that arrives as a multi-link rope, where TLV headers straddle link "
        "boundaries and every peek is a walk with stitching, swept over link count at a fixed "
        "frame and registry size.",
        "ns p50 · allocations per hop"),
    instrument_t(
        "bench_terminus_tier.cpp", "framed", (),
        "Resolves the same frame through both reader tiers — the eager arena reader and the "
        "lazy rope reader — plus a flatten-then-arena arm, swept over frame size and link count.",
        "ns p50 · allocations per resolve"),
    instrument_t(
        "bench_originate.cpp", "framed", (),
        "Drives the node that *starts* a remote operation: no inbound frame to read an address "
        "out of, so it encodes the `dst` and `src` PATHs from scratch, measured against the "
        "minted-label form of the same operation.",
        "ns p50 · wire bytes per frame"),
    instrument_t(
        "bench_hop_chain.cpp", "framed", (),
        "Five nodes and four hops: a full-path address against a minted label, and the cold "
        "first operation against the warm steady state, recording the frame bytes each hop "
        "actually carries so the address collapse is asserted rather than assumed.",
        "ns p50 per hop · wire bytes per hop"),
    instrument_t(
        "bench_mount_descent.cpp", "framed", (),
        "Times the mount descent's width loop — key widths `k = W..1`, each pass walking the "
        "whole child table — against a single-pass alternative, swept over registry size and "
        "widest registered mount.",
        "ns p50 · slot visits per descent"),
    instrument_t(
        "bench_transport_iov.cpp", "counted", (),
        "Assembles the real transports' `::iovec` table at rising span counts to find the width "
        "at which it stops fitting inline and allocates.",
        "allocations · bytes, per span count"),
    instrument_t(
        "bench_iov_spill_cost.cpp", "counted", (),
        "Censuses the egress span counts the rope forward arm actually hands a transport, then "
        "times the gather at the spill width against an inline control pair at the same +1 delta.",
        "spans per frame · ns p50"),
    instrument_t(
        "bench_wire_heap.cpp", "counted", (),
        "Drives real loopback TCP/UDP/WS sockets with thread-local allocation counters, so one "
        "frame's egress cost on the sending thread and its ingress cost on the transport's "
        "receive thread are attributed separately.",
        "allocations · bytes, per frame per direction"),
    instrument_t(
        "bench_conn_ram.cpp", "counted", (),
        "Stands up a real server transport and drives K raw client peers at it, reading the live "
        "heap balance when the server is up and quiesced, at K established connections, and "
        "after teardown.",
        "live bytes per link · live bytes per connection"),
    instrument_t(
        "bench_failable_census.cpp", "counted", (),
        "Counts the blocks each peer-driven control-plane operation draws from the injected "
        "resource against those that escape to the global heap, and A/Bs the two growable-array "
        "guard shapes a nothrow migration chooses between.",
        "blocks per operation · ns per growth"),
    instrument_t(
        "bench_tcp_fanin.cpp", "net", (),
        "Raises the number of simultaneous TCP peers against the server's single poll thread and "
        "counts frames delivered, to find where that thread saturates before the host does.",
        "frames/s, per peer count"),
    instrument_t(
        "bench_storage_policy.cpp", "counted", (),
        "Times the two RFC-0022 storage knobs on the paths that read them — the policy read at "
        "graph depth 1 and depth 8 (the only shape that separates one inline load from an "
        "ancestor walk), the STREAM store, and the plain write — beside an invariant control "
        "leg that the change cannot move, so an A/B against another build reports the change "
        "and not the machine.",
        "ns per operation, per leg"),
    instrument_t(
        "bench_qos_census.cpp", "counted", (),
        "Counts, per vertex shape, whether an extension block is allocated at all and whether "
        "the storage policy it holds is byte-identical to the default — including the leaf that "
        "materialises a block only because it INHERITS an ancestor's override (RFC-0022 §3.C), "
        "which is the whole of what an override costs in RAM. Classifies by comparing the "
        "address returned by `settings()` against `kDefaultSettings`, so it needs no accessor "
        "of its own.",
        "vertices per bucket"),
    instrument_t(
        "bench_tcp_peer_scaling.cpp", "net", (),
        "Separates the single poll thread's three costs by moving one axis at a time: an "
        "`idle-fanout` arm holds the load at one active sender and raises the number of "
        "connected-but-silent peers, so any loss is descriptor scanning alone, and an "
        "`active-fanout` arm raises active peers below the core count to price the thread "
        "itself. Reports each point as a median with min/max and declines to state a ratio "
        "when the sweep's ends overlap.",
        "frames/s + ns p50 / p99, per peer count"),
    instrument_t(
        "bench_tcp_baseline.cpp", "net", (),
        "The same fan-in topology with timestamped payloads, on a fresh server per sweep point, "
        "so aggregate throughput and the full one-way latency distribution come from one run.",
        "frames/s · ns p50 / p99 / p999 / max"),
    # -- concurrency & scaling, run locally on a many-core host -------------
    instrument_t(
        "bench_fanout_clone_storm.cpp", "scaling", (),
        "T threads clone and release one shared segment view — the per-subscriber delivery "
        "primitive, with T standing in for fan-out width — as T scales 1 → 128.",
        "clone+release/s, aggregate and per thread"),
    instrument_t(
        "bench_await_wakeup_storm.cpp", "scaling", (),
        "One writer storms writes at a hot vertex while W threads each loop on `await` for it, "
        "as W scales 1 → 128.",
        "writes/s · wakeups/s"),
    instrument_t(
        "bench_route_handle_contention.cpp", "scaling", (),
        "T threads hammer `ensure_egress` reuse-reads on one already-advertised `(link, route)` "
        "flow — the steady-state read every remote delivery takes — as T scales 1 → 128.",
        "ops/s, aggregate and per thread"),
    instrument_t(
        "bench_rx_source_topology.cpp", "scaling", (),
        "T receive threads forward rope frames with the RX block source shared across all "
        "children, one pool shared, or one pool per child.",
        "frames/s · ns p50, per thread count"),
    instrument_t(
        "bench_lkv_slot.cpp", "scaling", (),
        "Concurrent publishers and readers on one last-known-value slot across five reclamation "
        "arms, plus `graph_t::write` driven from T threads against distinct vertices that share "
        "nothing but a lock stripe.",
        "ops/s · ns p50, per thread count"),
    instrument_t(
        "bench_contention.cpp", "scaling", (),
        "Measures the machine rather than libtracer: nine arms isolating a thread-private "
        "counter, a shared read, contended read-modify-writes, false sharing and lock costs, so "
        "a claim about a shared line can be checked against the box in front of you.",
        "ns per op · ops/s, per thread count"),
    # -- the instrument that measures an instrument -------------------------
    instrument_t(
        "bench_tail_validate.cpp", "meta", (),
        "Feeds the shared latency accumulator distributions whose quantiles are known by "
        "construction and prints measured against analytic. It touches neither the clock nor "
        "libtracer, so a failure is a defect in the estimator and nowhere else.",
        "quantile ns, measured vs analytic"),
)


@dataclasses.dataclass(frozen=True)
class probe_t:
    """@brief One armed window of the allocation-counting instrument."""

    id: str
    what: str
    gate: str


PROBES: tuple[probe_t, ...] = (
    probe_t("forward", "one FWD forward hop, from arrival to egress",
            "**zero** allocations, every CI run (ADR-0038 §16KB-RAM)"),
    probe_t("terminus", "one terminus resolve, which may allocate (ADR-0041)", "report-only"),
    probe_t("vertex", "a bare default leaf vertex at rest", "bytes +2% · blocks exact"),
    probe_t("vertex_value", "the same leaf plus the increment one small LKV write adds",
            "bytes +2% · blocks exact"),
    probe_t("vertex_app5", "a leaf carrying an owning five-field app descriptor table (ADR-0058)",
            "bytes +2% · blocks exact"),
    probe_t("vertex_app5_static", "the same table installed borrowed, its slots viewing caller flash",
            "bytes +2% · blocks exact"),
    probe_t("fanout_wide", "one publish at a large subscriber count", "report-only"),
    probe_t("reg_escape",
            "a wire-driven `/net/<module>/<name>` registration on a graph with a memory resource "
            "injected — what the resource never saw. Target: zero (ADR-0065)",
            "report-only"),
)


def _sources() -> set[str]:
    """@brief Every bench translation unit on disk, the registry's join key."""
    return {p.name for p in (REPO / "bench").glob("bench_*.cpp")}


def check_registry() -> None:
    """@brief Fail the build on a registry that has drifted from the tree.

    Three ways a hand-maintained list goes stale, and all three are checked here
    rather than noticed later: a bench lands with no description, a description
    outlives the bench it describes, or a chart section is drawn on this page with
    no instrument claiming it. Failing the docs build is the point — a new bench
    cannot reach the page without a sentence saying what it does.
    """
    on_disk, described = _sources(), {i.src for i in INSTRUMENTS}
    if missing := sorted(on_disk - described):
        raise SystemExit("gen_results_page: bench(es) with no INSTRUMENTS entry — add one "
                         f"sentence saying what each drives and reports: {', '.join(missing)}")
    if extra := sorted(described - on_disk):
        raise SystemExit(f"gen_results_page: INSTRUMENTS describes deleted bench(es): {', '.join(extra)}")
    known = {s.id for s in SURFACES}
    if bad := sorted({i.surface for i in INSTRUMENTS} - known):
        raise SystemExit(f"gen_results_page: unknown surface(s) in INSTRUMENTS: {', '.join(bad)}")
    charted = {f.get("section", "other") for f in render_history.FAMILIES}
    claimed = {s for i in INSTRUMENTS for s in i.sections}
    if orphan := sorted(charted - claimed):
        raise SystemExit("gen_results_page: chart section(s) with no instrument behind them: "
                         f"{', '.join(orphan)}")
    if ghost := sorted(claimed - set(_CH_NUM)):
        raise SystemExit(f"gen_results_page: instrument(s) claim a chapter that does not exist: {', '.join(ghost)}")
    probed = {m.group(1) for f in render_history.FAMILIES
              for name, _ in f.get("names", [])
              if (m := re.match(r"heap (?:allocs|bytes) per (\w+) \(probe\)", name))}
    if gap := sorted(probed - {p.id for p in PROBES}):
        raise SystemExit(f"gen_results_page: charted probe(s) with no PROBES entry: {', '.join(gap)}")


def surfaces_table() -> str:
    """@brief The comparability table. Harness and chapter columns are JOINED from
    INSTRUMENTS, so a surface can never claim a bench that no longer feeds it."""
    rows = ["| surface | what it measures | harness | where it appears | discipline |",
            "| --- | --- | --- | --- | --- |"]
    for s in SURFACES:
        members = [i for i in INSTRUMENTS if i.surface == s.id]
        harness = ", ".join([f"`{n}`" for n in s.extra] + [f"`{i.src[:-4]}`" for i in members])
        where = [ch(c) for c in dict.fromkeys(s.extra_at + tuple(c for i in members for c in i.sections))]
        if any(not i.sections for i in members):
            where.append("on demand")
        rows.append(f"| {s.title} | {s.measures} | {harness} | {', '.join(where) or 'on demand'} "
                    f"| {s.rule} |")
    return "\n".join(rows)


def lands(i: instrument_t) -> str:
    """@brief Where one instrument's numbers actually end up — chapters, gate, or nowhere.

    Derived from the same two fields the rest of the page is built from, so it cannot
    disagree with the charts: `sections` is where the numbers are drawn, `gate` is
    whether anything at all watches them between builds. An instrument with neither
    produces numbers that live only in the terminal of whoever last ran it — a real
    state, worth naming on the page rather than hiding. Some of those are deliberate
    (the many-core scaling arms have no shared-runner equivalent) and some are a
    standing gap; the surface column is what tells them apart.

    This deliberately does NOT try to classify `gate` as "gates" versus "records". That
    field is free prose describing a discipline, and a keyword sniff over it gets the
    answer wrong — `bench_forward_heap`'s "forward hop = 0 allocations" is one of the
    hardest gates in the tree and contains no such keyword. The exact discipline is
    already printed verbatim in the same row's **Gate:** clause, so this column answers
    only the question the row cannot otherwise answer: does anything watch it at all.
    """
    where = [ch(c) for c in i.sections]
    if i.gate != "—":
        where.append("watched")
    return ", ".join(where) if where else "**neither** — on demand only"


def instruments_table(section: str | None = None) -> str:
    """@brief What each harness does, in one or two sentences. `section` filters to a chapter.

    The full table carries a "where it lands" column and the per-chapter ones do not:
    inside a chapter every listed instrument lands in that chapter by construction, so
    the column would repeat the heading. In the full table it is the only place a
    reader can see that a bench exists whose numbers reach no chart and no gate.
    """
    picked = [i for i in INSTRUMENTS if section is None or section in i.sections]
    full = section is None
    head_row = "| harness | what it does | reports |"
    rows = [head_row + (" where it lands |" if full else ""),
            "| --- | --- | --- |" + (" --- |" if full else "")]
    for i in picked:
        gate = "" if i.gate == "—" else f" **Gate:** {i.gate}."
        rows.append(f"| [`{i.src[:-4]}`](https://github.com/avatarsd-llc/libtracer/blob/main/"
                    f"bench/{i.src}) | {i.what}{gate} | {i.units} |"
                    + (f" {lands(i)} |" if full else ""))
    return "\n".join(rows)


def unpublished_note() -> str:
    """@brief A counted, derived statement of the gap — not a hand-kept list.

    Every previous attempt to keep a list of "benches that are not published" went
    stale within a release, because nothing failed when it did. This one is computed
    from INSTRUMENTS at render time, so it is correct by construction or the docs build
    is broken. `check_registry` already guarantees the registry covers the tree.
    """
    orphans = [i for i in INSTRUMENTS if not i.sections and i.gate == "—"]
    if not orphans:
        return ("Every harness in the table above reaches a chart or a gate — there is no"
                " bench whose numbers land nowhere.")
    by_surface: dict[str, list[str]] = {}
    for i in orphans:
        by_surface.setdefault(i.surface, []).append(f"`{i.src[:-4]}`")
    parts = "; ".join(f"**{dict((s.id, s.title) for s in SURFACES)[k]}** — {', '.join(v)}"
                      for k, v in by_surface.items())
    return (f"**{len(orphans)} of {len(INSTRUMENTS)} harnesses reach neither a chart on this"
            f" page nor a gate**, by surface: {parts}. They are run on demand and read in a"
            " terminal. That is a deliberate state for the concurrency arms — they need a"
            " many-core host and a shared CI runner cannot produce a number worth recording"
            " — and a standing gap for the rest. A measurement that is neither published nor"
            " gated decays into a number nobody can check, so this count is derived here"
            " rather than kept in a list that could quietly go stale.")


def probes_table() -> str:
    """@brief The armed windows of the allocation-counting instrument, and what ratchets."""
    rows = ["| probe | what is armed around | gate |", "| --- | --- | --- |"]
    rows += [f"| `{p.id}` | {p.what} | {p.gate} |" for p in PROBES]
    return "\n".join(rows)


ROUTING_NOTES = """\
### Reading the two demux arms

`fixed` registers the target child first, so its lookup hits on the first compare and the
chart isolates the size-independent part of a hop. `scan` registers it last, so the lookup
walks the whole table. The rise of `scan` over `fixed` at the same registry size **is** the
scan's marginal cost. Read them as a pair; neither means anything alone.

Per-module keying does not narrow that scan — it changes the key, not the container, and a
node's links overwhelmingly sit in one module. It earns its place by keeping two modules'
same-named connections distinct, which is correctness, not lookup time (ADR-0061).

### What the scan costs a real frame

| path | registry work per frame |
| --- | --- |
| plain `FWD` write | mount descent + inbound lookup, **per frame** — the frame carries the full path, so there is nothing to cache against |
| `COMPACT` on a bound label | one dereference of the cached registry slot, **first frame only** — resolved once, then memoized |
| remote delivery to a subscriber | one lookup by link name, **per frame** — the subscriber record holds a name |

A binding holds the resolved target rather than a name, and both cached forms self-invalidate
without a callback: the terminus compares a retirement generation, and the forwarding hop
reads the registry slot, whose `link` teardown nulls in place — so a departed link reads
`nullptr`, the same clean miss an unresolved lookup gives (ADR-0062, ADR-0063).

The dominant term in a hop is TLV header parsing — the FWD header, the op, the `dst` PATH and
its segments, the selector peek and the `src` PATH on rebuild, each read exactly once — and it
sits at a local optimum for that structure. At `-O3` the parse inlines with values flowing in
registers into the narrow struct's stores, so there is no intermediate object to remove:
forcing the inline, or narrowing the header struct, each *regress* the hop by about 10 %.

A `COMPACT` re-emit builds no frame: the head goes to a 12-byte stack buffer and the payload
is handed to the transport by reference as a scatter-gather list, which is why the forward
charts are flat in payload size. Zero allocations **in the router** is not zero on the wire —
a transport that does not override the gather form concatenates once in
`transport_t::send(iov)`."""


def _parse_codec(out: str) -> list[tuple[int, float, int, int]]:
    """Parse `RESULT\\t...\\tcodec\\t...` lines (12 fields) -> (size, pub_s, p50, mean)."""
    rows = []
    for ln in out.splitlines():
        f = ln.split("\t")
        if len(f) == 12 and f[0] == "RESULT" and f[2] == "codec":
            rows.append((int(f[3]), float(f[6]), int(f[9]), int(f[11])))
    return rows


def _codec_cpp() -> tuple[list | None, str | None]:
    if not CODEC.exists():
        return None, "bench not built (`cmake --build bench/build --target bench_codec`)"
    rows = _parse_codec(run([str(CODEC), str(VECTORS)]))
    return (rows, None) if rows else (None, "bench produced no codec results")


def _codec_ts() -> tuple[list | None, str | None]:
    if shutil.which("node") is None:
        return None, "node toolchain not available"
    perf = REPO / "bindings" / "typescript" / "packages" / "core" / "bench" / "perf.mjs"
    if not perf.exists():
        return None, "ts perf bench missing"
    proc = subprocess.run(["node", str(perf), str(VECTORS)],
                          capture_output=True, text=True, cwd=REPO)
    rows = _parse_codec(proc.stdout)
    return (rows, None) if rows else (None, "ts-core bench produced no results")


def _codec_rust() -> tuple[list | None, str | None]:
    if shutil.which("cargo") is None:
        return None, "cargo toolchain not available"
    proc = subprocess.run(["cargo", "run", "--release", "--quiet", "--manifest-path",
                           "bindings/rust/Cargo.toml", "--example", "perf", "--", str(VECTORS)],
                          capture_output=True, text=True, cwd=REPO)
    rows = _parse_codec(proc.stdout)
    return (rows, None) if rows else (None, "rust-core bench unavailable (cargo build/run failed)")


def codec_block() -> str:
    """Cross-core codec comparison: run all three codec benches over the v1 vectors
    and tabulate the median throughput + p50/mean latency per core. Each runner
    degrades gracefully (a note instead of a crash) if its toolchain is absent, so
    the docs build never hard-fails on a missing core."""
    cores = [("cpp-core", _codec_cpp), ("ts-core", _codec_ts), ("rust-core", _codec_rust)]
    rows = ["| core | throughput (median) | p50 latency (median) | mean (median) |",
            "| --- | --- | --- | --- |"]
    notes: list[str] = []
    any_ok = False
    for name, fn in cores:
        try:
            data, err = fn()
        except OSError as e:  # toolchain present but blew up — degrade, don't crash
            data, err = None, str(e)
        if not data:
            notes.append(f"- _{name}: unavailable — {err}_")
            continue
        any_ok = True
        pub = statistics.median(r[1] for r in data)
        p50 = statistics.median(r[2] for r in data)
        mean = statistics.median(r[3] for r in data)
        rows.append(f"| {name} | {pub / 1e6:.1f} M roundtrips/s | {int(p50)} ns | {int(mean)} ns |")
    body = "\n".join(rows)
    if not any_ok:
        body = "_(no core codec bench available in this environment — toolchains absent)_"
    if notes:
        body += "\n\n" + "\n".join(notes)
    return body


def how_to_read() -> str:
    """@brief The reader's obligations: comparability, then the one chart idiom.

    Everything derivable was removed from here rather than restated. The surface
    table below is joined from INSTRUMENTS; what stops a regression and what one
    number is worth against runner noise are chapters of their own, spliced from
    `docs/methodology.md`, and are not summarized twice.
    """
    return f"""\
## How to read this page

Every number here belongs to exactly ONE **measurement surface**, and **a value is only
comparable to values from the same surface** — surfaces use different harnesses, processes
and units. A surface is a property of the series, not of the chapter it is drawn in: a
chapter groups by subject, a subject can be answered by more than one instrument, and this
table is what settles whether two numbers may be compared at all.

{surfaces_table()}

{ch("gates")} is what stops a regression, {ch("noise")} is what one number is worth against
the runner lottery, and {ch("surfaces")} says what each harness actually drives.

### Every chart on this page is the same chart

One idiom, learned once. A **family** is a set of series answering one question, drawn as
lines on shared axes; the x-axis is recorded `main` commits, oldest to newest, except in
{ch("zenoh")} where it is the swept parameter. Each chart names its own swept variable and
pinned scenario beneath its title — which is also why two charts both titled "throughput"
are not comparable to each other: different denominator, by construction.

- 🏷 *dashed* marker = a release tag (**≈** when the tag's commit is not itself a recorded
  point). 🔧 *dotted* = **the bench changed** at that commit, so points either side were
  taken with different rulers and are not comparable. Core changes are deliberately not
  marked: a core change moving the line is the signal the chart exists to show.
- A series label keeps one color across every history chart. The {ch("zenoh")} comparison
  charts use a fixed engine palette instead — their line dimension is which engine, not
  which parameter.
- Families with a numeric parameter carry four switchable views — trend, sweep, heatmap and
  an isometric 3D surface — drawn only over the commits where **every** series of the family
  has a value, so a series that started late cannot fake a trend.
- Hover any point for its exact value, the commit and its subject line.

### Two latency series per in-process mode

Every `<mode>` row is timed one operation at a time; every `<mode>-batch` row times a
calibrated batch of the same operation and divides. The clock is a large fraction of a
sub-100 ns write, so the per-op percentiles snap to coarse steps and read high at small
fan-out, converging on the batch row as the operation outgrows the clock — the *Clock
quantization* chart in {ch("dispatch")} plots exactly that gap. Use `-batch` to resolve a
small delta; use the per-op row for **tail shape**, since it is the one with a real p99. A
percentile of batch means measures interference between batches rather than the tail of an
operation, so the `-batch` rows publish no p99 at all rather than a fabricated one."""


def compare_intro() -> str:
    """@brief The comparison chapter's lede. Fairness is the methodology section below it."""
    return f"""\
{head("zenoh")}

Both engines in **one pass on one runner**, so the numbers are directly comparable on
identical hardware: three **in-process** axes — subscriber fan-out, payload size and topic
count — and a **network latency** comparison over the real loopback kernel path, one socket
and one paced value per transport in two processes. The charts plot **absolute** throughput,
latency and bandwidth as series on shared axes; there are no speed-up ratios.

libtracer is compiled from source at `-O3`; Zenoh is the upstream prebuilt `zenoh-c 1.9.0`
release binary that `bench/fetch_zenoh.sh` downloads, so its optimization profile is
upstream's rather than a flag this repo sets. Both are optimized builds. Which rows do equal
work, why there is no network *throughput* comparison, and which transports are absent and
why, are all below."""


def raw_data() -> str:
    """@brief Where the store comes from and how to read it directly."""
    return f"""\
{head("raw")}

The charts above are one view of a persisted store. Every push to `main` runs the full bench
on **three independently-drawn runners**, archives all raw transcripts as a per-commit CI
artifact (`bench-results-<sha>`, on the `perf` workflow run), and records every
`(mode, size, fanout, endpoints)` point — latency, throughput and memory footprint as
**separate series** — to a build-to-build history on the machine-maintained `gh-pages` branch
([benchmark-action/github-action-benchmark](https://github.com/benchmark-action/github-action-benchmark)).
Per metric the recorded value is the **best across the three runners**, which approximates
the code's capability rather than the machine lottery.

The store carries roughly three times as many series as this page charts — every recorded
point, including the ones no family groups. **[Open the raw per-series trend
browser ↗](https://libtracer.avatarsd.com/dev/bench/)** — one chart per series, zoomable,
with per-point commit links. It is the archive; the families above are the reading."""


def _load_history() -> dict | None:
    """@brief Load the benchmark-action store (gh-pages `dev/bench/data.js`) as JSON.

    Source order: a local `dev/bench/data.js` (present when a caller pre-mirrored the
    branch), else a best-effort shallow fetch of `origin/gh-pages` + `git show`.
    Returns None (a note, not a crash) when the store is unreachable — e.g. a fork
    without the branch or an offline build.
    """
    import json
    raw = None
    local = REPO / "dev" / "bench" / "data.js"
    if local.exists():
        raw = local.read_text()
    else:
        subprocess.run(["git", "fetch", "--depth=1", "origin", "gh-pages"],
                       capture_output=True, cwd=REPO)
        for ref in ("FETCH_HEAD", "origin/gh-pages"):
            p = subprocess.run(["git", "show", f"{ref}:dev/bench/data.js"],
                               capture_output=True, text=True, cwd=REPO)
            if p.returncode == 0 and p.stdout:
                raw = p.stdout
                break
    if not raw:
        return None
    raw = raw.strip()
    raw = raw.removeprefix("window.BENCHMARK_DATA = ").rstrip(";")
    try:
        return json.loads(raw)
    except ValueError:
        return None


def history_sections(data: dict | None) -> tuple[dict[str, str], str]:
    """@brief The family trend charts, split into the page's chapters.

    Every chart on this page is the same kind of object: one series-family, all
    its series as lines on shared axes, release + instrument markers, and — for
    numeric-parameter families — the sweep / heatmap / 3D views. There is
    deliberately no second chart style anywhere; a reader learns the idiom once.

    Returns (section -> raw-html block, reason-this-is-empty). The reason matters:
    three different things produce no blocks and they are NOT interchangeable —
    the store was unreachable, the store was reachable but unparseable, or the
    store was read fine and simply carries no family with two or more series yet.
    Reporting all three as "unreachable" publishes a false cause, and does it on
    a page that may be showing live charts from that same store two chapters up.
    """
    if not data:
        return {}, ("the per-commit history store was not reachable in this build")
    try:
        blocks = render_history.html_blocks(data)
    except Exception as e:  # a malformed store must never break the docs build
        return {}, f"the history store could not be rendered in this build — {e}"
    return blocks, "the history store carries no chartable family for this chapter yet"


def charts_for(sections: tuple[dict[str, str], str], name: str) -> str:
    """@brief One chapter's chart block, or a note naming why it is absent."""
    blocks, reason = sections
    if name in blocks:
        return blocks[name]
    return (f"_({reason} — the [raw per-series browser]({anchor('raw')})"
            " serves the full store once published)_")


def tests_block() -> str:
    """@brief The unified test rollup: live ctest summary inline on this page,
    with the per-suite detail remaining on the Test report page."""
    try:
        import gen_test_report as tr
        results = tr.run_ctest_junit(tr.BUILD)
    except Exception:
        results = []
    if not results:
        return ("_(the Release test build was not available in this pass — see the"
                " [full test report](test-report.md))_")
    total = len(results)
    passed = sum(1 for _, s, _ in results if s == "pass")
    wall = sum(t for _, _, t in results)
    cats: dict[str, list[int]] = {}
    for name, s, _ in results:
        c = cats.setdefault(tr.category_of(name), [0, 0])
        c[0] += 1
        c[1] += 1 if s == "pass" else 0
    verdict = "✅ all green" if passed == total else f"❌ {total - passed} failing"
    rows = " · ".join(f"{k} {v[1]}/{v[0]}" for k, v in sorted(cats.items()))
    return (f"| suites | passing | wall time | verdict |\n| --- | --- | --- | --- |\n"
            f"| {total} | {passed}/{total} | {wall:.2f}s | {verdict} |\n\n"
            f"By area: {rows}. Full per-suite detail: [Test report](test-report.md).")


def zenoh_compare_block() -> str:
    """Run both grids and render the absolute-value comparison charts.

    Two absences are treated differently on purpose:

    - **Zenoh missing** is soft. `docs.yml` builds `bench_zenoh` best-effort and says so
      in its own comment: a failed vendor must never break a deploy. The chapter degrades
      to a note and libtracer's own numbers stand.
    - **libtracer's own rows missing** is hard under `LIBTRACER_DOCS_STRICT`. That means
      the sweep did not run at all, and the surrounding prose describes charts that are
      not there. CI always creates the TSV file (`|| true` in the workflow), so an EMPTY
      file is the shape this failure actually takes — checking that the path exists is
      not enough to catch it, which is why the check is on the parsed rows.
    """
    tsv = os.environ.get("LIBTRACER_COMPARE_TSV")
    combined = ""
    if tsv and pathlib.Path(tsv).exists():
        combined = pathlib.Path(tsv).read_text()  # CI runs the sweep once, shared with the PR comment
    if not combined.strip() and BENCH.exists():
        combined = run([str(BENCH), "grid"])
        if BENCH_ZENOH.exists():
            combined += "\n" + run([str(BENCH_ZENOH), "grid"])
    rows = render_compare.parse(combined)
    if not any(r["sys"] == "libtracer" for r in rows):
        return _missing("bench_libtracer (comparison sweep produced no rows)",
                        "cmake --build bench/build --target bench_libtracer")
    if not render_compare.has_zenoh(rows):
        return ("_(Zenoh not vendored in this build, so the comparison charts are omitted."
                " Run [`bench/fetch_zenoh.sh`](https://github.com/avatarsd-llc/libtracer/tree/main/bench)"
                " before the bench build to generate them; the libtracer numbers above still apply.)_")
    return render_compare.html_block(rows, provenance())


def provenance() -> str:
    """A one-line CI-generated stamp: date, commit, run, runner.

    In GitHub Actions the GITHUB_* / RUNNER_OS env vars pin exactly which deploy
    produced these numbers (so the figures are auditable per deploy, not a stale
    hand-edit). Off CI it degrades to a plain local-build note.
    """
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    if os.environ.get("GITHUB_ACTIONS") != "true":
        return f"_Generated from a local build on {date} (not a CI deploy)._"
    sha = os.environ.get("GITHUB_SHA", "")
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    run_id = os.environ.get("GITHUB_RUN_ID", "")
    runner = os.environ.get("RUNNER_OS", "")
    commit = f"[`{sha[:7]}`]({server}/{repo}/commit/{sha})" if sha and repo else "unknown commit"
    run = f"[run {run_id}]({server}/{repo}/actions/runs/{run_id})" if run_id and repo else "CI run"
    runner_note = f" · runner `{runner}`" if runner else ""
    return f"**🤖 CI-generated** on {date} · commit {commit} · {run}{runner_note}."


# ---------------------------------------------------------------------------
# methodology prose (docs/methodology.md, spliced in — not a page of its own)
# ---------------------------------------------------------------------------
METHODOLOGY = REPO / "docs" / "methodology.md"


def _methodology() -> dict[str, str]:
    """@brief Parse `docs/methodology.md` into {heading text: body}, both `##` and `###`.

    The methodology used to be a SEPARATE page, and the split had a cost the two
    pages could not see: they described the same six measurement surfaces in the
    same order, one saying what a surface measures and the other showing what it
    measured, so a reader had to hold two tabs open to read one number. Worse, the
    duplication drifted — a caveat updated on one page stayed stale on the other.

    Merging them by CONCATENATION would have kept that structure and just removed a
    click. Instead each surface's methodology is spliced next to its own results
    below, which is the same principle the chart sectioning already follows: the
    explanation sits with the thing it explains.

    The prose stays in its own Markdown file rather than moving into this
    generator's f-string, so it is still editable by anyone who can write Markdown
    and still reviewable as a diff. `docs/conf.py` drops it from `include_patterns`
    so it renders once, here, instead of twice.

    Heading levels are spliced UNCHANGED: the file's `###` subsections land under
    this page's `##` chapters, which is exactly one level down and satisfies
    `check_heading_depth`.
    """
    out: dict[str, str] = {}
    if not METHODOLOGY.exists():
        return out
    lines = METHODOLOGY.read_text().splitlines()
    cur: str | None = None
    lvl = 0
    buf: list[str] = []
    fenced = False
    for ln in lines:
        if ln.startswith("```"):
            fenced = not fenced
        if not fenced and ln.startswith("#"):
            n = len(ln) - len(ln.lstrip("#"))
            if n in (2, 3) and ln[n:n + 1] == " ":
                if cur is not None:
                    out[cur] = "\n".join(buf).strip()
                cur, lvl, buf = ln[n:].strip(), n, []
                continue
            if n == 1:  # the file's own title; the merged page supplies its own
                if cur is not None:
                    out[cur] = "\n".join(buf).strip()
                cur, buf = None, []
                continue
        if cur is not None:
            buf.append(ln)
    if cur is not None:
        out[cur] = "\n".join(buf).strip()
    return out


def mprose(sections: dict[str, str], heading: str, *, keep_heading: bool = False) -> str:
    """@brief One methodology section by heading, or a hard failure.

    Missing is an ERROR, never an empty string. A renamed heading would otherwise
    delete a paragraph from the published page silently, which is the same failure
    mode as a chart family that matches nothing — the page still builds and simply
    says less than it did. Fail the docs build instead.
    """
    if not sections:
        return ""  # file absent entirely (partial checkout) — degrade, do not crash
    if heading not in sections:
        raise SystemExit(
            f"gen_results_page: docs/methodology.md has no section {heading!r}.\n"
            f"  available: {sorted(sections)}\n"
            "  (renaming a methodology heading must be mirrored here — the splice is by name)")
    body = sections[heading]
    # `keep_heading` emits a CONSTANT subheading, never the source heading: the source
    # headings carry methodology's own chapter numbers ("2 · In-process latency"), which
    # collide with this page's numbering the moment they are spliced under it, and they
    # restate the chapter title they now sit beneath. One consistent line instead.
    return f"### How this surface is measured\n\n{body}" if keep_heading else body


def check_heading_depth(page: str) -> None:
    """@brief Fail the build on a heading hierarchy that skips a level.

    This is what shipped to the live page: a `##` section whose subsections
    were emitted at `####`, so the reader saw chapters belonging to nothing.
    It went unnoticed because each block owns its own heading level and no
    single file sees the assembled sequence — which is exactly why the
    assembled page is where the rule is enforced.

    Descending back up a level is legal and not flagged; only a downward jump
    of more than one is structurally wrong.

    Depth also has a hard ceiling here: `docs/conf.py` sets
    `myst_heading_anchors = 3`, so an `h4` gets **no anchor** and cannot be
    deep-linked. Nothing that a reader is meant to link to may sit below `###`;
    the tables under "Full history" are the only permitted `h4`, being an
    index rather than a destination.

    Fenced code blocks are skipped so a `#` comment inside one is never read as
    a heading.
    """
    depths: list[tuple[int, str]] = []
    fenced = False
    for line in page.splitlines():
        if line.startswith("```") or line.startswith(":::"):
            fenced = not fenced
            continue
        if fenced or not line.startswith("#"):
            continue
        level = len(line) - len(line.lstrip("#"))
        if level and line[level:level + 1] == " ":
            depths.append((level, line.strip()))
    prev = 0
    for level, text in depths:
        if prev and level > prev + 1:
            raise SystemExit(f"heading depth skips a level ({prev} -> {level}): {text}")
        prev = level


def main() -> int:
    check_registry()
    summary, passed = cross_core_block()
    M = _methodology()
    history = _load_history()
    charts = history_sections(history)
    assets = render_history.assets_block()
    page = f"""\
# Performance & Conformance

```{{note}}
This page is **auto-generated** from the live test + benchmark harnesses on each docs
build (`bench/gen_results_page.py`, ADR-0032) — the published response surface, not a
hand-edited snapshot. All rates and latencies are **absolute measured values**,
representative of the CI runner. Each chapter opens with the method for its own surface;
the cross-cutting rules are {ch("metrics")}–{ch("noise")}, and {ch("surfaces")} says what
every harness in `bench/` drives and reports.

{provenance()}
```

{how_to_read()}

{head("metrics")}

{mprose(M, "The metric taxonomy")}

{head("gates")}

{mprose(M, "What actually stops a regression")}

{head("noise")}

{mprose(M, "Reading the numbers (noise & variance)")}

{head("surfaces")}

{mprose(M, "The measurement surfaces")}

Every harness in [`bench/`](https://github.com/avatarsd-llc/libtracer/tree/main/bench),
including the ones whose results are not charted here — each one drives something specific
and reports it in its own units:

{instruments_table()}

{unpublished_note()}

{head("conformance")}

{mprose(M, "1 · Cross-core conformance (correctness, not speed)", keep_heading=True)}

Live driver summary:

{summary}

{head("dispatch")}

{mprose(M, "2 · In-process latency & throughput (the dispatch thesis)", keep_heading=True)}

{instruments_table("dispatch")}

{charts_for(charts, "dispatch")}

{head("routing")}

{mprose(M, "3b · Routing & delivery (the network plane, per frame)", keep_heading=True)}

{instruments_table("routing")}

{charts_for(charts, "routing")}

{ROUTING_NOTES}

{head("memory")}

{mprose(M, "3 · Memory footprint (allocations counted, not sampled)", keep_heading=True)}

{instruments_table("memory")}

Eight armed windows feed the store, each counted around exactly one operation:

{probes_table()}

Every probe reports two independent quantities and **both ratchet**: `bytes=`, the live
usable-size balance, and `allocs=`, the number of heap blocks. They ratchet on different
terms because they are different kinds of number — bytes carry a 2 % tolerance since an
allocator size-class flip is not a regression, block counts carry none.

{charts_for(charts, "memory")}

```{{note}}
**Allocation churn is not resident footprint, and this page charts both — separately.**
Resident bytes are what a live object *holds*; the heap-footprint series are the transient
churn of one operation; the timed rows are what it costs to get a block and give it back.
Neither number bounds the other: a release can take per-frame allocations to zero and move
resident bytes almost not at all. Churn also costs more on the target than these host
figures show — glibc's tcache serves a hot same-size malloc/free in tens of nanoseconds,
an MCU allocator in hundreds — so a host reading of churn is a *lower* bound.
```

{compare_intro()}

{mprose(M, "4 · libtracer vs Zenoh (absolute, one pass, same runner)")}

### Fairness in the comparison

{mprose(M, "Fairness in the Zenoh comparison")}

{zenoh_compare_block()}

{head("codec")}

{mprose(M, "5 · Cross-core codec (like-for-like across implementations)", keep_heading=True)}

{instruments_table("codec")}

{codec_block()}

{head("tests")}

{tests_block()}

{head("repro")}

{mprose(M, "Reproducing locally")}

{raw_data()}

### Provenance & auditability

{mprose(M, "Provenance & auditability")}

{assets}
"""
    check_heading_depth(page)
    OUT.write_text(page)
    print(f"wrote {OUT} ({'conformance PASS' if passed else 'conformance check ran'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
