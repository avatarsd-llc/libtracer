<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0026 — The ACE `access_mask` canonical wire width is u32

| Field | Value |
| ---- | ---- |
| **RFC** | 0026 |
| **Title** | The ACE `access_mask` canonical wire width is u32 |
| **Status** | **accepted** (2026-08-12, maintainer-ratified in the [#993](https://github.com/avatarsd-llc/libtracer/issues/993) grilling session; doc + vector + binding edits land in the acceptance train) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-08-13 |
| **Comment window** | waived by default while solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"). Verified: `docs/implementations.md` still reads `_(none yet)_`, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** Reference 05 is normative by incorporation (spec v1 §3, ADR-0007), its §`0x0A` layout text changes (`u16` → `u32`), a *published* conformance vector is re-cut, and a published binding type widens — each individually outside the erratum instrument, which must not change the wire surface. |
| **Tracking issue** | [#993](https://github.com/avatarsd-llc/libtracer/issues/993) |
| **Target spec version** | v1 itself — the immutability clause has never triggered (`docs/spec/v1.md:1` reads "(DRAFT)"). Same route RFC-0023 took. |

---

## 1. Summary

The ACE `access_mask` field ([reference 05 §`0x0A`](../../reference/05-protocol-tlvs.md)) had two
spellings in the published corpus: the layout text, the `acl/acl-aces` conformance vector and the
Rust `Ace` builder said **u16**, while the C++ typed builder (`tr::graph::encode_acl`) — the
encoder behind every `:acl` read a reference node serves — emitted **u32**. Nothing misparsed
(the acceptance rule bounds a numeric ACE payload at *no wider than* the parsed width, and
little-endian narrowing is exact), but the two cores emitted different bytes for the same typed
ACE and only one of them matched the published vector.

This RFC names **u32** as the one canonical width and aligns every artifact to it:

- **Reference 05 §`0x0A`**: the layout line becomes `VALUE <u32 bitfield>`; the paragraph that
  disclosed the divergence and linked #993 is rewritten, because the divergence no longer exists.
- **The `acl/acl-aces` vector**: re-cut with 4-byte `access_mask` children (179 → 183 bytes).
- **The Rust binding**: `structured::Ace::access_mask` widens `u16` → `u32`, its builder emits
  four bytes, and its reader stops narrowing the parsed mask to 16 bits.
- **The C++ core**: `encode_acl` already emits u32 — unchanged.

## 2. Why u32, not u16

- Reference 05's own bit registry ends with "`0x100`+ **reserved**" — text that is dead the day
  the wire width is u16 with `0x8000` the last expressible bit after eight are assigned.
  The u16 spelling was never the intent; it was an under-specified transcription.
- Canonicalizing on u16 would put a **permanent 16-bit ceiling on the rights registry of a
  security surface** to save two bytes per ACE on a control-plane field — the wrong trade.
- The C++ core's `:acl` read path has emitted u32 since `encode_acl` existed, so u32 is also the
  width with deployment inertia.

## 3. What does NOT change

- **`parse_acl`'s acceptance rule** (`non-empty && size ≤ parsed width`, #906) is untouched: a
  payload narrower than u32 still parses and zero-extends — the old two-byte spelling remains
  readable, so this amendment obsoletes no stored ACL bytes. Tightening acceptance to
  exactly-canonical was considered and **rejected** in the #993 ruling as #906's scope.
- The in-memory C++ type (`ace_t::access_mask`, `std::uint32_t`) and the bit assignments.
- The ACL evaluation model, inheritance, and the reserved-subject rule.

## 4. Gating

The conformance harness gates the codec only (`encode(decode(input.bin)) == input.bin`) and can
never catch a typed-builder width divergence, so each core gains a host test that consumes its
**typed builder** against the vector bytes: the C++ suite byte-compares `encode_acl` of the
vector's ACE list with `input.bin`, and the Rust suite already byte-compares `structured::acl`
with the vector (plus a new round-trip pinning that a mask above the old 16-bit ceiling survives
build → decode → read untruncated).
