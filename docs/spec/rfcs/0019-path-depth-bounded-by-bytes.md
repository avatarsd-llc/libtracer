<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0019 — Path depth is bounded by bytes: the 32-segment `PATH` cap is deleted

| Field | Value |
| ---- | ---- |
| **RFC** | 0019 |
| **Title** | Path depth is bounded by bytes: the 32-segment `PATH` cap is deleted |
| **Status** | **superseded** (2026-08-02) by [RFC-0023](0023-path-segment-cap-repriced-32-to-255.md) — the maintainer ruled that a normative bound stays (this RFC's own §12.5 falsifier), so the cap was **repriced 32 → 255** rather than deleted. This document's body is retained unrewritten as the groundwork RFC-0023 inherits (§6.1 corpus census, §6.2 core audit, §6.5 bindings audit). |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-31 |
| **Comment window** | waived by default while solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"); invoke explicitly if outside input is wanted. Verified: `docs/implementations.md` still reads `_(none yet)_`, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** GOVERNANCE.md:54 — "the normative surface itself changes: a new or altered MUST/SHOULD … a behaviour a conforming peer could observe." Not an erratum: GOVERNANCE.md:53 excludes it by name — "if applying it would change what a conforming implementation does, it is not an erratum." |
| **Tracking issue** | _(to be filed, `rfc`-labelled)_ |
| **Target spec version** | v1 itself. The immutability clause has never triggered: `docs/spec/v1.md:1` reads "(DRAFT)", `:3` reads "The wire format is not yet stable", and all three Changelog entries read `_unreleased_`. Same route [RFC-0006](0006-resource-bounded-nesting-depth.md) and [RFC-0018](0018-packed-path-segments.md) took. |

> **Numbering note.** 0012 was used and withdrawn; 0015 never existed. Neither gap is reusable —
> see [RFC-0016](0016-composed-branch-read.md) §ghost history and RFC-0018's numbering note. 0019
> is the next unused number.

---

## 1. Summary

Delete `kMaxSegments` and every normative statement of "segment limit 32". Put **nothing** in its
place. A `PATH` is already bounded by two closed-form byte predicates every conforming encoder
evaluates anyway — each `NAME` payload is `1..64` bytes, and the `PATH` TLV's `length` field is
`≤ 1024` — and those two predicates are the whole bound. The encode-time MUST at `docs/spec/v1.md:95`
does not disappear; it goes from a three-clause conjunction to a two-clause conjunction, which is
strictly simpler to state, strictly cheaper to check, and strictly more permissive.

This RFC is **forced to pin one thing it did not set out to change**: "1024 bytes" is currently
three mutually incompatible numbers across the two incorporated reference documents and the code.
The bound cannot be "expressed once, in bytes" until the unit is decided, so §4.3 pins it to the
`PATH` TLV's `length` field — the unit C++ and Rust already implement.

**This is a deliberate ~6.4× widening, not a redundancy removal.** Measured arithmetic, §5: the
1024-byte cap admits **204** single-byte segments, so the 32 binds 6.4× tighter for short segments
and the two caps are *not* redundant. Deleting the 32 raises the depth ceiling from a declared 32
to a derived 204 — that is the actual normative content of this amendment, and it is stated
plainly rather than sold as a simplification.

## 2. Motivation

### 2.1 This is the deferral commit `6d76200` promised

Commit `6d76200` — the RFC-0006 implementation — says so verbatim in its message:

> `kMaxSegments` is KEPT: it is a documented reference/03 addressing limit enforced by a linear
> parser (no per-level resource), so its removal is a separate RFC, not RFC-0006 fallout.
> `kMaxDispatchDepth` untouched (RFC-0007 W4 unit).

This is that separate RFC. Its stated reason — *no per-level resource* — is also the reason
RFC-0006's answer does not transfer, and why the honest replacement is not a *different* bound but
**no** bound, with the byte budget that already exists doing the work (§7).

### 2.2 The 32 is the real blocker on network diameter

`docs/reference/13-network-formation.md:285-288` makes the segment cap the network **diameter**:

> - **Depth is capped by the route.** A `FWD` frame's `dst` names every hop and is consumed
>   monotonically, so a delivery travels exactly as far as its explicit source route — segment
>   count ≤ the PATH segment cap of 32 ([03 — Addressing](../reference/03-addressing.md);
>   `kMaxSegments`, `core/include/libtracer/path.hpp:34`).

Per ADR-0061's erratum, `src`/`dst` grow by the **full** mount run per hop — `net` / `<module>` /
`<name>`. Arithmetic over the encoding rule at `docs/reference/05-protocol-tlvs.md:348` (each
`NAME` costs `4 + len`):

| mount run | segs/hop | encoded B/hop | hops under the 32-segment cap | hops under 1024 B alone |
| --- | --- | --- | --- | --- |
| `/net/ws-client/board-01` | 3 | 32 | **10** | **32** |
| `/net/can/c0` | 3 | 20 | **10** | **51** |
| `/net/ws-server/s/alice` | 4 | 34 | **8** | **30** |

A **3.2×–5.1× diameter uplift on the same 1024 bytes**, with no new mechanism, no new RAM and no
new code. This is arithmetic over the shipped encoding, not a routed measurement (§11).

**Sequencing.** The 2026-07-30 ruling is "do the 32 amendment first, then uncap width". The code
agrees, in writing: `core/include/libtracer/fwd_frame_view.hpp:428-431` warns that lifting
`kMountPeekMax` and re-deriving `kFwdMaxIov = 6 + 2 * depth` "would cross 17 at depth 6 and put a
heap allocation on every deep-mount forward hop, silently" (against `kMaxInlineIov = 16`;
instrument named in-tree: `bench_transport_iov`). Uncapping *width* first has a known allocation
cliff. Uncapping the 32 has none — this RFC does not touch `kMountPeekMax`.

