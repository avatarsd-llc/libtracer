<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0018 — Packed path segments: a `PATH` body becomes length-prefixed records

| Field | Value |
| ---- | ---- |
| **RFC** | 0018 |
| **Title** | Packed path segments: a `PATH` body becomes length-prefixed records |
| **Status** | **draft** (2026-07-30) — *supersedes the 2026-07-30 "Folded path segments" draft of this number* |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-30 |
| **Comment window** | waived by default while solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"); invoke explicitly if outside input is wanted |
| **Tracking issue** | [#680](https://github.com/avatarsd-llc/libtracer/issues/680) (scope changes with this revision) |
| **Target spec version** | v1 (DRAFT — no stable release, so the "immutable once released" clause has never triggered). **Amends** `docs/spec/v1.md` §3.1.1 and §3.1.2, `docs/reference/05-protocol-tlvs.md` §`0x06`, and **reverses** [RFC-0004](0004-remote-operation-addressing.md) §"Considered options" line 211 and its §133 "PATH is untouched" statement. |

> **Numbering note.** 0012 and 0015 remain skipped for the ghost history
> [RFC-0016](0016-composed-branch-read.md) records. This RFC takes **0018**, the next unused
> number after [RFC-0017](0017-element-addressing-value-plane-index.md).

---

## 1. Summary

A `PATH` TLV's body is today a sequence of child `NAME` TLVs, each costing a 4-byte TLV header to
carry three bits of real information. This RFC changes the `PATH` body to a self-delimiting
sequence of **`[u8 len][utf8 bytes]` records**:

```
06 40 2D 00  02 00 03 00 "net"  02 00 09 00 "ws-client"  …   ; today, dst = 49 B
06 00 1E 00  03 "net"  09 "ws-client"  …                     ; packed,     dst = 34 B
```

`PATH` keeps its type code (`0x06`) and its outer TLV envelope. It flips to `opt.PL=0`, because
its body stops being a child sequence. The per-segment walk becomes `p += 1 + body[p]`: one byte
load and one add, with no option decode, no `header_t` construction, and no `expected<>`/
`optional<>` round trip.

**This RFC does not add a label mechanism.** The previous draft of this number proposed a `FOLD`
child — a minted per-link `u16` mixing with `NAME` children inside a `PATH`. §3 of that draft is
withdrawn. §9 explains why on measurement, and §8 keeps a two-byte escape reserved so the idea
costs nothing to revisit.

## 2. What changed from the withdrawn draft, and why

The draft was written assuming the wire was near-frozen, so it reached for a mechanism that could
be bolted beside the existing encoding. That constraint is lifted. Three of its load-bearing
claims did not survive being measured.

### 2.1 The depth-scaling premise is refuted

The draft's §2.2 argued that folding *partially* is the interesting case because "each hop is
parsing a *longer* address" and "cost tracks **address depth**."

**Measured, this is false.** A transit hop is flat in address depth
(`scratchpad/depth.cpp`, host x86-64, `-O3 -DNDEBUG`, warmed out of the idle P-state, p50 of 11
reps × 8192 iterations, four consecutive runs):

| dst segments | frame | `peek_fwd_dst_segs` | `rebuild_fwd_forward` | full hop |
| ---: | ---: | ---: | ---: | ---: |
| 4 | 68 B | 33–34 ns | 16–17 ns | 85–88 ns |
| 7 | 94 B | 33–37 ns | 16–17 ns | 85–88 ns |
| 10 | 120 B | 33–34 ns | 16–17 ns | 84–87 ns |
| 13 | 146 B | 33–35 ns | 16–17 ns | 85–86 ns |

A **3.25× deeper address in a 2.1× larger frame costs the same hop.** The cause is in the code and
is not subtle: `peek_fwd_dst_segs` stops at `kMountPeekMax = 4`
(`core/include/libtracer/fwd_frame_view.hpp:94`, loop guard at `:161` and `:194`), and
`rebuild_fwd_forward` forwards the residual `dst` as **one untouched span**
(`fwd_frame_view.hpp:604-605`, emitted once by `gather` at `:470`). Segments 5..13 are never
parsed at any hop.

Two corrections follow. The draft's "**eight** `read_fwd_header` calls" is **seven**: three
structural (FWD, op `VALUE`, dst `PATH`) plus at most four capped segment reads. And an
address-shortening scheme's per-hop ceiling is **bounded and constant**, not proportional to
depth — which removes the only argument the draft had for preferring a partial fold over the
whole-route label §E.1 already ships.

### 2.2 The headline latency delta is ~40% a reply-leg confound

The draft's §2.1 and §6 rest on `bench_hop_chain`'s 4-hop `chain-path` vs `chain-label` delta.
Reproduced today on `main` (in tree, three runs × three reps): `chain-path` p50
**1315–1396 ns**, `chain-label` p50 **384–428 ns**, the bench's own SUMMARY reporting
**944–951 ns** per warm operation (the draft says ~1042; it did not reproduce).

The two arms do not differ only in how the destination is named. `chain-path` injects
`FWD{op=WRITE}`, and a `WRITE` terminus **always** assembles a `RESULT` reply
(`core/src/op_resolve_walk.hpp:713-718`) which then routes four hops home. `chain-label` injects
`COMPACT`, and the `COMPACT` terminus **never** replies (`core/src/fwd_router.cpp:864-898`).
Instrumented (`scratchpad/probe_chain.cpp`, reverse counters added to `wire_link_t`):

| arm | forward | **reverse** |
| --- | ---: | ---: |
| `chain-path` | 588 B / 4 frames | **459 B / 5 frames** |
| `chain-label` | 72 B / 4 frames | **0 B / 0 frames** |

The path arm transits **eight** hops per operation; the label arm transits four. Adding a
`chain-path-noreply` ablation (the terminus's upstream peer cut, so the reply is still assembled
but never relayed) to the same binary, three runs × three reps:

| arm | warm p50 |
| --- | ---: |
| `chain-path` | 1337–1396 ns |
| **`chain-path-noreply`** | **965–986 ns** |
| `chain-label` | 384–428 ns |

The reply relay leg is **~379 ns** — about **40% of the published 944 ns delta**, and consistent
with four transit hops at the independently measured 85–87 ns. The residual ~572 ns is an **upper
bound** on the addressing term, not a measurement of it: it still mixes frame type, the terminus
arena decode `COMPACT` skips, and reply assembly. Honest per-hop, that is **≤143 ns**, not 236.

`chain_t::wire_bytes()` (`bench/bench_hop_chain.cpp:214-218`) sums the `down` array only, so the
published "588 B across 4 hops" also excludes 459 B of reply.

### 2.3 The `146 B → 18 B` row prices a different frame

18 B is `COMPACT{VALUE label, VALUE u32}` = 4 + 6 + 8 (`build_compact`,
`bench/bench_hop_chain.cpp:146-150`) — a frame with **no FWD envelope, no op, no `dst`, no `src`**.
A `FOLD` keeps all four. A fully folded FWD is `4 + 5 + (4+6) + 13 + 8` = **40 B**, and the
draft's own motivating *partial* fold (one hop's three segments) is **126 B**. The 8.2× wire row
belongs to RFC-0004 §E.1's `COMPACT`, which ships today; it was never available to `FOLD`.

