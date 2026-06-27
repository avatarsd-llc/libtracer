# Continuous cross-core performance + conformance matrix: ranged over many axes, on vector data, baseline-tracked, auto-published

Status: accepted (the testing/benchmarking strategy). Extends [ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md)
(native cores kept consistent by conformance vectors) from *correctness-only* to
*correctness + performance*, and from *curated points* to a *ranged response surface*.
Implementation lands incrementally (see the linked issues); the first slice — two-core
cross-match + a perf floor gate, baseline-tracked across builds — is in CI now (#94).

## Context

ADR-0028 made the conformance gate verify that every native core decodes/encodes the
shared vectors **identically** (a DISAGREE fails CI). Two needs push it further:

1. **Performance is a first-class property**, not a footnote — the whole thesis (µs latency,
   zero-copy, [ADR-0031](0031-direct-browser-to-robot-binding-and-webtransport.md)) is a
   *speed* claim. A correctness gate that ignores latency/throughput lets a 10× regression
   ship green. The `bench/` harness already measures speed+latency across a parameter sweep;
   it should gate continuously, not be a manual snapshot (`bench/RESULTS.md`).
2. **The interesting behavior is multi-dimensional and on real wire shapes.** A single
   number hides where a core is fast or slow. The honest artifact is a **response surface**
   over many axes, measured on **vector data** (structured TLVs — SETTINGS, SUBSCRIBER,
   ROUTER, nested — not synthetic scalars), per core implementation.

## Decision

**Build one continuous cross-core matrix that gates both correctness and performance,
ranged over the axes below, on vector data, with the baseline tracked across builds and the
results auto-published.**

**Axes (the ranged dimensions):**
- **core-impl-lang** — C++, TypeScript (now), Rust (later). The cross-core dimension.
- **n-cores** — host CPU parallelism (dispatch/fan-out scaling across threads).
- **ep-size** — payload size per endpoint (1 B … 8 KiB+).
- **ep-count** — number of endpoints / topics.
- **ep-type** — the dispatch class per vertex: **stream** (history/append), **lean** (the
  minimal sink vertex — the lightweight input port, ADR-0026-style), **lean-cached** (the
  zero-alloc loaned/`out_cache` read). *(Naming provisional — these map to the bench modes
  `inproc` / `inproc-borrow` / `inproc-path` and `role_t`; to be finalized when implemented.)*
- **n-layer-folded** — module composition depth across L0–L5 (e.g. `mem_heap`+`view_basic`
  vs pooled vs borrowed vs rope/scatter): how many layers fold into the hot path.
- **n-routers** — bridge/ROUTER hop count (cross-node fan, bounded by `MAX_HOPS`).

**On vector data:** the workload is the conformance vectors (real structured TLVs), so the
surface measures *protocol* cost on *real* shapes, and the perf map and the correctness map
share the same inputs — one matrix, two metrics (agree? + how fast?).

**Coverage target = 100%:** every type-code × `opt`-bit (and, for perf, every axis cell that
is meaningful) is exercised, tracked by `coverage_audit.py`. The remaining type codes
(ERROR 0x08, ACL 0x0A, SPEC 0x0E) are spec-decision-blocked (#82 etc.) — 100% is the target,
reached as those unblock, not faked.

**Baseline tracked across builds:** the perf reference persists across CI runs (Actions cache
now, #94; a committed/Pages-stored history as the matrix grows) so regression is measured
against history, not just absolute floors. Refreshed from `main`, gated on PRs.

**Auto-published results:** the matrix + surfaces publish to GitHub Pages from CI (replacing
the static `RESULTS.md` snapshot), so the live response surface is the artifact — alongside
the existing docs site.

## Considered options

- **Keep correctness-only conformance + manual `RESULTS.md`.** Rejected: lets perf regress
  silently and lets the published numbers go stale.
- **A single perf number / one fixed config.** Rejected: hides the response surface; the
  axes are exactly where the design's behavior lives.
- **Synthetic scalar workload.** Rejected: must be **vector data** — real structured TLVs —
  so the perf and correctness maps share inputs and measure real protocol cost.

## Consequences

- The conformance gate becomes a **perf+correctness matrix**; the two-core cross-match + a
  perf floor, baseline-tracked, is live (#94). The full ranged surface + per-core perf
  (incl. a TS perf bench for the lang axis) + Pages publishing land incrementally.
- Adding a core (Rust) or a dimension (n-routers, n-layer-folded) is "add a harness/axis,"
  not a redesign — same shape as ADR-0028's "add a core."
- Honest by construction: 100% is a tracked target with named spec blockers, not a claim;
  perf floors/baselines catch real regressions without flapping on jitter.

## Relates

- [ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md) — the cross-core conformance gate this extends to perf + ranges.
- [ADR-0031](0031-direct-browser-to-robot-binding-and-webtransport.md) — the µs-latency thesis that makes perf a gate.
- tests/conformance/ (run-all.py, coverage_audit.py, harnesses.json), bench/ (the sweep + perf_gate.py).
- Tracking issues: ranged cross-core perf matrix; auto-publish results to Pages; push conformance coverage to 100% (#60); npm subpackages monorepo.
