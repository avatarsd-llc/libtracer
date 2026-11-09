<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# The libtracer plane

**Status: explanatory.** This page describes the whole protocol and node model in one place.
It sets no rule. [Tracer protocol v1](spec/v1.md) and the annexes its §3 incorporates hold every
rule, and they win wherever this page differs. Each section ends with the normative sections it
explains.

## In one paragraph

A libtracer network is one graph of named **vertices** spread across devices. A device is a
**node** that holds part of the graph and reaches the rest through **connection vertices**, which
are ordinary vertices too. Everything a node can do to another is a `read`, a `write` or an
`await` on a path, and each one travels as a single self-routing `FWD` frame. A subscription, an
access rule or a new connection is not a separate verb: it is a write to a `:` field or a creator
endpoint. The protocol never looks inside a value. Deadlines come from the application's clock,
the library starts no timer, and whoever receives a cost pays for it from that link's own memory.

## The picture

```{mermaid}
flowchart LR
  accTitle: Two nodes and one forwarding hop
  accDescr: An application writes a vertex on node A. Node A holds a subscriber edge and delivers the value in a FWD WRITE frame through its connection vertex to node B, which forwards it over a second link to a target vertex on node C.
  APP(["application"]) -- "write" --> V("vertex /sensor/temp")
  V -- ":subscribers[] edge" --> C1("connection /net/uplink/b")
  C1 == "tcp · FWD{WRITE}" ==> B[["peer B (forwarder)"]]
  B == "can · FWD{WRITE}" ==> T[["peer C"]]
  T --> TV("target vertex")
```

Rounded boxes are vertices, double-sided boxes are peers, the stadium is the application, and
thick edges are links labelled with their transport.

## 1. The graph model: vertices, fields, edges

A **vertex** is an addressable slot at a path such as `/sensor/temp`. A vertex holds a value, or
it is a position in the tree that groups other vertices. The role of a vertex, which decides what
a write does to it, is declared by its owner: a stored value is overwritten, a stream appends to a
ring, and a handler consumes the write and stores nothing.

A **value** is a TLV: a type code, an options byte, a length, a payload and an optional trailer.
The bytes in memory are the bytes on the wire, and the protocol treats a `VALUE` payload as opaque.
The meaning of those bytes belongs to the application.

A **field** is a named facet of a vertex, reached after the `:` separator. `/`-segments name
vertices and `:`-chains name fields. The fields a vertex may have are:

| Field | Purpose |
| --- | --- |
| `:subscribers[]`, `:subscribers[N]` | subscription edges |
| `:acl` | access rules |
| `:schema` | self-description of fields and types |
| `:settings.app.<name>` | owner-declared application properties |
| `:identity` | the node's public key |
| `:stats.<class>.<seam>` | counters for one bounded resource |
| `:children[]` | enumeration of child vertices, and creation of a typed child |

Every field is optional. A field a vertex does not implement answers
`tr::schema::not_found`, the way a device answers an unsupported `ioctl`. Only the fields a node
chooses to offer exist, and there is no per-vertex settings namespace beyond `settings.app`.

An **edge** is a subscription: a `SUBSCRIBER` record held by the producer in `:subscribers[N]`. It
observes the vertex and all its descendants. When the producer is written, the record tells it
where to deliver. The delivery is itself a write and stops at its target. A target that wants to
pass the value on does so in its own application code.