### 2.4 The §6 breakeven does not reproduce

`chain-path` cold measured **13,024 / 13,475 / 13,886 / 14,167 / 15,459 / 15,820 / 16,421 /
17,944 / 30,427 / 31,729 / 32,712 ns** across eleven runs; `chain-label` cold **19,436–23,765 ns**.
Cold is a **single un-batched timed operation**, and in several runs `path.cold` *exceeded*
`label.cold`, making the printed breakeven negative and clamped to 0. One run printed the draft's
"~5 operations"; others printed 0. **The ~5–6 µs binding cost and the ~5-operation threshold are
not reproducible as stated and are withdrawn from this document.**

## 3. Motivation, measured

What survives is real, and it is what this RFC now proposes to take.

**Latency.** The `dst` walk is a genuinely large term and is *not* an already-free one:
`bench_forward_demux` axis 3 (in tree, run today) prices the fixed hop at **87 ns**, the
cacheable resolve leg at **38 ns (43.7%)** and the per-frame rebuild at **19 ns (21.8%)**. A
packed arm carrying the **same rejections and the same `fwd_pre_t` fill** as the production peek
(`scratchpad/packed.cpp`, one binary, warmed, arms interleaved and order-alternated, three runs):

| | TLV `PATH` (today) | packed `PATH` |
| --- | ---: | ---: |
| walk, 4-segment dst | 34 ns | **19–21 ns** |
| walk, 13-segment dst | 33–34 ns | **19–21 ns** |
| frame, 1 hop | 68 B | **53 B** |
| frame, 4 hops | 146 B | **104 B** |

