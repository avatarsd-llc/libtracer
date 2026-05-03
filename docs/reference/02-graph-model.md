# Reference 02 — Graph Model and Same-Substrate Insight

> **Status**: draft, v0.1, 2026-05-03. Defines what a graph IS in libtracer and the load-bearing claim that the in-memory graph and the wire bytes are the same substrate.
> **Audience**: implementers writing the router/dispatcher; anyone reasoning about zero-copy semantics.
> **See also**: [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) for API rationale; [01-data-format.md](01-data-format.md) for the byte layout this section interprets structurally.

---

## Definitions

- **Vertex** (also called endpoint, point, node-in-graph): a named, addressable position in the graph at which data can be written, read, or subscribed to. A vertex has a path, an optional schema, zero or more subscribers, and an optional last-known-value.

- **Edge**: a subscription. Concretely, a SUBSCRIBER TLV stored in some vertex's `:subscribers[N]` slot, naming a target path that should receive a copy of every future write.

- **Path**: an ordered list of UTF-8 NAME segments rooted at `/`, addressing a vertex (or a field on a vertex). Syntax in [03-addressing.md](03-addressing.md).

- **Schema**: a structured TLV (typically a `POINT` or a `SETTINGS`-shaped record) returned at `<vertex>:schema`, enumerating the writable fields the vertex exposes. Includes the core fields (`:subscribers[]`, `:settings.*`, `:liveness.*`, `:acl`) plus any module-namespaced fields. Read-only.

- **Router** (also called bridge): a vertex that receives TLVs from one transport and re-publishes them onto the local graph. To downstream subscribers a router is indistinguishable from a local source.

- **Buffer segment**: a refcounted region of real memory backing one or more views. The unit of ownership.

- **View**: a `{owner_segment, offset, length}` triple naming a contiguous span of bytes inside a buffer segment. Views never own bytes; segments do. A view holds a refcount on its owner segment.

- **Same-substrate**: the architectural claim that a TLV in memory IS a graph node IS the wire bytes. The in-memory graph is a tree of views; serialization is a walk of the tree; deserialization is constructing the tree of views over the received bytes.

---

## The six-layer model

The protocol stack has six distinct layers of concern, numbered bottom-up from memory at L0 to application semantics at L5. Concepts in this reference suite belong to exactly one layer; conflating them produces design confusion. Layers below L2 are platform substrate (memory, ownership, I/O); L2 and above are the protocol proper.

| Layer | Concern | What it sees | Doc that specifies it |
| ---- | ---- | ---- | ---- |
| **L0 — Memory substrate** | Real buffers, MMIO, queues, pools, peripheral FIFOs; allocation, lifetime, cache, DMA | platform-specific memory backends | [09-memory-substrate.md](09-memory-substrate.md) |
| **L1 — Views and ownership** | Refcounted memory views, ropes (chains of views), the TLV-as-cast | `segment`, `view`, refcount, rope chain | [08-views-and-ownership.md](08-views-and-ownership.md) |
| **L2 — Frame envelope** | Slice the byte stream into framed units; verify integrity; carry wire-time | `length`, `payload`, optional `trailer_ts` and `trailer_crc` | [01-data-format.md](01-data-format.md) |
| **L3 — TLV semantics** | Interpret the type code; recurse into structured (PL=1) containers | `type`, `opt.PL`, payload-as-bytes-or-children | [05-protocol-tlvs.md](05-protocol-tlvs.md) |
| **L4 — Graph endpoint logic** | Route TLVs to vertices, fan out to subscribers, enforce QoS / ACL, manage liveness, handle bridges | paths, vertices, edges, schemas, settings | [02-graph-model.md](02-graph-model.md) (this doc), [03-addressing.md](03-addressing.md), [04-communication-flows.md](04-communication-flows.md) |
| **L5 — Application semantics** | What the bytes inside a `VALUE` mean; what an endpoint's value represents; control logic over the data | application-defined | application code |

The substrate layers (L0 and L1) are what give libtracer zero-copy reach. A TLV in flight or at rest is **a view tree over real memory**, not a decoded message struct. The wire bytes IS the in-memory representation IS the graph node — across boundaries, the trailer attaches/strips ([01-data-format.md](01-data-format.md)) but the payload bytes are invariant.