### 2.3 The cap protects nothing from a peer

Verified from source, not assumed. Nothing on the wire path counts segments:

- `core/src/frame.cpp:166-179` (`wire::path_key`) sums child lengths only to reserve, then emits.
  No cap test.
- `core/src/op_resolve_walk.hpp:660-669` (`path_lookup_key`) rejects a non-`NAME` child and counts
  nothing.
- `core/src/graph.cpp:458-483` (`ensure_vertex_ptr`) calls `key_view_t::split_levels`, which checks
  only `NAME` framing and exactness (`core/include/libtracer/key_view.hpp:122-136`), then
  `mkdir -p`s every missing level with no count or byte check.

`kMaxSegments` is enforced in **exactly one place** — `core/src/path.cpp:110`, inside
`path_t::parse`, the local string→bytes builder. A peer can already write-create a vertex at
arbitrary depth. The amendment therefore changes what a conforming **encoder** may emit and what
the **local string API** accepts, and changes nothing about a node's exposure to a hostile peer.
Any objection that the cap is a safety property is describing a protection that does not exist.

RFC-0016's merged 2026-07-30 erratum already ratified this and was worded to survive this
amendment:

> **The walk was always safe, for a better reason.** It is iterative over a heap-backed stack, so
> the bound is the allocator and exhaustion surfaces as `BACKPRESSURE` … the corrected one is the
> one that survives `kMaxSegments` being lifted.

PR #685 corrected the three code comments that claimed "graph depth is `kMaxSegments`-bounded
structurally" (`core/include/libtracer/graph.hpp:580-586`, `core/src/graph.cpp:1851`).

### 2.4 Doctrine

`CONTEXT.md:269` §Resource bound states the rule without qualification and enumerates its ratified
instances — nesting depth (RFC-0006), the deleted dispatch cap (RFC-0007 / ADR-0051), transport
max-frame. **The addressing segment cap is not among them.** An exhaustive grep of `CONTEXT.md` for
it returns nothing: the cap has been surviving as an *unrecorded* exception to a rule that records
no exception for it. `:270`'s `_Avoid_` list names "nesting depth cap 32" as a phrase not to use.

ADR-0038's 2026-07-30 erratum already retracted the sibling cap on the routing plane, citing this
same ruling — "it is precisely the shape CONTEXT.md §Resource bound forbids — a hardcoded magic
constant policing user designs" — and supplies the replacement bound this RFC leans on for
`reference/13`: "`dst` strictly shrinks by at least one segment per hop, so a route terminates in
at most `len(dst)` hops with no counter, no state and no cap. The MTU bounds `len(dst)`."

## 3. What this RFC does **not** do

Stated up front, because three unrelated `32`s now exist in the tree and conflating them is the
failure mode ADR-0038's erratum already had to undo.

- **Not touched:** `kMaxSegmentBytes = 64` — RFC-0018 is settling it and turns it into a genuine
  `u8` wire field width. This RFC must not, and does not, answer RFC-0018 §12 open question 2
  ("does the segment-length bound belong in `config.hpp`?") in the affirmative; §9.2 explains why.
- **Not touched:** `kMaxFieldDepth = 8` (`core/include/libtracer/path.hpp:36`) — a different axis.
- **Not touched:** `kMountPeekMax` — that is the width uncapping, deliberately second (§2.2).
- **Not touched:** `tests/conformance/coverage_audit.py:66-67`, which still raises
  `ParseError("nesting > 32")` — a live residue of the *nesting* cap RFC-0006 removed, a different
  axis. Named here so a future reader does not read the two `32`s as the same number.
- **Not touched:** the wire-tier admission check. `docs/reference/05-protocol-tlvs.md:292-293` says
  the limits are bounded "where the address is constructed **or admitted**"; only *constructed*
  exists in code. That clause is aspirational and this RFC neither implements nor relies on it.
  Issue #688 (`create_child` accepts a peer-supplied name with no reserved-character check while
  `path_t::parse` rejects all seven) is the sibling hole in that tier — a named follow-up, not
  this RFC.

## 4. Proposed change

### 4.1 `docs/spec/v1.md` lines 57–59 — the §3 incorporation bullet

**Before:**

```
- **[docs/reference/03-addressing.md](../reference/03-addressing.md)** §path
  syntax — canonical PATH constraints (segment limit 32, NAME limit 64 bytes,
  total ≤ 1024 bytes, reserved characters, UTF-8).
```

**After:**

```
- **[docs/reference/03-addressing.md](../reference/03-addressing.md)** §path
  syntax — canonical PATH constraints (NAME limit 64 bytes, PATH `length`
  ≤ 1024 bytes, reserved characters, UTF-8). There is **no limit on segment
  count**: a path is as deep as its byte budget allows
  ([RFC-0019](rfcs/0019-path-depth-bounded-by-bytes.md)).
```

### 4.2 `docs/spec/v1.md` line 95 — the load-bearing encode-time MUST (§3.1.2)

**Before:**

```
- Path constraints from [docs/reference/03-addressing.md](../reference/03-addressing.md) §path syntax (segment limit 32, name limit 64 bytes, total ≤ 1024 bytes) MUST be checked at encode time. A pre-encoded PATH TLV that violates these limits is non-conforming.
```

**After:**

```
- Path constraints from [docs/reference/03-addressing.md](../reference/03-addressing.md) §path syntax MUST be checked at encode time, and they are exactly two: every NAME child's payload MUST be 1..64 bytes, and the PATH TLV's `length` field — its encoded payload size, whatever the body grammar — MUST be ≤ 1024. A pre-encoded PATH TLV that violates either limit is non-conforming. **There is no limit on the number of segments.** An encoder MUST NOT reject a PATH for its segment count alone: path depth is bounded by that byte budget and by nothing else ([RFC-0019](rfcs/0019-path-depth-bounded-by-bytes.md)). *(Consequence, informative: with today's 4-byte NAME headers a 1024-byte PATH admits at most 204 single-byte segments; the ceiling is derived from the encoding, not declared.)*
```

