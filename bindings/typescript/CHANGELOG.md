# Changelog — libtracer TypeScript packages

All notable changes to the npm packages under `bindings/typescript/`. The core
package follows [Semantic Versioning](https://semver.org/); see
[ADR-0033](../../docs/adr/0033-npm-subpackage-monorepo.md) for the
versioning/publish strategy.

## [Unreleased]

### Changed

- **BREAKING — `@avatarsd-llc/libtracer-client`: `encodeConnSpec` emits the creator-endpoint
  SPEC; `type` and `role` are gone** ([#1492](https://github.com/avatarsd-llc/libtracer/issues/1492),
  RFC-0014 Amendment 4 / S7). The device no longer accepts
  `write /net:children[] += SPEC{ type, name, config{ role, … } }`. The one wire door for
  creating a connection is now a WHOLE-VERTEX write to the owning module's creator endpoint,
  `write /net/<module>/conn <- SPEC{ name, config{…} }`, with removal spelled as the same
  endpoint written with a bare `NAME{<name>}`.

  - `ConnSpecOptions.type` and `ConnSpecOptions.role` are **removed**, as is the exported
    `ConnRole` type. The module segment of the path fixes both the transport and the link
    direction, so neither is expressible in the payload — a `role` pair on the wire is now an
    ignored unknown pair, not an override.
  - The emitted config pair ORDER changed to `kind`, `addr`, `port`, `peer_named`,
    `max_peers`, matching the shared `conn/create-via-spec` conformance vector, which
    replaces the retired `spec/conn-client-ws` as this encoder's byte pin. Readers are
    order-insensitive, so only byte-level comparisons are affected.
  - **`encodeConnSpec` no longer throws on a DIAL with no `addr`.** Without `role` the encoder
    cannot tell a DIAL (where a missing `addr` is fatal) from a LISTEN (where `addr` is
    ignored); only the endpoint the caller writes to knows. It now emits what it is given, and
    a DIAL module answers `TYPE_MISMATCH` — the same contract as the C++
    `tr::net::conn_spec_t`, which validates no key against any kind either.

  Migration: `encodeConnSpec({ type: 'client', name, role: 'dial', port, kind, addr })` and
  `client.writeField('/net', ':children[]', spec)` become
  `encodeConnSpec({ name, port, kind, addr })` and
  `client.write(['net', 'ws-client', 'conn'], spec)`. `writeField` with `":children[]"` is
  otherwise unchanged — generic (`stored_value`) in-band creation still uses it.

### Fixed

- **`@avatarsd-llc/libtracer-client`: FWD replies are correlated by the reply's own `src`, not
  by FIFO order** ([#1530](https://github.com/avatarsd-llc/libtracer/issues/1530)).
  `LibtracerClient` paired each inbound `FWD{REPLY}` with `pending.shift()`, which is only
  correct while at most one request is outstanding. Nothing on the wire promises reply order —
  a request FORWARDED across a mounted link answers in tens of milliseconds while a local one
  answers in about one — so a later request routinely overtakes an earlier one, and from the
  first overtake on **every caller received the previous request's answer**. Silently: the
  reply is a well-formed `RESULT`, just for somebody else's path, so a decoder returned `null`
  ("the device does not have that") or surfaced an `ERROR NOT_FOUND` meant for another read.

  The client now correlates on the reply's `src` — the responder's own endpoint, which RFC-0004
  §B requires a REPLY to carry — matching the oldest outstanding request whose `dst` ENDS WITH
  it (`dst` shrinks per hop, so the terminus echoes a suffix: `/net/ws-client/peer0/hw/variant`
  answers `src=hw/variant`). Requests the wire genuinely cannot tell apart — the same endpoint
  reached locally and through a mount — keep the FIFO tiebreak among themselves. An empty
  `src`, a `PATH_REF` src on a reply to a bound operation, or a `src` matching nothing
  outstanding falls back to the oldest entry, i.e. to the previous behaviour, so no correlation
  that used to work can break.

  **Behavioural change for callers:** keeping several one-shot ops in flight on one client is
  now correct. The documented workaround — serialising every op on one promise chain — is no
  longer needed and costs pipelining.

## [0.15.1] — 2026-08-23

No changes. These packages are an independent TypeScript implementation of the protocol —
they do not compile the C++ core — so neither 0.15.1's ESP-IDF boot fix (a C++-core
thread-identity primitive) nor its `compose_batch` addition reaches them. The wire format is
untouched, so a 0.15.0 client and a 0.15.1 node interoperate byte-for-byte. Released at 0.15.1
only because one tag publishes every package in lockstep.

## [0.15.0] — 2026-08-23

### Added

- **`DELIVERY_CLASS`, `deliveryClassBits()` and `deliveryClassOf()`** (`@avatarsd-llc/libtracer`)
  — bits 6–7 of the packed `delivery_policy` word (RFC-0025 §4.1): `0` conflate, `1`
  immediate, `2` batch, `3` stream. `CONFLATE` is `0`, so a conflate-class subscriber emits
  no `SETTINGS` child at all and is byte-identical to a pre-RFC-0025 one. The client is a
  codec: it carries the class, it does not honour it. The reserved range in
  `SubscriberOptions.deliveryPolicy`'s documentation narrows from 6–15 to 8–15 accordingly;
  no bytes change (RFC-0025 §4.1.2 clause 7).

## [0.14.0] — 2026-08-21

No API change to any of the four packages. One behaviour a client sees *from the
node* changed, and it is worth knowing about.

### Changed

- **Against a 0.14.0+ node, a module's `:children[]` no longer lists `conn`** (core
  RFC-0014 S4, [#492](https://github.com/avatarsd-llc/libtracer/issues/492)). The
  creator endpoint is now hidden from the listing at the source, so
  `/net/<module>:children[]` is exactly that module's member connections — and a
  module that carries none legitimately lists **nothing**. Module discovery is
  unaffected and always lived one level up, in `/net:children[]`; the endpoint stays
  addressable, and RFC-0014 §6's creatability probe (`read /net/<module>/conn:schema`)
  is unchanged.

  **`CONN_ENDPOINT_NAME` and `walkTopology`'s skip of it stay, and are still
  required** ([#1302](https://github.com/avatarsd-llc/libtracer/issues/1302)): a node
  older than S4 keeps listing the endpoint, and this client talks to those too. Nothing
  to change in caller code — a filter that was load-bearing against 0.13.x nodes is
  simply a no-op against 0.14.0 ones. The `mesh-testbed` expectations were updated to
  the post-S4 listings; `topology-completeness`'s stub is deliberately left as a
  **pre-S4** node, because that is the case the skip exists for.

## [0.13.0] — 2026-08-16

### Added

- **`CONN_ENDPOINT_NAME` (`"conn"`), and `walkTopology` skips it** ([#1302](https://github.com/avatarsd-llc/libtracer/issues/1302); RFC-0014 §3, core S2b). Every module's `:children[]` now lists its **creator endpoint** alongside its member connections, because the endpoint is a real vertex under the module. It is not a link: it is the write-only control a creator sends `SPEC`/`NAME` to, with no peer behind it — so the topology walk descended into it, minted a phantom node, and then reported a bogus gap for the subtree that was never there. The reserved name is now skipped, and exported so a caller filtering its own listing spells it the same way. Skipping is right regardless of RFC-0014 **S4** (which will hide the endpoint from that listing at the source): a node older than S4 keeps listing it, and this client talks to those too.

### Changed

- **A `PATH` (`0x06`) body is packed `[u8 len][utf8]` segment records
  ([RFC-0018](../../docs/spec/rfcs/0018-packed-path-segments.md),
  [#680](https://github.com/avatarsd-llc/libtracer/issues/680)) — BREAKING.**
  `encodePath` / `pathTlv` emit `opt.PL = 0` with the records in `payload` and no children, so
  every `FWD` `dst`/`src` and every `SUBSCRIBER` target this client builds is 3 bytes per
  segment shorter and does not round-trip against the previous version's vectors.
- **New `pathSegments(path)`** (exported from `@avatarsd-llc/libtracer-client`) — read a packed
  `PATH` back into its segments. It is **canonical/key context**, so it throws on the RFC-0018
  §5.4 `len == 0` escape, on ragged framing, and on a structured (`opt.PL = 1`) `PATH`: the
  escape is admissible in a frame a node is only forwarding and never in an address a caller
  is about to look something up by. `PACKED_ESCAPE_LEN` is exported alongside it.
  Callers that read `path.children` to get segments must switch to this function.
- **The segment count (255) is now the binding cap.** Packed, a one-byte segment costs 2 bytes,
  so the 1024-byte body budget admits 512 records; under the retired encoding the byte cap
  fired at 204 segments first.
- `nameTlv` still validates a segment and `NAME` is otherwise untouched — it remains the wire's
  string carrier for SETTINGS keys, `:schema` labels and `:children[]` members. RFC-0018
  removes `NAME` from `PATH` bodies only.

## [0.12.0] — 2026-08-14

### Added

- **`TYPE.PATH_REF_REVERSE` (`0x15`) and `isPathRefType()`** ([#1260](https://github.com/avatarsd-llc/libtracer/issues/1260); RFC-0024 §7.1 **amendment 2**). **Wire change.** The reverse-direction bound-path list a mint-flagged request accumulates has its own type code, and is identified by that code rather than by its position. Its body grammar is `PATH_REF`'s exactly, so the codec's structural gate now applies to both codes — a core that checked `0x14` alone would have accepted an unframeable `0x15` body. `decodeFwd` gained `reverse` (the parsed child, or `null`) and no longer mistakes a raw `PATH_REF` payload on a mint-flagged `WRITE` for the reverse list. New conformance vectors `fwd/fwd-reverse-mint` and `path-ref/reverse-len-not-multiple-of-8`.

## [0.11.0] — 2026-08-13

No changes. These packages are an independent native implementation of the protocol —
they do not compile the C++ core — and nothing in this release touched them; the core's
0.11.0 changes are C++ implementation surface, not wire surface. Released at 0.11.0
because one tag publishes every package in lockstep.

## [0.10.0] — 2026-08-12

No API changes. This package is an independent native implementation of the protocol —
it does not compile the C++ core — so the core's 0.10.0 wire-behaviour changes do not
reach it. What does land here: the reserved-segment set this tier already enforced is
now agreed cross-tier — #996 tightened the C++ `valid_segment` to the same
seven-character set `/ : . [ ] * ?`, pinned by the new `path/path-reserved-brackets`
conformance vector that `packages/client/test/vectors.test.mjs` now carries
bit-for-bit — and the topology walk's bus no-descend rule is restated on its true
grounds (#1147): every shipped bus transport names peers with legal segments, so the
per-peer hop is *expressible*; the walk withholds it by **policy** (unbounded peer
counts would multiply request volume at every bus crossed), not impossibility.
Doc/JSDoc and test only; no runtime behaviour changed. Released at 0.10.0 because one
tag publishes every package in lockstep.

## [0.9.1] — 2026-08-10

No changes. This package is an independent native implementation of the protocol —
it does not compile the C++ core — so nothing in 0.9.1 (a core codec bound, a WebTransport
allocation scoping, and CI/bench gating) reaches it. Released at 0.9.1 only because one tag
publishes every package in lockstep.

## [0.9.0] — 2026-08-10

### Added

- **`firstChild(tlv, type)`** (`@avatarsd-llc/libtracer-client`, `src/tlv.ts`) — the package's one
  child-by-type accessor, and the mirror of the Rust binding's `Tlv::first_child`: the first
  **direct** child with the given type code, or `null`. Both cores now answer "the X child of this
  structured TLV" the same way, so the open-coded `children[0]?.type === TYPE.X` that produced the
  divergence above has a named replacement.

### Fixed

- **`encode` no longer mints a `PATH_REF` frame this core's own `decode` rejects (#1004)**
  (`@avatarsd-llc/libtracer`, `src/codec.mjs`). The grammar has exactly one per-type structural
  rule — a `PATH_REF` body is a fixed-stride 8-byte record array, so `opt.PL` and `opt.LL` are
  both forbidden and the length is a bounded multiple of 8 (RFC-0024 §4.2/§4.3) — and `parseOne`
  has always enforced it. The generic `encode` did not: it serialized any `Tlv` verbatim, so a
  `PATH_REF` built with `opt.pl` even took the children branch and emitted per-child TLV
  framing. All four ill-formed shapes produced bytes this core answers with `FRAME_INVALID`.
  This package exports no `PATH_REF` builder, so a caller composes the object literal directly
  and `encode` is the only door. The four clauses now live in one module-private predicate that
  `parseOne` and `encode` share, rather than the encoder gaining a copy.

  The C++ core closed the same asymmetry in #886 and this core did not, so the three cores
  diverged on one input tree; that divergence is now closed on this side. No wire change, and no
  well-formed tree encodes differently.

  **API note — the failure mode is a new `encode` postcondition.** `encode` returns a
  `Uint8Array` and has no error channel, so refusal is spelled **emits nothing**: an ill-formed
  `PATH_REF` anywhere in the tree makes the whole call return an EMPTY array, and a refused TLV
  refuses its ancestors rather than being dropped into a frame that DOES decode, one component
  short. Empty is unambiguous — an accepted TLV always carries at least its 4-byte header, so no
  well-formed tree encodes to nothing. This matches the C++ core's `wire::encode` exactly. The
  client package's byte-returning builders (`encodeFwd`, `encodeValue`, …) pass that empty
  result through unchanged: they return a `Uint8Array` and have no error channel either, so
  refusal reaches a caller in the same documented spelling.

- **A peer's error was invisible to this core whenever its STATUS carried anything before the
  ERROR** (`@avatarsd-llc/libtracer-client`, `src/fwd.ts`). `replyErrorTlv` required the ERROR
  to be the STATUS's `children[0]`; a `kind=ERROR` reply whose STATUS leads with its optional
  DESCRIPTION — a child type reference/05 §`0x09` names explicitly — read back as
  `FwdError` code `0`, the same answer given for a STATUS carrying no ERROR at all. The typed
  code and its `tr::…` path were lost, so a diagnosable failure surfaced as `UNKNOWN(0x0)`.

  The rule is now the one the Rust binding already applied: **the first `ERROR` child of the
  STATUS, at whatever position**. reference/05 §`0x09` pins no order over a STATUS's children,
  and RFC-0002 §C pins position only one level down, inside the ERROR ("its first child is the
  identity") — a doc that states a positional rule exactly where it means one, and states none
  here. Emitters are unchanged and still write the canonical order (ERROR first); this is an
  acceptance rule, not a licence to emit. No wire bytes change, and every first-position ERROR
  reads exactly as before.

  Pinned by the new shared vector `fwd/fwd-reply-error-after-description`, asserted from **both**
  bindings — a divergence in either core's reader now fails that core's own suite. A test private
  to one language could not have caught this, which is why the drift survived (#878).

## [0.8.0] — 2026-08-06

### Changed

- No TypeScript-visible changes — version-lockstep release with `core` 0.8.0 (its new
  `for_each_vertex` / subscription-observer surface is C++ `core` API only).

## [0.7.1] — 2026-08-04

### Added

- **The RFC-0024 forwarding car's two vectors are pinned** in `packages/client/test/vectors.test.mjs`:
  `fwd/fwd-bound-forward` and `fwd/fwd-bound-forwarded`, the same operation one bound hop apart —
  the `dst` losing exactly one element (the hop's own, consumed, never rewritten), the re-headed
  `PATH_REF` keeping `PL` clear, and `src` growing canonically so the return route is the one every
  canonical hop builds. No API change: the client carries the shape and, having no router, never
  interprets an element.
- **`ParsedFwd` learns the RFC-0024 §5-§7 routing surface.** Two new fields — `mintRequest`
  (the `op` byte's bit 7) and `dstBound` (`dst` is a `PATH_REF`, not a `PATH`) — plus the
  exported `FWD_OPCODE_MASK` (`0x3f`) and `FWD_OP_FLAG_MINT_REQUEST` (`0x80`).
  **Behaviour change, and both halves matter for interop:** `parseFwdTlv` now masks the `op`
  byte before comparing it, so a mint-flagged operation parses as its plain opcode instead of
  falling through every arm; and it accepts a `PATH_REF` in the `dst` and `src` positions,
  which previously threw. An element is node-scoped, so this binding carries one and never
  interprets it.

- **`TYPE.PATH_REF` (`0x14`) and its structural decode rule**
  (`@avatarsd-llc/libtracer`, `src/codec.mjs`;
  [RFC-0024](../../docs/spec/rfcs/0024-bound-paths-node-scoped-vertex-ref-source-routing.md) §4).
  The bound-path address form: a bare array of fixed 8-byte `(u32 index, u32 generation)`
  little-endian elements. New exports `PATH_REF_ELEMENT_BYTES` (8) and `MAX_PATH_REF_ELEMENTS`
  (255), reachable from both the barrel entry and the `./wire` subpath.
  `decode` now requires, for type `0x14`, that `opt.PL` and `opt.LL` are clear, that `length`
  is a multiple of 8, and that the element count is at most 255 — a violation throws
  `CodecError` with `code === ERROR.FRAME_INVALID`. **Behaviour change:** bytes that previously
  decoded as an opaque unknown-type TLV with type `0x14` now throw. `0x14` was unassigned, so
  no frame any libtracer version has emitted is affected.

## 0.7.0 — 2026-08-02

### Changed

- **The PATH segment cap is 255, not 32, and an encode-time 1024-byte check now exists**
  (`@avatarsd-llc/libtracer-client`, `src/tlv.ts`;
  [RFC-0023](../../docs/spec/rfcs/0023-path-segment-cap-repriced-32-to-255.md), accepted; #767).
  Two changes to `pathTlv` / `encodePath` (and every builder over them — `encodeSubscriber`,
  `./fwd.ts`):
  - `segments.length > 32` becomes `> 255` — a **widening**; a 33–255-segment path that used to
    throw `RangeError` now encodes.
  - **New reject:** the client validated the per-segment 64-byte rule but had **no total-length
    check at all**, so it could emit a PATH whose `length` exceeded the normative 1024-byte
    budget — bytes a conforming peer must refuse. `pathTlv` now throws `RangeError` when the
    encoded body (`4 + len` per NAME, i.e. exactly the PATH TLV's own `length` field) exceeds
    1024. Under this encoding that bound binds **before** the segment count, at 204 segments.

  `WalkOptions.maxDepth` documentation updated: a route is bounded by the byte budget in
  practice (≈ 30–50 hops at 3-segment mount runs), not by the segment count.

- **BREAKING — `walkTopology` composes routes from mount paths, not bare connection names**
  ([RFC-0014](../../docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)
  §1, [ADR-0061](../../docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)).
  A connection's routing key **is** its vertex path, `/net/<module>/<name>`. Every entry in
  `TopologyNode.routes` therefore changes shape:

  ```
  before:  ['b']  →  ['b', 'c']
  after:   ['net', 'ws-client', 'b']  →  ['net', 'ws-client', 'b', 'net', 'ws-client', 'c']
  ```

  Any consumer that reads `routes`, joins them into a path, uses a route as a map key, or
  counts hops by array length sees different values. A hop is now three segments, not one.

  **A connection NAME is no longer a unique key.** Link discovery is two levels — `/net`
  enumerates the per-(transport, role) modules and each module enumerates its own connections —
  so the same NAME may legitimately appear under two modules (`ws-client/b` and `tcp-server/b`
  are different links). Code that indexed links by NAME alone must key on `(module, name)`.

  **A module that cannot be read no longer fails the walk.** It appends to `warnings` and the
  walk continues, so a partially reachable topology returns a partial graph instead of throwing.

  This also closes the `/net` routing-prefix contradiction: the reference docs described the
  prefixed form throughout, and the bare-NAME demux was the divergence — not the other way
  round.

### Fixed

- **`walkTopology` could report `authoritative: true` on a graph missing whole subtrees;
  `TopologyGraph` gains `complete` / `gaps` / `pruned`** (`@avatarsd-llc/libtracer-client`,
  `src/topology.ts`; #676).

  **If you check `authoritative` to decide whether to trust a topology, that check does not
  do what the docs said.** `authoritative` answers one question only — *did every node
  report an identity, so are these nodes really devices?* It says nothing about whether the
  walk read everything it meant to. Three sites dropped data without touching it, and the
  worst is silent: when a node's `/net` read failed, that node became a **leaf** and its
  **entire subtree disappeared** from `nodes` and `edges` — with `authoritative: true` and
  only an unstructured string in `warnings` to say so. Detecting the loss meant regexing
  warning text.

  Completeness is now its own property:

  - **`complete: boolean`** — true only if every intended read succeeded. Cleared by all
    three drop sites: the node's `/net` module listing (subtree lost), one module's
    `:children[]` (that module's links lost), and a link's peer listing (the link's shape
    was guessed, so a bus may be under-reported).
  - **`gaps: TopologyGap[]`** — each loss itemized: `site` (`"net"` / `"module"` /
    `"peers"`), the `route` and node `at` it happened on, the `module` / `name`, and the
    registered wire ERROR `code` + `codeName` when the peer answered with one. Empty
    exactly when `complete`. No string parsing.
  - **`pruned: string[][]`** — routes abandoned after a transport-class failure. A node
    that has stopped answering used to cost one `requestTimeoutMs` (default 10 s) **per
    module**; the walk now stops asking after the first such failure.

  **Migration:** replace `if (graph.authoritative)` with
  `if (graph.authoritative && graph.complete && !graph.truncated)`. The three flags are
  independent — identity, dropped reads, and the `maxDepth` bound are different failures.
  A structural type that spells out `TopologyGraph` will need the three new fields.

  Also: **`NOT_FOUND` is now treated as an answer, not a failure.** A node that genuinely
  has no `/net` is a legitimate leaf — it costs no warning and no completeness, where the
  old blanket `catch` could not tell it apart from a refused read. Expect *fewer* warnings
  from a healthy walk over leaf-bearing networks.

## 0.6.0 — 2026-07-23

## 0.5.0 — 2026-07-21

### Added

- **`walkTopology(client, opts)` + `routeKey()` in
  `@avatarsd-llc/libtracer-client` (#409, ADR-0044 pt 3)** — the client-side
  projection of the decentralized graph. A libtracer node's graph is rooted at
  the node you ask; the whole-network view is assembled by a **client**, by
  descending through transport vertices and composing routes. ADR-0044 pt 3
  states that the deduplicated "real graph" is client/app logic keyed by an
  identity **it** chooses — the core never matches device identities across
  paths, at any layer. This is that logic: it reads each node's connections at
  `[...route, 'net']:children[]`, treats each connection NAME as the next `dst`
  segment, and stitches nodes + edges for a renderer.
  **Termination is the load-bearing caveat:** without a node identity a cyclic
  topology cannot terminate (`/b`, `/b/a`, `/b/a/b`, … are all distinct routes,
  and nothing reveals they are one device), so the walk is bounded only by
  `maxDepth` and reports `authoritative: false` — a shape, not a map. Supply
  `identify()` (the `:identity` facet of #406 / RFC-0011, once it exists) and
  nodes collapse, the cycle closes, and the walk self-terminates. The degraded
  mode is kept honest rather than papered over with a heuristic: guessing that
  two routes are one device is precisely the identity-matching ADR-0044 forbids.
  A **bus** link (a `peer_named` ws listener, a CAN segment) is reported via
  `busLinks` and never descended — routing through its connection NAME
  broadcasts to every peer, and descending per enumerated peer is withheld on
  cost grounds: peer counts are unbounded, so it would multiply the walk's
  request volume at every bus crossed (#1147).

- **`LibtracerClient.writeField(path, selector, valueTLV)` in
  `@avatarsd-llc/libtracer-client` (#408)** — the write counterpart of
  `readField`, emitting `FWD{op=WRITE, dst, field, src, payload}`
  (vector-pinned by `fwd-write-subscriber-field`). `write()` takes no
  selector, so **every field-addressed write — the whole in-band formation
  plane — was previously out of reach from this client**: neither
  `:children[]` vertex creation (ADR-0017 / ADR-0027) nor a
  `:subscribers[]` subscribe-write (ADR-0026) could be expressed at all.
- **`encodeConnSpec(options)` in `@avatarsd-llc/libtracer-client` (#408,
  ADR-0027)** — builds the connection-creation `SPEC` TLV that the in-band
  `write /net:children[] += SPEC{…}` carries: the formation write a third
  party (typically a web UI holding delegated admin) issues on a device to
  bring a transport link up, leaving two devices talking with nothing in the
  data path (reference/13 §2). Covers `type`/`name`/`role`/`port`/`kind`/`addr`
  plus the ws-private `peerNamed`/`maxPeers` keys (ADR-0043 §5, ADR-0044).
  **Byte-pinned against the C++ emitter** in `test/conn-spec.test.mjs` — the
  SPEC is now built independently by two encoders and a device only forms the
  link if they agree exactly, so the vectors are the real C++ output.

## [0.4.0] — 2026-07-09

### Removed

- **BREAKING — `MAX_DEPTH` is removed from `@avatarsd-llc/libtracer` (RFC-0006).**
  Nesting depth is receiver-resource-bounded, never a constant: `decode()`'s
  explicit work stack is a growable array, so the depth-cap reject is gone.
  `ERROR.TLV_NESTING_TOO_DEEP` remains registered ("exceeds the receiver's
  decode resources") for harness parity and peers' ERROR frames; `decode()`
  itself no longer throws it.

## [0.3.0] — 2026-07-07

### Added

- **New package `@avatarsd-llc/libtracer-webtransport` — the browser
  WebTransport transport (ADR-0043 Phase B / ADR-0031, #92).**
  `TransportWebTransport` drives the runtime's `WebTransport` (HTTP/3 extended
  CONNECT), opens ONE bidirectional stream as the frame channel, and carries
  each libtracer TLV as a `u32-LE length ++ frame` record — wire-compatible
  with the C++ `tr::net::webtransport_transport_t` (`libtracer_quic` module).
  Satisfies the client SDK's `ClientTransport` seam structurally
  (send/onFrame/onClose), exactly like `TransportWs`. Dev trust via
  `serverCertificateHashes` (ECDSA cert valid <= 14 days — the browser rule;
  see the package README). The pure length-prefix codec is exported at the
  `./framing` subpath (`encodeRecord`, `FrameReassembler`,
  `MalformedPrefixError`, `MAX_FRAME`). Node has no native WebTransport
  client, so unit tests run over a mocked session (web streams); a
  puppeteer/chrome-headless interop harness against the C++
  `wt_interop_server` echo binary skips gracefully unless
  `LIBTRACER_WT_INTEROP_SERVER` is set and puppeteer is installed (the wire is
  additionally proven end-to-end in C++ by `core/tests/webtransport_test.cpp`).

- **Client session hardening (v0.1 must-fix bundle).**
  `@avatarsd-llc/libtracer-client`: pending one-shot requests now **reject when
  the transport closes** (`ClientTransport` gains an optional `onClose` hook,
  implemented by `TransportWs`; after a close, new requests fail fast with
  `"transport closed"`); a **per-request timeout** rejects a reply-less request
  (`ClientOptions.requestTimeoutMs`, default 10 000 ms, `0`/`Infinity` disables —
  a timed-out slot still consumes its late FWD{REPLY} so FIFO correlation holds);
  and an inbound **ADVERTISE (`0x11`) / COMPACT (`0x12`)** frame now surfaces a
  typed `CompactFlowError` on `onError` (compact route-handle delivery is not
  supported by this client yet) instead of being silently dropped.
- **RFC-0002 error-registry parity.** `FWD_ERROR` now carries the full 15-code
  registry (adds `FRAME_TRUNCATED`, `FRAME_INVALID`, `FRAME_CRC_FAIL`,
  `TLV_NESTING_TOO_DEEP`, `ADDRESS_SHIFT_GAP`, `TRANSPORT_DOWN`,
  `VERSION_MISMATCH`), mirroring `docs/reference/05` §`0x08` / the C++
  `error.hpp` registry, plus `FWD_ERROR_PATH` / `fwdErrorPath` /
  `fwdErrorCodeForPath` for the canonical `tr::…` path names. `FwdError` exposes
  `.path`, and a string-form (`NAME` `tr::…`) ERROR reply now surfaces typed on
  `FwdError` (a registered path resolves back to its code) instead of collapsing
  to `UNKNOWN`.

### Changed

- **`@avatarsd-llc/libtracer-ws`: the RFC 6455 codec is no longer re-exported from
  the package barrel.** `TransportWs` frames over the runtime's own WebSocket and
  never uses the hand-rolled codec, so re-exporting the codec (`Opcode`,
  `encodeFrame`, `acceptKey`, …) from the main entry forced every `TransportWs`
  consumer to bundle SHA-1 + base64 + the frame (de)coder. The codec now lives
  **only** at the `./ws` subpath (`@avatarsd-llc/libtracer-ws/ws`) — unchanged for
  its real consumers (the `ws_diff_fuzz` interop oracle, the ws-codec tests,
  no-native-WebSocket runtimes), and it now tree-shakes out of transport-only
  bundles. No effect on `TransportWs`, which the barrel still exports.

- **Version alignment for v0.1**: `@avatarsd-llc/libtracer` (core) bumped
  `0.0.1` → `0.1.0`, so all three packages ship as `0.1.0` (matching the C++
  `project(libtracer VERSION 0.1.0)`). The client/transport peerDependency
  ranges on the core (and the client's on `@avatarsd-llc/libtracer-ws`) are now
  the explicit `>=0.1.0 <0.2.0` — avoiding the `^0.x` caret pitfall.

- **`TYPE.FWD` (`0x0f`) and `TYPE.FIELD` (`0x10`)** in the wire codec's type-code
  registry (RFC-0004 / ADR-0035, slice 1). Both are structured TLVs handled by the
  existing generic codec; no codec logic changed. Cross-core conformance vectors
  under `tests/conformance/vectors/v1/{fwd,field}/` round-trip byte-for-byte
  against the C++ and Rust cores.
- **`@avatarsd-llc/libtracer-client` now speaks the RFC-0004 remote operations over
  `FWD`** (#56, ADR-0035): `read` / `write` / `await_` / `readField` / `subscribe`,
  each a path-addressed `FWD` frame whose source-routed `FWD{REPLY}` is decoded back
  (a `kind=ERROR` reply surfaces as a typed `FwdError`). Adds the `encodeFwd` /
  `encodeField` / `decodeFwd` builders (pinned to the `fwd-*` / `field-*` /
  `fwd-reply-*` vectors byte-for-byte) and an end-to-end interop test against a live
  C++ `fwd_node_server` over a real ws socket (all five ops, incl. a live subscribe
  delivery), CI-gated by the new `fwd-interop` job. The package is **promoted from
  `private` / `0.0.0` to a public `0.1.0`** (still experimental, pre-1.0);
  error-reply codes are provisional pending the ERROR registry (#8).

### Changed

- **`bindings/typescript/` is now an npm workspace monorepo** (ADR-0033). The
  existing core moved from `bindings/typescript/` to
  `bindings/typescript/packages/core/` and continues to publish as
  `@avatarsd-llc/libtracer` (unchanged public API). The single lockfile now lives
  at the workspace root.
- **`@avatarsd-llc/libtracer` gained a tree-shakeable `exports` map.** The barrel
  stays at `.`; the wire codec is now also reachable at the subpath entry
  `@avatarsd-llc/libtracer/wire`. Added `sideEffects: false`, an `engines.node`
  floor of `>=18`, and an explicit `directory` in `repository`. No exported
  symbols were removed or renamed.

### Added

- **`@avatarsd-llc/libtracer-client` — EXPERIMENTAL client SDK (`private`,
  `0.0.0`, #56, ADR-0034).** A payload-builder + frame-I/O client over the
  cross-validated codec (`@avatarsd-llc/libtracer`, a `peerDependency`) and an
  injected transport seam (`@avatarsd-llc/libtracer-ws`'s `TransportWs` satisfies
  it structurally; it is an optional peer). Implements only the wire byte-products
  v1 pins: `encodeValue` / `encodePath` / `encodeSubscriber` (matching the
  `value-*`, `path-sensor-temp`, and `subscriber-path` conformance vectors
  byte-for-byte), a `LibtracerClient` with `write` (VALUE frame) / `subscribe`
  (SUBSCRIBER frame + handler) / `onValue` (inbound VALUE delivery, shedding one
  ROUTER wrapper) / `onError`. The path-addressed request envelope
  (`write(path,…)` / `read` / `await` / `subscribe(producerPath,…)`) is **deferred**
  because the v1 wire format for an addressed remote operation is unspecified
  (`spec/v1.md` §3). Not published. The workspace root `build` script now builds
  `@avatarsd-llc/libtracer` first so the dependent packages always see fresh
  core `dist/`.
- **Both packages are now cleanly publishable to npm (ADR-0033, #98).** Each of
  `@avatarsd-llc/libtracer` and `@avatarsd-llc/libtracer-ws` gained
  `publishConfig.access: "public"` (scoped packages default to restricted), a
  `prepublishOnly` script that rebuilds `dist/` via `tsc` so a publish always
  ships fresh output, an Apache-2.0 `LICENSE` in the package directory, and a
  tightened `files` allowlist (`dist`, `README.md`, `LICENSE`) so the tarball
  excludes `src/`, tests, `tsconfig.json`, and bench. The
  `@avatarsd-llc/libtracer-ws` core dependency stays a `peerDependency` (not
  bundled). A standalone `.github/workflows/publish-npm.yml` publishes both
  packages (ESM, public access, npm provenance) on a published release / `v*`
  tag, plus a manual dry-run dispatch; it leaves the required core gates
  untouched. Real publishes require the `NPM_TOKEN` repository secret.
- **`@avatarsd-llc/libtracer-ws` is now a functional package (`0.1.0`, #54).**
  The WebSocket transport scaffold is implemented: an RFC 6455 frame codec at the
  `/ws` subpath (`acceptKey`, `encodeFrame`, `encodeClientFrame`, `decodeFrame`,
  overflow-safe) cross-validated byte-for-byte against the C++ `tr::net::ws`
  codec, plus a `TransportWs` client (barrel entry) that carries a libtracer TLV
  as one BINARY frame, wire-compatible with the C++ `tr::net::transport_ws`. The
  package is no longer `private`; the core stays a `peerDependency`. Tests cover
  the RFC 6455 known vectors and a Node `ws` round-trip.
- The earlier scaffold entry below is superseded by this implementation.

- **`@avatarsd-llc/libtracer-ws`** package scaffold (WebSocket transport,
  ADR-0029) — package boundary, name, and `exports` shape only; `private: true`,
  no functional transport code yet (deferred to #54).

### Notes

- The `./mem`, `./view`, `./graph` per-layer subpaths are **reserved** in the
  ADR; they are not implemented in TS yet and are not part of this change.