The `type` byte sits at the L2 / L3 boundary. It is **carried** in the wire header (so a router can decide whether to recurse without parsing payload) but its **meaning** is L3. A pure-framing parser that just dispatches by `length + CRC` could ignore `type` entirely; a TLV-aware router uses `type` (and `opt.PL`) to decide whether to walk into nested children.

Priority is **NOT** an L2 concern. An earlier design carried priority bits in `opt`; that was wrong — priority is transport-time and per-link, not coherent across the network. A router that wants priority-aware dispatch reads `:settings.priority` once per subscription (L4) and caches it. See [01-data-format.md](01-data-format.md) §why no priority bits.

Implementations MAY refactor `type` out of the wire header (into "first byte of payload") in a future major version without semantic change; this is a layout question internal to L2/L3, not a protocol-level decision.

---

## The same-substrate insight

This is the load-bearing technical claim of the libtracer protocol.

**A TLV in memory IS a graph node IS the wire bytes.**

In most middleware:

- The wire encoding is one representation (CDR, Protobuf, Cap'n Proto, Zenoh's `z_encoding`).
- The in-memory message struct is another (decoded fields).
- The routing-topology graph is a third (separate metadata).

In libtracer, all three collapse into one. The mechanism: **buffer chains of views over real memory.**

### How nested TLVs work structurally

When the `PL` (payload-is-structured) bit is set in the header `opt` byte, the payload is interpreted as a sequence of child TLVs concatenated end-to-end. Each child has its own header (4 or 6 bytes per [01-data-format.md](01-data-format.md), depending on `opt.LL`) and optional trailer; any child may itself have `PL=1` for further nesting.

```
Outer structured TLV (PL=1, e.g. PATH, SETTINGS, ROUTER, or a user-range record):
  +-----------+--------+----------+
  | type=0xXX | opt=PL | length   |  header (4 bytes default; 6 if LL=1)
  +-----------+--------+----------+
  | inner TLV 1: header + payload  |
  +--------------------------------+
  | inner TLV 2: header + payload  |
  +--------------------------------+
  | inner TLV 3: header + payload  |
  +--------------------------------+
  | optional outer trailer         |  trailer (0 / 4 / 6 / 8 / 10 / 12 / 14 / 16 bytes)
  +--------------------------------+
```

Inner TLVs typically carry no trailer of their own — the outer's CRC (if present) covers the whole concatenated content. A bridge that wants to split children out and re-route them independently MAY emit them with their own trailers, paying the per-child cost.

This structured TLV IS the graph node. To walk a vertex's children: parse the children, iterate them, recurse (iteratively, per [01-data-format.md](01-data-format.md)) into any with `PL=1`.

### How that structured TLV exists in memory

Underneath, each inner TLV is represented as a **view**: a struct holding `{owner_segment, offset, length}` where `owner_segment` is a refcounted pointer to the real memory backing the buffer. The graph "contains" inner TLVs by holding views into the parent's memory.

```
Real memory (received from socket):
  [TCP recv buffer; 4 KiB; refcount=1]
   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Outer structured TLV view:
  { owner = recv_buffer_segment,
    offset = 0,
    length = 1024 }                       refcount on segment += 1

Inner TLV 1 view:
  { owner = recv_buffer_segment,           // same backing memory
    offset = 8 + len_of_outer_length_field,
    length = inner_1_length }              refcount on segment += 1

Inner TLV 2 view:
  { owner = recv_buffer_segment,           // same backing memory
    offset = 8 + len_of_outer_length_field + 8 + ... ,
    length = inner_2_length }              refcount on segment += 1
```

Operations that look like data manipulation — split a list into two, concatenate two lists, insert a new child, slice off the trailing N children — **do not move bytes**. They construct new view structs whose `owner_segment` field bumps the refcount of the underlying buffer.

When all views into a segment are released, the segment's refcount drops to zero, the segment's `destroy` callback fires, and the underlying memory is returned to whatever pool / allocator owns it (heap free, recv-pool return, mmap unmap, etc.).

### Structured TLV as abstraction, memory as rope

There is a careful distinction between **the structured TLV as a logical container** and **the memory backing it**.

A structured TLV's logical content is "an ordered sequence of children." That definition says nothing about how the children's bytes are laid out in memory. In practice:

- **Wire-receive case**: the structured TLV has just been reconstituted from a transport buffer. All children's bytes live contiguously in one segment. The "rope" has one link — a flat buffer.
- **In-memory assembled case**: the structured TLV was built up via append / concat / split operations. Different children may live in different segments — possibly received from different transports, possibly carved out of a memory-mapped region, possibly synthesized from static data. The rope has many links.

```
Logical structured-TLV view:
   ┌──────────────────────────────────────────┐
   │  child_0  child_1  child_2  child_3  ... │
   └──────────────────────────────────────────┘

Underlying rope (in-memory case):
   ┌─ view ─┐  ┌─ view ─┐    ┌─ view ──────────┐
   │ child_0│  │ child_1│    │ child_2 child_3 │
   │ in seg │  │ in seg │    │   in segment C  │
   │   A    │  │   B    │    │                 │
   └────────┘  └────────┘    └─────────────────┘
       │           │                 │
       ▼           ▼                 ▼
   segment A   segment B        segment C
   (refcounted) (refcounted)    (refcounted)
```

**Operations on a structured TLV do not move bytes**:

- `concat(C1, C2)` produces a new container whose rope is the concatenation of C1's view chain and C2's view chain. Refcounts on the underlying segments are bumped per child.
- `split(C, K)` produces two containers whose ropes share underlying segments with the original. The view chain is partitioned at child K.
- `insert(C, K, child)` produces a container whose rope is `C[0..K-1] + child + C[K..]`. The new child becomes another link in the chain.

**Serialization is the rope-to-flat-buffer walk**: when a structured TLV is sent on the wire, the serializer iterates the rope in order and emits bytes contiguously. The wire form IS contiguous; the in-memory form is NOT required to be. The proof obligation below guarantees that this walk produces the same bytes regardless of how the rope was assembled.

**The parser must handle both substrates** ([01-data-format.md](01-data-format.md) §two parser contexts):

- A wire-receive parser walks byte offsets within one buffer.
- An in-memory walker steps across view boundaries when crossing from one rope link to the next.

The same iterative pattern (recurse on `PL=1`, bound depth at 32) applies to both. Implementations typically share the parsing logic with two different cursor advance functions.

### Spec-level proof obligation

> Any sequence of mix / split / concat operations on a view tree, followed by a `serialize_to_wire()` walk, MUST produce the same bytes as if the corresponding mutations had been applied to a fresh buffer.

This invariant is testable: construct a view tree by parsing wire bytes, mutate it, serialize, and compare to the wire-bytes equivalent constructed from scratch. The reference implementation has `tests/test_substrate_invariant.c` exercising this in week 1 of [../plans/02-roadmap-weeks-1-to-8.md](../plans/02-roadmap-weeks-1-to-8.md).

A second-language implementation that fails this invariant is **not conforming**, regardless of whether its wire output is otherwise valid.

---

## Buffer ownership and refcounts

### Segment lifecycle

```
created (refcount = 1)              ←  initial owner (e.g. transport rx)
   |
   +─→ view created   (refcount += 1)
   |   view created   (refcount += 1)
   |   view destroyed (refcount −= 1)
   |   view destroyed (refcount −= 1)
   |   ...
   |
   ↓
initial owner releases (refcount −= 1)
   |
   ↓
last view released (refcount drops to 0)
   |
   ↓
destroy callback fires; memory returned to pool/allocator
```

### Required atomic operations

Implementations on multi-threaded hosts MUST use atomic refcounts with these memory orderings (canonical Boost intrusive_ptr pattern):

| Operation | Order | Why |
| ---- | ---- | ---- |
| Increment (clone view) | `relaxed` | Caller already holds a reference; data dependency travels via that existing reference |
| Decrement (release view) | `acq_rel` | release: flush all writes before someone else observes count drop; acquire: if we observe drop to zero, sync with all prior releases |
| Read for inspection (debug, metrics) | `acquire` | Pairs with each decrement; gives consistent snapshot |
| Weak-to-strong upgrade (CAS loop) | `acq_rel` on success, `acquire` on failure | Same logic as inc + sync with last decrementer |

Rationale and reference C23 implementation are in [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) §refcount memory ordering. Implementations in C++ (`std::atomic`), Rust (`Arc<T>` / `AtomicUsize`), or any other language MUST implement equivalent semantics.

### Single-threaded mode