Rules: [annex 01 §Frame layout](reference/01-data-format.md#frame-layout);
[annex 05 §`0x01` VALUE](reference/05-protocol-tlvs.md#0x01--value) and
[§`0x04` SUBSCRIBER](reference/05-protocol-tlvs.md#0x04--subscriber);
[§`0x0B` SETTINGS](reference/05-protocol-tlvs.md#0x0b--settings).
Depth: [Reference 02](reference/02-graph-model.md),
[Reference 11](reference/11-vertex-roles-and-aggregation.md).

## 2. The operations: read, write, await

There are three operations. `read` and `write` work on a vertex or on any field it offers. `await`
works on a vertex only: an `await` with a `:field` selector answers `tr::schema::not_found`, even on
a field that reads and writes normally, because a field write wakes no one.

| Operation | Effect | Wire form |
| --- | --- | --- |
| `read` | return the current value or field | `FWD{op=READ=0}` |
| `write` | apply a value or field write | `FWD{op=WRITE=1}` |
| `await` | return at the next write, or fail at the requester's deadline | `FWD{op=AWAIT=2}` |

A fourth opcode, `REPLY=3`, carries the answer back. There is no `subscribe`, `connect` or
`create` operation on the wire. Those are writes:

| To do this | Write this |
| --- | --- |
| subscribe | a `SUBSCRIBER` to `<vertex>:subscribers[]` (needs `READ` on the producer) |
| unsubscribe | an empty `STATUS` to `<vertex>:subscribers[N]` |
| set an access rule | an `ACL` to `<vertex>:acl` (node owner only) |
| open a connection | a `SPEC{name, config}` to `/net/<module>/conn` |
| close a connection | a `NAME` to `/net/<module>/conn` |
| create a typed child | a `SPEC` to `<parent>:children[]` |

Client libraries may offer a `subscribe()` helper, and the reference API calls it that too. On the
wire it is still a write.

A write whose `src` is an empty `PATH` is **unacknowledged**: it is applied, and nothing comes
back, whether it succeeded or was refused. `READ`, `AWAIT` and subscribe need a return route, so an
empty `src` makes them malformed.

**Planned, not shipped.** [#1946](https://github.com/avatarsd-llc/libtracer/issues/1946) retires
`AWAIT` and `REPLY` in favour of a reply that is itself a remote write. Until a spec amendment says
so, `await` and `REPLY` are what a device implements.

Rules: [v1 §1](spec/v1.md#1-scope);
[annex 05 §Reserved range](reference/05-protocol-tlvs.md#reserved-range-0x0f--0x1f), which also
carries the [`FWD`/`FIELD` grammar](spec/v1.md#3-wire-format) by reference.
Depth: [Reference 04](reference/04-communication-flows.md).

## 3. Addressing and forwarding through peers

### Paths

A path is a list of `/`-separated names. On the wire it is a `PATH` TLV with `opt.PL=0` and a
packed body: for each segment one length byte, then that many UTF-8 bytes.
`/sensor/temp` is `06 00 0C 00 06 "sensor" 04 "temp"`. A name is 1 to 64 bytes, a path has at most
255 segments and 1024 body bytes, and a name may not contain `/ : . [ ] * ?`. An empty body is the
root.

The field chain travels separately, in a `FIELD` TLV. Each level is a `NAME`, optionally followed
by a `VALUE` u32 index and a `VALUE` u8 `index_mode` (`SCALAR=0`, `ELEMENT=1`, `WILDCARD=2`).

### One frame per operation

```text
FWD (0x0F, opt.PL=1) {
  VALUE op              READ=0 WRITE=1 AWAIT=2 REPLY=3; bits 6-7 reserved
  PATH  dst             the route still to travel
  FIELD selector        optional, the :field tail
  PATH  src             the return route so far, empty if unacknowledged
  VALUE kind            REPLY only: RESULT=0 ERROR=1
  <payload TLV>         WRITE and REPLY
  VALUE await_timeout   optional, AWAIT only: a hint the terminus may ignore
}
```

### Path as route

A route is written out in full from the sender's own root, walking through connection vertices:
`/net/uplink/b/sensor/temp` means "through my connection `net/uplink/b`, then `/sensor/temp` as that
peer sees it". Each forwarding node removes the whole leading mount run
(`net/<module>/<name>`, plus `/<peer>` on a bus) from `dst` and adds the mount run of the link the
frame came in on to the front of `src`. `dst` shrinks, so a route is finite and cannot loop, and
forwarders keep no per-request state. The node where `dst` runs out applies the operation and
answers along `src`.

A node with one connection never forwards. A node with two or more connection vertices forwards
between them.

There is no automatic multipath: a peer reachable two ways has two addresses.

### Compact forms a device may ignore

A node may issue and honour a **PAIR**, an 11-byte path element `00 16 08 <u32 index> <u32
generation>` that names one of its vertices directly. A reply's `src` teaches the sender a chain
of PAIRs for later requests. A stale PAIR answers `tr::path::not_found`, and the sender falls back
to names. Both issuing and honouring are optional, but every parser has to step over the record by
its declared length. A PAIR is an address and grants no rights.

### Subscriptions across hops

A subscribe is an ordinary `FWD{WRITE}` to `:subscribers[]`. On a routed subscribe the accumulated
return route is the target, so the `SUBSCRIBER` needs no `target_path`. The producer then holds the
edge and sends each delivery as `FWD{WRITE}` along that route with an empty `src`. Forwarders may
add their reverse element to a `PATH_REF_REVERSE` (`0x15`) child, so that deliveries can use a
shorter chain. A forwarder that does not issue PAIRs adds its name run.

Rules: [annex 03 §Path syntax](reference/03-addressing.md#path-syntax);
[annex 05 §`0x06` PATH](reference/05-protocol-tlvs.md#0x06--path) and
[§Path element PAIR](reference/05-protocol-tlvs.md#path-element-pair-escape-kind--0x16--routing-semantics);
[§Producer fan-out](reference/05-protocol-tlvs.md#producer-fan-out-to-remote-subscribers);
[v1 §3.1](spec/v1.md#31-static-path-handle-conformance).
Depth: [Reference 03](reference/03-addressing.md),
[Reference 07](reference/07-host-embedding.md),
[Reference 13](reference/13-network-formation.md).

## 4. ACL and session admission

These are two gates, at two layers.

**Admission decides who gets a session on a link.** The wire specification does not define it,
because it is below the TLV layer. The reference implementation offers three admission points on a
WebSocket link: a check on the opening handshake, an in-band credential frame for clients such as
browsers that cannot set a header, and a concurrent-peer cap (`max_peers`) that refuses newcomers
and never evicts an existing peer. A session that presents no credential by the application's
deadline is closed. The close codes are `4401` (credential refused), `4408` (deadline expired) and
`4403` (session revoked).

**The ACL decides what a session's subject may do to a vertex.** A subject resolver turns the
caller into an opaque token, and the vertex's effective ACL is evaluated against it.

| Operation | Needs |
| --- | --- |
| read, `await`, reading a field, reading `:acl` | `READ` on the vertex |
| write, writing a field | `WRITE` on the vertex |
| subscribe | `READ` on the producer |
| a delivery arriving | `WRITE` on the target |
| creating a vertex or connection | `WRITE` on the parent or creator endpoint |
| crossing a mount | the operation's own right on the connection vertex |

- There are two rights: `READ=0x01` and `WRITE=0x02`. Any other bit is refused.
- `:acl` is written by the node's owner only.
- `EVERYONE@` is the only special subject.
- An ACE can carry `expires_ns`, judged against the `now` the application supplies. With no `now`,
  an expiring ALLOW grants nothing.
- The wire format carries the full NFSv4-style model. The MCU subset enforces ALLOW-only with a
  single `INHERIT` flag, and refuses an `:acl` write it could not honour.
- The default is open: enforcement is off until a subject resolver is installed, and a vertex
  with an empty effective ACL is unrestricted. One sentence of annex 05 implies the opposite, and
  [#2730](https://github.com/avatarsd-llc/libtracer/issues/2730) tracks correcting it.
- `:identity` is served to anyone, before any ACL check.

The protocol does authorization and not authentication. Proving who a peer is belongs to the
transport or a security module.

Rules: [annex 05 §`0x0A` ACL](reference/05-protocol-tlvs.md#0x0a--acl) and
[§`:identity`](reference/05-protocol-tlvs.md#the-node-identity-record--identity).
Depth: [Reference 16](reference/16-websocket-session-auth.md),
[security ACL module](modules/security-acl.md).

## 5. Transports and Noise

A transport moves whole frames and knows nothing about what is in them. Each connection is a vertex
at `/net/<module>/<name>`. Its value is the link's liveness, so a peer can `read`, `await` or
subscribe to it. Framing below the TLV belongs to each transport:

| Kind | Framing | Status |
| --- | --- | --- |
| `udp` | one frame per datagram | shipped |
| `tcp` | `u32` LE length, then the frame | shipped |
| `ws` | one RFC 6455 BINARY message per frame | shipped |
| `can` | the 29-bit ID is the path, so the TLV header is elided; multi-frame by address-shift | shipped (SocketCAN, TWAI) |
| `quic` | one stream with the `tcp` framing | shipped, optional module |
| `webtransport` | one stream with the `tcp` framing | shipped, optional module |
| `uart`, `unix` | not defined | planned |

An operation on a `DIAL` link that is not `up` fails at once with link-down and counts as a drop.
On a `dormant` link it also starts a connection attempt. Nothing is queued. Self-heal retries run
on the application's `now`.

**Noise** is an optional encrypted link binding, `Noise_NNpsk0_25519_ChaChaPoly_SHA256` with a
32-byte application-supplied PSK. It sits below the TLV layer, so the frames inside it are unchanged.
It costs a 58-byte first message, a 50-byte second message and 25 bytes per frame. A Noise link
never sends a frame in clear and never falls back to the plain carrier. Protection is per hop, not
end to end.

The reference implementation ships Noise on a host, over UDP. It does not run on ESP-IDF yet
([#2064](https://github.com/avatarsd-llc/libtracer/issues/2064)). **Noise over WebSocket is planned**
([#2296](https://github.com/avatarsd-llc/libtracer/issues/2296),
[#2297](https://github.com/avatarsd-llc/libtracer/issues/2297)).

Rules: [v1 §1](spec/v1.md#1-scope) leaves a transport's framing to its own binding; the Noise
binding is [annex 23](reference/23-noise-link-binding.md), which a link using Noise follows in full;
link behaviour on a down link is in [annex 23 §1](reference/23-noise-link-binding.md#1-what-the-binding-is-and-where-it-sits).
Depth: [Reference 19](reference/19-transports-are-vertices.md),
[Reference 14](reference/14-can-transport.md),
[transport module](modules/transport.md),
[connection config](modules/connection-config.md).

## 6. Batching and backpressure as an application sees them

### Batching

The application batches, and the library does not. To batch, the application:

1. composes one value out of its samples: a `BATCH` record (`0x80`, `opt.PL=1`) holding a `TIME`
   base and either framed `VALUE` children or one packed block of `n × width` bytes;
2. swaps it in as the vertex's value;
3. pushes it with `propagate`, or with the FOLD emission that sends one branch write for a subtree.

A `0x80` record is for a standalone push only, because a branch-write node admits no `0x80` child.
A folded batch has the same layout in the swept node's single structured `VALUE` (`opt.PL=1`).

The batch is a plain value, so the graph does not know it is one. There is no flush timer, rate
cap or batch window in the library. The `batch_count` and `batch_window_ns` subscription keys are
retired and nothing reads them. The `delivery_class` bits in a `SUBSCRIBER` policy are assigned but
not honoured.

### Backpressure

Every stage is bounded by memory the application injected, and running out is an answer, not a
crash. Each flow uses one of two arms:

| | Reliable | Best-effort (default) |
| --- | --- | --- |
| On a full ring | refuse admission, ring unchanged | shed the oldest entry |
| The local writer sees | `BACKPRESSURE` | success |
| The consumer sees | no gap | `tr::flow::address_shift_gap`, in order |

- The reliable arm is **local only**. There is no credit or backpressure frame on the wire, so a
  remote producer does not stall.
- A remote write refused for memory answers `tr::flow::backpressure` as an addressed error reply
  when a route exists.
- A frame the receiver has no memory for is drained and counted, with no reply, because the route
  has not been parsed yet.
- When a retry is possible the application decides whether it is safe. A lost request and a lost
  reply look the same to the sender.
- To confirm a stream, the receiving application writes back one "received up to N" value per
  batch. The library adds no per-frame acknowledgement.

Rules: [annex 05 §User range](reference/05-protocol-tlvs.md#user-range-0x80--0xff) for `BATCH`;
[§Error registry](reference/05-protocol-tlvs.md#error-registry-trconcepterror) for the error codes;
[§`0x04` SUBSCRIBER](reference/05-protocol-tlvs.md#0x04--subscriber) for the retired keys.
Depth: [Reference 22](reference/22-backpressure-and-sizing.md),
[Reference 18](reference/18-composition-over-the-network.md).

## 7. Implementing a device

There are two routes. **Port libtracer** by supplying its platform seams, or **write your own**
implementation against the specification in any language.

### What a minimal compatible device must do

A minimal device is one connection and a few vertices. It must:

1. parse and emit frames in every header, length, CRC and timestamp combination, accepting all of
   them on receive even if it only sends the smallest;
2. walk nested TLVs iteratively, bounded by its own decode memory;
3. refuse a frame with reserved `opt` bits or an invalid header, and survive a truncated one;
4. skip unknown type codes safely and answer the retired ones;
5. parse canonical `PATH` bodies, enforce the name and length limits, and step over PAIR records;
6. decode `FWD` and `FIELD`, reading the opcode as `op & 0x3F` and sending bits 6 and 7 as zero;
7. answer `read`, `write` and `await` on its own vertices, with `REPLY` and the registered errors;
8. honour an empty `src` as unacknowledged;
9. answer `tr::schema::not_found` for any `:field` it does not offer, and for every `await` with a
   `:field` selector, including on fields it does offer;
10. stop a delivery at its target, never relaying it onward;
11. give its application a path handle for each repeated address, usable without allocating or
    parsing on the hot path.

It does not need subscriptions, an ACL, a schema, creation, Noise or forwarding. A peer can poll a
device that offers none of those.

### Capability map

Every required capability, with its normative section and the vector that pins its bytes. A
vector proves the codec carries those bytes. It does not prove what a device does with them, so a
behaviour with no wire bytes has no vector and belongs in your own test suite.

| Required capability | Rule | Vector |
| --- | --- | --- |
| Header, `opt` bits, lengths | [annex 01 §Frame layout](reference/01-data-format.md#frame-layout) | `framing/empty-status-ok`, `framing/pl-bit-opaque`, `framing/pl-bit-structured`, `tlv-types/value-ll-u32` |
| All header variants accepted | [annex 01 §Interop](reference/01-data-format.md#interop-minimal-vs-feature-rich-implementations) | `framing/opt-all-bits-nested` |
| CRC trailer | [annex 01 §CRC](reference/01-data-format.md#crc) | `crc/value-crc32c`, `crc/value-crc16` |
| Timestamp trailer | [annex 01 §Timestamp form](reference/01-data-format.md#timestamp-form) | `crc/value-ts-abs-crc32c`, `tlv-types/value-ts-abs`, `framing/relative-ts-nested` |
| Reserved `opt` bits refused | [annex 01 §Handling unknown type codes](reference/01-data-format.md#handling-unknown-type-codes) | `framing/reserved-bit0-reject`, `framing/reserved-bit7-reject` |
| Iterative parse | [annex 01 §Iterative parsing](reference/01-data-format.md#iterative-parsing-requirement) | `framing/opt-all-bits-nested` |
| Truncated or invalid frame | [annex 01 §Truncation](reference/01-data-format.md#truncation-handling) | missing: [#2731](https://github.com/avatarsd-llc/libtracer/issues/2731) |
| Unknown type codes skipped | [annex 01 §Handling unknown type codes](reference/01-data-format.md#handling-unknown-type-codes) | missing: [#2731](https://github.com/avatarsd-llc/libtracer/issues/2731) |
| Retired codes answered | [annex 05 §Reserved range](reference/05-protocol-tlvs.md#reserved-range-0x0f--0x1f) | `tlv-types/retired-route-handle-advertise`, `-compact`, `-handle-nack` |
| Core TLV payloads | [annex 05](reference/05-protocol-tlvs.md) | `tlv-types/*` |
| Canonical `PATH` | [annex 03 §Path syntax](reference/03-addressing.md#path-syntax) | `path/path-sensor-temp`, `path/path-deep-255-packed`, `path/path-reserved-brackets` |
| Stepping over PAIR records | [annex 05 §`0x06`](reference/05-protocol-tlvs.md#0x06--path) | `path/element-pair-encode`, `path/element-pair-bad-len`, `path/path-record-overruns-body` |
| `FWD` head | [annex 05 §Reserved range](reference/05-protocol-tlvs.md#reserved-range-0x0f--0x1f) | `fwd/fwd-read`, `fwd/fwd-write-value`, `fwd/fwd-await-timeout`, `fwd/fwd-reply-result` |
| `op` masking and reserved bits | [annex 05 §Reserved range](reference/05-protocol-tlvs.md#reserved-range-0x0f--0x1f) | missing: [#2731](https://github.com/avatarsd-llc/libtracer/issues/2731) |
| `FIELD` selector | [annex 05 §Reserved range](reference/05-protocol-tlvs.md#reserved-range-0x0f--0x1f) | `field/field-scalar`, `field/field-indexed`, `field/field-append`, `field/field-nested`, `field/field-wildcard` |
| Replies and errors | [§Error registry](reference/05-protocol-tlvs.md#error-registry-trconcepterror) | `errors/*`, `fwd/fwd-reply-error`, `fwd/reply-error-bare-ignored` |
| Unacknowledged write | [annex 05 §Reserved range](reference/05-protocol-tlvs.md#reserved-range-0x0f--0x1f) | missing: [#2731](https://github.com/avatarsd-llc/libtracer/issues/2731) |
| Unoffered field | [annex 03 §Field-path resolution](reference/03-addressing.md#field-path-resolution) | `settings/removed-knob`, `settings/delivery-mode-not-found` |
| `await` with a `:field` refused | [annex 05 §`:stats`](reference/05-protocol-tlvs.md#the-seam-census-record--stats) | missing: [#2731](https://github.com/avatarsd-llc/libtracer/issues/2731) |
| Path handles | [v1 §3.1](spec/v1.md#31-static-path-handle-conformance) | `path/path-sensor-temp` for the bytes; no wire bytes for the API |
| Delivery stops at the target | [annex 05 §`0x04`](reference/05-protocol-tlvs.md#0x04--subscriber) | no wire bytes |

### Optional parts

Each row is all-or-nothing: if you offer it, its rules apply.

| Optional | Offer it when | Rule | Vector |
| --- | --- | --- | --- |
| Forwarding | the device has two or more connections | [annex 05 §Reserved range](reference/05-protocol-tlvs.md#reserved-range-0x0f--0x1f) | `fwd/fwd-routed-two-mount`, `fwd/fwd-src-accumulated`, `fwd/reply-src-accumulates-forward-route-name` |
| Issuing PAIRs | you want shorter routes | [annex 05 §Path element PAIR](reference/05-protocol-tlvs.md#path-element-pair-escape-kind--0x16--routing-semantics) | `fwd/pair-*`, `fwd/subscribe-reverse-*` |
| Subscriptions | peers should be pushed values | [annex 05 §`0x04`](reference/05-protocol-tlvs.md#0x04--subscriber) | `fwd/fwd-write-subscriber-field`, `subscriber/*`, `framing/empty-status-ok` (unsubscribe) |
| ACL | the link is not trusted | [annex 05 §`0x0A`](reference/05-protocol-tlvs.md#0x0a--acl) | `acl/*` |
| Self-description | strangers should integrate it | [annex 05 §`0x07`](reference/05-protocol-tlvs.md#0x07--point) | `tlv-types/point-schema-app`, `schema/*` |
| Settings and counters | you expose app properties or seam counts | [annex 05 §`0x0B`](reference/05-protocol-tlvs.md#0x0b--settings) | `settings/*` |
| `:identity` | peers should pin a key | [annex 05 §`:identity`](reference/05-protocol-tlvs.md#the-node-identity-record--identity) | missing: [#2731](https://github.com/avatarsd-llc/libtracer/issues/2731) |
| Creation | peers may create vertices or connections | [annex 05 §`0x0E`](reference/05-protocol-tlvs.md#0x0e--spec) | `spec/*`, `conn/*` |
| `BATCH` records | you send many samples at once | [annex 05 §User range](reference/05-protocol-tlvs.md#user-range-0x80--0xff) | `stream/*` |
| Noise link | the link needs encryption | [annex 23](reference/23-noise-link-binding.md) | `noise/nnpsk0-appendix-a`; first and transport messages missing: [#2424](https://github.com/avatarsd-llc/libtracer/issues/2424), [#2402](https://github.com/avatarsd-llc/libtracer/issues/2402) |
| A transport | the device talks to anything | its own binding, [v1 §1](spec/v1.md#1-scope) | framing missing: [#2402](https://github.com/avatarsd-llc/libtracer/issues/2402) |

`:liveness.*` is not implemented anywhere, and a device should answer it as any unoffered field
([#586](https://github.com/avatarsd-llc/libtracer/issues/586)).

The compatibility test is [v1 §4](spec/v1.md#4-conformance): honour every MUST and pass the
vectors. A device that does not offer Noise does not replay the `noise/` transcript, and
[#2730](https://github.com/avatarsd-llc/libtracer/issues/2730) tracks making §4 say so.

### Porting libtracer to a new platform

The C++23 core builds with `-fno-exceptions -fno-rtti` and has no operating-system dependency in
its required modules. A port supplies:

| Seam | What you provide |
| --- | --- |
| Memory | one injected `tr::mem::block_source_t`, or the static arena on an MCU build. Exhaustion is a null return. |
| Links | a `transport_t`: send one frame's bytes, deliver inbound frames to a sink |
| Time | a `now` from your clock, passed to every deadline seam. The library reads no clock. |
| Concurrency | atomics, plus a guard policy chosen at compile time when more than one thread touches the graph |
| Noise (optional, hosts only today) | a crypto backend: OpenSSL, libsodium or PSA |

Packaged ports exist for ESP-IDF and PlatformIO. See the
[capability matrix](capability-matrix.md) for the current status of each port and core, and the
[ESP32 node profile](interop/esp32-production-node.md) for a worked embedded build.

To write your own implementation, you need atomic refcounts, a view over external memory, an
iterative parser, little-endian integer access and CRC-32C. Register it in the
[implementation registry](implementations.md) once it passes the vectors.

## Worked example: one acknowledged write

These are the bytes of the `fwd/fwd-write-value` vector: write the `u32` 1234 to `/sensor/temp`,
with replies to `/reply-ep`.

```text
0F 40 2A 00                          FWD, opt.PL=1, length 42
  01 00 01 00  01                    VALUE  op = WRITE
  06 00 0C 00  06 sensor 04 temp     PATH   dst = /sensor/temp
  06 00 09 00  08 reply-ep           PATH   src = /reply-ep
  01 00 04 00  D2 04 00 00           VALUE  payload = 1234
```

A subscribe is `fwd/fwd-write-subscriber-field`. It adds `FIELD{NAME "subscribers", VALUE
u8 = ELEMENT}` after `dst` and carries a `SUBSCRIBER` as the payload.

## Cost

| | Bytes or size |
| --- | --- |
| Smallest frame (empty `STATUS`) | 4 bytes |
| Frame header | 4 bytes, or 6 with `opt.LL` |
| Trailer | 0 to 12 bytes, sender's choice |
| A `FWD{WRITE}` of 4 payload bytes, above | 46 bytes |
| A path segment | 1 byte plus its name |
| A PAIR element | 11 bytes |
| Noise | 25 bytes per frame |

| Target | What applies |
| --- | --- |
| NARROW (MCU, tens of KB) | P0 is gated at 16 KB stripped on Cortex-M0; a single-transport leaf has a 25 KB design target ([Reference 12](reference/12-deployment-profiles.md)) |
| MID (gateway) | measured on the [performance page](performance.md) |
| WIDE (many-core host) | measured on the [performance page](performance.md) |

## What the library does not do

- It starts no timer and reads no clock.
- It does not interpret a value's bytes.
- It keeps no hidden buffer that a peer can grow.
- It does not retry on your behalf outside a link's self-heal, and it has no queue on a down link.
- It does not pick a route, fail over or fan a delivery onward.
- It does not batch or rate-limit for you.

## Pitfalls

| Symptom | Cause | Fix |
| --- | --- | --- |
| Peers reject your paths | a `PATH` written with child `NAME` TLVs | use the packed body with `opt.PL=0` |
| A forwarder fails on a valid frame | switching on the raw `op` byte | mask with `0x3F` |
| A subscriber never gets values through a mount | `WRITE` missing on the connection vertex | grant `READ` and `WRITE` on it |
| An ACL has no effect | no subject resolver installed | install one |
| An expiring ACE never grants | the application supplies no `now` | pass `now` on the ACL check |
| A remote producer overruns a reliable ring | the reliable arm is local only | confirm with a watermark value |

## Where the rules live

- [Tracer protocol v1](spec/v1.md), with its §3 annexes: [data format](reference/01-data-format.md),
  [protocol TLVs](reference/05-protocol-tlvs.md), [addressing §path syntax](reference/03-addressing.md#path-syntax),
  and [the Noise link binding](reference/23-noise-link-binding.md).
- The [conformance vectors](https://github.com/avatarsd-llc/libtracer/tree/main/tests/conformance/).
- The [glossary](../CONTEXT.md), incorporated by v1 §2 for terminology.