### 4.3 Pinning the unit — an erratum-shaped sub-change, folded in

`1024` is measured in **three mutually incompatible units** today:

| site | unit | segments admitted at 1 B/segment |
| --- | --- | --- |
| `docs/reference/03-addressing.md:30` | the string form | 512 |
| `docs/reference/05-protocol-tlvs.md:275` | "sum of NAME bytes + segment separators" (excludes TLV headers) | 512 |
| `core/src/path.cpp:112` | `p.payload_.size()` — the encoded `PATH` payload, **including** each 4-byte `NAME` header | **204** |

Rust agrees with the code (`bindings/rust/src/tlv_builders.rs:262-266`,
`payload_bytes += 4 + child.payload.len()`). Worked divergence *inside* today's cap: 32 segments of
29 bytes is 960 B doc-legal and 1056 B code-rejected.

This RFC pins **the `PATH` TLV's `length` field**, because (a) it is what C++ and Rust already
implement, so no shipped behaviour changes; (b) `length` is by definition the encoded payload size
in *any* body grammar, so the wording survives RFC-0018's repacking verbatim where "4-byte header"
wording would not; (c) it is the unit that maps to real memory, which is the only reason to keep a
byte bound at all.

Taken alone this sub-change would qualify as an **erratum** (GOVERNANCE.md:52 — the text
contradicts shipped, already-agreed behaviour). It is folded in rather than split because the
amendment must rewrite these same lines anyway.

### 4.4 Companion normative edits — same commit, or the spec self-contradicts

`v1.md:95` restates the numbers **inline in v1.md's own body** while `reference/03:31` states them
as the incorporated source. `v1.md:63-64`'s precedence rule resolves *incorporated vs. other* and
is structurally silent when both sides are incorporated — PR #685 (commit `77adb66`) hit exactly
that hole and resolved it editorially, not by rule. No rule would repair a partial edit here.

**`docs/reference/03-addressing.md:30`**

- Before: `- Maximum **total path** length: 1024 bytes.`
- After: `- Maximum **total path** length: 1024 bytes, measured as the PATH TLV's `length` field — the encoded payload size, each NAME child's header included.`

**`docs/reference/03-addressing.md:31`**

- Before: `- Maximum **segment depth**: 32 (an addressing limit on PATH construction; the TLV parser itself has no depth cap — nesting is receiver-resource-bounded per RFC-0006).`
- After: `- **Segment depth: unbounded by the address grammar** ([RFC-0019](…)). A path is as deep as the total-path limit above allows; no segment count exists anywhere in the protocol. (The TLV parser has no depth cap either, for a different reason on a different layer — nesting is receiver-resource-bounded per [RFC-0006](…).) Under today's NAME-TLV encoding the 1024-byte budget admits at most 204 single-byte segments; that ceiling is a derived consequence of the encoding, not a limit, and it moves when the encoding does.`

`:35` — "A path that violates any limit MUST be rejected with `ERROR{tr::path::invalid}`" — needs
**no edit**. Its quantifier still ranges over the surviving limits (single-name length, total path,
field-chain depth, index value). Verified by reading the section, not assumed.

**`docs/reference/05-protocol-tlvs.md:275-276`**

- Before: `- Total path length (sum of NAME bytes + segment separators) ≤ 1024 bytes.` / `- Segment count ≤ 32.`
- After: `- Total path length ≤ 1024 bytes, measured as this PATH's `length` field (the encoded payload size, each NAME child's header included — *not* the string form's byte count, which is smaller; the two readings had diverged and this pins the wire one).` / `- No limit on segment count ([RFC-0019](…)).`

**`docs/reference/05-protocol-tlvs.md:352`**

- Before: `A path that resolves to more than 32 segments, has a single segment longer than 64 bytes, or whose total segment-bytes exceed the addressing-level cap MUST fail to encode.`
- After: `A path with a single segment longer than 64 bytes, or whose encoded `length` exceeds the addressing-level cap of 1024 bytes, MUST fail to encode. Segment count is not checked and MUST NOT be ([RFC-0019](…)).`

**Error registry: unchanged.** `tr::path::invalid` (`0x0021`, `warn` / `permanent`,
`docs/reference/05-protocol-tlvs.md:524`) keeps its code, severity, disposition and meaning. This
RFC's error-surface delta is **zero** — contrast RFC-0006, which had to amend `0x0010`'s meaning.

### 4.5 Code

One comparison, no restructuring.

- Delete `core/include/libtracer/path.hpp:34` (`kMaxSegments`) and update the `parse` doc comment at
  `:105`.
- Delete `core/src/path.cpp:110`:
  `if (++p.segments_ > kMaxSegments) return std::unexpected(status_t::INVALID_PATH);`
  — **retain `++p.segments_` as a bare increment**: `segments_` (`path.hpp:126`) backs the public
  `segment_count()` accessor (`path.hpp:121`), so `sizeof(path_t)` is unchanged.
- The `kMaxPathBytes` check at `path.cpp:112` already sits immediately after `emit_name` and becomes
  the sole segment-count bound with no reordering.
- The reserve guard at `path.cpp:99` (`want <= kMaxPathBytes`) is **untouched** — this RFC keeps
  `kMaxPathBytes`, so the ~1 KiB reserve ceiling on a 16 KB node is preserved exactly. (A design that
  dissolved the byte cap would have had to re-derive it; see §9.1.)