For Cortex-M0/M0+ (no LDREX/STREX) and bare-metal single-threaded contexts, an implementation MAY substitute plain (non-atomic) integer refcount, provided the application guarantees no cross-thread sharing of segments. The reference C implementation exposes this as `LIBTRACER_NO_ATOMIC=ON`.

### Ownership transfer at endpoint delivery

When a transport module receives bytes from the wire, it constructs a top-level view over the received memory and hands it to the router via the recv callback. The router walks the view tree, finds the destination endpoint, and delivers the view to the endpoint's queue.

**At delivery, the view's ownership is *transferred* to the endpoint** — meaning the endpoint takes the existing refcount, no new copy is made. The transport module relinquishes its reference (refcount decrement; the segment survives because the endpoint now holds the count).

If multiple subscribers are attached, the view is **cloned** (refcount bumped per subscriber, no byte-level copy). Each subscriber sees the same backing memory through its own view struct.

This is the mechanism that makes "as fast as Cap'n Proto with pub/sub semantics" credible.

---

## Read is zero-copy; write is single-copy where the medium demands

The asymmetry is real and worth naming explicitly.

### Read paths

A reader that walks the graph or consumes a delivered TLV does so through views. **No bytes are copied.** The reader gets a `(pointer, length)` pair; it can `memcpy` into its own buffer if it wants a private copy, but the protocol does not impose this.

This is true regardless of where the bytes came from: a TLV constructed in-process from a static buffer, a TLV received over TCP and held in the recv-buffer segment, a TLV materialized from a memory-mapped GPIO register — all are reads through views.

### Write paths

A write of a TLV constructed in-process is similarly zero-copy at the API boundary: the caller hands over a TLV (which is a view tree); ownership transfers to the router. No copy.

The medium under a transport module determines whether a copy happens at the wire boundary:

| Transport | Send-side copy? | Receive-side copy? |
| ---- | ---- | ---- |
| In-process / in-thread | none | none |
| `transport_unix` (Unix domain socket) | one (kernel `write`) | one (kernel `read` into recv segment) |
| `transport_tcp` | one (kernel `send`) | one (kernel `recv` into recv segment) |
| `transport_shm` | none (the segment IS shared mem) | none |
| `transport_iceoryx2` (future) | none (loan-publish-borrow) | none |
| `transport_can` | one + per-CAN-frame fragmentation | one (HAL ISR copies each frame to RX buffer) |
| `transport_uart` / `transport_i2c` | one (DMA or per-byte) | one (RX buffer, then framed-TLV view over it) |
| `transport_rdma` (future) | none on the data plane | none |

The receive side on a stream/byte transport (UART, CAN, I²C, TCP) intrinsically needs an **RX buffer** because the bytes arrive incrementally and the framer needs to reconstitute a complete TLV. Once reconstituted, the framed TLV is a **view over the RX buffer**, and from that point on no further copies happen — the same view propagates through the router, fans out to subscribers, and is read by application code.

A subscriber that processes the TLV synchronously and releases its view immediately allows the RX buffer segment to be returned to the transport's free pool quickly. A subscriber that holds the view (e.g. enqueues for later processing) keeps the segment alive — backpressure on the segment pool is one of the QoS knobs (`queue_max_bytes`).

### The trailer enables payload-bytes invariance across boundaries

The wire-format trailer (`trailer_ts`, `trailer_crc`; see [01-data-format.md](01-data-format.md)) is what makes the read/write asymmetry above clean. The trailer is **append-only at egress, strip-only at ingress** — the payload region is never touched.

This means the same payload bytes flow through every state of the TLV's life:

| State | Form |
| ---- | ---- |
| Stored at a vertex (graph data) | `header + payload` |
| Recorded to disk by a recorder module | `header + payload` (trailer dropped at ingress to recorder) |
| Sent on a transport | `header + payload + trailer` (trailer attached at egress) |
| Received over a transport | `header + payload` (trailer validated and dropped) |
| Re-emitted by a bridge to another transport | `header + payload + new_trailer` (fresh wire-time, fresh CRC) |
| Replayed by a recorder module | `header + payload + new_trailer` |

In every state the **payload bytes are byte-identical** to every other state. A view that names those payload bytes survives unchanged through bridge re-emission, recorder round-trip, and subscriber fan-out. Subscribers can compute hashes / equality / signatures over the payload region without worrying about whether the TLV is currently in flight or at rest.

