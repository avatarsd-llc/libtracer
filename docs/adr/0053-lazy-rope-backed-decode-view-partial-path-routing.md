# Lazy rope-backed decode view: `tlv_view_t` and partial-path routing

Status: **accepted** (2026-07-06 — maintainer-ratified in design review; this is the
architecture that realizes the direction ratified in
[ADR-0052](0052-rope-aware-decode-sink-node-type.md): steady-state zero-copy rope decode
is a **hard requirement**, including WS/CAN reassembly, delivered by a **new lazy
decode-side type** rather than by mutating the existing sinks).

## Context

[ADR-0052](0052-rope-aware-decode-sink-node-type.md) established the crux: both existing
decode sinks name payload bytes with a borrowed contiguous `std::span`
(`tlv_t.payload`, `arena_tlv_t.wire`/`body`), and a rope-delivered payload breaks both
the contiguity and the single-borrowed-buffer assumption those spans bake in. The
maintainer ratified the zero-copy direction and rejected both flatten-once-as-end-state
and mutating the existing types. The root glossary
([CONTEXT.md](../../CONTEXT.md)) had in fact already committed to the same direction:
reassembly *is* rope construction ("zero-copy, never `memcpy`"), a contiguous copy is
legal only at a transport-**egress** DMA boundary, and a view boundary may fall anywhere
— including mid-TLV-header.

