# ROUTER cycle termination is guaranteed by `hop_count`; the dedup recent-set is a bounded best-effort optimization

Status: accepted — **superseded by [ADR-0040](0040-net-plane-is-explicit-source-routed-only.md) / [ADR-0051](0051-delivery-terminates-at-target-no-dispatch-limits.md)**

> **Erratum (2026-08-02) — this decision's mechanism no longer exists; read [ADR-0040](0040-net-plane-is-explicit-source-routed-only.md) instead.**
> ADR-0040 retired `bridge_t` and the ROUTER-flood mechanism outright: the net plane is
> explicit source-routed `FWD`, a forwarding node strips its own segment and passes the rest,
> and the result is loop-free by construction. `fwd_router_t` therefore has **no visited-set and
> no hop counter** (ADR-0040 erratum), and [ADR-0051](0051-delivery-terminates-at-target-no-dispatch-limits.md)
> removes the in-process dispatch cap on the same grounds. On the wire,
> [reference/05](../reference/05-protocol-tlvs.md) §`0x0D` — normative by incorporation via
> [spec/v1.md](../spec/v1.md) §3 — makes `0x0D` ROUTER *"a reserved, decodable codepoint with no
> implemented mechanism"* and says senders **MUST NOT** emit it. Every `hop_count` / `MAX_HOPS`
> clause below (including the conformance MUST and the reference-impl status note) is dead: no
> such field is carried, honoured, or emitted anywhere. The text is left as written rather than
> edited, because the record of why the recent-set was demoted is what makes ADR-0040's later
> "no dedup state at all" step legible.

A bridge has two independent loop defenses ([05-protocol-tlvs.md](../reference/05-protocol-tlvs.md) §`0x0D` ROUTER, [07-host-embedding.md](../reference/07-host-embedding.md) §cycle handling): a **recent-set** of seen `(origin_peer_id, origin_timestamp)` pairs (already-seen TLVs dropped silently), and a **`hop_count`** incremented by each bridge with a hard `MAX_HOPS` cap (recommended 32) beyond which a bridge MUST drop the TLV and emit a local error.

**`hop_count`/`MAX_HOPS` is the normative termination guarantee.** The recent-set is therefore a **bounded, evictable best-effort optimization** — time-windowed and size-capped — that suppresses *most* redundant forwarding early. A node MAY run an arbitrarily small (even size-zero) recent-set without compromising termination; the only cost of eviction is **bounded duplicate delivery** — at most `MAX_HOPS` × fanout copies of a message before `hop_count` stops the loop — which pub/sub subscribers already tolerate (idempotent handling or app-level dedup).

## Considered options

- **Make the recent-set the primary guarantee and require it be sized `deepest_fanout × longest_window`** (the prior `05:614` recommendation). Rejected: that size is unbounded in principle and hostile to the ≤ 16 KB Cortex-M target, and it still cannot *guarantee* termination if an entry is evicted under memory pressure. Leaning on `hop_count` for the guarantee turns the recent-set's memory into a free tuning knob.

## Consequences

- **Conformance:** a bridge MUST honor `MAX_HOPS` (termination); maintaining a recent-set is SHOULD (optimization).
- **Reference-impl status:** the `hop_count >= MAX_HOPS` drop **and** the `STATUS=ERROR(NESTING_TOO_DEEP)` local-error emission are implemented — the latter via `bridge_t::set_status_path(...)` ([#77](https://github.com/avatarsd-llc/libtracer/issues/77); see [reference/07](../reference/07-host-embedding.md) §cycle handling step 4).
- Embedded bridges can bound dedup memory hard; the trade is **bounded, transient duplicate delivery**, never a loop.
- `05` §cycle handling and `07` §cycle handling are reworded to state which mechanism is load-bearing.
- The recent-set's eviction policy (time-window + size-cap; LRU or ring) is implementation-defined ([ADR-0013](0013-v1-scope-boundaries.md)) — only its *bounded best-effort* role is normative.
