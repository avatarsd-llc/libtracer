# ACL evaluation is a pure per-target policy over typed ACEs, with the effective-ACE merge cached graph-side and invalidated by generation

Status: accepted. Gives the ALLOW-only-MCU vs full-DENY-host split of [ADR-0020](0020-acl-nfsv4-style-aces-with-inheritance.md) its implementation seam (the `security_acl` module the code comments have promised); selected per target under the [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) appropriateness rule; maintainer-ratified 2026-07-04.

> **Amendment (2026-07-08): the cached effective merge is implemented, subtree-precise via the [ADR-0057](0057-graph-composite-vertex-tree.md) child links.** §2's cache landed as `effective_acl_t` (`security_acl.hpp` — the pure merge semantics: own ACEs first, ancestors nearest-first filtered to `INHERIT`, open-by-default over an empty merge, any-present-ACE-closes) plus a per-vertex cached merged-ACE list behind `vertex_t::with_effective_aces`. Invalidation is **subtree-precise** rather than the generation counter sketched here: a `:acl` write re-marks the written vertex's subtree dirty by walking its Composite child links (wiring-frequency), and a dirty vertex rebuilds lazily on its next check — no graph-global generation exists. The correctness note stands: only the merge is cached, never a verdict.

## Context

ACE evaluation lives inline in `graph.cpp`'s anonymous namespace (`parse_aces`, `ace_grants`, `acl_allows`) with no seam, while comments in two files defer "full DENY / ordered first-match-per-bit evaluation" to a `security_acl` host module that has nowhere to plug in. The evaluation is also mis-costed: `graph.hpp` calls the ancestor walk "control-plane frequency only," but `acl_allows` runs on **every gated read/write/await** — each check walks ancestors taking each ancestor's mutex plus a `system_clock::now()`. And ACE parsing is only reachable through hand-assembled `:acl` wire bytes: a ~30-line ACE byte-builder is duplicated across `acl_test` and `subtree_test` because no typed surface exists.

## Decision

### 1. The policy is pure and selected per target

`verdict_t policy::allows(subject, right, span<const ace_t>, now_ns)` — no graph access, no locks, no clock reads of its own. Two adapters, chosen through the target's module set per ADR-0047 §1 (the choice is per-target configuration — the MCU conformance-profile subset vs the host module — and the check runs on the data plane, so the rule grants compile-time selection):

- `allow_only_policy` — the required-modules MCU profile of ADR-0020 (ALLOW-only, single `INHERIT` flag);
- `full_acl_policy` — the `security_acl` host module (ordered first-match-per-bit, DENY, full flag set).

Typed ACE parse/build (`ace_t` ↔ wire ACE TLV) lands in the same module; the duplicated test byte-builders die, and ACE edge cases (expiry, `INHERIT`, ordering) become unit-testable without a live graph.

### 2. The graph owns the ancestor walk and caches the merge

The graph collects a vertex's **effective ACEs** (own + inherited per `INHERIT`, using the `key_view` ancestor navigation) and hands the policy the ranges. On top, the merge is **cached per vertex** with a **generation counter bumped by any `:acl` field-write**: the data-plane check becomes a generation compare plus a pure policy call — the per-operation ancestor mutex-walk leaves the data plane entirely. Correctness note that makes the cache safe: **only the merge is cached, never the verdict** — `expires_ns` is evaluated by the policy against `now` at check time, so expiry needs no invalidation; `:acl` writes are control-plane-rare, so generation bumps are cheap.

## Considered options

- **Pure policy, walk-per-check (no cache).** Rejected as the end state (kept as the landing order — the cache is a follow-up commit behind the same interface): it preserves today's data-plane cost that the review flagged.
- **Policy owns the walk (takes a vertex handle + parent accessor).** Rejected: the policy stops being pure — graph coupling, locks inside the policy, unit tests need a live graph.
- **Cache the verdict per (vertex, subject, right).** Rejected: verdict caches must invalidate on expiry and subject changes; merge caching gets the same win with none of the hazards.
- **Runtime-pluggable evaluator object.** Rejected under ADR-0047 §1: the choice is per-target configuration, not runtime dynamism; a hot-path indirect call buys nothing.

## Consequences

- Swapping ALLOW-only for full DENY evaluation is a target-configuration change, not an edit to `graph.cpp` — the seam the code comments promised now exists, with both adapters real from day one.
- The wrong "control-plane frequency" comments are corrected as part of the change.
- `acl_test`/`subtree_test` drop their byte-builders for the typed surface (the wire-level path stays covered by the existing end-to-end `:acl` write tests).
- The effective-ACL semantics of ADR-0020 (ACE shape, `INHERIT`, special subjects, first-match-per-bit) are unchanged — this ADR is only *where* evaluation lives and *when* the merge is computed.
