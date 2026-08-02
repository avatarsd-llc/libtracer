<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0023 — The path segment cap is repriced: 32 → 255, derived from the wire's own widths

| Field | Value |
| ---- | ---- |
| **RFC** | 0023 |
| **Title** | The path segment cap is repriced: 32 → 255, derived from the wire's own widths |
| **Status** | **accepted** (2026-08-02, maintainer-ratified; spec + code edits landed in the acceptance train, [#767](https://github.com/avatarsd-llc/libtracer/issues/767)) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-08-01 |
| **Comment window** | waived by default while solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"); invoke explicitly if outside input is wanted. Verified: `docs/implementations.md` still reads `_(none yet)_`, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** GOVERNANCE.md:54 — "the normative surface itself changes … a behaviour a conforming peer could observe." A path of 33–255 segments goes from MUST-reject to MUST-accept; GOVERNANCE.md:53 excludes that from the erratum instrument by name. |
| **Tracking issue** | [#767](https://github.com/avatarsd-llc/libtracer/issues/767) |
| **Target spec version** | v1 itself. The immutability clause has never triggered: `docs/spec/v1.md:1` reads "(DRAFT)", `:3` reads "The wire format is not yet stable", and every Changelog entry reads `_unreleased_`. Same route RFC-0006, RFC-0018 and RFC-0019 took. |

> **Numbering note.** 0012 was closed unmerged and 0015 was withdrawn (PR #446); neither gap is
> reusable — see [RFC-0016](0016-composed-branch-read.md) §ghost history and RFC-0018's numbering
> note. 0014 was a phantom mislabel when RFC-0016 wrote that note, but it was **subsequently issued
> as a real document** ([RFC-0014](0014-creator-endpoint-connection-lifecycle-and-link-liveness.md),
> accepted 2026-07-24), so it is not a gap at all. 0023 is the next unused number.

> **Relationship to [RFC-0019](0019-path-depth-bounded-by-bytes.md).** RFC-0019 (draft,
> 2026-07-31) proposed deleting the segment cap outright, leaving the byte budget as the only
> bound. Its own §12.5 named the falsifier: *"the maintainer rules that an encode-time MUST cannot
> be deleted without a named replacement — this design fails."* That ruling has now been given
> (2026-08-01 grill, [#767](https://github.com/avatarsd-llc/libtracer/issues/767)): **keep a
> normative bound, but choose it deliberately.** This RFC is the reshape RFC-0019 §12.5 called
> for, and on acceptance RFC-0019's status becomes **superseded** (by this RFC). Its verified
> groundwork is inherited, not re-litigated: the unit-pin erratum (already landed —
> `docs/reference/03-addressing.md:30-34`, `docs/reference/05-protocol-tlvs.md:275-279`), the
> audit that `kMaxSegments` sizes nothing in `core/` (its §6.2, re-verified below), the
> conformance-corpus census (its §6.1), and the bindings audit (its §6.5, re-verified in §5.6 — RFC-0019's TypeScript
> finding still holds: the literal 32 is at `bindings/typescript/packages/client/src/tlv.ts:125`).

---

## 1. Summary

Replace the normative "segment limit 32" with **"segment limit 255"** everywhere it is stated —
`docs/spec/v1.md:58` and `:95`, `docs/reference/03-addressing.md:35`,
`docs/reference/05-protocol-tlvs.md:280` and `:358` — and reprice
`kMaxSegments` (`core/include/libtracer/path.hpp:34`) and the bindings' mirrors to match.

The value is not asserted; it falls out of three prices computed in §4 from the spec's own
encoding and the RAM record:

1. **The wire carries no segment-count field**, so no wire width dictates the cap — the only
   physical bound is the byte budget, which derives a ceiling of **204** segments under today's
   NAME-TLV encoding and **512** under RFC-0018's packed encoding. The cap must sit at or below
   what bytes can ever express, or it is dead text.
2. **255 is the largest count that fits u8** — every per-segment counter, slot index, and table
   dimension in an implementation, a dissector, or a tool stays one byte, matching RFC-0018's
   per-segment `u8` length field (all per-segment quantities single-byte).
3. **The RAM price of 255 on an rv32-class bounded node is ≤ 1 KB, optional, and byte-symmetric**
   (§4.4): nothing in `core/` is dimensioned by segment count, so the only buffer the cap sizes
   is a receiver's *optional* per-segment index table — ~4 B × 255 ≈ 1020 B, the same order as
   the 1024-byte path payload it indexes.

The effective bound is therefore **`min(255, byte-derived ceiling)`**: under today's encoding the
byte cap binds first (204 < 255) and the segment clause is implied; under RFC-0018's packing the
255 becomes the binding cap for short segments (§4.2). Stated plainly rather than sold: **the
observable widening this amendment delivers today is 32 → 204** — identical to RFC-0019's outcome
— and the 255 is the deliberately chosen ceiling that starts doing independent work when the
packed encoding lands.

## 2. Motivation

### 2.1 The 32 was inherited, not priced

`kMaxSegments = 32` entered the tree with the extraction from the originating production firmware
([ADR-0001](../../adr/0001-extraction-from-production-firmware.md)) as a string-parser guard, and
`docs/reference/13-network-formation.md:301-304` then made it the effective network **diameter**
by accident: a `FWD` route's `dst` names every hop, each hop consumes a whole mount run of
typically 3 segments (`net/<module>/<name>`, [ADR-0061](../../adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)),
so 32 segments ≈ **10 hops**. RFC-0019 §8 records the history: an earlier hop constant
(ADR-0014's `MAX_HOPS`) dissolved with its mechanism, and `kMaxSegments` inherited the
diameter role from a constant designed for something else. No pricing argument for 32 exists
anywhere in the record.

### 2.2 Two rulings force the question now

1. **Mount width is being lifted** ([#523](https://github.com/avatarsd-llc/libtracer/issues/523)
   ruling): mounts wider than 3 segments become legal. A wider mount spends the segment budget
   *faster* — at 5-segment mounts the 32-cap diameter drops from ~10 hops to **6**. Uncapping
   width while keeping the inherited 32 makes deep reachability strictly worse.
2. **The wire is DRAFT** (`docs/spec/v1.md:3`). Changing a normative MUST costs nothing today; the
   same change after v1 ships is an ecosystem break requiring protocol v2. This is the cheapest
   this amendment will ever be.

### 2.3 Why a bound remains at all

The 2026-08-01 ruling: interop needs a bound **both peers can size buffers from**. Concretely:

- A fixed-table receiver — the inline-NAMEs strategy of `docs/spec/v1.md` §3.1.5, an MCU resolver,
  a dissector, a binding in a language without cheap growable arrays — may hold **per-segment
  state** (an offset/length index over the PATH body, one entry per segment). Only a normative
  count lets that table be statically sized; a bytes-only bound makes its worst case
  `⌊1024 / min-cost⌋`, a number that **moves whenever the encoding moves** (204 today, 512 under
  RFC-0018) — which is how the 32 got inherited rather than chosen in the first place.
- Analyzers and route tooling get a closed-form worst case for route depth that does not require
  knowing which body encoding a peer speaks.

**Doctrinal contradiction, surfaced explicitly** (house rule): `CONTEXT.md` §Resource bound says
every limit is an injected resource or per-target configuration, "never a hardcoded magic
constant" — and RFC-0019 §2.4 showed the segment cap living as an *unrecorded* exception to that
rule. This RFC keeps a hardcoded constant anyway, on the maintainer's ruling, for the same reason
the 64-byte NAME and 1024-byte PATH constants stand: **the address bounds are wire-grammar
constants, identical on every peer**, and making them per-target is foreclosed by name
(`docs/design/config/00-configuration-space.md` §"What is deliberately not configurable": "an
interoperability failure dressed as a RAM saving"). On acceptance, `CONTEXT.md` §Resource bound
records the addressing bounds as the named, ratified exception (§9).

## 3. What this RFC does **not** do

- **Not touched:** `kMaxPathBytes = 1024` (`core/include/libtracer/path.hpp:32`) — the byte cap
  stays, in the unit the 2026-07-31 erratum pinned (the PATH TLV's `length` field). It remains
  the dominant bound for realistically named paths (§4.2).
- **Not touched:** `kMaxSegmentBytes = 64` (`path.hpp:30`) — RFC-0018's `u8` field width.
- **Not touched:** `kMaxFieldDepth = 8` (`path.hpp:36`) — a different axis.
- **Not touched:** `kMountPeekMax` (`core/include/libtracer/fwd_frame_view.hpp:94`) — the mount
  *width* lift is [#523](https://github.com/avatarsd-llc/libtracer/issues/523)'s own train, second
  per the 2026-07-30 sequencing ruling. §4.5 verifies this RFC does not move its buffers.
- **Not decided here:** RFC-0018's body-grammar change. §4.2 prices both encodings; the packed
  ceiling (512) and crossover are RFC-0018's to own. RFC-0019's own reconciliation list
  (RFC-0019:371) already names `0018-packed-path-segments.md:207` as the line to edit; this RFC
  inherits that item — RFC-0018:207 updates from "≤ 32" to "≤ 255" in the same train, §9.

## 4. The pricing argument — the number falls out, it is not asserted

### 4.1 What the wire physically allows: there is **no segment-count field**

This must be said first because the obvious sizing input does not exist. A PATH TLV
(`docs/reference/05-protocol-tlvs.md` §`0x06`) is a `PL=1` TLV whose payload is **concatenated
NAME TLVs**; the count is implicit in the body. No frame carries a segment count anywhere — not
PATH, not FWD, not SUBSCRIBER. The physical widths in play are only:

| width | value | bound it implies |
| --- | --- | --- |
| PATH `length` field | u16 (u32 with `opt.LL=1`) | 65 535 B raw; **normatively capped at 1024 B** |
| per-segment cost, today | `4 + len` (NAME header + bytes, `reference/05:353`) | min 5 B ⇒ **≤ 204 segments** in 1024 B |
| per-segment cost, RFC-0018 packed | `1 + len` (`u8` length + bytes, RFC-0018:37) | min 2 B ⇒ **≤ 512 segments** in 1024 B |

So a segment cap cannot be read off a field width; it can only be **chosen** in the corridor the
byte budget leaves. Any value above 512 is unreachable under both encodings (dead text); any value
whose rationale is "what the current encoding derives" (204, or 512) re-inherits an encoding
accident — the exact defect [#767](https://github.com/avatarsd-llc/libtracer/issues/767) exists to
end, and the reason those two values are rejected in §8.

### 4.2 The u8 argument, and where 255 actually binds

**255 is the largest count for which every per-segment quantity fits one byte**: the count itself,
the largest slot index (254), a per-segment table dimension, a dissector's column. RFC-0018 makes
the per-segment *length* a genuine `u8` wire field; this RFC makes the per-path *count*
u8-representable to match. No implementation is forced to store either in a byte — the point is
that all of them **may**, forever, on both sides of the seam.

Where the two caps bind, computed (`c` = encoded cost of a segment):

| encoding | count cap binds when | byte cap binds when | derived ceiling |
| --- | --- | --- | --- |
| today (`c = 4 + len`) | never — `255 × 5 = 1275 > 1024` | always | **204** |
| packed (`c = 1 + len`) | mean `c ≤ 4` (mean name ≤ 3 B) | mean `c > 4` | **512** |

Honest consequences, stated rather than smuggled:

1. Under today's encoding the new clause "segments ≤ 255" is **implied by** "length ≤ 1024" — the
   check can never fire. The observable widening today is **32 → 204**, byte-governed.
2. Under RFC-0018's packing the 255 is a **real cap** in the short-segment corner (single-letter
   and 2–3-byte names — precisely the mount-run and generated-name shapes), and the byte cap
   governs everywhere else. The two caps are complementary, not redundant — the same structure
   RFC-0019 §5 measured for the old pair, now with the crossover placed deliberately.

### 4.3 Diameter — the forcing function, repriced

Arithmetic over the shipped encoding rule (each NAME costs `4 + len`,
`docs/reference/05-protocol-tlvs.md:351`; mount runs per ADR-0061; a route is one mount run
per hop). Not a routed measurement (§10).

| mount run | segs/hop | B/hop today | hops @ cap 32 | hops @ cap 255 (byte-gov.) | hops @ 255, RFC-0018 packed |
| --- | --- | --- | --- | --- | --- |
| `/net/ws-client/board-01` | 3 | 32 | 10 | **32** (byte) | 44 (byte) |
| `/net/can/c0` | 3 | 20 | 10 | **51** (byte) | **85 (count!)** |
| `/net/ws-server/s/alice` | 4 | 34 | 8 | **30** (byte) | 46 (byte) |
| 5-segment mount (post-#523) | 5 | ~40 | 6 | **~25** (byte) | ~36 (byte) |

Three readings matter:

- The repriced cap lifts the diameter **3–5×** at today's mount width, and keeps a post-#523
  5-segment mount at ~25 hops instead of the 6 the inherited 32 would leave it.
- For realistically named mounts **the byte budget, not the segment cap, is the diameter** — the
  uplift comes from removing the 32, and would be the same at any cap ≥ 204. The 255 earns its
  place in the short-name/packed corner (the `can/c0` row: 85 count-bound hops), which is exactly
  the corner constrained deployments occupy.
- No plausible deployment approaches either bound; the cap is a buffer-sizing constant again, not
  a topology constraint. That is the intended end state.

### 4.4 The rv32 buffer price — what 255 actually costs a bounded node

Verified against `core/` at `549ec0d`, inheriting and re-running RFC-0019 §6.2's audit:

- **Nothing in `core/` is dimensioned by `kMaxSegments`.** The one cap-sized scratch is
  `core/src/fwd_router.cpp:63` — `kMaxSegmentBytes × kMountPeekMax` = 256 B of stack — keyed to
  segment *bytes* and mount *width*, both untouched here.
- **Worst-case path storage is byte-governed and does not move**: a `path_t` payload is ≤ 1024 B
  at cap 32 and ≤ 1024 B at cap 255 (`core/src/path.cpp:99`), and the parse-time reserve guard
  (`path.cpp:86`) stays keyed to `kMaxPathBytes`. The rv32 RAM census figure for stored paths is
  unchanged **by construction**.
- **The C stack no longer sees depth.** RFC-0019 §6.2 found four wire-reachable recursive subtree
  walks (up to 208 B/level — 32 → 255 would have been +46 KB of worst-case stack). All four are
  now O(1)-memory iterative ascents via `vertex_t::for_each_descendant`
  (`core/include/libtracer/vertex.hpp:1143`; sites `core/src/graph.cpp:398`, `:473`, `:569`,
  `:722`; [#690](https://github.com/avatarsd-llc/libtracer/issues/690), fixed by #692) — no
  auxiliary storage, nothing to fail. The prerequisite RFC-0019 flagged is already satisfied.
- **Decode state is resource-keyed, not count-keyed**: terminus decode draws per-open-level nodes
  from the injected block source (RFC-0006; `core/include/libtracer/grammar.hpp:209-218`,
  [ADR-0065](../../adr/0065-failable-allocation-gets-its-own-seam-block-source.md)), and a bounded
  node runs it over an [ADR-0067](../../adr/0067-bounded-recycling-source-and-per-owner-topology.md)
  `pool_source_t` slab. A deeper path costs more *draws from an injected budget*, never a bigger
  static buffer.

What the cap then actually sizes is the **optional receiver-side per-segment index** (§2.3): at
`{u16 offset, u8 len}` ≈ 4 B/entry aligned —

| cap | table worst case |
| --- | ---: |
| 32 | 128 B |
| **204** (byte-derived, today) | 816 B |
| **255 (this RFC)** | **1020 B** |
| 512 (byte-derived, packed) | 2048 B |

**≈ 1 KB — symmetric with the 1024-byte payload it indexes**: a node that chooses to fully index
one path pays at most one path-budget again, whether drawn from a slab or laid out statically. On
the RAM record that is affordable and honest: ADR-0067's census traced a Cortex-M0 sentinel's
`.bss` to the C runtime, not this library ("libtracer's own static RAM is approximately zero"),
and this RFC keeps it so — `core/` holds no such table, so the reference implementation's static
delta is **zero by construction**. 512 would double the optional table for a value that also
needs u16 counters and is encoding-pinned; 255 is where the u8 line and the ~1 path-budget line
coincide.

### 4.5 Worst-case FWD head — priced, and found inert

The remaining sizing input named in [#767](https://github.com/avatarsd-llc/libtracer/issues/767)
turns out not to price anything, which is itself a result:

- A worst-case `FWD` frame carries `dst` + `src` routes, each a PATH bounded by **bytes**
  (≤ 1024 B body + ≤ 6 B header each) — segment count does not appear in the frame's worst case,
  before or after this RFC.
- The forward hop's stack heads are structural, not depth-keyed: `kFwdHead1Cap = 64`
  (`core/include/libtracer/fwd_frame_view.hpp:398`) is FWD header + op + shrunk-dst header;
  `kFwdSrcHdrCap = 6` (`fwd_frame_view.hpp:409`) is deliberately a *header-only* bound after the
  synthetic-limit incident recorded in its own comment; and `kFwdMaxIov = 9`
  (`fwd_frame_view.hpp:441`) is counted from the emit sequence with the mount as **one span** —
  its comment states it "does not move when the descent is uncapped", 8 regions of headroom under
  the transports' `kMaxInlineIov = 16` spill.

Repricing the segment cap therefore moves **no** FWD buffer, no iovec bound, and no per-hop
allocation behaviour. (The known allocation cliff in this area belongs to lifting `kMountPeekMax`
— #523's train, not this one.)

### 4.6 Conclusion

The corridor is (must not exceed 512; should exceed 204 only knowingly; per-segment quantities
should stay u8; the optional index should stay ~one path budget). **255 is the unique value that
sits on all four lines at once.** If RFC-0018 were rejected, 255 would remain safe but forever
byte-shadowed — at which point the honest cap would be the derived 204, and this RFC should be
amended, not reinterpreted (§11 falsifier 2).

## 5. Proposed change

The spec edits below land **after acceptance, in a follow-up PR** — this PR adds only the RFC.
All normative edits must land atomically (the precedence rule of `docs/spec/v1.md:63-64` is
degenerate between incorporated documents — RFC-0019 §4.4's finding, unchanged).

### 5.1 `docs/spec/v1.md:57-59` — the §3 incorporation bullet

> - **[docs/reference/03-addressing.md](../reference/03-addressing.md)** §path
>   syntax — canonical PATH constraints (segment limit **255**
>   ([RFC-0023](rfcs/0023-path-segment-cap-repriced-32-to-255.md)), NAME limit 64 bytes,
>   PATH `length` ≤ 1024 bytes, reserved characters, UTF-8).

### 5.2 `docs/spec/v1.md:96` — the encode-time MUST (§3.1.2)

> - Path constraints from [docs/reference/03-addressing.md](../reference/03-addressing.md) §path
>   syntax (segment limit **255**, name limit 64 bytes, total ≤ 1024 bytes measured as the PATH
>   TLV's `length` field) MUST be checked at encode time. A pre-encoded PATH TLV that violates
>   these limits is non-conforming. *(Informative: under the current NAME-TLV body encoding the
>   1024-byte budget admits at most 204 segments, so the byte limit binds first; the segment limit
>   is the encoding-independent ceiling — [RFC-0023](rfcs/0023-path-segment-cap-repriced-32-to-255.md) §4.)*

### 5.3 `docs/reference/03-addressing.md:35`

> - Maximum **segment depth**: **255** ([RFC-0023](../spec/rfcs/0023-path-segment-cap-repriced-32-to-255.md)
>   — chosen from the wire's own widths, superseding the inherited 32; the total-path byte cap
>   above binds tighter whenever mean encoded segment cost exceeds 4 bytes, which under the
>   current encoding is always). (An addressing limit on PATH construction; the TLV parser itself
>   has no depth cap — nesting is receiver-resource-bounded per
>   [RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md).)

### 5.4 `docs/reference/05-protocol-tlvs.md:280` and `:358`

> - Segment count ≤ **255** ([RFC-0023](../spec/rfcs/0023-path-segment-cap-repriced-32-to-255.md)).

> A path that resolves to more than **255** segments, has a single segment longer than 64 bytes,
> or whose **encoded `PATH` body** exceeds the addressing-level cap MUST fail to encode.

The enforcement-placement rules at `docs/reference/05-protocol-tlvs.md:282-298` (codec does not
enforce; resolver enforces child-type; count/length bound where constructed or admitted) are
**unchanged** — this RFC reprices the number, not the tier that checks it.

### 5.5 `docs/reference/13-network-formation.md:301-304` — the diameter bullet

> - **Depth is capped by the route, and the route by its bytes.** A `FWD` frame's `dst` names
>   every hop and is consumed monotonically, so a delivery travels exactly as far as its explicit
>   source route — segment count ≤ **255** ([RFC-0023](../spec/rfcs/0023-path-segment-cap-repriced-32-to-255.md);
>   `kMaxSegments`, `core/include/libtracer/path.hpp:34`), and for realistically named mounts the
>   1024-byte PATH budget binds first (≈ 30–50 hops at 3-segment mount runs).

### 5.6 Code (reference implementation, after acceptance)

- `core/include/libtracer/path.hpp:34` — `kMaxSegments = 255`, comment gains the RFC pointer and
  the min(255, byte-derived) note. The check at `core/src/path.cpp:97` and everything around it is
  untouched; `segments_` already counts in a `std::size_t`.
- **Rust** — `MAX_SEGMENTS = 255` (`bindings/rust/src/tlv_builders.rs:25`; checks at
  `tlv_builders.rs:256`, `src/path.rs:53`). The **decode-side** check at `src/path.rs:89` (an
  already-parsed `&Tlv`, reached from `fwd_dst_path` / `fwd_src_path` /
  `subscriber_target_path`) is misplaced per `reference/05:284-298` ("the codec does not enforce,
  and is not expected to") and was RFC-0019 §10(b)'s live-regression finding: an accumulated `src`
  return route is legal at any byte-reachable depth. Reprice it to 255 **and** file its relocation
  to the resolver tier as a named follow-up; the 33-segment `TooManySegments` case at
  `bindings/rust/tests/conformance_vectors.rs:257` inverts into a must-accept, and a 256-segment
  must-reject takes its place. `MAX_SEGMENTS` stays exported (`src/lib.rs:56`) — value change is
  a semver-visible constant change, noted in `bindings/rust/CHANGELOG.md`.
- **TypeScript** — the client validates the per-segment 64-byte rule
  (`bindings/typescript/packages/client/src/tlv.ts:25,102`) and carries the literal 32-segment
  check RFC-0019 §6.5 reported, still in place at `tlv.ts:125`; it has **no 1024-byte check at
  all**. This train reprices the existing 32 → 255 and adds the missing 1024-byte encode-time
  check, and lands in `bindings/typescript/*/CHANGELOG.md`.
- `core/CHANGELOG.md` notes the constant change (public header).

### 5.7 Conformance vectors

- **Zero existing vectors change** — RFC-0019 §6.1's census holds: deepest PATH in the corpus is
  5 segments; a raised maximum cannot invalidate bytes that never approached it.
- **One vector added:** `path/path-deep-204` — 204 single-byte segments, `length` = 1020, a
  positive round-trip case pinning the byte-derived ceiling. A **255-segment positive vector is
  not expressible under today's encoding** (255 × 5 = 1275 > 1024) — it becomes expressible only
  under RFC-0018's packing, and that RFC owns adding it (`path/path-deep-255-packed`).
- A 256-segment **reject** vector is not expressible in the round-trip harness at all — the cap is
  an encode-time MUST, and `v1.md:131-143`'s conformance procedure has no encode-reject shape
  (RFC-0019 §10's structural point, unchanged by repricing). The Rust/TS unit suites carry the
  reject case instead.

## 6. Impact on conforming implementations

**rv32-class bounded node.** Static delta zero by construction (§4.4): no core buffer is
count-dimensioned, path payload worst case is unchanged at 1024 B, decode is slab-drawn
(ADR-0067), depth no longer reaches the C stack (#690 fixed). A node that opts into a full
per-segment index prices it at ≤ 1020 B — one path budget — from its own slab. The standing
census rule applies before any figure is banked (§10).

**Host.** No effect beyond the constant: host-side parse and resolve are already linear in bytes,
and the FWD plane's buffers are byte- and region-keyed (§4.5).

**Every implementation** must move its encode-time check 32 → 255 (or, equivalently until
RFC-0018 lands, may rely on the 1024-byte check alone, which implies it — §4.2). A **resolver**
that rejects a 33–255-segment address after this amendment is non-conforming in a way a peer can
observe: the address is legal, and `reference/05:296-297` places the count bound at
construction/admission, not at decode.

## 7. Compatibility

**Monotone widening on the count axis.** The encode predicate keeps its three-conjunct shape with
one constant repriced: `(segments ≤ 255) ∧ (each NAME 1..64) ∧ (length ≤ 1024)`. Every previously
conforming PATH remains conforming; nothing any deployed device emits becomes illegal; nothing a
receiver must *store* grows (worst-case payload 1024 B before and after). The error surface is
untouched — `tr::path::invalid` (`0x0021`) keeps its code, severity and disposition; contrast
RFC-0006, which had to amend an error's meaning.

**No capability region.** Both surviving bounds stay closed-form predicates over the candidate
bytes, answerable by an encoder alone — no negotiation (`CONTEXT.md` §Capability negotiation:
does not exist), no per-target knob (§2.3), no declared budget.

**Migration.** Deployed devices: none required. An encoder still hardcoding 32 emits only legal
paths and stays a conforming *emitter*; it merely cannot spell addresses other peers can, which is
a quality gap, not a conformance breach — the conformance-visible obligation is on the
**resolver/admission** side (§6). The Rust constant change is semver-visible (§5.6).

## 8. Rejected alternatives

- **Keep 32** — rejected by the forcing rulings (§2.2): it was never priced, and #523's wider
  mounts turn it into a 6-hop network.
- **Delete the bound (RFC-0019's shape)** — refuted by the 2026-08-01 maintainer ruling: interop
  buffer sizing wants a count both peers share, and "the byte-derived ceiling" is a number that
  moves with the encoding (204 → 512 across RFC-0018), which is inheritance again, one level up.
  Recorded as RFC-0019 → superseded.
- **A negotiated per-link segment capability** — rejected for minimalism: v1 has no capability
  negotiation and deliberately so (`CONTEXT.md` §Capability negotiation, ADR-0013), the address
  bounds are foreclosed from per-target variation by name
  (`docs/design/config/00-configuration-space.md`), and RFC-0019 §9.3 already showed a
  floor-plus-capability split creates a region where two conforming peers disagree about what is
  encodable.
- **204 or 512 (the byte-derived ceilings)** — rejected as encoding-pinned: each is an artifact of
  a body grammar (§4.1), moves when RFC-0018 lands, and 512 additionally forfeits u8 counters
  while doubling the optional index table (§4.4).
- **128 (or another power of two below u8)** — buys nothing: no core buffer shrinks (none is
  count-keyed), the optional table saves ~500 B only for nodes that chose to build it, and it
  halves the packed-encoding depth corridor for no priced return.

## 9. On acceptance (same train as the spec edit)

- `docs/spec/rfcs/0019-path-depth-bounded-by-bytes.md` status → **superseded** (by this RFC).
- `docs/spec/rfcs/0018-packed-path-segments.md:207` — "segment count ≤ 32" → "≤ 255
  ([RFC-0023](0023-path-segment-cap-repriced-32-to-255.md))"; RFC-0018 additionally owns the
  `path/path-deep-255-packed` vector and the packed crossover note (§4.2).
- `CONTEXT.md` §Resource bound — records the address bounds (64 / 1024 / 255 / 8) as the named
  exception: wire-grammar constants, not resource limits, per the §2.3 rationale.
- Changelogs per §5.6; `docs/modules/path.md` comment ripple.

## 10. What is UNMEASURED

Labelled per the standing rule (a quantitative claim names its instrument or is labelled).

- Every number in §4 is **arithmetic over the shipped encoding rule or a corpus census**, not a
  measurement. No multihop leg has been run at depth > 5; `bench_hop_chain` remains recorded as
  confounded and must not be cited.
- **Deep-path parse cost**: unmeasured; expected linear (`path_t::parse` is a single pass and
  §4.4 shows no count-keyed resource). Instruments if wanted: a `path_t::parse` microbench at
  depths 1/32/204; `bench_transport_iov` for the forward hop.
- **Flash/RAM delta of the constant change**: expected zero (an `inline constexpr` value change);
  the rv32 size census is the instrument, and no "beat" is banked without it.

## 11. What would falsify this RFC

1. **A count-keyed resource surfaces.** If any array, reserve, or table in `core/`, a binding, or
   the dissector turns out dimensioned by `kMaxSegments`, §4.4's "zero by construction" fails and
   the price must be re-run. *Check:* `grep -rn kMaxSegments MAX_SEGMENTS`, ASan at depth 204.
2. **RFC-0018 is rejected.** Then 255 is permanently byte-shadowed (204 binds forever) and the
   honest constant is the derived 204 — amend, do not reinterpret (§4.6).
3. **A deployment needs > 85 hops of short-mount route or > 255 segments.** Then the corridor was
   mis-priced and the u16 shape (with its costs, §4.4) must be revisited.
4. **The maintainer's keep-a-bound ruling is reversed.** Then RFC-0019's deletion is the standing
   design and this RFC withdraws in its favour.

## 12. Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window",
this is an **amendment**: RFC plus maintainer approval, comment window **waived by default** while
solo-maintained (verified §header; the waiver reverts the moment `docs/implementations.md` gains a
registered implementer). Targeted at the v0.7.0 window while the wire is DRAFT (§2.2): the spec
edit lands after approval, in its own PR, per §5.
