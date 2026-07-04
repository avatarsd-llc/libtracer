<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0006 — Nesting depth is receiver-resource-bounded: the fixed cap of 32 is removed

| Field | Value |
| ---- | ---- |
| **RFC** | 0006 |
| **Title** | Nesting depth is receiver-resource-bounded: the fixed cap of 32 is removed |
| **Status** | **accepted** (2026-07-04, maintainer-ratified design grill) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-04 |
| **Comment window** | waived by the maintainer (solo-maintainer project, GOVERNANCE.md window dead ceremony) |
| **Tracking issue** | [#196](https://github.com/avatarsd-llc/libtracer/issues/196) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |

## Summary

The wire format ceases to impose a **maximum nesting depth**. The former normative cap — "Maximum nesting depth: 32; deeper TLVs MUST be rejected" ([reference/01](../../reference/01-data-format.md) §iterative parsing requirement, incorporated normatively by [v1.md](../v1.md) per [ADR-0007](../../adr/0007-normative-wire-format-by-incorporation.md)) — is replaced by a **receiver-resource bound**: a receiver parses as deep as its decode resources allow (one explicit work-stack/arena node per open level, drawn from the implementation's injected memory resource) and MUST reject a frame that exceeds them with `ERROR{tr::tlv::nesting_too_deep}`, whose meaning is amended to **"exceeds this receiver's decode resources."** The registered code `0x0010` and the identity path are unchanged.

## Motivation

The project's target spectrum is 16 KB MCUs to 128 GB many-core hosts; a single frozen constant serves neither end (maintainer: "no reason for a 32 depth — it may be any depth; imagine the most complicated graphs over multiple datacenters"). Concretely:

1. **The recorded rationale is obsolete.** The cap guarded a *recursive* parser's call stack (reference/01's own rationale line). Conforming parsers are required to be **iterative**; the per-level cost is an explicit arena/work-stack node, already bounded by the injected memory resource ([ADR-0039](../../adr/0039-pmr-memory-model-host-aligned-allocation.md)/[ADR-0041](../../adr/0041-terminus-arena-decode-span-contract.md)). The stack-overflow hazard the cap addressed cannot occur.
2. **Deep nesting is legitimate.** A branch write ([RFC-0005](0005-subtree-subscriptions.md)) of a deep vertex subtree produces equally deep `POINT` nesting — a 500-level tree is a 500-deep frame. (Multi-hop routing does **not** deepen nesting — longer routes grow `PATH` *width*, more NAME children, not depth.)
3. **The honest bound already exists physically.** A 16 KB node rejects what its pool cannot hold — a per-receiver capability of the same kind as maximum frame size — while a large host parses arbitrary depth. A frozen 32 under-serves the host and over-promises nothing useful to the MCU.

## Normative changes

1. **reference/01 §iterative parsing requirement**: the "Maximum nesting depth: 32" and "work stack ~1 KiB" items are replaced by: nesting depth is **unbounded by the wire format**; a receiver's capability is bounded by its decode resources; exceeding them ⇒ reject with `ERROR{tr::tlv::nesting_too_deep}` (amended semantics above). The iterative-parsing requirement itself (explicit work stack, no recursion) is **unchanged**.
2. **v1.md** incorporation line: "nesting depth cap 32" → receiver-resource-bounded per this RFC.
3. **No conformance floor is introduced.** Protocol-defined TLV shapes (`FWD`, `PATH`, `SETTINGS`, `ERROR`, array-whole reads, …) nest ≤ 5 by construction, so every conforming receiver parses every protocol frame at any budget; user-data depth is a deployment capability, exactly like frame size. A sender MUST NOT assume a peer accepts any particular user-data depth without out-of-band knowledge.

## Conformance-suite impact

The deep-nesting reject vector becomes **resource-keyed**: the harness contract ([tests/conformance/HARNESS.md](../../../tests/conformance/HARNESS.md)) gains a declared decode-arena budget under which the vector's expected `ERR:TLV_NESTING_TOO_DEEP` outcome is well-defined; a harness running with a larger budget parses the same vector successfully — both outcomes conform, keyed to the declared budget. The differential fuzzers gain unbounded-depth generation (agreement is required between cores *given the same declared budget*).

## Compatibility

Wire bytes are unchanged; no frame that was valid becomes invalid. Frames deeper than 32 — formerly a mandatory reject everywhere — become parseable on receivers with sufficient resources. Since v1 is unreleased, no deployed peers rely on the mandatory reject.