A subscriber that wants the application-domain timestamp reads it from a sibling `TIME` TLV inside the payload (if present) — that lives inside the payload bytes and survives every transition. The wire-trailer `TS` is for transport diagnostics only; it does not survive at-rest storage.

---

## Schema and field discipline

A vertex exposes a **schema** describing every writable field. The schema lives at `<vertex>:schema` as a read-only structured TLV (typically a `POINT` whose children describe each field, or a `SETTINGS`-shaped record).

### Core writable fields (frozen for v0.1)

| Field path | Type | Writable | Meaning |
| ---- | ---- | ---- | ---- |
| `:subscribers[N]` | SUBSCRIBER | yes | Subscription record N |
| `:subscribers[]` | sequence of SUBSCRIBER | read-only; write to `[]` appends | Full list (read) or new slot (write) |
| `:settings.reliability` | u8 enum | yes | `0=best-effort, 1=reliable` |
| `:settings.durability` | u8 enum | yes | `0=volatile, 1=transient-local` |
| `:settings.history_keep_last` | u32 | yes | N samples retained for late joiners |
| `:settings.deadline_ns` | u64 | yes | Max time between writes before liveness fault |
| `:settings.priority` | u8 | yes | `0=low ... 255=critical` (transport hint) |
| `:settings.queue_max_bytes` | u32 | yes | Per-subscriber queue cap (back-pressure threshold) |
| `:liveness.heartbeat_hz` | u8 | yes | Subscriber heartbeat rate; 0 = no liveness check |
| `:liveness.last_seen_ns` | u64 | read-only | Wall-clock of last write observed |
| `:liveness.missed_deadlines` | u32 | read-only | Counter |
| `:schema` | structured TLV | read-only | Self-describing schema of fields and types |
| `:description` | UTF-8 | yes (with permission) | Human-readable description |
| `:acl` | ACL | yes (with permission) | Access control list |

### Module-namespaced extension fields

A transport module like `transport_tcp` MAY add per-subscriber settings such as `:subscribers[N].settings.transport_tcp.send_buf_kb`. Rules:

- Module fields MUST live under their own module name (here, `transport_tcp`).
- Module names MUST match the module's directory in `libtracer/modules/` for the reference implementation; cross-implementation module-name uniqueness is a registry concern.
- Module fields MUST appear in the vertex's `:schema` output if they apply to that vertex.
- Reading a module field on a vertex where that module is not active returns `ERROR=SCHEMA_NOT_FOUND`.

### The graph imposes no shape

A vertex's writable fields are **whatever the schema says**. The protocol specifies a small set of mandatory core fields and a namespacing rule for extensions; it does NOT specify:

- How many subscribers a vertex may have.
- How many child vertices a parent vertex may have.
- The shape of an endpoint's data payload (a `VALUE` TLV's bytes are opaque to the protocol).
- The relationship between sibling vertices (e.g., `/camera/frame[0]` and `/camera/frame[1]` share a timestamp domain only if the application chooses).
- Whether a vertex is backed by RAM, MMIO, file, or function-on-read.

This is by design. [06-user-data-packing.md](06-user-data-packing.md) shows the full dynamic range — from a single boolean to a streaming 1 GB/s ADC — using the same vertex/edge primitives.

---

## Cross-walk to other middleware

For readers familiar with existing systems:

| Concept | libtracer | ROS 2 | DDS | MQTT | Zenoh |
| ---- | ---- | ---- | ---- | ---- | ---- |
| Named address | path (`/sensor/temp`) | topic (`/sensor/temp`) | topic | topic | keyexpr |
| Producer | writes to path | publishes to topic | DataWriter on topic | publishes to topic | put/publication |
| Consumer | subscribes (writes SUBSCRIBER) | DataReader subscribes | DataReader on topic | subscribes | get/subscription |
| Wildcards | `*`, `**` | not in topic, only in QoS partition | partition wildcard | `+`, `#` | `*`, `**` |
| Single API for ctrl + data | yes (field-write) | no (separate services) | no (DCPS + RPC) | no (extra packets) | no (separate `z_get` etc.) |
| Wire format | TLV (this doc) | DDS-CDR | CDR | proprietary | Zenoh proto |
| Discovery | module (mDNS/static/gossip) | DDS Simple Discovery | RTPS | broker | Zenoh scouting |
| Bridge | core | rmw bridges (rmw_zenoh) | DDS routing service | bridges | Zenoh routers |

