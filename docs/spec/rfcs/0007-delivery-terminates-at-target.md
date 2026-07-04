<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0007 — SUBSCRIBER delivery terminates at the target: no automatic re-dispatch to the target's subscribers

| Field | Value |
| ---- | ---- |
| **RFC** | 0007 |
| **Title** | SUBSCRIBER delivery terminates at the target: no automatic re-dispatch to the target's subscribers |
| **Status** | **accepted** (2026-07-04, maintainer-ratified design grill) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-04 |
| **Comment window** | waived by the maintainer (solo-maintainer project, GOVERNANCE.md window dead ceremony) |
| **Tracking issue** | [#197](https://github.com/avatarsd-llc/libtracer/issues/197) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |

## Summary

A **SUBSCRIBER delivery** into its target vertex T performs exactly the **target-local** effects of a write — the store per T's role (overwrite for stored-value, append for stream), T's write-ACL gate, `await` readiness wake, and T's local reaction (handler/callback) — and **MUST NOT re-dispatch to T's own `:subscribers[]`**. Whether, when, and with what value T's subscribers are notified is exclusively the decision of **the logic behind T**: a controller reads its input ports and writes its output ports on its own execution; a handler re-emits when it chooses; a plain stored-value vertex simply holds the value.

One `write()` therefore propagates exactly **one hop plus upward bubbling** ([RFC-0005](0005-subtree-subscriptions.md) — strictly toward the root, bounded by tree height). Dispatch-level subscription cycles are **impossible by construction**, and no depth cap, hop counter, or deduplication mechanism exists in the delivery plane — matching the FWD plane, which is already loop-free by construction ([ADR-0040](../../adr/0040-net-plane-is-explicit-source-routed-only.md)).

## Motivation

1. **The runtime must not police user designs with synthetic limits.** The previous implementation cascaded deliveries as recursive writes, which made subscription cycles possible and required an arbitrary depth cap (32) that silently truncated deep legitimate chains. Both the cycle hazard and the cap dissolve when delivery terminates at the target. Detecting suspicious wiring (feedback loops, dead chains) is **design-time analyzer/reconciler tooling**, not runtime enforcement.
2. **It matches the wiring model.** Edges deliver producer → consumer; **controllers mediate propagation** (the create-then-bind patch-cable model of [ADR-0017](../../adr/0017-in-band-vertex-creation-controller-orchestration.md)). Automatic transitive relay through plain vertices was never designed-for — it was an implementation accident of delivery-as-recursive-write.
3. **The target-perspective claim is preserved and sharpened.** "Delivery is an ordinary write, indistinguishable from a direct write" holds **at the target** (subscription-unaware: store, ACL, readiness identical). This RFC pins what was previously unspecified: the *continuation* of propagation is not part of delivery.

## Normative statement

For a write W to vertex V:

1. W stores at V and notifies V's covered subscriptions (V's own and, per RFC-0005, each ancestor's), each notification delivering the written TLV as-is to that subscription's target (or callback).
2. Each delivery into a target T applies the target-local write effects at T (store, ACL, `await` wake, local reaction). It **MUST NOT** trigger notification of T's `:subscribers[]`, nor bubbling from T.
3. Any subsequent notification of T's subscribers occurs only via a new write issued by the logic behind T (or any other writer), which is a new instance of (1).

## Compatibility and migration

Interop-visible: implementations must agree whether chains relay — this RFC pins **no**. A wiring `A→B, B:subscribers→C` no longer relays A's writes to C. Migration: subscribe C to A directly (a subtree subscription covers whole subtrees with one edge), or make B a controller/handler whose logic re-emits. Since v1 is unreleased and the reference implementation's cascade was undocumented behavior, no deployed peers rely on transitive relay.

Implementation record: [ADR-0051](../../adr/0051-delivery-terminates-at-target-no-dispatch-limits.md). Reference docs (02/04) are aligned in the docs batch; CONTEXT.md §SUBSCRIBER and §Cycle termination are updated with this RFC.