- Comment-only sites needing the corrected wording: `core/include/libtracer/graph.hpp:580-586`,
  `core/src/graph.cpp:1851`, `docs/modules/path.md:15` and `:69-72`.

## 5. The arithmetic — measured, and the maintainer's redundancy hypothesis is refuted

The structural observation ("the segment cap may be redundant with the byte cap") is
**arithmetically correct and conclusionally wrong**. Computed over the shipped encoding rule
(`docs/reference/05-protocol-tlvs.md:348`, each `NAME` costs `4 + len`; `emit_name` →
`emit_tlv(…, type_t::NAME, opt_t{}, name)`, `core/include/libtracer/tlv_emit.hpp:59-61`):

| segment length S | encoded cost/segment | max segments in 1024 B | bytes for exactly 32 segments |
| --- | --- | --- | --- |
| 1 | 5 | **204** | 160 |
| 5 | 9 | 113 | 288 |
| 27 | 31 | 33 | 992 |
| **28** | **32** | **32** | **1024 — exact co-bind** |
| 29 | 33 | 31 | 1056 — 32 unreachable |
| 64 | 68 | 15 | 2176 |

**The caps cross at S = 28** (`32 × 32 = 1024` exactly). Below it the 32 binds, by up to **6.4×**;
at or above 29 bytes/segment the byte cap binds alone and 32 segments were *already* unreachable.
Neither cap dominates, so they are **not redundant**. Deleting the 32 loosens precisely the case
the ruling cares about — short mount segments — and loosens nothing where a real memory bound
already applies.

Two consequences that must be said plainly rather than smuggled in:

1. Removing `kMaxSegments` raises the depth ceiling **32 → 204**, not 32 → unbounded.
2. The naive worst case people fear — `32 × 64 = 2176` bytes — was **already unreachable**: the
   byte cap rejects it at 1024. The segment cap never contributed to the memory bound it appears to
   defend. A `path_t`'s worst-case payload is 1024 bytes before this RFC and 1024 bytes after.

## 6. Blast radius — counted, not estimated

### 6.1 Conformance vectors: **zero existing vectors change**

Independent TLV walk of every `input.bin` / `reject.bin` under `tests/conformance/vectors/v1/`:
**41 vector directories, 12 carrying ≥1 `PATH`** — reproducing RFC-0018 §6's independently-derived
"12 of 41" exactly, which cross-validates the instrument. The **deepest `PATH` in the entire corpus
is 5 segments** (`fwd/fwd-routed-multihop`, `/net/board/can0/ow/sensor`); longest `NAME` 9 bytes;
largest `PATH` body 40 bytes. Nothing is within an order of magnitude of any cap, and neither of the
corpus's two `reject.bin` cases tests a cap (both are reserved-bit cases).

A removed maximum cannot invalidate bytes that never reached it. This matters because `v1.md:141-142`
makes *changing* an existing vector's bytes a spec change in its own right while *adding* one
explicitly is not. Sharp contrast with RFC-0018, which must rewrite all twelve because it changes
the body grammar — the "harder" amendment is the cheaper one, exactly as the sequencing ruling
predicted.

**One vector added:** `path/path-deep-204` — 204 single-byte segments, `length` exactly 1020, an
`input.bin` every core must round-trip byte-exactly. It pins the new ceiling as a positive,
decodable case, which is the only shape the harness supports.

### 6.2 Code: one enforcement site

`kMaxSegments` is enforced at exactly one site (`core/src/path.cpp:110`) and **sizes nothing**. No
array, reserve, or bitset in `core/` is dimensioned by it. The one cap-sized allocation in the tree
is `core/src/fwd_router.cpp:62`, `std::array<std::byte, graph::kMaxSegmentBytes * tr::net::kMountPeekMax>`
= 64 × 4 = 256 B of stack — keyed to the *byte* cap and the mount peek width, neither touched here,
and double-guarded (`fwd_router.cpp:67`, `:81`). `kFwdHead1Cap = 64`
(`core/include/libtracer/fwd_frame_view.hpp:397-398`) is derived from header widths, not from
`kMaxSegmentBytes` — a coincidence of value, so no forward-hop buffer needs re-deriving.

**This section previously concluded "lifting the segment cap cannot overflow anything." That was
wrong, and the audit behind it was too narrow: it enumerated arrays, reserves and bitsets, and
never audited the C stack.**

Four subtree walks in `core/src/graph.cpp` are **genuine self-recursion**, one frame per graph
level, and recursion depth *is* segment count:

| walk | recurses at | frame (x86-64, `-O2 -fstack-usage`) |
| --- | --- | ---: |
| `graph_t::bump_subtree_listeners` (`:521`) | `:524`, inside the `for_each_child` lambda | 208 B/level |
| `graph_t::retire_subtree` (`:363`) | `:382` | 144 B/level |
| `collect_subscribed` (`:426`) | `:428` | 80 B/level |
| `graph_t::mark_subtree_acl_dirty` (`:675`) | `:677` | 32 B/level + lambda |

All three reachable from the wire: `bump_subtree_listeners` via `admit_subscriber`
(`:1285`, whose comment calls it *"the single admission step (ADR-0049): every door lands here"*),
`retire_subtree` via the RETIRE op, `mark_subtree_acl_dirty` via the `:acl` write. At 204 levels
that is ~42 KB of stack for the first walk alone.