That is **~14 ns off a ~34 ns walk (~41% of the walk, ~16% of an 85–87 ns hop)** and **−42 B
(−29%) on the four-hop frame** — with no binding, no minting, no advertise, no release protocol,
no staleness, and no dependency on [#603](https://github.com/avatarsd-llc/libtracer/issues/603).

Both the latency and the byte figures are **flat in depth**, which is the honest shape of this
change: it is a constant per-hop win and a per-segment wire win, not a depth-scaling one.

**Wire.** Exactly 3 bytes per segment, deterministically. The byte arithmetic reproduces the
bench's own published sizes before being used on the proposal: `4 + 5 + (4 + 4×26 + 8) + 13 + 8`
= **146 B**, matching `bench_hop_chain`'s reported originator frame; packed gives **104 B**,
matching the probe's measured frame.

**Correctness — and this is the part that actually justifies spending a wire revision.** See §4.

## 4. What this deletes: a conformance hazard, at its root

`ADR-0062` §"Considered options" (`docs/adr/0062-…md:34`) rejected byte-keying the registry
verbatim:

> **Encode the registry key as raw TLV bytes and `memcmp`** — the originating idea. **Rejected on
> conformance:** a `NAME` may legally be emitted with `opt.LL=1`, and `opt` also carries `TS`/`CR`,
> so the same path has multiple valid encodings […] A raw-byte key silently fails to route a
> conforming peer.

That rejection is correct *given today's encoding*. A packed record has **no per-segment option
byte**, so there is exactly one spelling per address and the premise dissolves. Concretely, these
collapse:

- `is_canonical_name` (`core/src/tlv_arena.cpp:20-22`) tests exactly `h.opt == opt_t{}`;
- the `canonical_path` flag (`core/src/tlv_arena.cpp:101`, contract at `tlv_arena.hpp:56-63`);
- `path_lookup_key`'s re-emit fallback (`core/src/op_resolve_walk.hpp:545-557`), whose fast path
  already returns `path.body()` when canonical — under this RFC that becomes **unconditional**,
  and the span-alias into the graph's vertex-map key is guaranteed rather than tested.

This is not hypothetical tidying. A live misroute already lived in that machinery:
[#436](https://github.com/avatarsd-llc/libtracer/issues/436) — before the fix, a
`PATH{VALUE "sensor"}` was silently rewritten to `/sensor` and the stored value returned. The
enforcement note at `docs/reference/05-protocol-tlvs.md:279-299` and the conformance vector
`path/path-value-children-illegal` exist because of it.

**A second, still-unfixed locus of that same bug is closed by this RFC.** `wire::path_key`
(`core/src/frame.cpp:166-178`) emits **every** child's payload through `wire::emit_name`
with no type check — the #436 fix landed only in the arena tier. Its wire-facing caller
`resolve_route_vertex` (`core/src/fwd_router.cpp:949-951`) resolves an ADVERTISE route, so a peer
sending `PATH{VALUE "sensor"}` binds a label to `/sensor` today while the arena tier correctly
answers `INVALID_PATH`. With a packed body there are no child TLVs to mistype, and the divergence
is structurally impossible. (If this RFC is rejected, that locus still needs fixing on its own —
it is a bug against a published rule, not a consequence of this proposal.)

## 5. Wire encoding

`PATH` (`0x06`) — **amended**:

- `opt.PL` MUST be `0`. The body is not a child sequence.
- The body is zero or more **segment records**, each `[u8 len][len bytes of UTF-8]`, in order.
- `len` MUST be in `1..64`. `len == 0` is **reserved** (§8) and MUST be rejected by a resolver.
- Total path length ≤ 1024 bytes; segment count ≤ 255
  ([RFC-0023](0023-path-segment-cap-repriced-32-to-255.md)). *(Unchanged by this RFC, and both
  are already normative in `docs/reference/03-addressing.md` §path syntax. Under the packed body
  the 255 becomes a **binding** cap for short segments — `1 + len` per record admits 512
  segments in 1024 bytes — so this RFC additionally owns the `path/path-deep-255-packed` vector
  and the crossover note, RFC-0023 §4.2.)*

`docs/reference/05-protocol-tlvs.md:274` — *"Each child MUST be a NAME TLV (`type=0x02`); other
types are invalid in PATH context"* — is replaced by the body grammar above. This is an
**amendment**, not an erratum: the normative wire surface changes.

**On the `u8` length bound.** The 64-byte per-segment limit is pre-existing
(`docs/reference/03-addressing.md:29`, restated normatively at `docs/spec/v1.md:95`), so `u8` is
derived from the spec rather than invented. **But this RFC hardens it**: today the limit is a
one-line `inline constexpr kMaxSegmentBytes = 64` at `core/include/libtracer/path.hpp:30` and the
TLV length field is `u16`, so a per-target build could raise it; after this change the encoding
caps it at 255 forever. That is a real, irreversible narrowing and it is stated here rather than
buried. **A LEB128/varint length is explicitly not the escape hatch** —
`docs/reference/01-data-format.md` §"Rejected designs" already rejected varints ("branchy parser,
unpredictable payload offset, hostile to streaming and SIMD"). If the 64-byte limit must ever
rise, the correct instrument is §8's reserved escape, not a variable-width length.

**`NAME` (`0x02`) survives.** It is still used for SETTINGS keys and `:schema` labels
(`docs/reference/05-protocol-tlvs.md` §`0x02` "Where it appears"). This RFC removes `NAME` from
`PATH` bodies only; it does not delete the type or its decoder.

### 5.1 What is preserved, and how it is checked

- **Zero-copy strip-K.** `rebuild_fwd_forward` advances `p += 1 + len` exactly as well as it
  advances by `seg_h->total`, and still emits the residual `dst` as one span with a fresh 4-byte
  `PATH` header (`fwd_frame_view.hpp:604-605`, `gather` at `:470`). No length table is rewritten.
- **`key_view_t`'s byte-prefix-implies-ancestor invariant** (`core/include/libtracer/key_view.hpp:12-17`).
  Records are self-delimiting and parsed left-to-right, so a shared byte prefix parses identically
  in both keys and every prefix boundary is a record boundary. `/a` = `01 'a'` is a prefix of
  `/a/b` = `01 'a' 01 'b'`; `/ab` = `02 'a' 'b'` correctly is not. **Falsifier:** the existing
  `key_view` ancestor/descendant tests must pass unmodified against packed keys.
- **Injectivity.** One spelling per address, by construction — strictly better than today, and it
  is what `docs/reference/02` depends on.
- **FWD-level skip-unknown.** A `PATH` is still an ordinary self-describing TLV that a generic
  walker skips by length.

### 5.2 What is lost

- **Skip-unknown *inside* a `PATH`.** Today an unrecognised `PATH` child is skippable by its own
  TLV length. After this change the body has one reserved value (§8) and nothing else. This is a
  real reduction in forward-compatibility surface and belongs in the ledger, not in the benefits
  column.
- **Per-segment `TS`/`CRC` trailers.** Nothing emits them — `encode_mount_tlv`
  (`fwd_frame_view.hpp:656-673`) always writes `opt = 0` — and the `PATH`'s own trailer still
  covers the whole address.
- **A non-`NAME` child in a `PATH`.** Already forbidden (`05-protocol-tlvs.md:274`); the vector
  `path/path-value-children-illegal` exists precisely to reject it. That vector becomes
  **unrepresentable** and must be retired with this RFC (§6).
- **Arena child nodes for `PATH`.** With `opt.PL=0` the arena emits no per-segment child nodes, so
  every `path.children()` consumer on a `PATH` must be rewritten to walk the packed body. This is
  the largest single code cost and the draft's cost section did not name it.

## 6. Costs and blast radius

Counted, not estimated.

**Conformance vectors — 12 of 41** carry a `PATH` (measured by walking every `input.bin` /
`reject.bin` as TLV): all nine under `fwd/`, both under `path/`, and `tlv-types/subscriber-path`.
All twelve need new bytes. `path/path-value-children-illegal` is **retired** rather than
rewritten. Per `tests/conformance/README.md:50` and ADR-0028 the C++ reference blesses the new
bytes; adding a vector needs no manifest edit (`run-all.py` discovers by `rglob`), but *changing*
existing bytes is a spec change — which is what this RFC is.

**Sequencing hazard:** `fwd/fwd-routed-multihop`'s `expected.json` is contested in
[#419](https://github.com/avatarsd-llc/libtracer/issues/419) ("needs a maintainer call"). That
call must be made before, not during, this rewrite.

**C++ core.** 5 files under `core/src` + `core/include` reference `type_t::PATH`; 40 `emit_name`
call sites. The named loci: `fwd_frame_view.hpp` (both `peek_fwd_dst_segs` overloads,
`peek_fwd_first_dst_seg`, the `rebuild_fwd_forward` strip walk, `encode_mount_tlv`),
`fwd_router.cpp` (`resolve_mount_segs`, `resolve_mount_at`, `on_advertise`, `resolve_route_vertex`),
`op_resolve_walk.hpp` (`parse_fwd`, `path_lookup_key`), `tlv_arena.cpp`, `frame.cpp`
(`path_key`), `graph.cpp` (`parse_subscriber_tlv`), `child_registry.hpp` (`encode_mount_name`),
`path.cpp`, `key_view.hpp`.

**Vertex-map key.** The graph key **is** the `PATH` body (`path.hpp:114,124`; `graph.cpp:1639`;
`vertex.hpp:541` subscription keys), so it moves with the encoding. A `/sensor/temp` key goes
18 B → 12 B. **RAM effect: UNMEASURED**, and this project's own record says a static-RAM census
belongs on rv32, not host — and that `std::vector` has no small-buffer optimisation
(`vertex.hpp:547`), so logical bytes are not heap bytes.

**Bindings.** Rust: 5 files (`path.rs`, `fwd.rs`, `tlv_builders.rs`, `structured.rs`, `lib.rs`) —
`path.rs:95-98` currently returns `TypeMismatch` on a non-`NAME` child and must be rewritten to a
body walk. TypeScript: `client/src/tlv.ts`, `fwd.ts`, `topology.ts`, `client.ts`, `index.ts`, plus
`core/src/codec.mjs`'s type table. **The codec cores get *cheaper*** — `05-protocol-tlvs.md:281`
already says the codec does not enforce the child-type rule; with a packed body there is nothing
to not-enforce.

**Wireshark.** `tools/wireshark/libtracer.lua`'s `PATH` handler (`:357-370`) joins `type == 0x02`
children; it must walk the packed body. Its checked-in `sample.pcap` needs regeneration.

**Docs.** `v1.md` §3.1.1 clause 1 and §3.1.2's NAME-child bullets; `05-protocol-tlvs.md` §`0x06`
and its enforcement section; ~34 lines across 12 files carry literal `02 00 …` NAME-header bytes.
This project has been burned by touching only the changed page; the whole doc set needs the sweep.

**MCU.** rv32 code-size delta **UNMEASURED**. Direction is plausibly favourable — a byte-load walk
replaces `parse_header`'s option decode and `header_t`/`optional<fwd_hdr_t>` construction — but
the packed walker is **additive** to the `NAME` decoder, which survives for SETTINGS and `:schema`,
so it is not a replacement. **rv32 latency: UNMEASURED.** Both named instruments are host-only.
Against `project_dual_target_esp32_and_hpc`, that is a real gap.

**Rope cursor: UNMEASURED.** A `[u8 len]` record can straddle a link boundary exactly as a TLV
header can. The packed walk reads single bytes so it should stitch at least as cleanly, but
`rope_cursor` was not exercised. `core/tests/fwd_rope_forward_test.cpp` is the gate.

## 7. Normative interaction: `v1` §3.1.1 and §3.1.2

The draft's §7 spent 25 lines arguing that §3.1.1 does not block a `FOLD`. With the freeze lifted
that argument is unnecessary, and it was also wrong on its own terms: §3.1.1's byte-equivalence
sentence (`v1.md:82`) is explicitly a **relation between two API spellings** — "a write through a
path handle MUST be indistinguishable on the wire from a write through the equivalent string-form
path, **after both are canonicalized**" — not a statement about the emitted frame.

This RFC does not need that clause narrowed. It needs it **re-pointed**:

- **§3.1.1 clause 1** cites "the canonical PATH TLV (`type=0x06`, `opt.PL=1`, NAME children …)".
  `opt.PL=1` and "NAME children" become the packed body grammar. The byte-equivalence guarantee
  is **preserved verbatim and is easier to hold**, because a packed `PATH` has exactly one
  spelling per address — injectivity is now structural rather than conventional.
- **§3.1.2**'s bullets *"Each NAME child is type `0x02`, `opt.PL`=0"* and *"total payload size MUST
  match the sum of child NAME TLV sizes"* become the record grammar. Pre-encoded `.rodata` PATH
  TLVs are unaffected in kind and **shrink by 3 bytes per segment**.
- **§3.1.5**'s inline-NAMEs / reference-NAMEs strategies both still satisfy §3.1.1; its
  "wire bytes MUST be identical regardless of strategy" guarantee is unchanged.

**Reversal on the record.** [RFC-0004](0004-remote-operation-addressing.md):133 states *"`PATH`
(`0x06`) is **untouched** — its 'children MUST be NAME' invariant and every existing parser
stand"*, and :211 rejected extending `0x06` because it *"breaks `PATH`'s 'children MUST be NAME'
invariant and touches every existing `PATH`/`SUBSCRIBER`/`ROUTER` parser."* [RFC-0017](0017-element-addressing-value-plane-index.md)
re-affirmed it. **This RFC reverses that rejection**, and does so explicitly rather than silently.
The reversal is justified on two grounds RFC-0004 did not have: the freeze is lifted, and
`05-protocol-tlvs.md:279-299` (added by #577, after RFC-0004) establishes that the codec tier does
not enforce the child-type rule at all — so the "every existing parser" blast radius is the
resolver plus two thin binding builders, not three reimplementations. That is smaller than
RFC-0004 assumed, and it is still large (§6).

## 8. The reserved escape — and why `FOLD` is deferred, not dead

`len == 0` is reserved. It is the natural place for a future non-literal segment, and reserving it
now costs one branch that the walk already performs as a bounds check.

The recommended shape, **if** a label inside an address is ever justified, is
**`00 <u8 kind> <u8 len> <len bytes>`** — self-delimiting, so a node that does not implement
`kind` can still *skip* it by length rather than hard-rejecting. This deliberately differs from
the naive `00 <u16 label>` form: on an MCU, the node least likely to implement folding is exactly
the node that must still step over it.

**`FOLD` is deferred because its motivating measurement was refuted (§2.1) and its remaining case
is not yet distinguishable from the mechanism that already ships.** RFC-0004 §E.1's `COMPACT`
already collapses a whole route to 18 B and measures 384–428 ns at four hops. `FOLD`'s claimed
advantage was the *partial* fold, justified entirely by depth-scaling — and the transit hop is
flat in depth. What `FOLD` would still buy over `COMPACT` is UNMEASURED, and **cannot** be
measured today: `bench_hop_chain` has no FWD-vs-FWD arm, and building one requires implementing
`FOLD`. That circularity is itself a reason to defer.

Nothing here forecloses it. Reviving it needs: (a) #603 closed, (b) a `chain-fold` arm that is
FWD-shaped, replies, and is compared against `chain-path` rather than against `COMPACT`, and
(c) that arm beating `chain-path` by more than the ~14 ns/hop this RFC already takes for free.

## 9. Alternatives considered

- **The withdrawn `FOLD`-inside-`PATH` draft.** Rejected on §2.1–§2.4: the depth premise is
  refuted, the headline delta is ~40% a reply-leg confound, the wire row priced a `COMPACT`, and
  the breakeven does not reproduce. It also gates on #603 — a live misdelivery bug — where this
  RFC does not.
- **Keep RFC-0004 §E.1 as-is; whole-route labels only.** *Not* rejected. §E.1 ships, works, and
  measures well. This RFC is **complementary**: §E.1 is the amortised path for a hot flow, and a
  packed `PATH` is the unconditional win on the cold one-shot path that §E.1 §168 deliberately
  makes pay full route. They compose.
- **Fixed FWD preamble** (`[op][flags][u16 dst_len][u16 src_len]`, no nesting). Measured faster
  and smaller in probe form, but the measured arm performed **no** bounds or type validation and
  did not fill `fwd_pre_t`, so its number is the cost of a program that cannot ship. It also
  cannot encode 3 of the 9 shipped `fwd/` vector shapes (the `FIELD` selector's extent, `REPLY`'s
  `kind`, `AWAIT`'s timeout), and it forks the one wire grammar ADR-0048 §1 exists to keep
  singular. Rejected — but recorded, because the *direction* was the best-performing arm anyone
  built and it deserves a fair re-measurement if this RFC's win proves insufficient.
- **Varint / LEB128 segment lengths** for headroom past 64 bytes. Rejected by
  `docs/reference/01-data-format.md` §"Rejected designs" already; §5's escape is the correct
  instrument instead.
- **Content-addressed folding.** Unchanged from the draft: a hash used as an address makes a
  collision a silent misroute, reversing #660's filter-never-a-decision ruling.
- **Do nothing.** The baseline is cheaper than it looks — per-hop CPU is O(1) in address depth
  (§2.1) and it holds zero routing state. What it is not is *correct*: the encoding-variance
  hazard of §4 is live in `wire::path_key` today.

## 10. What would falsify this RFC

Stated as runnable experiments, not as sentiment.

1. **The primary falsifier.** Add a packed arm to `bench_forward_demux` axis 3, next to
   `fwd-demux-resolve`, in **one binary**, on the **same frame**, filling `fwd_pre_t` and
   performing the same rejections. **If the packed arm does not beat the literal arm on the
   resolve leg, this RFC is void.** My probe says 34 → 19–21 ns; if the in-tree arm disagrees,
   the in-tree arm wins.
2. **The rebuild leg on packed frames.** `rebuild_fwd_forward` (~19 ns, 21.8% of the hop) was
   **not** measured on a packed frame. If it regresses by more than the peek gains, the net is
   zero or negative and the RFC is void.
3. **The graph-side key change.** No instrument prices moving the vertex-map key. If `graph.cpp`
   and `key_view.hpp` cost more than the walk saves — `bench_libtracer`'s resolve axes are the
   place to see it — the RFC is void. This is the falsifier the withdrawn draft *stated* and could
   not run; it is still not run.
4. **`key_view` invariant.** If the existing ancestor/descendant tests do not pass unmodified
   against packed keys, the RFC is void as written.
5. **Rope cursor.** If `core/tests/fwd_rope_forward_test.cpp` cannot be made to pass with a packed
   body across a straddled link boundary, the RFC is void.
6. **MCU.** If an rv32 `-Os` size census shows the packed walker *plus* the surviving `NAME`
   decoder costs more flash than today, the RFC needs re-scoping for the constrained target even
   if the host numbers hold.

Falsifiers 2, 3, 5 and 6 are **currently unrun**. This RFC should not leave draft until 1–4 are
green and 5–6 are at least measured.

## 11. Prerequisites and sequencing

1. **[#419](https://github.com/avatarsd-llc/libtracer/issues/419)** — the contested
   `fwd-routed-multihop` `dst` form needs a maintainer call *before* twelve vectors are rewritten.
2. **This RFC**, in slices: (S1) the encoding + C++ core + `key_view`; (S2) vectors + both
   bindings + dissector; (S3) the docs sweep and the `v1` §3.1.1/§3.1.2 amendment.
3. **#603** is **not** a prerequisite of this RFC — that dependency belonged to the withdrawn
   `FOLD`. It remains a prerequisite of §8's revival path, and it remains a live bug worth fixing
   on its own.

`wire::path_key`'s missing child-type check (§4) should be filed and fixed **independently**, now,
against the published rule in `05-protocol-tlvs.md:279-299` — it is a bug today whether or not
this RFC lands, and it should not be bundled into a wire revision.

## 12. Open questions

1. **Does `src` pack too?** Yes, trivially and by the same clause — `src` is a `PATH`. The
   accumulated return route is where depth actually lives (`src` grows by the full three-segment
   mount run per hop, ADR-0061 erratum), so the wire saving there is larger than on `dst`. It is
   listed as open only because **no bench times a reply leg today**; §2.2's ablation shows how to
   build one.
2. **Does the segment-length bound belong in `config.hpp`?** ADR-0068/ADR-0070 made build config
   plain C++, but `kMaxSegmentBytes` is still a bare `constexpr` in `path.hpp`. This RFC freezes
   it at ≤255 on the wire; whether the *lower* per-target bound should become a traits member is a
   separate call.
3. **Retiring `path/path-value-children-illegal`.** The vector becomes unrepresentable. The rule it
   pins (an illegally-spelled address answers `ERROR{tr::path::invalid}`) still needs *some*
   expression — a `len == 0` or over-long-record case is the natural replacement.
4. **Does `NAME` stay in the core fast-track range** once it no longer appears in `PATH`? It is
   still used by SETTINGS and `:schema`, so yes — but the registry note at
   `05-protocol-tlvs.md` §`0x02` "Where it appears" needs updating.

---

### Appendix — instruments

| Figure | Instrument | Status |
| --- | --- | --- |
| 87 ns hop; 38 ns resolve; 19 ns rebuild | `bench/bench_forward_demux` axis 3 | in tree, run today |
| 1315–1396 / 384–428 ns, 588 B / 72 B, cold spread | `bench/bench_hop_chain` | in tree, run today (11×) |
| 151–155 / 107–110 ns, 79 B / 18 B | `bench/bench_originate` | in tree, run today |
| depth flatness (85 ns at 4 and at 13 segments) | `scratchpad/depth.cpp` | **out of tree — must land as a bench axis** |
| 459 B / 5 reverse frames; `chain-path-noreply` 965–986 ns | `scratchpad/probe_chain.cpp` | **out of tree — must land as a bench arm** |
| packed walk 19–21 ns; 104 B frame | `scratchpad/packed.cpp` | **out of tree — this is falsifier 1** |
| rv32 code size, rv32 latency, RAM, rope tier | — | **UNMEASURED** |

All host figures: x86-64, `g++ -O3 -DNDEBUG -std=gnu++23`, Release, arms in one binary, warmed out
of the idle P-state, p50 over ≥9 reps, order-alternated. Three of the six rows above come from
probes that do not yet exist in the repository; **no number in this document should be republished
until its probe has landed as a bench arm.**