The unifying feature: in libtracer, every cross-walk row is the **same primitive** (a TLV at a path), not a separate API.

---

## Graph data vs in-flight messages: the ROUTER shedding rule

The TLV substrate plays two distinct roles:

- **Graph data** — what's stored at a vertex. Identity = vertex path. Content = the user's payload, possibly a structured TLV with sibling metadata (e.g., `TIME`). No routing metadata.
- **In-flight message** — what crosses a transport between vertices, especially when bridged. Identity = `(origin_peer_id, origin_timestamp)`. Content = a `ROUTER` TLV (type `0x0D`, [05-protocol-tlvs.md](05-protocol-tlvs.md)) **wrapping** the data TLV: ROUTER is structured (PL=1) with NAME-tagged metadata children followed by `NAME "data"` and the wrapped TLV as its last child.

Both roles use the **same TLV substrate** — same wire format, same in-memory view tree. The difference is structural and lives in the `ROUTER` TLV's presence and where it appears.

### The shedding rule (mandatory)

When a bridge dispatches an incoming TLV into the local graph:

1. **Read the ROUTER** for `(origin_peer_id, origin_timestamp)` and check the bridge's recent-set ([07-host-embedding.md](07-host-embedding.md) §cycle handling).
2. If already seen, drop silently. No further action.
3. If new, add to recent-set and **strip the ROUTER from the structure** — the local graph stores only the bare data TLV at the proxy vertex.
4. Keep `(origin_peer_id, origin_timestamp, hop_count)` in the bridge's per-proxy metadata table for re-emission and dedup.

When the bare data TLV is then bridged out again to another transport:

1. Look up the ROUTER metadata from the bridge's table.
2. **Attach a fresh `ROUTER {...}, NAME "data", data}` wrapping** at egress, with `hop_count` incremented (ROUTER is the wrapper itself; the data TLV is its last child).
3. Append the wire trailer (`trailer_ts`, `trailer_crc`) per the egress transport.

### Why this matters

- **Graph reads are clean.** A subscriber reading `/can-bridge/wheel/left` gets the bare data TLV — same shape regardless of whether the value originated locally or arrived over CAN. No ROUTER pollution; no need for application code to skip routing metadata.
- **Recorder is simple.** Recording a vertex's value writes the bare data TLV. Replay does NOT replay the original ROUTER (which would alias the original sender's identity); replay is a fresh write from the recorder's identity.
- **Same substrate, two clean roles.** The protocol does not have separate "wire format" and "graph format" — it has one format with one optional wrapping that distinguishes the two roles.

### A worked sequence

```
                            Wire on CAN              Graph on Linux brain        Wire on TCP
Sender (STM32):
  emits ROUTER{origin=A, ts=T0, hop=1, ..., data=VALUE{...}}
  ────────────────────────────────────────►
                            (CAN frames carrying the ROUTER wrapping)
                                                     │
                            Bridge ingests, checks recent-set, NOT seen.
                            Strips ROUTER. Stores VALUE{...} at /can-bridge/X.
                            Records (A, T0, hop=1) in metadata.
                                                     │
                                                     ▼
                                             Subscriber on Linux reads
                                             /can-bridge/X → VALUE{...}
                                             (no ROUTER visible)
                                                     │
                                             ALSO subscribed by remote ESP32
                                             via TCP. Bridge re-emits:
                                                     │
                                                     ▼
                                             ROUTER{origin=A, ts=T0, hop=2, ..., data=VALUE{...}}
                                             ────────────────────────────►
                                                                          (TCP carrying the ROUTER wrapping)
                                                                          │
                                                                          ESP32 bridge ingests.
                                                                          (A, T0) NOT in its recent-set.
                                                                          Strips ROUTER. Stores at
                                                                          /peer/linux-brain/can-bridge/X.
```

If the ESP32 then bridges back to a different CAN bus that the original sender STM32 also listens on (cycle), the STM32's bridge would receive the wrapped TLV, look up `(A, T0)` in its recent-set, find it (because A is the STM32 itself), and drop. **Cycle terminates.**

This shedding rule is what keeps the global topology safe for any shape — see [07-host-embedding.md](07-host-embedding.md).