**This exposure exists TODAY and is not created by this RFC** — §2.3 establishes that
`ensure_vertex` counts no segments, so `kMaxSegments` never bounded the wire path. What this RFC
removes is the last *local-only* speed bump. It is nonetheless a hard **prerequisite**: tracked as
[#690](https://github.com/avatarsd-llc/libtracer/issues/690), and this RFC MUST NOT land before it.

The fix shape is in the same file: the iterative heap-backed stack machine at
`core/src/graph.cpp:1865-1875` (`std::vector<work_t>` + `detail::try_push_back` →
`status_t::BACKPRESSURE`). Two of the four return `void`, so an error channel changes their
signatures — that is the open design question in #690, not something this RFC decides.

Note that `vertex_t` carries an immutable `parent_` link (`core/include/libtracer/vertex.hpp:2394`,
accessor `:983`), so an O(1)-memory parent-link traversal is also available and would need no error
channel at all. #690 should weigh both.

No C++ test asserts any of the four caps: `core/tests/path_test.cpp` is 82 lines with zero cap
assertions. Deleting the check breaks no C++ test — itself a finding.

### 6.3 Documents: 5 normative clauses in 3 files, plus counted ripple

**Normative (must land atomically, §4.4):** `docs/spec/v1.md:58`, `:95`;
`docs/reference/03-addressing.md:30`, `:31`; `docs/reference/05-protocol-tlvs.md:275`, `:276`, `:352`.

**Descriptive ripple (not normative — `reference/13` is absent from v1.md §3's incorporation list,
which contains only `reference/01`, `reference/05`, and `reference/03` §path syntax):**
`docs/reference/13-network-formation.md:285-288` (§8), `docs/reference/01-data-format.md:259`
(§6.4), `docs/modules/path.md:15`, `:69-72`, `tests/conformance/README.md:14`,
`docs/design/config/00-configuration-space.md:257-268` (§9.2).

**RFCs to reconcile:** `docs/spec/rfcs/0018-packed-path-segments.md:207` (§7.2 — a direct
contradiction). RFC-0016's erratum at `:229-254` needs no edit: it was written to survive this
change.

**Changelogs** (per CLAUDE.md): `core/CHANGELOG.md`, `bindings/rust/CHANGELOG.md`,
`bindings/typescript/CHANGELOG.md`.

### 6.4 `reference/01:259` gets simpler

That sentence's parenthetical exists *solely* to explain why the addressing 32 survived RFC-0006:

> **No nesting-depth constant exists at any layer of this protocol.** (Scoped deliberately: this
> sentence read "no depth constant exists at any layer", which contradicts the **addressing**-layer
> segment limit …)

With the segment cap gone the parenthetical can be **deleted**, restoring the plain sentence PR #685
had to narrow.

### 6.5 Bindings — Rust is a breaking API change, TypeScript regresses unless fixed in the same train

- **Rust** enforces `MAX_SEGMENTS` at `src/tlv_builders.rs:256` and `src/path.rs:53` (both
  **encode**-side) and at `src/path.rs:89` — which is **DECODE**-side, on an already-parsed `&Tlv`,
  and is therefore an interop regression rather than a cosmetic removal (§10(b)); asserts
  it in `tests/conformance_vectors.rs:257` (a 33-segment `TooManySegments` case that this RFC
  **inverts into a must-accept**); and **re-exports `MAX_SEGMENTS` as public API**
  (`src/lib.rs:56`). Removing it breaks downstream compiles — deprecate rather than hard-delete
  within the release, and note it in `bindings/rust/CHANGELOG.md`.
- **TypeScript** enforces the 32 as a bare literal (`packages/client/src/tlv.ts:125`) and has **no
  1024-byte total-path check at all** — an exhaustive grep of `packages/client/src` for `1024`
  returns nothing. Delete the 32 without adding the byte check and the TS core loses *every* size
  bound on a `PATH`, which is strictly worse than today. **The TS byte check is a required part of
  this train, not a follow-up.**

## 7. Precedent, and where this diverges

### 7.1 RFC-0006 is a weaker precedent than it looks — do not overclaim it

RFC-0006 did **not** replace an encoder-side limit with a receiver-side one. Commit `4547d74` shows
the clause it deleted was

> `- **Maximum nesting depth**: 32. Deeper TLVs MUST be rejected with `ERROR{tr::tlv::nesting_too_deep}`.`

— a **receiver**-side MUST-reject — and its replacement is also a receiver-side MUST-reject carrying
the same code `0x0010`. RFC-0006 changed the **threshold**, not the **side**. It never faced the
"remove an encode-time MUST" problem, and there is therefore **no in-repo precedent** for what
`v1.md:95` requires. This RFC builds that argument from scratch.

Two further reasons not to lean on it:

- RFC-0006's "no conformance floor is introduced" rests on a **structural floor** with no addressing
  analogue: "Protocol-defined TLV shapes … nest ≤ 5 **by construction**, so every conforming
  receiver parses every protocol frame at any budget." Every `FWD` carries a deployment-shaped
  address, so demoting segment count to a *capability* would be a strictly larger claim. **This RFC
  does not demote it to a capability** — it converts it to a byte count identical on every peer.
- RFC-0006's promised conformance machinery was **never built**. `tests/conformance/HARNESS.md`
  contains no declared decode-arena budget and no nesting vector exists; the resource-keying lives
  only in `core/tests/frame_test.cpp:94-118`. Citing "resource-keyed vectors" as a working precedent
  would be citing a three-week-old paper promise.

The transferable half of RFC-0006 is narrow and this RFC uses only that: *state the bound in terms of
a real resource, and leave the error registry's identity alone.*

RFC-0007 / ADR-0051 is closer in **shape** (a cap deleted with nothing replacing it) but its cap was
never normative — ADR-0015:26 calls `kMaxDispatchDepth` "an *implementation* decision about `core/`,
not a wire/protocol commitment … the exact value is implementation-defined."

### 7.2 RFC-0018 — direct contradiction, surfaced not glossed

`docs/spec/rfcs/0018-packed-path-segments.md:207` states, in its normative wire-encoding section:

> - Total path length ≤ 1024 bytes; segment count ≤ 32. *(Unchanged, and both are already normative
>   in `docs/reference/03-addressing.md` §path syntax.)*

That is a direct contradiction of this RFC. RFC-0018 is a merged **draft**, so this RFC proposes the
edit rather than leaving the two documents disagreeing:

> - Total path length ≤ 1024 bytes, measured as the `PATH` `length` field. **No segment count
>   limit** ([RFC-0019](0019-path-depth-bounded-by-bytes.md), which lands first per the
>   2026-07-30 sequencing ruling). Note that this RFC's packing moves the byte-derived segment
>   ceiling from 204 to 512 — a consequence of the encoding change, which this RFC therefore owns
>   in its Compatibility section.

**RFC-0018 moves the derived ceiling and must own it.** Packing drops the per-segment floor from 5 B
to 2 B, so the same 1024 admits **512** segments instead of 204, and the co-bind crossover moves
from S = 28 to S = 31. Nobody would have decided that; naming it here is the point.

**One free win worth recording:** under RFC-0018's packing, one `u8` length byte per segment equals
one `/` separator per segment, so the packed-body count and the string-form count become
**numerically identical** — the unit divergence §4.3 is forced to pin would retire permanently.

## 8. The `reference/13` diameter consequence

Deleting the cap leaves `docs/reference/13-network-formation.md:285-288` asserting a constant that no
longer exists. The replacement is already ratified and needs no new argument — ADR-0038's 2026-07-30
erratum: `dst`-monotonicity plus the hop's frame budget, "no counter, no state and no cap". Proposed
rewrite of that bullet:

> - **Depth is capped by the route, and by the frame.** A `FWD` frame's `dst` names every hop and is
>   consumed monotonically — `dst` strictly shrinks by at least one segment per hop, so a delivery
>   terminates in at most `len(dst)` hops with no counter, no state and no cap
>   ([ADR-0038 §erratum](../adr/0038-…)). `len(dst)` is bounded by the `PATH` `length` field and by
>   the link's frame budget, not by a segment constant ([RFC-0019](../spec/rfcs/0019-path-depth-bounded-by-bytes.md)).

Its sibling bullet at `:289` (loop-freedom) stands on `dst`-monotonicity alone and is unaffected.

**Historical note worth recording:** a third `32` already died silently. ADR-0014's `MAX_HOPS`
dissolved with the `ROUTER` mechanism — `v1.md:53` now calls `0x0D` "a reserved codepoint with no
mechanism" and `hop_count` is absent from `core/`. `kMaxSegments` inherited the network-diameter role
**by accident**, from a constant designed as a string-parser guard.

## 9. Alternatives considered

### 9.1 Remove the 1024-byte cap too, keying extent to a receiver resource — **rejected**

The literal RFC-0006 shape cannot be written for an **encode-time** MUST: an encoder has no
observation of a receiver's memory, and `max_frame` is a *local, receive-side* cap
(`core/include/libtracer/transport_vertex.hpp:127`, "Only tightens, never raises") that is never
advertised on the wire. There is no negotiated byte budget to point an encoder at. Making it work
would require:

- relocating the obligation to the receiver, which means **building** the ingress admission check
  that `reference/05:292`'s "or admitted" clause only describes (§3) — the precise failure mode
  ADR-0038's erratum had to retract;
- a peer-relative rejection, which the error registry structurally cannot express:
  `tr::path::invalid` is `permanent` = "don't retry this request", and the same address succeeds
  against a bigger peer (`docs/reference/05-protocol-tlvs.md:512-515`). A new code **and** a new
  disposition concept;
- re-deriving the `path.cpp:99` reserve guard, whose failure mode on a 16 KB node is a 4× amplification
  of the caller's string length into the allocator.

It is also **more than the ruling authorized** ("do the 32 amendment first"), and it would take the
ceiling to ~13107 (the `u16` `length` field width) rather than 204.

### 9.2 Make the address bounds per-target configuration — **foreclosed by name**

`docs/design/config/00-configuration-space.md` §"What is deliberately not configurable":

> - **The address bounds** — maximum segment length, maximum path length, maximum segment count,
>   maximum field depth ([03-addressing.md](../../reference/03-addressing.md)). These are normative
>   and incorporated by the spec: making them per-target would let one node accept a path another
>   must reject, which is an interoperability failure dressed as a RAM saving. A node that wants a
>   smaller bound is asking for a *profile*, and that is a spec question.

This pre-answers RFC-0018 §12 open question 2 with **no**, and this RFC does not contradict it.

**Contradiction surfaced, not fixed here** (house rule): six lines later the same document says
"Field depth is the worked example: resource-keyed rather than configurable" — but
`core/include/libtracer/path.hpp:36` is a bare `inline constexpr kMaxFieldDepth = 8` with literal
comparisons at `core/src/path.cpp:129` and `core/src/op_resolve_walk.hpp:380`. It almost certainly
means TLV *nesting* depth (RFC-0006). Separate erratum; flagged because §9.2 leans on the first
bullet.

### 9.3 A conformance floor plus a per-node capability above it — **rejected**

Inverting the 1024 from a sender ceiling into a receiver floor is coherent, but `CONTEXT.md:270`'s
`_Avoid_` list names "conformance minimum depth" by name, and it would create a region above the
floor where two conforming implementations legitimately disagree about what is encodable. §10's
monotone-widening argument is strictly stronger: it keeps the encodable set a **closed-form byte
predicate** with no capability region at all.

### 9.4 An erratum narrowing v1.md §3's incorporation of `reference/03` — **rejected, but it found something**

It cannot deliver: `v1.md` restates the 32 **twice in its own normative body**, at `:95` (inline
MUST) and via `:102` ("Validates per the rules of [reference/03]", unqualified), both inside §3.1,
which line 71 declares normative. De-incorporating would leave both standing, and striking them
changes what a conforming implementation does — GOVERNANCE.md:53 excludes exactly that from the
erratum instrument.

**But the investigation found a real provenance defect, recommended as a separate one-line
erratum.** RFC-0001 §A.2 — the accepted RFC that created normative-by-incorporation — reads: "§3
(Wire format) **normatively incorporates** `reference/01-data-format.md` and
`05-protocol-tlvs.md`; those two files' status line changes `descriptive` → `normative`."
`reference/03` appears in neither §A.2 nor its acceptance note; ADR-0007 also names only 01 and 05
and treats 03 as subordinate. Confirmed in tree today: `reference/01` and `reference/05` each carry
`> **Status**: normative, v1 … (incorporated by docs/spec/v1.md §3 per RFC-0001 §A.2)` — and
`reference/03` carries **no status banner at all**. The third §3 bullet entered in drafting commit
`ccca475`, citing ADR-0007 as authority ADR-0007 does not grant. Do **not** de-incorporate
`reference/03` wholesale: §path syntax also carries the EBNF, the reserved characters and the index
form, so that would de-normativize the path grammar itself — strictly worse than the cap.

## 10. Compatibility

**Does this break protocol-v1 implementations? No — it is a monotone widening.** The encode
predicate goes from

```
(segments ≤ 32) ∧ (each NAME ≤ 64) ∧ (length ≤ 1024)
```

to

```
(each NAME ≤ 64) ∧ (length ≤ 1024)
```

Dropping a conjunct can only **enlarge** the accepted set **on the segment axis**.

**Two corrections to what this section used to claim, both material.**

**(a) It is NOT a pure widening — the byte axis NARROWS.** The predicate above is stated in the
*post-pin* unit, which assumes the very change §4.3 makes; proving compatibility over it is
circular. Stated honestly over the units in force **today**, `docs/reference/03-addressing.md:30`
and `docs/reference/05-protocol-tlvs.md:275` measure the 1024 in a byte set that **excludes** TLV
headers, while `core/src/path.cpp:112` includes each 4-byte `NAME` header. Pinning the unit to the
`PATH` TLV `length` field therefore tightens the surviving cap by roughly **2.5×** against a
literal reading of the reference text. Concretely: **a 32-segment path of 29-byte segments is
conforming under `03-addressing.md:30` today and is non-conforming after this RFC.** No shipped
implementation emits it (C++ and Rust both already count the encoded unit), so nothing in the wild
breaks — but it is a real normative narrowing and belongs in this section, not buried in §4.3.

**(b) A decoder DOES enforce the 32, and it must be fixed in the same train.** This section used to
read "no decoder changes at all", generalising a `core/src/*.cpp`-only audit to every
implementation. `bindings/rust/src/path.rs:89-91` rejects `tlv.children.len() > MAX_SEGMENTS` on an
**already-decoded** `&Tlv`, and all three of its callers are receive-side:

- `bindings/rust/src/fwd.rs:279` — `fwd_dst_path`, a FWD route straight off the wire
- `bindings/rust/src/fwd.rs:289` — `fwd_src_path`, the **accumulated return route**
- `bindings/rust/src/structured.rs:141` — `subscriber_target_path`, a peer's subscribe target

`fwd_src_path` is the sharpest: `src` grows by the full mount run at **every** hop, so a Rust node
on a long route would fail to read the return path even where `dst` is short. Left in place, this
RFC ships a live interop regression rather than a widening. **`path.rs:89-91` must be deleted in
the same train** (the `MAX_SEGMENTS` re-export at `src/lib.rs:56` may stay, deprecated, for source
compatibility — that is orthogonal). §6.5 cites this line but files it beside the encode-side
sites without distinguishing them; that is corrected there too.

Beyond that decode site, the caps genuinely were not enforced at decode —
`docs/reference/05-protocol-tlvs.md:278-297` states this deliberately ("decode is not where the rule
lives"), and §2.3 confirms the C++ wire path counts nothing.

**Two implementations still agree on what is encodable**, and for a stronger reason than today: the
encodable set stays defined by **closed-form byte predicates over the candidate bytes**. No
configuration, no injected resource, no capability negotiation, no peer-relative quantity, no sender
disclaimer, no declared budget, no new error code. An encoder asks two questions it can answer alone
— is every `NAME` payload in `1..64`, and is the `PATH` `length` ≤ 1024 — and both are pure functions
of the bytes. That is the property the *current* text lacks: "total ≤ 1024 bytes" is not decidable,
because the three governing texts count three different byte sets (§4.3).

**The uncomfortable half, stated rather than buried.** "Removing an encode-time MUST lets conforming
implementations disagree about what is encodable" describes the **present**, not the proposal:

1. The three cores already disagree. TypeScript enforces the 32 and the 64 but has **no** 1024-byte
   check (§6.5) — a TS client can already build a `PATH` that C++ and Rust reject as `INVALID_PATH`.
2. The MUST at `v1.md:95` has **zero** conformance-vector coverage and *structurally cannot have
   any*: `v1.md:132-142` defines conformance as honouring MUSTs plus round-tripping every
   `input.bin`, and an over-budget `PATH` must still **decode**. An encode-time MUST is not
   expressible in a round-trip vector suite. "Passes conformance" never implied "checks the caps",
   for anybody, ever.

This RFC narrows that divergence by one clause and makes the surviving clause the one both remaining
checks share.

**Migration.** Deployed devices need no action: nothing they emit becomes illegal, and nothing they
must accept grows (a `path_t`'s worst-case payload is 1024 bytes before and after, §5). Encoders that
hardcode a 32 keep working — they merely become more restrictive than the spec requires, which the
new MUST NOT in §4.2 then makes non-conforming; that is the one behaviour change and it is
deliberate.

## 11. What is UNMEASURED

Labelled explicitly, per the standing rule that a quantitative claim names its instrument or is
labelled unmeasured.

- **Cost of a deep path.** No bench exists for a 204-segment path. `path_t::parse` is linear and
  `fwd_router`'s mount scratch is keyed to `kMaxSegmentBytes × kMountPeekMax` rather than to
  `kMaxSegments`, so I **expect** no cliff — expectation, not measurement. Instruments that would
  settle it: (a) a `path_t::parse` microbench at depths 1 / 32 / 204; (b) `bench_transport_iov`,
  which `fwd_frame_view.hpp:428-431` already names as the one that would catch the deep-mount cliff.
  Neither has been run for this change. Note `bench_hop_chain` is recorded as **confounded with its
  depth claim refuted** and must be repaired before it is cited at all.
- **Flash delta.** One `if` removed plus one `inline constexpr` that never occupied storage.
  Direction is down; **magnitude unmeasured**. The instrument is the existing rv32 size census
  (census on rv32, never on host) — do not bank a "beat"; the sign flips inside the ranges.
- **Static RAM delta: zero, by construction, not by measurement.** `segments_` is retained so
  `sizeof(path_t)` is unchanged, and §6.2 verified nothing is dimensioned by `kMaxSegments`.
- **Everything numeric in §2.2, §5 and §6.1 is arithmetic or a corpus census, not performance.** The
  hop table is arithmetic over the encoding rule at `reference/05:348`; a real multihop leg has never
  been run at depth. The 41/12/5-segment corpus figures come from a TLV walk of every `input.bin` /
  `reject.bin`, cross-checked against RFC-0018 §6's independent count.

## 12. What would falsify this RFC

Concrete, checkable conditions. If any holds, the design is wrong and should be withdrawn or
reshaped.

1. **A per-level resource is found on the address path.** The whole argument rests on `kMaxSegments`
   bounding no allocation. If any array, reserve, bitset or stack frame in `core/`, the bindings, or
   the dissector turns out to be dimensioned by segment count, then deleting the cap is a
   memory-safety change and needs a resource bound, not a deletion. *Check:* `grep -rn kMaxSegments`
   plus an ASan run of `path_test` / `fwd_multihop` at depth 204.
2. **A deep path shows superlinear cost.** If a `path_t::parse` microbench or a `fwd_router` multihop
   leg at depth 204 shows worse than linear scaling, or a per-hop heap allocation appears, the
   ceiling should be lowered deliberately rather than derived.
3. **The `length`-field unit turns out to be the wrong pin.** If a third core (or a future transport
   binding) counts the 1024 in string-form bytes for a reason, §4.3's pin is wrong and the erratum
   should land separately, first, with the amendment sequenced behind it.
4. **The monotone-widening claim fails somewhere.** If any conforming behaviour depends on a `PATH`
   being *rejected* for segment count — a vector, a test, a deployed check — then the change is not
   purely permissive. *Check:* the corpus census in §6.1 says no vector does; a cross-core run of
   `tests/conformance/run-all.py` before and after must stay green with only additions.
5. **The maintainer rules that an encode-time MUST cannot be deleted without a named replacement.**
   §7.1 establishes there is no in-repo precedent for the deletion. If that gap is judged
   disqualifying, this design fails and the fallback is §9.3's floor-plus-capability — which costs a
   `CONTEXT.md` carve-out against its own `_Avoid_` list, or §9.1's receiver bound, which costs a new
   error code and an unbuilt admission check.

## 13. Discussion

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window",
this is an **amendment**: RFC plus maintainer approval, comment window **waived by default** while
solo-maintained. The waiver's revert trigger has not fired — `docs/implementations.md` still lists
`_(none yet)_`. Invoke the window explicitly if outside input is wanted.

**On acceptance**, `CONTEXT.md:269` §Resource bound gains this RFC in its ratified-instances list.
That list currently enumerates nesting depth (RFC-0006), the deleted dispatch cap (RFC-0007 /
ADR-0051) and transport max-frame, and does **not** record the addressing segment cap as an
exception — the cap has been running as an unrecorded exception to a rule stated without
qualification.

**Acknowledged doctrinal residue.** This RFC converts two magic constants into one; the survivor —
1024 — is still a hardcoded number, which `CONTEXT.md:269` forbids without qualification. The claim
made here is that this is the right stopping point: a byte count maps to real memory and is identical
on every peer, whereas a segment count maps to nothing; the configurable escape is foreclosed by name
(§9.2); and a receiver-resource formulation costs a new error code, a new disposition class and an
unbuilt admission check (§9.1). That is a judgment, not a proof, and it is the most likely place a
reviewer will push back.

**Latent defects noted and deliberately not bundled** — each deserves its own erratum, and bundling
would make an already-large amendment unreviewable:

1. The immutability trigger is spelled three different ways in three governing documents — `v1.md:148`
   "immutable once **finalized**", `GOVERNANCE.md:45` "immutable once **released**",
   `docs/spec/index.md` "Once a version is **published**". None has fired, so this RFC's target-version
   conclusion is robust under every reading.
2. `v1.md` incorporates `reference/03` at **two different widths**: §3 scopes to "§path syntax" (one
   H2, lines 8–103) while §3.1.3 line 102 says "Validates per the rules of [reference/03]" with no
   qualifier, pulling in the whole document including Address-shift slicing and Path canonicalization.
3. `v1.md:63-64`'s precedence rule is **degenerate** when both conflicting documents are incorporated
   — it yields "incorporated wins" on both sides. PR #685 (commit `77adb66`) hit exactly this and
   resolved it editorially. This is why §4.4's edits must land atomically.
4. The `reference/03` incorporation has no RFC authority (§9.4).
5. `docs/design/config/00-configuration-space.md` contradicts itself within six lines on field depth
   (§9.2).
