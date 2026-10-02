# libtracer — context glossary

The canonical vocabulary of the libtracer protocol. It tracks the [reference suite](docs/reference/00-overview.md) and the normative [protocol specification](docs/spec/v1.md), and it is the vocabulary of record: where a term here and a term elsewhere in the doc set disagree, the other page is brought into line with this one.

Each entry names the canonical term, defines it, and lists the near-misses that must not be used for it. Mechanics, code locations and history live on the page an entry's **Detail** link names; the page for a C++ header is in the [file map](docs/modules/file-map.md), and the status of an ADR or RFC is in the [ADR and RFC index](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr-rfc-index.md). An _Avoid_ line is not stylistic — every phrase on it denotes something the protocol does not have, or denotes the wrong one of two things the protocol keeps deliberately separate.

## Language

### Versioning

**Protocol version**:
The integer version of the wire format and its specification — **v1** — frozen on release and learned at the discovery layer, never per frame. [Detail](docs/reference/01-data-format.md#versioning-and-compatibility).
_Avoid_: "wire format v0.1", per-frame version, `VR` / version bit.

**Release version**:
An implementation's semantic version, **decoupled** from the protocol version; it never signals a wire change. [Detail](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0002-versioning-protocol-vs-release-no-per-frame-version.md).
_Avoid_: calling this "the protocol version"; reading wire compatibility out of a release number.

**Discovery-layer versioning**:
The mechanism that keeps incompatible protocol versions apart — a distinct service name / port / CAN-ID prefix per protocol version — used **instead of** a per-frame version field.

**Version bit (`VR`)**:
Does not exist. The wire format carries no per-frame version field; `opt` bit 7 is a forever-reserved MUST-be-zero bit (a `VR` version-bump bit is a rejected design, `01` §rejected designs).
_Avoid_: "`VR` bit", "version bit in `opt`", "`opt.VR`".

**Capability negotiation**:
Does not exist: receivers MUST accept every `LL`/`CW`/`TF` variant. [Detail](docs/reference/01-data-format.md#interop-minimal-vs-feature-rich-implementations).
_Avoid_: "per-peer capability discovery", "feature negotiation handshake".

### Wire format

Canonical per reference `01` and `05`.

**`opt` byte**:
The 1-byte options bitfield of every TLV, bits 7→0 `R│PL│TS│CR│LL│CW│TF│R`. [Detail](docs/reference/01-data-format.md#options-bitfield).
_Avoid_: any `VR` (version) or `FP` (finite-pool) bit.

**TLV header**:
A 4-byte header (`type` u8, `opt` u8, `length` u16 LE), or 6 bytes when `opt.LL=1` (`length` u32 LE). Integrity and wire-time live in the optional **trailer**, never the header.
_Avoid_: "8-byte header", "`crc` in the header", "`length: varint`".

**Length field**:
Fixed-width little-endian — u16 (default) or u32 (`opt.LL=1`). No u64; oversize payloads address-shift across `ep[0..N]`.
_Avoid_: "LEB128", "finite-pool length encoding" (both rejected, `01` §rejected designs).

**Trailer**:
Optional bytes appended at egress and stripped at ingress, leaving the payload byte-identical across hops. Carries an optional wire-time timestamp (`opt.TS`) and/or CRC (`opt.CR`).

**CRC**:
A trailer-resident bit-flip check gated by `opt.CR`: CRC-32C, or CRC-16-CCITT when `opt.CW=1`. [Detail](docs/reference/01-data-format.md#crc).
_Avoid_: "XOR-16", "CRC in the header", "CRC always present".

**Structured TLV**:
A TLV with `opt.PL=1` whose payload is only child TLVs; its type code says what they mean. [Detail](docs/reference/05-protocol-tlvs.md#structured-tlvs).
_Avoid_: "LIST", "type `0x05`".

**Validation timing (lazy, per-level)**:
Validity is checked where a level is consumed; ingress checks only the CRC and the top header. [Detail](docs/reference/08-views-and-ownership.md#rope-aware-decode).
_Avoid_: "ingress rejects malformed frames"; "depth cap / `kMaxDepth`"; "validation is a separate pass".

### Graph, addressing & API

**read / write / await**:
The entire data API — three calls, plus refcount management. There is **no** `connect` / `disconnect` / `subscribe` primitive.
_Avoid_: "connect", "disconnect", "subscribe()" as API verbs.

**Field-write (the `:` control plane — the vertex's `ioctl`)**:
The control surface: subscriptions, ACLs and settings are optional `:` fields of one vertex, its `ioctl`, beside the `read`/`write` data plane and the `await` readiness plane. [Detail](docs/reference/02-graph-model.md#schema-and-field-discipline).
_Avoid_: "control facets are sub-vertices" (control facet ⇒ `:`, distinct identity ⇒ `/`); "every vertex must implement the control fields".

**Application field / field descriptor table**:
An owner-defined property at `:settings.app.<name>`, declared in a per-vertex table through the local host API only. [Detail](docs/reference/02-graph-model.md#owner-declared-application-fields-settingsapp).
_Avoid_: "remote field declaration"; "app knobs flat in `settings.*`"; "app fields wake `await`".

**Schema (`:schema`) — exactly one per vertex, describing *that* vertex**:
A vertex's synthesized self-description; a child's schema is read from the child. [Detail](docs/reference/05-protocol-tlvs.md#the-schema-read--two-parts-defined-precedence).
_Avoid_: "`:children.schema`"; "a schema per field"; "`:schema` lists the children".

**Node identity (`:identity`)**:
The node's public key, node-scoped and readable above the READ gate so a peer can pin it. [Detail](docs/reference/05-protocol-tlvs.md#the-node-identity-record--identity).
_Avoid_: "per-vertex identity"; "identity is behind the ACL".

**Announce write**:
A field write wakes nothing; the owner follows an applied change with an ordinary write at the vertex. [Detail](docs/reference/02-graph-model.md#owner-declared-application-fields-settingsapp).
_Avoid_: "await on a `:field`"; "consumers poll fields for changes".

**Field promotion (notification by vertex-promotion)**:
A `:` field has no subscribers; a datum that must be observed on its own is promoted to a `/` child vertex, by the producer. [Detail](docs/reference/02-graph-model.md#owner-declared-application-fields-settingsapp).
_Avoid_: "per-field subscriptions"; "every config knob is a vertex"; "the consumer can promote a producer's field".

**Structural vertex**:
A vertex that only holds a position in the path tree, such as `/net/<module>`; only its minter can tell it apart. [Detail](docs/reference/11-vertex-roles-and-aggregation.md#structural-vertices-the-net-planes-own-bookkeeping).
_Avoid_: "grouping vertex"; "the graph knows which vertices are structural".

**Path-as-route (a transport vertex mounts the peer's graph)**:
A remote vertex is addressed by its full path from the caller's root, walking through transport vertices; replies retrace the link. [Detail](docs/reference/03-addressing.md#routed-scope-path-as-route).
_Avoid_: "each hop strips one NAME segment"; "a global device name"; "a reply correlation-id"; "the path is location-independent".

**Path element**:
The unit an address is made of: a NAME (a segment record) or a PAIR, one node's owner-issued `(index, generation)` (an escape record). The string path is the truth; a PAIR chain is a learned cache. [Detail](docs/reference/05-protocol-tlvs.md#path-element-pair-escape-kind--0x16--routing-semantics).
_Avoid_: "path segment" for the element; "bound path", "vertex ref", "path label"; bare "label"; "a PAIR authorizes".

**Delivery compaction**:
Opt-in per-link stream labels: `ADVERTISE` binds a `u16` route handle per hop and `COMPACT` frames carry it. [Detail](docs/reference/05-protocol-tlvs.md#route-handle-frames--0x11-advertise-0x12-compact-0x13-handle_nack).
_Avoid_: "COMPACT plane", "compact streams"; "path label".

**SUBSCRIBER direction (producer-holds)**:
The edge lives on the source's `:subscribers[]` and names one target; the source's `:acl` gates subscribe, the target's gates fan-in, and delivery terminates at the target. [Detail](docs/reference/04-communication-flows.md#delivery-is-terminal).
_Avoid_: "the subscriber stores its sources"; "the consumer's ACL gates subscribe"; "the target knows which subscription wrote it"; "subscribing needs `connect`"; "a delivery relays onward".

**Graph (address) composition / composite subscription**:
The vertex tree as a composition axis: one subscription to a parent covers its subtree, and a read of it serves the composed branch. [Detail](docs/reference/02-graph-model.md#observing-structural-change).
_Avoid_: "a SUBSCRIBER per leaf"; "the graph tree is the TLV tree"; "`read('/x:[]')`".

**Subtree subscription / vertical bubbling**:
A subscription observes writes to its vertex and every descendant; `await` does not. [Detail](docs/reference/02-graph-model.md#subtree-subscriptions-branch-writes-and-write-creates).
_Avoid_: "a subscription sees only its own vertex"; "bubbling path-tags the delivery"; "await wakes on descendant writes".

**Branch write / decomposition**:
A POINT-tree write that decomposes: each value lands at its descendant vertex as a zero-copy subview. [Detail](docs/reference/02-graph-model.md#subtree-subscriptions-branch-writes-and-write-creates).
_Avoid_: "stored opaquely at the parent"; "the branch is a transaction"; "a wire batch container".

**Write-creates**:
A local write to a missing vertex creates it, `mkdir -p` style, under the CREATE bit; a remote one answers `not_found`. [Detail](docs/reference/02-graph-model.md#subtree-subscriptions-branch-writes-and-write-creates).
_Avoid_: "a remote write to an unknown path creates it"; "creation needs `:children[]`"; "field writes create".

**In-band vertex creation / creator endpoint**:
Creation is an ACL-gated write of a controller-spec to a device's creator endpoint, selecting a device-known type. [Detail](docs/reference/11-vertex-roles-and-aggregation.md#in-band-creation-and-the-type-catalog).
_Avoid_: "registered only out-of-band"; "creation is a `create` primitive"; one global creator path under `/net`.

**Controller vertex / controller ports / binding**:
A device-known unit: **create** exposes its port vertices, and **bind** is a separate SUBSCRIBER wiring step. [Detail](docs/reference/13-network-formation.md#creation--controllers-and-transport-connections-through-one-mechanism).
_Avoid_: "creation wires the controller"; "a controller is one monolithic vertex"; "the orchestrator defines the type".

**Transport vertex / connection vertex**:
A transport and each connection in it are `/` vertices, created by a `SPEC` write to `/net/<module>/conn`; the module fixes transport and role. The vertex is persistent; the link under it self-heals. [Detail](docs/reference/19-transports-are-vertices.md#what-is-a-vertex-and-what-is-not).
_Avoid_: "transport config is a `:settings` field"; "reconfigure by writing `:settings`"; "an idle connection is torn down"; "a global connection catalog"; "link token".

**Link state**:
A connection's six liveness values: `DORMANT`, `DIALING`, `RECONNECTING`, `UP`, `LISTENING`, `BIND_FAILED`. [Detail](docs/reference/13-network-formation.md#link-liveness).
_Avoid_: a seventh state; `UP` for a listener.

**Peer / peer symmetry**:
Anything that speaks the wire format; peers are symmetric, and the only asymmetries are per-operation. [Detail](docs/reference/13-network-formation.md#transient-hats-not-fixed-roles).
_Avoid_: "satellite", "main node", "master/slave", "the hub", "the coordinator".

**Naming authority / minting boundary**:
Where a name enters the graph; it must be addressable, checked by one shared predicate, and naming policy is the application's. [Detail](docs/reference/03-addressing.md#reserved-characters).
_Avoid_: "enumerable but not addressable"; "core derives the module name"; "`/net` is the network root" as a protocol fact.

**Network formation / orchestrator (ephemeral admin peer)**:
Cross-node wiring by ordinary writes: an orchestrator is a peer with `WRITE_ACL` that creates, binds and departs. [Detail](docs/reference/13-network-formation.md#the-formation-flow).
_Avoid_: "the orchestrator needs its own protocol"; "the orchestrator proxies the data"; "the reconciler is protocol".

**Access control (ACL) / subject-token**:
Local authorization of `subject → rights`, where the subject is a pluggable token the transport authenticates. [Detail](docs/reference/05-protocol-tlvs.md#0x0a--acl).
_Avoid_: "capabilities vs ACL"; "ACL authenticates"; "stronger identity means X.509 PKI".

**ACL entry (ACE, NFSv4-style) / inheritance**:
An NFSv4-style ALLOW/DENY entry with a subject and `access_mask`; `admin` is `WRITE_ACL`, and `INHERIT` covers a subtree. [Detail](docs/reference/05-protocol-tlvs.md#0x0a--acl).
_Avoid_: "admin is a catch-all"; "ACL is per-vertex only"; "MCU must implement DENY ordering".

**Per-subscriber delivery policy**:
A subscription's QoS, carried per edge in its SUBSCRIBER and enforced producer-side; byte-agnostic. [Detail](docs/reference/02-graph-model.md#subscriber-delivery-policy).
_Avoid_: "deadband is a QoS field"; "delivery policy is per-vertex"; "a magnitude in the policy bits".

**Owner-side storage declaration**:
`:settings` has no core knobs; retention and the copy-or-share threshold are owner-declared host-API parameters, never inherited. [Detail](docs/reference/02-graph-model.md#storage-is-declared-owner-side-and-nothing-is-inherited).
_Avoid_: "the vertex's QoS block"; "`:settings` resolves up the tree"; "`store_ref_min_bytes`" for the declaration; "pin ratio" / "`K`".

**Retention (`retention_t { NONE, LAST, N }`)**:
What a vertex keeps after delivery: nothing, its last value, or the last N; `NONE` is the pure relay. [Detail](docs/modules/graph.md#what-it-does).
_Avoid_: "durability" for retention; "a relay role".

**Pin borrow (of the inbound RX segment)**:
A pinned value holds its receive segment for its whole lifetime; the application owns that budget. [Detail](docs/reference/02-graph-model.md#storage-is-declared-owner-side-and-nothing-is-inherited).
_Avoid_: "pinning saves memory"; "the pin lasts for the callback"; "`K` bounds pool occupancy".

**Lazy / on-demand source (subscriber-gated production)**:
A vertex that produces only while subscribed, observing its own subscriber count. [Detail](docs/reference/12-deployment-profiles.md#rung-3--rtsp-source-p2).
_Avoid_: "a dedicated on-subscribe wire hook"; "the source always runs".

**Array-whole read / atomic multi-field write** (the LIST replacement):
An array-whole read like `read('/x:subscribers[]')` returns a `PL=1` reply whose children are the element TLVs (SUBSCRIBER `0x04` for subscribers). An atomic multi-field write is a **SETTINGS (`0x0B`)** TLV. Neither uses a generic container.
_Avoid_: "returns a LIST", "write a single LIST TLV".

**Element addressing (`[]` appends, `[n]` addresses)**:
`[n]` selects the n-th child of a field or value and `[]` appends one; indexing is structural, never temporal. [Detail](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0017-element-addressing-value-plane-index.md#b-semantics--n-is-structural-never-temporal).
_Avoid_: "`[n]` selects append-vs-overwrite"; "`[n]` reads the history ring"; "subscribe to element n".

**Addressed whole (a field with no member or slot surface)**:
A field addressed as one unit (`:acl`, `:subscribers`, `:children`, `:schema`); a deeper selector answers `not_found`. [Detail](docs/reference/04-communication-flows.md#subscribers-is-addressed-whole).
_Avoid_: "extra selector steps are harmless".

**`:subscribers[N]` is the unsubscribe**:
An empty `STATUS` clears the slot, a `SUBSCRIBER` replaces it, anything else is `TYPE_MISMATCH`. [Detail](docs/reference/02-graph-model.md#the-payload-discriminating-subscribersn-write).
_Avoid_: "write a SUBSCRIBER to `[N]` to install record N".

**Index mode (`SCALAR` / `ELEMENT` / `WILDCARD`)**:
A FIELD level's three forms: `:name`, `:name[N]` / `:name[]`, and `:name[*]`, which v1 encodes but no operation performs. [Detail](docs/reference/03-addressing.md#the-index-form-on-the-wire).
_Avoid_: other names for the modes; a textual path wildcard.

**Fixed-stride array**:
An array field of equal-size elements, indexed by offset; array-ness is a schema property, never a wire bit. [Detail](docs/reference/03-addressing.md#index-forms).
_Avoid_: "array type code", "`opt.ARRAY` bit".

**Address-shift slicing**:
A large payload split across `ep[0..N]` sharing one `(origin_peer_id, ts)`; totality is opt-in. [Detail](docs/reference/03-addressing.md#address-shift-slicing-replaces-wire-level-fragmentation).
_Avoid_: grouping by `ts` alone; "fragmentation".

**`origin_timestamp` (per-producer monotonic) / coherent sampling**:
The per-producer, strictly increasing `ts`; one `(origin, ts)` marks one coherent sample. Wire, sample and playout time are separate clocks. [Detail](docs/reference/01-data-format.md#application-domain-timestamps-are-not-the-wire-trailer-ts).
_Avoid_: "`origin_timestamp` is wall-clock"; "two nodes' timestamps are comparable"; "the trailer TS is the sample time".

**Batch convention / user-orchestrated batching**:
N samples folded into one written value with one `TIME` base, composed and pushed by the application. [Detail](docs/reference/22-backpressure-and-sizing.md#35-high-rate-acquisition--recipe-c-compose--swap--push).
_Avoid_: "the graph batches"; "a batch is a new type/role"; "composing a batch copies the samples".

**Cycle termination**:
Both planes are loop-free by construction; no dedup set, hop counter or depth cap exists. [Detail](docs/reference/07-host-embedding.md#loop-safety-by-explicit-source-routes).
_Avoid_: "a `hop_count`/dedup set"; "the dispatch-depth cap (32)".

**Wildcard delivery metadata**:
How a subtree subscriber learns a delivery's concrete path; no wire tag carries it. [Detail](docs/reference/03-addressing.md#subscriber-identity-across-a-subtree).
_Avoid_: "remote delivery carries the matched concrete `PATH`".

**Framing modes: full-TLV (full caps) vs header-elided (non-interactive bindings)**:
Self-describing full-TLV frames, or frames keyed on the transport's native id with the header elided; they coexist. [Detail](docs/reference/14-can-transport.md#the-in-band-advertise-frame-and-the-dynamic-map).
_Avoid_: "an either/or"; "the forwarder maps CAN IDs"; "the TLV header rides the CAN bus".

**Advertise + id-match → dynamic rope groups**:
An advertised manifest whose id-matched slices chain into one rope. [Detail](docs/reference/14-can-transport.md#the-in-band-advertise-frame-and-the-dynamic-map).
_Avoid_: "it obviates the rope delivery seam".

### Errors

**`tr::` error namespace**:
Protocol error identities, `tr::<concept>::<error>`, keyed by the eight protocol concepts. [Detail](docs/reference/05-protocol-tlvs.md#error-registry-trconcepterror).
_Avoid_: a flat byte registry; `tr::<layer>::<module>`; a user-error range.

**`tr::` (two registers — error identities vs. C++ symbols)**:
Concept-keyed error identities on the wire; layer-keyed C++ namespaces in the reference implementation. [Detail](docs/modules/index.md#c-api-reference).
_Avoid_: a C++ namespace named for an error concept.

**Registered code / string identity**:
An error's on-wire identity is either a compact **registered code** (a `u16` the frozen registry assigns to a built-in `tr::…` path) or the literal **string** path (for unbounded third-party stack extensions). Optional structured detail may attach to either. The split *is* the built-in-vs-extensible split.

**Severity / disposition**:
Per-error properties of the **registry entry**, never on the wire: `severity` ∈ `warn|error|critical`; `disposition` ∈ `transient` (retry) | `permanent` (don't retry this request) | `fatal` (tear down the peer). Derived at L4 on receipt.

**Closed error boundary**:
Applications **never** emit a protocol error; there is no user error range. An application failure is ordinary **data**, self-described by the application's schema — the same way the protocol defines no application data *types* ([ADR-0010](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0010-closed-protocol-error-boundary.md)).

**`ERROR` (`0x08`)**:
The structured TLV carrying a `tr::` error identity plus optional detail; its first child is the identity. [Detail](docs/reference/05-protocol-tlvs.md#0x08--error).

**Flow gap (`tr::flow::address_shift_gap` — a discontinuity in an ordered flow)**:
The one signal that in-order elements did not arrive, always accounted. [Detail](docs/reference/22-backpressure-and-sizing.md#2-the-two-pressure-arms-stated-once).
_Avoid_: a new gap code per producer; "silent drop-oldest"; confusing it with `tr::flow::backpressure`.

**`tr::version::mismatch`**:
A discovery/link-level error — "peer advertised an incompatible protocol version". Not a frame-parse outcome, because there is no per-frame version field to read. It replaces a byte code (`VERSION_MISMATCH 0x06`) in a flat registry.
_Avoid_: "`opt.VR` set higher than receiver supports"; the `0x06` byte code as an identity.

### Modules & memory substrate

**Required modules**:
The modules every conforming node links (frame codec, path resolver, view/refcount machinery, FWD forwarder/dispatcher when ≥2 transports) — equivalently conformance profile **P0**. They are not architecturally privileged.
_Avoid_: "Core" as a noun for a fixed privileged build (the `core/` *directory* and "core type codes `0x01–0x1F`" are unaffected).

**`io_dir_t`**:
The cache-coherency direction enum: `DEVICE_TO_CPU` (invalidate) and `CPU_TO_DEVICE` (clean). [Detail](docs/reference/09-memory-substrate.md#cache-coherency).
_Avoid_: `IO_DIR_READ`/`IO_DIR_WRITE`, or the unscoped form.

**Memory-binding spectrum / transparent byte router**:
Bytes bound as a snapshot, a shadow, or a live view; live, libtracer is a transparent byte router. [Detail](docs/reference/08-views-and-ownership.md#memory-binding-contract).
_Avoid_: "endpoints must snapshot/copy".

**Module ABI**:
Implementation-defined contracts between modules; nodes interoperate over the wire only. [Detail](docs/reference/10-module-catalog.md#module-abi).
_Avoid_: "the protocol defines the module ABI"; "the L0 seam is a C vtable".

**Module set (build-time-closed)**:
The per-target set of module types at a seam, closed at build time; instances stay runtime. [Detail](docs/reference/09-memory-substrate.md#module-set-traits).
_Avoid_: "registry", "catalog" or "manifest" for this; "closing the set makes connections static".

**Resource bound (no synthetic limits)**:
Every limit is an injected resource or per-target configuration, never a magic constant; the addressing bounds are the named exception. [Detail](docs/reference/22-backpressure-and-sizing.md#7-what-must-never-be-the-fix).
_Avoid_: "nesting depth cap 32"; "a hardcoded max frame size"; "the runtime protects users from bad designs".

**Block source / failable allocation**:
The single seam every core allocation draws from: raw single-owner blocks, with exhaustion reported by value. [Detail](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0083-one-allocation-seam.md).
_Avoid_: "control-plane allocation seam"; "only peer-provoked allocations use it"; "init allocates from the heap"; `std::pmr` or a throwing std allocator as the seam; "exhaustion throws".

**Placement module**:
The one owner of a block's header, padding and the choice between one block and a split, decided against the configured size-class table. [Detail](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0083-one-allocation-seam.md#decision).
_Avoid_: "each backend lays out its own header"; "the header always shares the payload's block".

**Store composition (folded / per-plane / per-thread)**:
One injected root per graph by default; per-plane and per-thread are opt-in sub-pool layouts the library derives from it. [Detail](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0083-one-allocation-seam.md#decision).
_Avoid_: "NARROW / MID / WIDE composition"; "no composition is the default"; "per-plane is the default"; "the deployer wires one source per plane"; "per-plane avoids contention".

**Sub-pool**:
A per-purpose share of the graph's root (values, tables, net) that the library derives so it can account and cap per purpose. [Detail](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0083-one-allocation-seam.md#decision).
_Avoid_: "a separately injected source"; "a sub-pool is a buffer the library owns".

**Reclamation domain (hazard domain)**:
Freeing a block a lock-free reader may still hold; the general domain was refuted, and each tenant answers it alone. [Detail](docs/reference/17-reclamation-policy.md#why-there-is-no-single-answer).
_Avoid_: "the reclamation domain" as a thing that exists; "hazard pointers" as the general answer.

**Seam park / collect**:
Retirement parks a vertex's value seam; the embedder's explicit `collect` frees it outside every graph lock. [Detail](docs/modules/graph.md#pitfalls).
_Avoid_: "park until teardown"; "collect() is reclamation".

**Segment / view**:
A view windows a refcounted memory segment; a NAME segment is one path component, encoded as a segment record. [Detail](docs/reference/05-protocol-tlvs.md#0x06--path).
_Avoid_: bare "segment" for a path component; "each PATH child is a NAME".

**Rope delivery / owning receiver**:
The owning delivery tier hands over a rope of views, a contiguous frame being one link. [Detail](docs/modules/transport.md#two-delivery-tiers).
_Avoid_: "a third receiver tier"; "a scattered frame must be flattened at ingress".

**Rope / assembly (reassembly)**:
A chain of views; assembly and reassembly chain views and never copy. [Detail](docs/reference/08-views-and-ownership.md#rope_tappendview--rope_tconcatrope--operator).
_Avoid_: "reassemble = copy into a contiguous buffer"; a per-hop copy called "reassembly".

**Published value / value block (`value_t`) and value reference (`value_ref_t`)**:
The refcounted block a vertex's last-value slot holds, and the owning handle `read` and `await` return. [Detail](docs/modules/graph.md#what-a-read-hands-back).
_Avoid_: "the LKV `shared_ptr`"; "the value rope"; calling the block a segment.

**Two compositions (memory vs TLV)**:
Two orthogonal trees over the same bytes: memory (view → rope) and meaning (opaque → structured TLV). [Detail](docs/reference/02-graph-model.md#the-two-compositions-storage-and-meaning).
_Avoid_: "a rope is a list of TLVs"; "a memory split must align to a TLV boundary".

**Enqueue-then-write**:
How a one-record-at-a-time link serializes senders: the first writes, later ones queue and return, a full queue drops and counts. [Detail](docs/modules/transport.md#shared-scaffolding).
_Avoid_: "the queue makes the write asynchronous"; "a full queue blocks".

## Terms that are commonly confused

Each row names a word with several meanings, or the near-miss most often used for a term. Words whose confusion an entry above already settles are not repeated here.

- **"version"** — two axes, one word. **Protocol version** is the integer wire-format version (**v1**), conformance-bearing and carried by the discovery layer; **release version** is an implementation's semantic version, arbitrary with respect to the wire. Say which axis. "v0.1 is the wire format" is a category error; "protocol v1 is the wire format" is the claim meant.
- **"segment"** — the most overloaded word in the vocabulary, with three unrelated senses. A **segment** is the refcounted block of backing memory a **view** windows (L1). A **NAME segment** is one `/`-separated path component, encoded as one packed **segment record** `[u8 len][utf8]` (RFC-0018), *not* as a NAME TLV. A **route segment** is a segment record a transport vertex strips as it forwards; a hop strips the whole **mount run** of them, not one (§Path-as-route). Never write bare "segment" for either of the last two. All three senses are unaffected by the §Path element layer above them: an element is what an address is made of, a segment record is one way of spelling one.
- **"Core"** — not a privileged unit and not a build. It means the **required modules** (conformance profile P0). The `core/` directory and the "core type codes `0x01–0x1F`" are different things that share the word.
- **"registry"** — three unrelated things carry the word: the wire **type-code registry**, the **error registry** (codes, severity, disposition), and — wrongly — the build-time **module set**, which is neither. Say **module set**.
- **"control plane"** — bound to the `:` field-write addressing plane and nothing else. The failable-allocation seam is the **block source**, not "the control-plane allocator"; a data-plane branch write draws from it.
- **"creator endpoint"** — the per-module `/net/<module>/conn` surface, one catalog per *(transport, role)* module. Not a single global creator path under `/net`, and not a `:children[]` creation *field* — that spelling was superseded by ADR-0059 and **removed** at RFC-0014 S7.
