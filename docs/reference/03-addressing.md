# Reference 03 — Addressing

> Defines how vertices and fields are named, how a subscription observes a subtree, and how application-level slicing replaces wire-level fragmentation.
> **See also**: [04-communication-flows.md](04-communication-flows.md) for API rationale; [02-graph-model.md](02-graph-model.md) for the schema discipline that gives field names meaning.

---

## Path syntax

EBNF (using ABNF-like notation). There is **one grammar**: the concrete `path` form is used everywhere — as the argument to `read` / `write` / `await` and inside a SUBSCRIBER's PATH child alike.

```
path             = root [ segment *( segment-sep segment ) ] [ field-sep field-chain ]
root             = "/"
segment-sep      = "/"
field-sep        = ":"
segment          = name [ index ]
name             = 1*64 ( UTF8-codepoint - reserved )
index            = "[" ( 1*5DIGIT / "" ) "]"
field-chain      = field *( "." field )
field            = name [ index ]
reserved         = "/" / ":" / "." / "[" / "]" / "*" / "?"
DIGIT            = %x30-39
```

There is **no wildcard grammar**. Subscriptions do not need one: every subscription is a **subtree subscription** ([RFC-0005](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md)) — subscribing to a vertex observes it and all of its descendants — so "everything under `/sensor`" is expressed by subscribing to `/sensor` itself, with no pattern syntax (see [§subtree subscriptions](#subtree-subscriptions-no-wildcards) below). A path containing `*` or `?` anywhere MUST be rejected with `ERROR{tr::path::invalid}`.

- All names are UTF-8, case-sensitive, **case-folded NOT performed** (Unicode normalization is the application's responsibility — `/Sensor/temp` and `/sensor/temp` are different paths).
- Maximum **single-name** length: 64 bytes (UTF-8 encoded).
- Maximum **total path** length: 1024 bytes, measured as the **encoded `PATH` body** — the
  concatenated `NAME` TLVs, i.e. exactly the `PATH` TLV's `length` field. Each segment
  therefore costs its 4-byte `NAME` header plus its UTF-8 bytes, not its bytes alone.
  (Erratum 2026-07-31: this line and [§`0x06` PATH](05-protocol-tlvs.md) each stated the cap
  in a *different* byte set from the one `path_t::parse` enforces — see the note below.)
- Maximum **segment depth**: 32 (an addressing limit on PATH construction; the TLV parser itself has no depth cap — nesting is receiver-resource-bounded per [RFC-0006](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0006-resource-bounded-nesting-depth.md)).
- Maximum **field-chain depth**: 8 (e.g., `:settings.transport_tcp.tls.cipher.suite` is at the limit).
- Maximum **index value**: 65535 (fits in u16).

A path that violates any limit MUST be rejected with `ERROR{tr::path::invalid}`.

> **Erratum (2026-07-31) — the total-path cap was stated in three incompatible units.**
> This section said "total path length", readable as the string form `/a/b/c`;
> [§`0x06` PATH](05-protocol-tlvs.md) said "sum of NAME bytes + segment separators", which
> **excludes** the per-segment TLV header; and `core/src/path.cpp` enforces it against the
> accumulated `emit_name` output, which **includes** a 4-byte header per segment. For 32 segments
> of 29 bytes those give 960, 959 and 1056 — so a path both documents called conforming is
> **rejected by every shipped implementation**. The encoded-body reading is what C++ and Rust both
> implement, so the text is corrected to it and no behaviour changes. It does mean a path that was
> nominally conforming under a literal reading of the old wording is not conforming under the new
> one; nothing in the wild emits such a path, because nothing in the wild implemented the old
> reading. See [RFC-0019](../spec/rfcs/0019-path-depth-bounded-by-bytes.md) §4.3, which depends on
> this unit being unambiguous.

### Examples

```
/sensor/temp                           — a vertex
/sensor/temp:subscribers[0]            — a control field on a vertex
/sensor/temp:subscribers[]             — append-or-list view of subscribers
/sensor/temp:settings.history_keep_last — a nested control field
/sensor/temp:settings.transport_tcp.send_buf_kb  — module-namespaced field
/net/can0/wheel-encoder/left           — a remote vertex, routed through a transport-vertex
/camera/frame[7]                       — an indexed child endpoint (one vertex per index)
/camera/frame[]                        — the append / list spelling (see §index forms)
/i2c-bus/0x68/accel                    — peripheral on I²C bus 0x68
/                                      — the root vertex (rarely addressed directly)
```

### Index forms

An index appears in two places, and they are **different planes**:

- **On a field step** (`:name[N]`, `:name[]`, `:name[*]`) — resolved against the vertex's field schema, and carried on the wire by the FIELD level's `index_mode` byte ([§the index form on the wire](#the-index-form-on-the-wire)).
- **On an address segment** (`/camera/frame[7]`) — part of the path grammar above. A PATH TLV's children are all NAME TLVs ([05-protocol-tlvs.md](05-protocol-tlvs.md) §`0x06`), so a segment's brackets travel inside that segment's NAME bytes; v1 assigns no resolution rule *below* the named vertex, so the interoperable reading is one registered vertex per concrete index (§pitfalls).

On a field step:

- `[N]` (decimal integer 0..65535): a specific slot.
- `[]` (empty index): the array as a whole. A read returns a `PL=1` reply whose children are the element TLVs; a write appends to the next free slot.
- `[*]`: every slot. Legal only on a subscriber path — see [§the index form on the wire](#the-index-form-on-the-wire) and §pitfalls.

Field indexing is resolved at **L4 from the field schema**, not from the storage layout: a **fixed-stride** array (uniform element size) resolves `[N]` by direct offset (O(1)) on contiguous backing; otherwise the children are walked ([ADR-0008](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0008-schema-driven-array-indexing.md)).

### The index form on the wire

The text spelling above is the human form. A remote operation carries the field
chain as a `0x10` **FIELD** TLV, one *level* per `.`-separated step, and each
level spells its index form with an optional trailing 1-byte `index_mode` VALUE
([RFC-0004](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md) §C):

| Text | FIELD level | `index_mode` |
| --- | --- | --- |
| `:name` | `NAME` | absent ⇒ `SCALAR` (0) |
| `:name[N]` | `NAME`, `VALUE` u32 = N, `VALUE` u8 | `ELEMENT` (1) |
| `:name[]` | `NAME`, `VALUE` u8 | `ELEMENT` (1), no index |
| `:name[*]` | `NAME`, `VALUE` u8 | `WILDCARD` (2) |

`index_mode` is **optional and defaults to `SCALAR`**, so `:name` and `:name[N]`
each have one canonical spelling while the mode byte is what separates `[]` from
`[*]`. A mode byte outside `{0, 1, 2}` is malformed and MUST be rejected with
`ERROR{tr::path::invalid}`.

Two consequences:

- `:name[]` and `:name[*]` differ **by exactly one byte**, and a decoder that
  reads only the u32 index renders `:name`, `:name[]` and `:name[*]`
  identically. The conformance corpus pins all three
  (`field/field-scalar`, `field/field-append`, `field/field-wildcard`).
- `[*]` is a **wire marker, not a path character**. The reserved-character rule
  below still forbids `*` inside a NAME, and there is still no textual wildcard
  grammar.

### Reserved characters

The five characters `/ : . [ ]` plus `*` and `?` cannot appear inside a NAME segment. Implementations MUST reject any NAME containing them with `ERROR{tr::path::invalid}`.

(`*` and `?` are not path characters in v1; they are reserved to keep the door open for a possible future per-segment wildcard grammar — see [§per-segment wildcards](#per-segment-wildcards-unratified).)

---

## Field-path resolution

The `:` separator divides a path into the **vertex address** (left of `:`) and the **field chain** (right of `:`).

```
/sensor/temp:settings.history_keep_last
  └────┬────┘└──────────┬───────────────┘
   vertex addr      field chain
```

Resolution proceeds in two stages:

1. **Resolve the vertex address** by walking the segment chain from the root. Each segment must match a child vertex name; index segments select indexed children.
2. **Resolve the field chain** against the vertex's schema (read `:schema` to enumerate). Each `.subfield` step descends one level; `[N]` selects a slot in an array-typed field.

If stage 1 fails: `ERROR{tr::path::not_found}`. If stage 2 fails: `ERROR{tr::schema::not_found}` for an unknown field name; `ERROR{tr::path::not_found}` for an out-of-range index on an existing array field.

### Reading vs writing array slots

- `read("/x:subscribers[0]")` returns the SUBSCRIBER TLV at slot 0, or `STATUS=ERROR(NOT_FOUND)` if empty.
- `read("/x:subscribers[]")` returns a `PL=1` reply whose children are all populated SUBSCRIBER slots, in slot-order.
- `write("/x:subscribers[3]", …)` is resolved by its **payload** ([RFC-0009](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md) §D.1): an empty `STATUS` clears slot 3, a `SUBSCRIBER` replaces its edge, anything else is `TYPE_MISMATCH`. `:subscribers[*]` on a write is `INVALID_PATH`. See [02-graph-model.md](02-graph-model.md) §the payload-discriminating `:subscribers[N]` write.
- `write("/x:subscribers[]", tlv)` allocates the next free slot and places the TLV there. The caller can recover the chosen index by reading `:subscribers[]` and looking for their TLV (typically by including a unique subscriber-id NAME in the SUBSCRIBER record).

### Atomicity of multi-field writes

A single `write(path, tlv)` is atomic: a concurrent reader sees either the full prior state or the full new state at that path, not a partial mixture. To update multiple fields atomically, write a single SETTINGS TLV (`0x0B`) containing all the fields to a parent path; the router applies the SETTINGS as one operation.

```cpp
// See the graph module: ../modules/graph.md
tr::graph::graph_t g;

// Non-atomic (reader between calls sees inconsistent state):
g.write(tr::graph::path_t("/x:settings.history_keep_last"), depth_value);
g.write(tr::graph::path_t("/x:settings.store_ref_min_bytes"), threshold_value);

// An atomic multi-field settings WRITE is not implemented: writes are per-knob, and a bare
// `:settings` write resolves no knob (SCHEMA_NOT_FOUND). Reads ARE atomic — `read /x:settings`
// serves the whole container in one traversal.
```

---

## Subtree subscriptions (no wildcards)

Every subscription is a **subtree subscription** ([RFC-0005](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md), accepted and implemented): a `SUBSCRIBER` edge on vertex V observes writes to V **and to every descendant of V** — a leaf subscription is just the trivial case. A write at vertex W therefore delivers, once per subscriber, to the subscribers of W and of each of W's ancestors ("vertical bubbling"). The delivered payload is the **written TLV as-is** — the exact frame the producer wrote, at the granularity it chose.

This covers the dominant "everything under a prefix" use case with **no pattern grammar at all**: subscribing to `/sensor` is what a `/sensor/**` wildcard would have meant, and subscribing to `/camera/frame` observes every indexed child `/camera/frame[0]`, `/camera/frame[1]`, … The full semantics — bubbling, branch-write decomposition, write-creates, and the near-free-when-idle cost model (an unobserved write takes no vertex lock and decides in two atomic loads) — are specified in [02-graph-model.md](02-graph-model.md) §subtree subscriptions and [05-protocol-tlvs.md](05-protocol-tlvs.md) §`0x04`.

### Subscriber identity across a subtree

A subtree subscriber receives TLVs produced at many concrete paths and may need to know which vertex produced each. Provenance travels **in the data** where the application needs it ([CONTEXT.md](../../CONTEXT.md) §SUBSCRIBER direction); for **local** delivery the concrete path is additionally available out-of-band (implementation-defined — typically a callback argument). Wire-level concrete-path tagging of **remote** deliveries is the separate, still-draft [RFC-0003](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0003-bridged-wildcard-delivery-path.md) proposal; without it, cross-implementation remote provenance beyond what the data carries is not guaranteed interoperable.

### Per-segment wildcards (unratified)

A per-**segment** wildcard grammar — `*` matching one segment (`/sensor/*/temp` for *horizontal* matching across siblings) — is an **unratified idea**, not part of v1: no implementation exists, and adopting it would require its own RFC. The characters `*` and `?` are reserved so such a grammar could be added without breaking existing names. Subtree subscription deliberately removes the need for the `**`-style *vertical* wildcard. This paragraph is non-normative.

The **field-index** `[*]` is a different thing and is not a future direction: it
exists in v1 as FIELD `index_mode = WILDCARD` ([§the index form on the
wire](#the-index-form-on-the-wire)). It is confined to subscriber-path targets —
a `[*]` level on any other field answers `ERROR{tr::path::invalid}`, which the
`fwd/fwd-wildcard-reject` conformance vector pins.

---

## Address-shift slicing (replaces wire-level fragmentation)

The wire format ([01-data-format.md](01-data-format.md)) deliberately omits fragmentation rules. The application-level mechanism is **address-shift slicing**: a logically large payload is split across **N child endpoints** with the **same timestamp**.

### Sender behavior

```
Logical message: 10 MB camera frame, timestamp T.

Publisher chooses slice size S = 64 KiB.
Number of slices N = ceil(10 MB / S) = 160.

For i in 0..159:
    write("/camera/frame[i]", VALUE{ts=T, bytes=slice_i})
```

Each slice is a complete, valid, independently-routable TLV. The publisher emits N writes; the router and transport see N separate dispatches.

### Receiver behavior

A subscriber registers once on the **parent** vertex — a subtree subscription ([RFC-0005](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md)) observes every indexed child:

```
write("/camera/frame:subscribers[]", SUBSCRIBER{path=/local/handler, settings})
```

Each subsequent `write("/camera/frame[i]", ...)` bubbles to the parent's subscription and produces a delivery to `/local/handler`, with the slice index recoverable from the producing path (out-of-band for local delivery; on-wire tagging for remote delivery is the draft [RFC-0003](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0003-bridged-wildcard-delivery-path.md)).

### The slice-group key

Slices assemble into a coherent group keyed by **`(origin, ts)`**, with each slice's `index` giving its position within the logical message.

`ts` is the slice TLV's optional timestamp (`opt.TS`), a **per-producer monotonic** value rather than literal wall-clock: strictly increasing per origin, never regressing or colliding, so it identifies a group within one origin without a sequence number.

**`origin` is a receiver-side identity, not a wire field.** v1 defines no delivery-borne producer identity. The `0x0D` ROUTER envelope that would have carried an `origin_peer_id` is a **reserved codepoint with no mechanism** — senders MUST NOT emit it ([05-protocol-tlvs.md](05-protocol-tlvs.md) §`0x0D`) — so an implementation MUST NOT build the group key from a field it will never receive. A receiver derives `origin` from **how the slice arrived**:

| Delivery | What identifies the origin |
| --- | --- |
| Local | The producing vertex path, available out-of-band. |
| Remote, full-route | The accumulated `src` route of the delivering `FWD{WRITE}` — the return route to the producer, retained in the subscriber slot at subscribe time ([05-protocol-tlvs.md](05-protocol-tlvs.md) §`0x04`) and grown one link-NAME per hop ([07-host-embedding.md](07-host-embedding.md) §loop safety). |
| Remote, compacted | The per-link `label` aliasing that return route ([05-protocol-tlvs.md](05-protocol-tlvs.md) §route-handle frames). A label is per-link and re-advertised after a reconnect, so it identifies an origin only within one flow. |
| Header-elided (CAN) | The link-local peer identity the transport derives from the frame id ([14-can-transport.md](14-can-transport.md)). |

Grouping by `ts` alone is wrong: two publishers that happen to emit at the same timestamp merge into one group.

Two properties follow from a route-derived origin:

- One producer reached over **two links** is two routed addresses, so it yields **two** group keys. That is the same deliberate redundancy that makes parallel links two subscriptions rather than auto-multipath (§routed scope); an assembler that must fuse them needs an application-level identity.
- A route that changes — a reconnect that renumbers a link — changes the key. An application that needs a **route-independent** origin reads the producer's node-scoped `:identity` facet once through the route and pins the mapping ([RFC-0011](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0011-node-identity-facet.md)). `:identity` is a readable field, not delivery metadata; it is never carried per slice.

([ADR-0011](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0011-address-shift-totality-opt-in.md) spells the key `(origin_peer_id, ts)`, naming the ROUTER envelope's field. The pair is the same; the origin half is derived as above.)

Two further grouping rules:

- A slice may arrive at any time within the deadline window.
- Loss of an **interior** slice is detected as a missing index at deadline; loss of **trailing** slice(s) is detectable only when the group total is known (see §loss detection).

### Subscriber assembly policies

The subscriber's QoS at `:settings.address_shift.*` controls assembly behavior. (Field names are
defined here as the v1 **design**; none of them is implemented, and the two knobs this table used
to borrow from the core namespace — `deadline_ns` and `queue_max_bytes` — no longer exist, having
been removed as inert by [RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.D. When assembly lands, its deadline and its bound are
its own module-namespaced magnitudes under `address_shift.`, not core knobs.)

| Field | Type | Default | Effect |
| ---- | ---- | ---- | ---- |
| `:settings.address_shift.assemble` | bool | false | If true, hold slices in a per-timestamp buffer until the group is complete or the deadline expires; deliver one assembled message. If false, deliver each slice immediately as it arrives. |
| `:settings.address_shift.expected_count` | u32 | 0 (unknown) | If non-zero, declares N up-front; missing indices are detectable before the deadline. |
| `:settings.address_shift.on_gap` | enum | `surface` | `surface` = deliver partial group with `STATUS=ADDRESS_SHIFT_GAP`; `drop` = silently discard incomplete groups; `wait_forever` = never give up (bounded by the assembler's injected buffer, which is where its backpressure comes from). |
| `:settings.address_shift.deadline_ns` | u64 | unset | Per-group assembly deadline. After the deadline relative to the first observed slice, the group is finalized per `on_gap`. |

### Loss detection

Missing index `k` in a group with `expected_count = N` and observed indices `{0..N-1} \ {k}`: at deadline, the assembler emits `STATUS=ADDRESS_SHIFT_GAP` with `ERROR.detail = k`.

**Group totality is opt-in.** For groups without `expected_count`, the assembler treats the largest-observed-index + 1 as the implicit `N` at deadline — so a dropped **trailing** slice is invisible (a 100-slice group missing index 99 looks complete at slice 98). v1 does not force a count: open-ended streams cannot always supply one. If guaranteed tail-loss detection is required, the publisher MUST declare totality — either set `expected_count`, or precede the group with a `:manifest` write carrying the index set as a structured (`opt.PL=1`) TLV. (An end-of-group marker on the final slice is a possible future mechanism — see [ADR-0011](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0011-address-shift-totality-opt-in.md).)

### Consequences

- **Lossless transport composition.** Whatever the transport does (drop a UDP datagram, lose a CAN frame), each slice is independently lost or delivered. No reassembly state to corrupt.
- **No special FRAGMENT type code.** The wire format from [01-data-format.md](01-data-format.md) doesn't need a fragment-with-reassembly-metadata type; the addressing scheme carries it.
- **Stream processing is natural.** The subscriber decides whether to assemble or to process as a stream; the publisher doesn't impose either choice.
- **Per-slice priority and QoS.** The addressing scheme lets a publisher tag different slices with different priorities (e.g., camera I-frames at high priority, P-frames at low) by writing them to differently-configured `ep[N]` slots.

### Costs and obligations

- **Index allocation discipline.** The publisher must agree with subscribers on what `[N]` means (byte offset / slice_size? row index? sample index in a window?). This is an application-layer convention; libtracer does not impose semantics.
- **Bubbling fan-out cost.** Every slice write walks the ancestor chain when a subscriber exists at or above it — near-free when idle, but a hot high-rate slice stream pays one delivery per covering subscription ([RFC-0005](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md) §A cost model).

---

## Address scopes: local, routed, global

The same path can resolve differently depending on which node evaluates it. The protocol distinguishes three scopes:

### Local scope

A path resolves within the host's own graph. No route prefix. Applies to:

- In-process publishers and subscribers on the same node.
- Vertex paths created by application code on this node.
- Module-exported vertex paths (e.g., `transport_i2c` exposing `/i2c-bus/0x68/accel`).

### Routed scope (path-as-route)

A remote vertex is reached by walking *through* a transport-vertex ([ADR-0027](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0027-transport-and-connections-are-vertices.md) / [CONTEXT.md §Path-as-route](../../CONTEXT.md)): the path `/net/<conn>/<remote path>` — e.g. `/net/can0/sensor/wheel/left` — is the local address of the remote vertex, and **the path is the route**. The prefix is the transport-vertex's own path, not a configured string; the send-side suffix and the receive-side prefix are the same address.

The operation travels as an `FWD` frame carrying its own route: each forwarder hop strips its leading `dst` segment and prepends the inbound-link NAME to `src`, so `dst` is always the remaining forward route and `src` the accumulated return route. Explicit source routes cannot loop **by construction** — `dst` shrinks monotonically per hop, so a delivery travels exactly as far as its explicit route and no further; a cycle in the physical topology is harmless per-op (the route is finite, so the walk is finite). There is no visited-set or revisit check — loop-freedom is protection-by-construction, not protection-by-rejection. Nothing is republished at a fixed prefix; a consumer addresses the routed path directly, and deliveries return along the accumulated route. See [reference/13](13-network-formation.md).

Two links to the same peer are two different routed addresses (e.g. `/net/ws0/...` and `/net/can0/...`) — deliberate redundancy the consumer subscribes to explicitly, not auto-multipath.

### Global scope

The "global" scope is the union of all hosts' local + routed graphs. There is no single authority that owns it; it is a logical view assembled by composing routes through transport-vertices.

A common convention (not normative): a peer's data is addressed through the connection that reaches it — `/net/<conn>/...` — and multi-hop reach composes one link segment per hop. This keeps the global graph navigable without name collisions.

### Collision rules

When two registrations would claim the same local path:

- **First-binder wins**: the first registrant to bind a vertex name owns it. Subsequent attempts return `ERROR{tr::path::in_use}` (a yet-to-be-assigned error code in the `0x0C..0x7F` reserved range).
- Configuration avoids collisions by giving each link a distinct connection NAME (`/net/can0`, `/net/ws0`).
- For routed addresses, uniqueness comes from the connection-NAME namespace of each node along the route. Conflicting peer identities on the network are a discovery-layer problem, not an addressing problem.

---

## Path canonicalization

Two textually-different paths that name the same vertex MUST canonicalize to the same internal representation:

- Trailing slashes: `/sensor/temp/` and `/sensor/temp` are the same. Implementations SHOULD strip trailing slashes during parse.
- Empty segments: `/sensor//temp` is **invalid**, not equivalent to `/sensor/temp`. Reject with `ERROR{tr::path::invalid}`.
- The root path is exactly `/`. `//` and beyond are invalid.

Field paths do not have a trailing-separator equivalent; `:settings.` (trailing dot) is invalid.

UTF-8 normalization: implementations MAY normalize path bytes to NFC at the parse boundary, but MUST be consistent: normalized paths and pre-normalized paths from peers must round-trip without collision. The recommended choice is to NOT normalize and to require senders to canonicalize before transmission. (Application authors generally use ASCII-only path components, so this is rarely an issue in practice.)

---

## Static path handles (MCU-friendly addressing)

> **Normative reference**: [../spec/v1.md](../spec/v1.md) §3.1.
> **See also**: [05-protocol-tlvs.md](05-protocol-tlvs.md) §`0x06` PATH for byte-precise PATH TLV layout.

The string form `"/sensor/temp"` is convenient at the API surface but hostile to the hot path on MCU-class hardware: it forces a parser walk, allocates segment structures per call, and pulls in `snprintf` (a few KB of code) when the path includes runtime indices. libtracer addresses this with a **static path handle**: a path is encoded into a PATH TLV exactly once — at build time or at node-init — and every subsequent reference uses the pre-encoded bytes directly.

**The contract.** A path handle is whatever opaque token an implementation hands back from path registration. It MUST resolve to wire bytes byte-equal to the canonical PATH TLV for the named vertex, and the resolution MUST NOT allocate, parse, or format strings on the hot path.

### Path lifecycle

Three modes, in order of preference for embedded targets:

| Mode | Where the PATH TLV lives | When the bytes are produced | Hot path cost |
| ---- | ---- | ---- | ---- |
| **Build-time literal** | `.rodata` / flash | At compile time (macro or codegen emits the byte literal) | Pointer-load — zero runtime work |
| **Init-time registration** | RAM (long-lived segment) | Once at node init (`register_vertex`) | Pointer-load |
| **String at hot path** (string-parsed, convenience) | RAM (short-lived) | On every call | Parse + alloc + canonicalize |

The string-at-hot-path mode is **NOT required** of conforming implementations and a minimum-feature (P0) build MAY omit the string entry points entirely.

### Static path construction and use

```mermaid
flowchart LR
  subgraph Build["Build time"]
    S["Source path string<br/>&quot;/sensor/temp&quot;"]
    M["path_t(&quot;/sensor/temp&quot;)<br/>parse-once ctor / codegen"]
    R[".rodata bytes:<br/>06 40 12 00 ... NAME &quot;sensor&quot; ... NAME &quot;temp&quot;"]
    S --> M --> R
  end

  subgraph Init["Node init (once)"]
    I["path_t::parse(string)"]
    H["heap segment with same bytes"]
    I --> H
  end

  subgraph Hot["Hot path (per write)"]
    HND["path handle<br/>(&rodata or &heap)"]
    W["g.write(handle, value)"]
    DISP["router dispatch<br/>(byte-compare on PATH bytes)"]
    HND --> W --> DISP
  end

  R -.holds bytes for.-> HND
  H -.holds bytes for.-> HND
```

Both paths land in the same shape: a const region whose bytes are a valid PATH TLV. The hot-path API treats them identically.

### Parse-once path construction

An implementation encodes a literal path exactly once at construction, and the resulting handle is reused on every subsequent write; the reference implementation spells that as the infallible parse-once `path_t("...")` constructor ([ADR-0054](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0054-path-t-parse-once-constructor.md)). A binding may additionally expose a build-time / `consteval` PATH encoder; the wire bytes are identical. The C++ sketch below shows the reference shape (see the [graph module](../modules/graph.md) and the [view module](../modules/views.md)):

```cpp
tr::graph::graph_t g;

// Parse-once handle: the PATH TLV is encoded a single time here.
// Reserved-char / length validation happens in the constructor.
tr::graph::vertex_handle_t sensor_temp =
    g.register_vertex(tr::graph::path_t("/sensor/temp"),
                       tr::graph::role_t::STORED_VALUE);

// Build a fresh VALUE view over f32 bytes (standard helper pattern).
tr::view::view_t value_f32(float f) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(4);
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    for (int i = 0; i < 4; ++i)
        seg->bytes[i] = static_cast<std::byte>((bits >> (8 * i)) & 0xFF);
    return tr::view::view_t::over(std::move(seg));
}

// Hot path — write by handle, no path parsing.
void on_sample(float t) {
    g.write(sensor_temp, value_f32(t));
}
```

Encoding the literal once walks the path, rejects reserved characters, counts segments, and emits the byte sequence:

```
06 PL=1+CR=0  LL=0  length=u16  | type, opt, length
02 00 06 00 's' 'e' 'n' 's' 'o' 'r'   ← NAME "sensor" (10 bytes)
02 00 04 00 't' 'e' 'm' 'p'           ← NAME "temp"   (8  bytes)
```

Since `.rodata` is read-only, the bytes are never modified. The router's dispatch table indexes by **byte-equality on the PATH TLV's payload**, so two TLVs that name the same vertex hash and compare identically regardless of where their bytes live (flash, heap, or transport receive buffer).

### Init-time registration for runtime-derived paths

Some paths are not known at compile time:

- Connection-routed paths (`/net/<conn>/sensor/temp`) — the connection name is established at runtime.
- Address-shift slice paths (`/camera/frame[0]`, `/camera/frame[1]`, …) — the index varies per slice.

For these, register each concrete indexed path once at init and keep its vertex handle. Runtime strings are parsed with a fallible entry point (`path_t::parse`, returning `std::expected`); literal indexed paths use the `path_t("...")` constructor directly:

```cpp
tr::graph::graph_t g;

// Validate, canonicalize, encode once. The vertex handle is stable for node lifetime.
std::vector<tr::graph::vertex_handle_t> frame_slice;
frame_slice.reserve(N);
for (std::size_t i = 0; i < N; ++i) {
    // Runtime-derived index → path_t::parse returns std::expected; deref on success.
    auto p = tr::graph::path_t::parse("/camera/frame[" + std::to_string(i) + "]");
    frame_slice.push_back(g.register_vertex(*p, tr::graph::role_t::STREAM));
}

// Hot path — zero-copy borrow of the DMA buffer, no string work.
void on_dma_complete(std::byte* frame, std::uint64_t /*ts*/) {
    for (std::size_t i = 0; i < N; ++i) {
        tr::view::view_t slice =
            tr::view::view_t::over(tr::view::borrow(std::span<std::byte>{frame + i * S, S}));
        g.write(frame_slice[i], slice);
    }
}
```

Registration encodes exactly one PATH TLV in a long-lived segment, validates it against §path syntax, and returns the handle. After init, the handle behaves identically to a build-time literal: a pointer-load and a dispatch.

### Indexed slot paths

For the common case of `name[i]` where `i` ranges over a known set, register each real indexed path (`/camera/frame[0]`, `/camera/frame[1]`, …) once and write by its handle:

```cpp
tr::graph::graph_t g;

// One vertex per real indexed path.
std::vector<tr::graph::vertex_handle_t> frame;
frame.push_back(g.register_vertex(tr::graph::path_t("/camera/frame[0]"), tr::graph::role_t::STREAM));
frame.push_back(g.register_vertex(tr::graph::path_t("/camera/frame[1]"), tr::graph::role_t::STREAM));
// …

void on_dma_complete(/* … */) {
    for (std::size_t i = 0; i < N; ++i) {
        g.write(frame[i], slice_view(i));
    }
}
```

A single-PATH-plus-index form — encoding `/camera/frame` once and supplying `i` as a separate u16 at the dispatch boundary — is a **permitted-but-not-implemented** optimization (non-normative): the reference core has no separate indexed-handle API. It would be equivalent to the real write to `/camera/frame[i]` above — the resolved vertex and the wire bytes (after index expansion) are identical.

### Hot-path dispatch with a static handle

```mermaid
sequenceDiagram
    participant App as Application (ISR / sample loop)
    participant Hnd as Path handle (.rodata)
    participant Disp as Router dispatch
    participant Vtx as Vertex
    participant Subs as Subscribers

    App->>Hnd: load pointer (1 cycle on Cortex-M)
    App->>Disp: g.write(handle, value)
    Disp->>Disp: dispatch_table[hash(handle.bytes)]
    Note over Disp: byte-compare on PATH bytes<br/>no string parse, no alloc
    Disp->>Vtx: store value_tlv as LKV
    Disp->>Subs: refcount-bump and enqueue (per subscriber)
```

The boxed note is the load-bearing one: dispatch never re-parses the path. The handle's bytes are the cache key.

### Effects on a constrained target

- **Code size.** Removing `snprintf` from the publisher saves 2–6 KB on Cortex-M, depending on libc. For a 16 KB target ([10-module-catalog.md](10-module-catalog.md) §profile sentinel), this is the difference between fitting and not fitting.
- **Determinism.** No allocation on the hot path means no fragmentation, no malloc-under-ISR, predictable worst-case latency.
- **Cache behavior.** Build-time PATH TLVs live in flash and are streamed via XIP / cached I-side accesses; they never compete with the data cache.
- **Wire correctness by construction.** Validation is done once at encode time; the hot path can assume the handle's bytes are a valid PATH TLV. There is no class of "malformed path on the hot path" bug to worry about.

### Conformance summary

A conforming node:

- MUST accept path handles at every read / write / await entry point ([../spec/v1.md](../spec/v1.md) §3.1.4).
- MUST treat a path handle and the equivalent string-form path as semantically identical.
- SHOULD provide a build-time encoding macro for paths known at compile time.
- MAY omit string-form entry points entirely on minimum-feature builds.
- MUST NOT require the application to format paths on the hot path.

The full byte layout of the encoded PATH TLV is in [05-protocol-tlvs.md](05-protocol-tlvs.md) §`0x06`. The init-time vs hot-path distinction is in [04-communication-flows.md](04-communication-flows.md) §the static-path write flow.

---

## Pitfalls

Each entry states the rule, then the shape of the failure an implementation produces when it gets the rule wrong.

### `[*]` treated as `[0]`

**Rule.** A `[*]` level is FIELD `index_mode = WILDCARD`, and it is legal only where the field chain's first step is `subscribers`. A `[*]` level on any other field answers `ERROR{tr::path::invalid}`. On a `write` it answers `ERROR{tr::path::invalid}` in every case: the write grammar has no wildcard axis.

**Failure mode.** `[*]` marks the level as *indexed* while carrying no index, so the index stays at its default `0`. An implementation that branches on "is this level indexed?" without also testing the mode resolves `:subscribers[*]` as `:subscribers[0]` — it clears or replaces **slot 0**, silently evicting a third party's subscription, and answers `RESULT`. The damage is a success report, not an error.

**Checks.** `fwd/fwd-wildcard-reject` pins the resolution-layer rejection of `[*]` on a non-subscriber target: the frame is round-trip-valid at the **codec** layer, so a codec-only conformance run passes it, and only a resolver test catches the defect. `field/field-wildcard` pins the addressing spelling.

### `:name`, `:name[]` and `:name[*]` read as one form

**Rule.** The trailing `index_mode` byte is the only thing that separates them: absent ⇒ `SCALAR`, `ELEMENT` with no index ⇒ `[]`, `WILDCARD` ⇒ `[*]`.

**Failure mode.** A decoder that reads only the u32 index — or that ignores a level's trailing VALUEs — renders all three identically, so "append one subscriber" and "address every slot" become the same operation. `field/field-scalar`, `field/field-append` and `field/field-wildcard` are the same shape apart from that byte; a decoder that passes one and not the others has this defect.

### `:subscribers[N]` written payload-blind

**Rule.** A write to `:subscribers[N]` is resolved by its **payload** ([RFC-0009](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md) §D.1): an empty `STATUS` clears the slot, a `SUBSCRIBER` replaces the edge, anything else is `TYPE_MISMATCH`.

**Failure mode.** An implementation that treats `[N]` as unconditional-clear destroys the edge a peer wrote a `SUBSCRIBER` to *replace*, and reports success. Both this and the `[*]` case above fail the same way — a third party is unsubscribed and nobody is told. The graph-model side of the same rule is in [02-graph-model.md](02-graph-model.md) §the payload-discriminating `:subscribers[N]` write.

### The wire index is u32; the addressing limit is 65535

**Rule.** A `[N]` level carries its index as a `VALUE` u32, while §path syntax caps an index at 65535.

**Failure mode.** Narrowing the decoded u32 to 16 bits without a range check aliases `[65536]` onto `[0]`, `[65537]` onto `[1]`, and so on — a write lands in the wrong slot with no diagnostic. Reject an index above the limit with `ERROR{tr::path::invalid}` at the point the address is admitted, not at decode.

### A segment index read as a field index

**Rule.** The two index planes are not interchangeable (§index forms). A field index travels as a FIELD level's `index_mode`. A segment index has no separate carrier: a PATH TLV's children are all NAME TLVs ([05-protocol-tlvs.md](05-protocol-tlvs.md) §`0x06`), so `/camera/frame[7]` puts the brackets inside the segment's NAME bytes.

**Failure mode.** An implementation that invents a resolution rule *below* `/camera/frame` for the `[7]` will not interoperate: no conformance vector under `path/` carries a bracketed segment, so nothing pins the byte spelling, and giving `[n]` a value-plane meaning is the subject of a draft proposal ([RFC-0017](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0017-element-addressing-value-plane-index.md)) rather than settled v1. The interoperable construction is one registered vertex per concrete index (§indexed slot paths).

### A slice group keyed on a field the wire does not carry

**Rule.** The group key is `(origin, ts)` where `origin` is derived from how the delivery arrived (§the slice-group key), never from a producer-identity field in the frame.

**Failure mode.** An implementation that reads an `origin_peer_id` out of a `0x0D` ROUTER envelope receives no such envelope — senders MUST NOT emit `0x0D` — so every slice keys on a zero origin, and slices from every publisher with a colliding `ts` merge into one corrupt group. The symptom is an assembled message whose interior is another producer's bytes, not a gap.
