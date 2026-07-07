<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0003 — Concrete-path delivery for bridged wildcard subscriptions

> **Deferred (2026-07-07):** the design direction is sound, but this RFC governs *bridged* wildcard delivery, which no implementation exercises yet. Per the standing "stay 0.x DRAFT — don't lock a wire commitment before it is exercised" stance, it is **deferred** (tracked in [#303](https://github.com/avatarsd-llc/libtracer/issues/303)) rather than accepted-now or left to silently lapse. Re-open for comment when a real consumer (the strawberry-fw migration, or a second implementation) exercises the path and the three open sub-questions in [§Discussion](#discussion) are resolved.

| Field | Value |
| ---- | ---- |
| **RFC** | 0003 |
| **Title** | Concrete-path delivery for bridged wildcard subscriptions |
| **Status** | **deferred** (2026-07-07 — sound direction; awaits a bridged-delivery consumer) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-06-25 |
| **Comment window** | deferred — re-opens when bridged delivery is exercised (see banner above) |
| **Tracking issue** | [#303](https://github.com/avatarsd-llc/libtracer/issues/303) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |

## Summary

A wildcard subscriber (e.g. `/sensor/*/temp`) receives TLVs from many concrete paths and MUST be able to determine which concrete path produced each. For **local** delivery this is implementation-defined (a callback argument). For **bridged/remote** delivery there is currently no wire-defined carrier — so a second-implementer remote subscriber cannot recover the concrete path, and bridged wildcard delivery is not interoperable. This RFC pins a normative wire mechanism: the matched concrete `PATH` (`0x06`) travels with the delivered TLV inside the `ROUTER` envelope. Local delivery stays out-of-band. This applies the wildcard-delivery decision from the design-forks grill ([03-addressing.md](../../reference/03-addressing.md) §subscriber identity).

## Motivation

[03-addressing.md](../../reference/03-addressing.md) §subscriber identity requires only that "a subscriber can determine which concrete path produced each delivered TLV," leaving the mechanism implementation-defined. That is acceptable in-node (subscriber and dispatcher share an address space, so the matched path is a callback argument). It is **not** acceptable across a bridge: the remote subscriber sees only the delivered data TLV, and nothing on the wire tells it whether the producer was `/sensor/A/temp` or `/sensor/B/temp`. Two independent implementations therefore cannot interoperate on bridged wildcard subscriptions — exactly the interop the reference suite exists to guarantee. The `ROUTER` TLV ([05-protocol-tlvs.md](../../reference/05-protocol-tlvs.md) §`0x0D`) already carries bridge metadata as NAME-tagged children and is the natural carrier.

## Proposed change — **Deferred (design of record; not yet open for acceptance)**

### A. The delivery-path child
When a node fans out a write to a subscriber whose subscription `PATH` contains a wildcard (`*`, `**`, or `[*]`) **and** the delivery crosses a bridge, the `ROUTER` envelope wrapping the data TLV MUST include the **matched concrete `PATH`** (`0x06`) as a metadata child, tagged `NAME "to"` immediately preceding it, placed among `ROUTER`'s metadata children **before** the terminating `NAME "data"` + wrapped data TLV:

```
ROUTER (PL=1) {
  NAME "from"  + VALUE origin_peer_id      ; existing bridge metadata
  NAME "ts"    + TIME  origin_timestamp     ; existing
  NAME "hops"  + VALUE hop_count            ; existing
  NAME "to"    + PATH  matched_concrete     ; NEW — present iff subscription is a wildcard
  NAME "data"  + <wrapped data TLV>         ; MUST remain the last child
}
```

(NAME-tag spellings are illustrative; the normative rule is "a `NAME \"to\"` + `PATH` child carrying the matched concrete path.")

### B. Local delivery
For local (in-node) wildcard delivery the matched path MAY be passed out-of-band (callback / binding API); **no** wire encoding is required.

### C. Non-wildcard subscriptions
A concrete (non-wildcard) subscription needs no delivery-path child — the subscriber already knows the single path. The `NAME "to"` child is present **iff** the subscription `PATH` is a wildcard.

### D. Bridge preservation
A bridge that sheds and re-attaches `ROUTER` ([07-host-embedding.md](../../reference/07-host-embedding.md) §cycle handling) MUST preserve the `NAME "to"` delivery-path child end-to-end (alongside `origin_peer_id`), so the concrete path survives multi-hop delivery.

### E. Conformance
New vectors under `tests/conformance/vectors/v1/wildcard-delivery/`: (1) a `ROUTER`-wrapped delivery to a wildcard subscriber carrying the matched concrete `PATH`; (2) round-trip preservation of the `NAME "to"` child across a shed/re-attach; (3) a non-wildcard subscription delivery with **no** `NAME "to"` child. `docs/spec/v1.md` §4 references this category once §3 incorporates `reference/05`.

## Compatibility

- **Breaks protocol-v1 implementations?** No released v1 exists; this is a draft refinement before the freeze gate.
- **Envelope extensibility:** `ROUTER` already permits inserting NAME-tagged metadata children before `NAME "data"` ([05-protocol-tlvs.md](../../reference/05-protocol-tlvs.md) §`0x0D` "future metadata extensions … can be inserted before it without breaking parsers"), so a parser that does not recognize `NAME "to"` skips it safely. The change is additive.
- **New conformance vectors:** the `wildcard-delivery/` set above.

## Alternatives considered

- **Encode the concrete path in the `SUBSCRIBER`'s target path** (03:140 option a). Rejected: the target is the subscriber's *address*, not the source; one wildcard subscriber has one target but many sources, so the source cannot live in the target.
- **A standalone delivery-envelope TLV separate from `ROUTER`.** Rejected: `ROUTER` already exists as the bridge envelope with extensible metadata; a second envelope duplicates it and doubles the shed/re-attach logic.
- **Leave it implementation-defined** (status quo). Rejected: breaks bridged wildcard interop for a second implementer.

## Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the tracking issue stays open ≥ 14 days (until 2026-07-09) before merge. Open points invited for comment: the `NAME` tag spelling (`to` / `match` / `dst`); whether to also mandate the child for a cross-process-but-unbridged delivery; and whether the child should carry the full concrete `PATH` or a delta from the subscription pattern (smaller, but couples the subscriber to pattern-diffing).