The parsing engine already exists: `grammar::rope_cursor` (#225) walks a rope through
the one grammar core ([ADR-0048](0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md)
§1), stitching straddled header fields and feeding CRC per-link, with the #227
differential fuzzer proving it byte-equivalent to the span cursor over 131k adversarial
splits. What was missing is the *output side*: the type a decoded rope frame becomes,
and *when* decoding happens at all.

The second ratified requirement reshapes that question. Delivery is hop-by-hop
source-routing ([CONTEXT.md](../../CONTEXT.md) §addressing): a hop needs only the next
PATH segment to forward a frame, and only the terminus needs the payload. Eagerly
decoding a whole frame at ingress therefore does work nobody asked for. The ratified
model is **partial decode**: parse only what the current consumer touches, hand the rest
onward as the rope region it already is.

## Decision

### 1. A lazy decode-side node: `tr::wire::tlv_view_t`

A new type in `tr::wire` (it interprets TLV meaning, so it lives at L2/L3 — same
placement rule as `view_as_tlv`) representing **one TLV whose bytes live in a rope**:

- Holds the decoded header facts (`type`, `opt`, lengths) plus **rope sub-regions** for
  the body and trailer — not copies, not spans: regions of refcounted `view_t` links.
- **Children are materialized on demand**, one `grammar::parse_header` over a
  `rope_cursor` per step: `child(i)` / iteration yields the next child's `tlv_view_t`
  by parsing only that child's header; the child's *body bytes are never touched*.
- **Nothing is decoded that is not accessed.** A structured payload handed to a
  consumer that does not descend into it is delivered as an opaque rope region — "up to
  the consumer to deal with it" is the contract, not a degraded mode.
- `materialize() → tlv_t` is the **single explicit copy point**: a consumer that wants
  the eager owning tree (or contiguous bytes) asks for it, and only then is anything
  flattened. `tlv_t` itself is untouched — it remains the eager encode-side /
  materialized representation and the cross-core parity type
  ([ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md)/[ADR-0032](0032-continuous-cross-core-perf-conformance-matrix.md)).

### 2. Ownership: the lazy view refcounts its links

`tlv_view_t` holds refcounts on the `view_t` links its region spans — the L1 substrate
already provides exactly this, so no new ownership machinery exists. A `tlv_view_t`
(and any sub-view handed to a next hop) keeps precisely its own links alive and may
outlive the transport read loop.

This is a **scoped revision of [ADR-0041](0041-terminus-arena-decode-span-contract.md)
§2**: the span arena (`tlv_arena_t`) remains pure-borrow ("holds structure only, never
owns bytes") for the contiguous non-owning tier; the lazy view tier **owns via
refcount** — that is what the owning delivery tier ([ADR-0042](0042-refcounted-receiver-seam-view-delivery.md),
generalized to ropes per the glossary) hands upward, and the two tiers do not mix.

### 3. Partial-path routing

Routing consumes the lazy view incrementally: a hop parses **only the PATH prefix it
needs** (the next NAME child), strips its segment, and forwards the remaining suffix +
payload **as rope sub-regions** — zero decode, zero copy of anything downstream of the
current hop. Final decoding of the payload happens at the terminus; a structured write
payload (several folded TLVs) may cross the entire graph undecoded.

### 4. Validation: fully lazy — bounds anchored at ingress, integrity at access

At ingress, the receiving edge does the **minimum that makes lazy access memory-safe**:
parse the root header fields and check `root.total == rope size`. That is a handful of
byte reads plus an O(links) size sum — **no byte-walk, no CRC, no structural descent**.
With the root bounds anchored, grammar containment does the rest lazily: every child
materialization checks that the child's declared extent fits inside its parent's region
(the cursor already enforces this, returning `err_t` on violation), so out-of-bounds
access is impossible without ever walking bytes that nobody reads.

**All integrity checking is deferred to access time, per TLV.** `opt.cr` is a per-TLV
trailer; the consumer that materializes a TLV verifies *that TLV's* trailer then — the
end-to-end argument placed in the codec. An ingress CRC pass was deliberately rejected:
it touches every byte, which is the same O(n) memory traffic as the flatten this design
exists to eliminate (a software CRC on an MCU costs *more* per byte than `memcpy`), and
it duplicates the link-layer integrity the transports below already provide (TCP/QUIC
checksums, CAN frame CRC).

**Partial-consumption is a feature, not a failure mode.** If one member of a TLV list is
corrupt, its own access fails with `err_t`; its siblings deliver normally; what to do
about the gap is the **final endpoint's policy** — the
[ADR-0051](0051-delivery-terminates-at-target-no-dispatch-limits.md) doctrine (delivery
terminates at the target; no middle-of-the-path gatekeeping) applied to integrity.
Every lazy accessor returns `std::expected` with the same `err_t` vocabulary as
`decode`.

The one hazard this places on endpoint authors is **torn application**: a structured
write whose members are one semantic transaction must not be applied member-by-member
as accesses succeed. The pattern for such an endpoint is **verify-all-then-apply** —
walk the payload's members (bounds + trailers) to completion *before* mutating any
state; the walk touches only bytes the endpoint was about to read anyway, so it costs
nothing extra over eager ingress validation while keeping the choice at the terminus,
where atomicity is actually definable. (Note an eager ingress CRC pass would *not*
provide this guarantee anyway: `opt.cr` is optional per-TLV, and a trailer-less frame
can still be semantically torn by a mid-list grammar error.)

`wire::validate_rope` (#225) remains available as an opt-in strict screen for
deployments that want full structural + CRC verification at ingress; it is not coupled
to the type.

### 5. Transports hand ropes upward

WS message reassembly and CAN frame-group reassembly construct ropes by chaining views
over their receive segments (the glossary's definition of reassembly) and deliver them
through the owning tier as-is. The pre-existing flatten-at-ingress paths are migration
debt, removed as each transport converts. A contiguous copy remains legal only where
the glossary already permits it: a substrate boundary the transport's own DMA cannot
span, at **egress**.

## Considered options

- **Mutate `tlv_t.payload` / `arena_tlv_t` to rope-capable bytes** ("A1") — rejected.
  It pays the cross-core parity break and per-node refcount cost on the *encode* side
  and the contiguous fast path, where ropes buy nothing; and eager decode is the wrong
  model anyway once routing is partial (§3). ADR-0052's rejection of its option (A)
  *as literally proposed* stands for this shape.
- **Eager rope arena** (`rope_arena_t`, ADR-0052's option (B)) — subsumed. A lazy view
  is strictly more capable (it can always walk fully; an eager arena can never un-walk),
  uses the same `rope_cursor`, and avoids a second arena + resolver surface.
- **Flatten-once at ingress** (ADR-0052's option (C)) — demoted to the **interim
  migration recipe** (`validate_rope → flatten → decode_into`) for paths not yet
  converted, and to the explicit `materialize()` escape hatch. It is not the end state;
  as end state it contradicts the glossary's "never `memcpy`" reassembly rule.
- **Hybrid validation (frame-CRC byte-walk at ingress, structure lazy)** — rejected:
  the CRC pass touches every byte, the same O(n) memory traffic as the flatten this
  design eliminates (software CRC on an MCU costs more per byte than `memcpy`); it
  duplicates the transports' link-layer integrity; it re-verifies nothing for the
  common trailer-less frame (`opt.cr` is optional); and per-hop re-checking would not
  localize the corruption classes that end-to-end trailers exist to catch. Torn
  application — the real hazard eager checking is imagined to prevent — is handled at
  the terminus by verify-all-then-apply (§4), the only place atomicity is definable.
- **Eager full structural validation at ingress** — not the default (every frame would
  pay a full parse when routing reads only a PATH prefix), but retained opt-in via
  `validate_rope`.

## Consequences

- **New public API** (`tlv_view_t`, ingress hook), each with a CHANGELOG note. No wire
  bytes change — this is decode-side only, so no spec RFC is required.
- **[ADR-0041](0041-terminus-arena-decode-span-contract.md) §2 contract text** gains
  the scoped carve-out of §2 above; the span arena's own contract is unchanged, and the
  Cortex-M0 span-only sentinel never links the rope TU (unchanged from #225).
- **Differential oracle extends**: a full lazy walk + `materialize()` over any rope
  split must equal `decode(flat)` node-for-node — the #227 fuzzer harness gains that
  mode. Late-surfacing grammar errors must also agree with `decode`'s `err_t`.
- **Cross-core**: `tlv_t` parity is untouched. TS/Rust cores add their own lazy view
  type independently, conformance-checked through the same wire vectors; until then
  they interop unchanged (wire bytes are identical by construction).
- **Emission symmetry** ([ADR-0048](0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md)
  §3 `compose → rope`): the encode-side dual (TLV-tree-of-views → flat rope, zero-copy
  egress until the DMA boundary) is now unblocked — it shares the ownership model
  ratified here and should be designed against `tlv_view_t`'s region vocabulary.
- **Migration order** (each step its own PR, each keeping the differential oracle
  green): ① `tlv_view_t` + ingress integrity screen + lazy-walk fuzzer mode;
  ② owning-tier delivery of ropes end-to-end (WS reassembly → rope); ③ CAN reassembly
  → rope; ④ partial-path routing consumes `tlv_view_t` at forwarding hops;
  ⑤ `compose → rope` emission; ⑥ remove interim flatten call-sites.
  *Steps ④–⑥ are refined by the 2026-07-06 amendment below (④ splits into ④a/④b).*

## Amendment (2026-07-06): rope-valued vertices, the lazy resolver, and the ratified ④ realization

Ratified in the second design review (after steps ①–③ landed), extending — not
revising — the decision above. Two poles calibrated every choice here: the **simplest
case** (a single scalar written into a vertex, where rope machinery must add nothing)
and the **hardest case** (a chunked media stream, e.g. RTSP frames arriving as
multi-link ropes from transport reassembly, written into one vertex and drained by a
consumer link-by-link — where any per-chunk copy is the exact copy this ADR exists to
delete).

### 6. Vertex values are ropes (L4)

The graph's stored value becomes `view::rope_t`; a `view_t` is the **single-link
trivial case** (§5's "delivers views and delivers ropes are one capability", applied to
storage). Concretely:

- `vertex_t`'s LKV slot, the `history_` stream ring, and subscriber fan-out hold rope
  values. The hot path is unchanged in kind: a lock-free `shared_ptr` swap per write, a
  refcount clone per fan-out edge — latency-flat regardless of link count.
- The vertex still **never parses its value** (a structured TLV payload is stored as
  opaque bytes; the consumer decodes — §1's contract). A multi-link value is stored as
  the rope it arrived as; the [ADR-0042](0042-refcounted-receiver-seam-view-delivery.md)
  §3 referenced store generalizes to *subrope into the slot* — zero copy on the store
  path at both poles.
- **Trivial-case cost guard**: `rope_t` gains small-buffer inline storage for 1–2
  links, so a single-scalar write allocates exactly what it allocates today. This is
  the acceptance gate for the change; the contention/latency benches must not move.
- **API migration is rename-and-migrate** (the #230 `view_receiver_t → rope_receiver_t`
  precedent, pre-1.0): `graph_t::write` takes `rope_t` (existing `view_t` callers
  compile unchanged via the implicit conversion), `read`/`await` return `rope_t`,
  subscriber callbacks receive `const rope_t&`. A consumer that needs contiguous bytes
  *visibly* calls the single-link accessor or `materialize()` — which form to consume
  in is the consumer's choice, made legible in the type. No silent-flattening parallel
  view API is kept.

### 7. The ratified ④ realization

- **Forward plane on the grammar cursor** — the peeks (`peek_fwd_first_dst_seg`,
  `peek_fwd_op`) and the forward-hop field walk become templates over
  `grammar::parse_header<Cursor>` (CRC `DEFER`), instantiated with `span_cursor`
  (single-link: byte-identical, zero-heap) and `rope_cursor` (multi-link). This deletes
  `fwd_router.cpp`'s private `read_header` — the last grammar fork outside the
  [ADR-0048](0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md) §1 core.
- **Multi-link egress iov** — the scatter-gather entry count is `~6 + link_count`,
  unbounded at compile time; a stack cap would be a synthetic limit
  ([ADR-0051](0051-delivery-terminates-at-target-no-dispatch-limits.md)). The
  multi-link instantiation builds its iov in a `std::pmr::vector` over the router's
  injected resource. *(Superseded by the erratum below: the container is now a
  `mem::block_array_t` over the router's `rx_`, so exhaustion drops the hop instead of
  throwing. The rest of this bullet is unchanged.)* This is a **scoped reading of
  ADR-0038 inv. #2**, not a revision:
  the "stack `std::array`, never a `std::vector`" invariant stays literally true on the
  span-tier hop where it was stated; the multi-link hop is owning-tier traffic that
  postdates it, bounded by the injected resource per the RFC-0006 doctrine.
- **Lazy resolver now, one templated walk** — the terminus does *not* materialize:
  the resolve walk is templated over a node-reader concept with two instantiations —
  the `arena_tlv_t` reader (span tier: byte-identical, still the MCU terminus and
  conformance oracle) and the `tlv_view_t` reader serving the **whole owning tier,
  single-link ropes included**, so the lazy path is exercised by every TCP/QUIC/WS
  frame, not only the rare fragmented ones. A second hand-written resolver was rejected
  as the same drift class ADR-0048 §1 eliminated in the grammar. Per-TLV verify-at-access
  (§4) lives once in the shared walk, gated to the view instantiation.
- **Value handoff** — a VALUE payload region subropes straight into the vertex slot
  (§6); fixed-width scalars the resolver itself reads (op discriminants, labels)
  stitch across links via the existing `rope_cursor` loads.

### Revised migration order (supersedes steps ④–⑥ of the original list)

- **④a — rope-valued vertices** (§6): L4 slot/history/fan-out + API
  rename + `rope_t` small-buffer storage. One documented interim: remote delivery of a
  multi-link value flattens inside the remote-delivery sink until ⑤.
- **④b — routing + lazy resolver** (§7): grammar-cursor forward plane, pmr iov,
  templated resolve walk; deletes the `on_frame_rope` pre-routing flatten.
- **⑤ — `compose → rope` emission**: data-path emission (subscriber delivery + reply)
  goes scatter-gather, deleting the ④a remote-sink flatten and the reply-path copies.
  Control-frame emission (ADVERTISE / COMPACT setup / NACK) stays eager `encode` —
  cold flow-setup paths, allowed to allocate per ADR-0039.
- **⑥ — flatten sweep**: remove remaining owning-path flatten call-sites; span-tier
  flattens are legitimate and stay.
- **Follow-up (post-⑥), WS unmask double copy**: first attempt unmasking directly into
  a fresh segment (no L0 change); only if the codec shape forbids it, admit an
  adopt-vector backend via an explicit [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md)
  amendment. Until it lands, WS RX pays one extra copy per fragment — the one
  documented exception to the steady-state zero-copy requirement.

## Erratum (2026-07-25): the lazy reader's ratified SCOPE is refuted by measurement

The amendment above ratified the lazy reader as serving

> the **whole owning tier, single-link ropes included**, so the lazy path is exercised by every TCP/QUIC/WS frame, not only the rare fragmented ones

**That is not what shipped, and measurement says what shipped is right.**

`fwd_router_t::on_frame_rope_impl` short-circuits a single-link rope onto the span path (`fwd_router.cpp:336`, commented "the pre-ADR-0053 view path, unchanged"), so the lazy reader serves **only** multi-link ropes — precisely the case the amendment said it must not be limited to. Nothing had measured which was correct: `bench_forward_demux` times the *forward* hop, which never resolves, and `bench_forward_heap` counts allocations without timing them.

`bench/bench_terminus_tier.cpp` closes that gap — the same frame through both public `op_resolver_t::resolve` overloads (the pairing `op_resolve_view_test` already uses as a correctness oracle), plus a `flat+arena` arm. Host p50 ns, arena decoding into a monotonic pmr buffer as production does (`fwd_router.cpp:532`):

| frame | arena L=1 | view L=1 | flat+arena L=2 | view L=2 | flat+arena L=8 | view L=8 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 53 B | **256** | 615 | 330 | 711 | 522 | 1286 |
| 4 KB | **299** | 747 | 480 | 794 | 761 | 1322 |
| 16 KB | **381** | 964 | 902 | 1064 | 1244 | 1703 |
| 64 KB | **886** | 1983 | 2373 | **2092** | 2870 | 2884 |

Allocations: arena **2 / 80 B**, view **8 / 157 B**; at L=8, flat+arena **14 / 944 B** vs view **33 / 2525 B**.

Three corrections follow:

1. **The single-link short-circuit is a fast path, not a missed one.** The eager arena wins **2.2–2.5× on latency AND on RAM at every frame size**. The premise that laziness should win on large payloads does not hold, because the arena never copies the payload — it references it — so its cost is O(nodes), not O(bytes). The §7 claim that routing single-link ropes through the view reader keeps the lazy path "exercised by every frame" was a testing-coverage argument, and it was paid for in latency on the hottest path in the system.

2. **Flatten is not the loser the ADR assumed.** `flat+arena` beats the rope walk at every size below the crossover, because flatten costs O(bytes) while the rope walk costs O(links). The crossover is **≈16–64 KB**.

   The **64 KB / L=2 cell is the one region where the lazy tier wins**, and it wins clearly: 2092 ns vs 2373 ns, **~12%**, reproducible across three runs (2115/2092/2116 vs 2373/2364/2428). The first revision of this erratum asserted that cell in prose with no row behind it; it is now measured and published, because any proposal to retire the tier turns on it. Its neighbour at 16 KB / L=2 goes the other way (902 vs 1064, flatten ahead ~15%), so the low-link crossover sits between them.

   Measurement caveat, because it changes how the columns compare: both rope arms build their rope **inside** the timed op, so `view` vs `flat+arena` is apples-to-apples, while the `arena L=1` column does no rope construction and is flattered relative to the rope columns. The single-link conclusion does not depend on it (that gap is 2.2–2.5×), but tier-vs-tier reasoning should use the multi-link columns.

3. **The lazy tier's real domain is narrow but real**: large, lightly-fragmented frames. It is not dead code — `tlv_view_t` has production consumers in `op_resolve_view.cpp`, `op_resolve_walk.hpp` and `fwd_router.cpp` — it is simply scoped an order of magnitude tighter than ratified.

4. **The DEVICE/RDMA justification for the lazy tier is false and is struck.** A heterogeneous ADR-0024 rope cannot be read by *either* tier: `tlv_view_t::over` rejects `!all_host()` with `FRAME_INVALID` (`tlv_view.cpp:15`), `rope_t::flatten` returns an empty `view_t` on the same condition (`rope.cpp:15`), and the router refuses a non-all-host rope before it reaches either (`fwd_router.cpp:350`). Lazy decode buys nothing for device-backed segments, and no future argument to keep the tier may rest on it.

**Decision recorded here:** the multi-link routing of ④b **stands**; the "single-link ropes included / every TCP/QUIC/WS frame" scope is **withdrawn**. `on_frame_rope_impl`'s short-circuit is the correct behaviour and must not be "fixed" to match the original wording.

**Adjudicated 2026-07-25 (agent workflow: three advocates, adversarially verified): the tier is KEPT.** The ruling favoured retirement on latency, RAM, throughput and minimalism — but gated it on measuring the 64 KB / L=2 cell first, with the explicit instruction to stop if the lazy view won that cell by a wide margin. It does, by ~12%, reproducibly. That cell is also the shape of the stated design centre — large, lightly-fragmented sensor payloads — so retiring would trade away the growth direction to win the cases already served by the single-link fast path. **Retirement is blocked on its own gate.**

What would reopen it: evidence that the 64 KB / L=2 advantage does not survive on the target (these are host numbers), or a decision that ≥64 KB frames are out of scope. The ruling's other findings are folded in above (corrections 2 and 4). Retiring would delete a reader, a resolver instantiation and their fuzz surface, at the cost of the ≥16 KB fragmented case — where the measured gap is small (2870 vs 2884 at 64 KB / 8 links) but favours the lazy path at low link counts. This erratum does not decide that; it records that the decision is now a measurement question, not an architectural one.

ADR-0055 inherits this correction: its "flatten sweep" reasoning is unaffected for the **owning** path it governs, but its framing of span-tier flattens as a concession rather than a legitimate optimum should be read in light of the numbers above.

## Erratum — §7's `pmr::vector` egress iov was a live instance of the #588 abort class. FIXED.

*(2026-07-27. **This erratum replaces an earlier one that got this backwards** and claimed the mechanism had been superseded. It had not; the check behind that claim looked for callers of `rope_t::to_iovec()` and found none, which is true and beside the point — the vector was built inline, not through that method. Resolved the same day by [#596](https://github.com/avatarsd-llc/libtracer/issues/596).)*

§7 sanctioned building the scatter-gather entry table as a `std::pmr::vector` drawn from the router's injected resource, and that is what `fwd_router.cpp`'s rope forward path did:

```cpp
// Rope source: a region may cross several links — gather into a pmr vector drawn
// from the terminus arena's resource (the forward hop still copies no payload).
std::pmr::vector<std::span<const std::byte>> iov{mr_};
rebuilt->gather(cur_src, [&](std::span<const std::byte> s) { iov.push_back(s); });
```

The entry count is `~6 + link_count()`, chosen by the **peer's** frame, on the **forward** path — which is behind no ACL and is not even the terminus. `push_back`'s growth went through `std::pmr`, so on a fragmented heap it threw, and on `-fno-exceptions` that is the link-wrapped `abort()` stub. Same class as [#588](https://github.com/avatarsd-llc/libtracer/issues/588), on the egress path instead of the decode path.

The **reply** egress was already a different story: it uses `rope_t::try_to_iovec`, which nothrow-reserves a plain `std::vector` and drops the reply on failure. That asymmetry — reply guarded, forward not — was the finding.

**The mechanism is now a `mem::block_array_t` over the router's injected `rx_`** ([ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md)); exhaustion returns `false` and the hop **drops the frame**. Dropping — rather than emitting the entries that did fit — is the only correct answer: a partial iov is a *truncated FWD on the wire*, which is worse than silence, and FWD is not delivery-guaranteed, so the sender retries.

§7's *reasoning* stands unchanged (no stack cap, no synthetic limit — the bound is a real injected resource per [RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md)). Only the container changed, and with it the failure mode: from `abort()` to backpressure.

`core/tests/fwd_rope_forward_test.cpp` pins it — the same maximally fragmented rope is forwarded through three seams: a `null_source()` (emits **nothing**, and the test asserts *nothing*, not *something short*), a bounded `bump_source_t` with room, and the default heap source; the latter two are byte-identical to the contiguous oracle. Reverting the fix fails the first two checks.
