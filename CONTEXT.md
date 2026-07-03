# libtracer — Context Glossary

The canonical vocabulary for the libtracer protocol. It **supersedes** the former `docs/plans/99-glossary.md` (removed), which predated the [docs/reference/](docs/reference/) suite and carried pre-spec definitions. It tracks `docs/reference/` and the normative [docs/spec/v1.md](docs/spec/v1.md) — except where a maintainer decision recorded here resolves a conflict the reference had not (e.g. **Versioning**, **LIST**, **ERROR `0x06`/`0x08`**, **`io_dir_t`**), in which case the affected docs are brought into line with this file.

## Language

### Versioning

**Protocol version**:
The integer version of the libtracer wire format and the spec that defines it — currently **v1**. Immutable once frozen; a wire-incompatible change would be **protocol v2**. Not encoded per-frame — peers learn it at the **discovery layer** (mDNS `_libtracer._tcp` = v1, `_libtracer-v2._tcp` = v2).
_Avoid_: "wire format v0.1", "format version 0.1", per-frame version, `VR` / version bit.

**Release version**:
The implementation's own semantic version — a git tag / package version (`library.json`, `Cargo.toml`, `package.json`), currently **0.0.x**. Arbitrary and **decoupled** from the protocol version: a 0.0.x release is an early, partial implementation of **protocol v1**.
_Avoid_: calling this "the protocol version" or "the wire format version".

**Discovery-layer versioning**:
The mechanism that keeps incompatible protocol versions apart — a distinct service name / port / CAN-ID prefix per protocol version — used **instead of** a per-frame version field.

**Version bit (`VR`)**:
Does not exist. The wire format carries no per-frame version field; `opt` bit 7 is a forever-reserved MUST-be-zero bit (a `VR` version-bump bit was a rejected design, `01` §rejected designs).
_Avoid_: "`VR` bit", "version bit in `opt`", "`opt.VR`".

**Capability negotiation**:
Does not exist. Receivers MUST accept every `LL`/`CW`/`TF` variant, so senders just default to compact and there is nothing to negotiate; protocol v1 has no other negotiable wire features ([ADR-0013](docs/adr/0013-v1-scope-boundaries.md)).
_Avoid_: "per-peer capability discovery", "feature negotiation handshake".

### Wire format (canonical per `docs/reference/01` + `05`)

**`opt` byte**:
The 1-byte options bitfield at offset 1 of every TLV; bits 7→0 are `R│PL│TS│CR│LL│CW│TF│R`. `PL`=payload-is-structured, `TS`=trailer-timestamp, `CR`=trailer-CRC, `LL`=length width, `CW`=CRC width, `TF`=timestamp form; bits 7 and 0 are reserved-must-be-zero (non-zero ⇒ reject as `INVALID`).
_Avoid_: the legacy `VR│PL│TS│FP│CR│reserved` layout; any `VR` (version) or `FP` (finite-pool) bit — neither exists.

**TLV header**:
A 4-byte header (`type` u8, `opt` u8, `length` u16 LE), or 6 bytes when `opt.LL=1` (`length` u32 LE). Integrity and wire-time live in the optional **trailer**, never the header.
_Avoid_: "8-byte header", "`crc` in the header", "`length: varint`".

**Length field**:
Fixed-width little-endian — u16 (default) or u32 (`opt.LL=1`). No u64; oversize payloads address-shift across `ep[0..N]`.
_Avoid_: "LEB128", "finite-pool length encoding" (both rejected, `01` §rejected designs).

**Trailer**:
Optional bytes appended at egress and stripped at ingress, leaving the payload byte-identical across hops. Carries an optional wire-time timestamp (`opt.TS`) and/or CRC (`opt.CR`).

**CRC**:
A trailer-resident frame check, gated by `opt.CR` — **CRC-32C** by default, CRC-16-CCITT when `opt.CW=1`, computed over payload + `trailer_ts` (header excluded). A bit-flip detector, not adversarial integrity.
_Avoid_: "XOR-16" (the stale code's checksum), "CRC in the header", "CRC always present".

**Structured TLV**:
Any TLV with `opt.PL=1`, whose payload is **purely** concatenated child TLVs; its **type code** declares what the children mean (PATH, SUBSCRIBER, POINT, FWD, …). There is no generic container type.
_Avoid_: "LIST", "type `0x05`", "graph-node-as-TLV LIST" — `0x05`/LIST is **retired**.

### Graph, addressing & API

**read / write / await**:
The entire data API — three calls, plus refcount management. There is **no** `connect` / `disconnect` / `subscribe` primitive.
_Avoid_: "connect", "disconnect", "subscribe()" as API verbs.

**Field-write (the `:` control plane — the vertex's `ioctl`)**:
The control surface: subscriptions, QoS, ACLs, liveness are all writable fields addressed via the `:` separator on a vertex (e.g. `/sensor/temp:subscribers[3]`). Subscribing *is* writing a SUBSCRIBER into a `:subscribers[]` slot. The mental model is **Linux on one fd**: `read`/`write` of the vertex = the **data** plane; `:field` writes = the **control** plane (the vertex's **`ioctl`**); `await` / `:subscribers[]` = the **readiness/notify** plane (`epoll`) — all on **one identity**. The `:` separator (vs `/`) is what keeps these as **facets of one vertex** rather than dissolving it into sub-vertices, exactly as `ioctl` does not spawn a new fd. `:fields` are **optional** (an unsupported one returns `SCHEMA_NOT_FOUND` — the `ENOTTY` of an unsupported ioctl), so a minimal endpoint (a 9-byte input) is a char-device with read/write only and no fields. They are also of **two kinds**, like ioctls: **protocol-defined standard** fields (`:subscribers`, `:acl`, `:settings`, `:children` — uniform, so a generic orchestrator works cross-device) and **device-private** fields (a device's own control ops, on the same vertex, device-bounded). The protocol owns the *addressing*; the device owns its *catalog* of what those ops accept.
_Avoid_: "a vertex's **control facets** are sub-vertices" (a sensor's `:acl`/`:subscribers` are `:` facets of one identity, not `/` children) — but a **genuinely distinct subsystem with its own lifecycle/stats/ACL (a transport, a live connection) IS its own `/` vertex**, created via the same `:children[]` mechanism ([ADR-0027](docs/adr/0027-transport-and-connections-are-vertices.md)); the rule is *control facet ⇒ `:`, distinct identity ⇒ `/`*; "every vertex must implement the control fields" (they are optional/`ENOTTY`-able); "device-specific control means leaving the protocol" (it is a device-private `:field`, like a driver-private ioctl).

**Path-as-route (a transport vertex mounts the peer's graph)**:
A remote endpoint is addressed by its **full path from the caller's own root, walking *through* transport-vertices** — there is no separate global name or destination field; the **path-suffix below a transport vertex *is* the address of a vertex on its peer**. A transport/connection vertex ([ADR-0027](docs/adr/0027-transport-and-connections-are-vertices.md)) therefore **mounts the peer's graph under itself**: its `:`-facets (`:settings`, `:stats`, `:acl`, `:children`) are the link's *own* control; its `/`-subtree is *the peer's tree*, reached by forwarding the unresolved suffix. e.g. from a web UI: `/net/<ws://ip>/can[0]/ow/<temp_sensor>` = walk the local `ws` connection vertex → forward `/can[0]/ow/<temp_sensor>` to the board → walk its `can[0]` vertex → forward `/ow/<temp_sensor>` over CAN → the 1-Wire sensor. Segments already carry the identifiers (`can[0]` = bus number, `<deviceid>` deduced from advertisement), so **the path needs no extra naming layer**. Routing is **hop-by-hop source-routing**: each transport vertex strips its own segment and forwards the rest (+ payload for `write`); read/await **replies retrace the same bidirectional link per-hop**, so no reply-address or correlation-id is needed. This is the send-side dual of the receive-side transport-vertex **mount**: the suffix a caller routes *through* a transport vertex equals the prefix that vertex mounts inbound data *under* — they MUST stay consistent, because they are **one operation** — the mount write is the `FWD` terminus `deliver_local` at the leaf ([ADR-0038](docs/adr/0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md)).
_Avoid_: "a remote write needs a separate target/destination field or a global device name" (the path-suffix is the address); "read/await need an RPC reply-address / correlation-id" (replies retrace the bidirectional link per-hop); "the path is location-independent" (it encodes the route — it is relative to the caller's root, like a URL or a mount path); "a transport vertex's `/`-subtree is local" (below a transport vertex is the *peer's* graph).

**SUBSCRIBER direction (producer-holds)**:
A SUBSCRIBER edge lives on the **source** vertex's `:subscribers[]` and carries a **target** (the delivery destination). The source holds its own subscriber list and fans out (matching the reference `Subscriber{target_key}` and the strawberry-fw `slot.subs` model). So "the controller input subscribes to the sensor" *means* `write('/sensor/temp:subscribers[]', SUBSCRIBER{target='/dev/ctrl0/in/temp'})` — the consumer is the **target**, the edge is **stored on the producer**, and **subscribe-authorization is gated by the source's `:acl`** (the producer authorizes its own subscribers). The subscribe-write is **consumer-initiated** — issued by the consumer acting as a *client* (REST-shaped: it holds no source endpoint, only its `origin_peer_id`), or by firmware / NVS config / an orchestrator issuing the *identical* write on its behalf ([ADR-0026](docs/adr/0026-consumer-initiated-subscription-client-write.md)). There is no privileged "default binding" — firmware-baked, NVS-restored, and third-party subscriptions are the same client-write.

The **`target` is singular** per edge, and the target is **subscription-unaware**: delivery is an ordinary **write** to the target, indistinguishable from a direct write — the target does not know *which* subscription, or *that any* subscription, produced it (load-bearing claim 2: delivery *is* a write). **Many subscriptions MAY fan into one target**; they resolve per the **target's role** (overwrite for stored-value, append for stream), not by the target arbitrating. **Fan-in is gated by the target's own `:acl`** ("who may write to me") plus optional firmware arity, so a single-input sink rejects a second writer **device-locally**, even with no orchestrator present — the dual of the source's subscribe-gate. The target is **control-passive but data-rich**: it grows no source plane, yet spans scalar → structured TLV → rope/stream (e.g. a GB RTSP frame group via advertise+id-match). Consequently, any **provenance** a consumer needs (which source/child changed) must travel **in the delivered data/metadata** ([RFC-0003](docs/spec/rfcs/0003-bridged-wildcard-delivery-path.md)), never be inferred by the target from the subscription.
_Avoid_: "the subscriber declares/stores its own sources" (producer-holds); "the consumer's ACL gates subscribe" (the *source's* does); "the source subscribes to the consumer" (the source fans out to its `target`s); "the target knows which subscription wrote it" (it is subscription-unaware — it just receives writes); "the sink cannot refuse fan-in" (the target's write-`:acl` does, device-locally); "the target must be a dumb/simple endpoint" (it is control-*passive*, not data-*simple* — it spans up to a GB rope/stream); "subscribing needs a `connect` primitive" (the consumer's `connect(to=…)` is SDK sugar over the field-write — no wire verb, [ADR-0006](docs/adr/0006-read-write-await-api-no-connect.md)).

**Graph (address) composition / composite subscription**:
The **third composition axis** — distinct from the memory-rope and TLV-tree of §_Two compositions_ — is the tree of **addressable vertices** by path / `:children[]` membership. A **composite** is a parent vertex; **subscribing to it covers its whole subtree with one edge** (every subscription is a §subtree subscription, [RFC-0005](docs/spec/rfcs/0005-subtree-subscriptions.md)), saving a per-leaf SUBSCRIBER. Delivery is the **written TLV as-is** (the producer's frame at its produced granularity — a leaf value or a whole §branch write); the aggregate **snapshot** stays available as a read (`read('/x:[]')`). The common pattern is read-snapshot-once on join, then subscribe (to the parent) for the tail.
_Avoid_: "subscribing a subtree needs a SUBSCRIBER per leaf"; "composite delivery re-encodes a delta or tags it with a path" (delivery is the written TLV as-is; remote concrete-path tagging is the separate, draft RFC-0003); "the graph tree is the same as the TLV tree" (the aggregate *projects* the subtree onto the TLV axis, but identity/hierarchy is its own axis).

**Subtree subscription / vertical bubbling** ([RFC-0005](docs/spec/rfcs/0005-subtree-subscriptions.md)):
Every subscription observes writes to its vertex **and to any descendant** — a leaf subscription is the trivial case. A write at a vertex therefore delivers to subscribers there and at each **ancestor** ("bubbling"), once per subscriber, carrying the **written TLV as-is** through the ordinary delivery machinery (local view clone; remote return-route FWD). Writes stay near-free when nobody listens: the ancestor fan-out happens only when a subscriber exists at-or-above the written vertex. `await` is NOT subtree-scoped — it observes stores at its own vertex only.
_Avoid_: "a subscription sees only its own vertex's writes"; "bubbling re-encodes or path-tags the delivery" (as-is; provenance travels in the data); "every write walks its ancestors" (only under a live ancestor subscriber); "await wakes on descendant writes" (subscription concern, not the readiness plane).

**Branch write / decomposition** ([RFC-0005](docs/spec/rfcs/0005-subtree-subscriptions.md)):
A write whose payload is a **POINT tree rooted at the target vertex**. It **decomposes**: each value-carrying node lands at the corresponding descendant vertex as a refcount **subview of the written frame** (zero copy — address-shift-style slicing, never re-encoding), creating missing vertices on the way; each covered subscription point is notified once with its slice (leaf ⇒ its VALUE, interior ⇒ its POINT subtree, root and above ⇒ the frame as-is). **Values are the truth at the vertices where they land; a branch is a view**, and reads keep **one store per vertex** — the latest stored value, never behind a notification. **Cross-leaf atomicity is not promised** (each leaf is a consistent refcounted snapshot; coherence is the `(origin, ts)` sample group). Batching several subtrees is N self-contained frames in one `send(iov)`; the producer owns cadence (rate caps / flush intervals / dirty tracking / timers are application concerns).
_Avoid_: "a branch write is stored opaquely at the parent" (it decomposes; only value-carrying nodes store); "the branch is a transaction" (admission is all-or-nothing, application is per-leaf); "a wire batch container" (retired-LIST lesson — N frames, one `send(iov)`); "libtracer throttles or schedules pushes" (the producer owns cadence).

**Write-creates** ([RFC-0005](docs/spec/rfcs/0005-subtree-subscriptions.md)):
A **data write** (including a decomposed branch write) targeting a nonexistent vertex **creates it**, `mkdir -p` style with its missing intermediates, gated by the existing **CREATE** access bit on the nearest existing ancestor's effective ACL (`PermissionDenied` when denied; no ancestor ⇒ open). A child's *appearance* is therefore just its first write bubbling to the parent's subscribers. `:field` writes, `read`, and `await` on a nonexistent vertex keep `tr::path::not_found`.
_Avoid_: "a write to an unknown path is NOT_FOUND" (it creates); "creation needs `:children[]`" (that remains the typed-catalog path; a plain data write creates stored-value vertices); "field writes create" (no vertex, no control surface).

**In-band vertex creation / creation field**:
Creating a vertex is an **in-band, ACL-gated field-write** — an orchestrator writes a **controller-spec** into a device's **creation field** (e.g. `:children[]` / `:controllers[]`), and the device instantiates one of its **own known controller types**. This supersedes the earlier "registration is out-of-band, roles invisible" stance ([reference 11](docs/reference/11-vertex-roles-and-aggregation.md)): creation is a first-class graph operation expressed through the existing read/write API — **no new primitive**, it is a field-write. Roles stay invisible; what becomes visible is the device's **controller-type catalog** (the creation field's schema), never the internal role.
_Avoid_: "vertices can only be registered locally / out-of-band"; "the orchestrator injects an arbitrary role or code" (it *selects a device-declared type*); "creation is a new `create` primitive".

**Controller vertex / controller ports / binding**:
A **controller** is a device-known unit (the strawberry-fw logic-executor / wiring-diagram pattern; a [reference 11](docs/reference/11-vertex-roles-and-aggregation.md) role-3/role-4 vertex). **Creation and binding are separate steps:**
- **Create** — an orchestrator writes a controller-spec `{type, path, config}` (a type from the device's catalog, **no bindings**). The instantiated controller **exposes its own port vertices** — input-port and output-port endpoints in the graph.
- **Bind** — a *distinct* step: the orchestrator wires those ports to other vertices with **SUBSCRIBER** edges (a source's `:subscribers[]` → a controller input port; a controller output port's `:subscribers[]` → a sink).

The controller's logic reads its input ports and writes its output ports; the graph carries data across the bindings. A controller therefore **subscribes to nothing at creation** — what it consumes/produces is decided entirely by the separate binding step (a patch-cable / dataflow model).
_Avoid_: "creation wires the controller" (creation exposes ports; binding is separate); "the controller subscribes during instantiation"; "a controller is one monolithic vertex" (it exposes port endpoints); "the controller is a separate process" (it is vertices in the same graph); "a controller type the orchestrator defines" (the *device* defines its catalog).

**Transport vertex / connection vertex**:
A transport (`/dev/net/quic`, …) and **each live connection or listener inside it** are **first-class `/` vertices** — addressed with `/`, configured with `:settings` (`addr`, `port`, `role=DIAL|LISTEN`, keepalive), observed with `await` (link up/down), ACL-gated, and **created with the same in-band `:children[]` mechanism as a controller** ([ADR-0017](docs/adr/0017-in-band-vertex-creation-controller-orchestration.md), [ADR-0027](docs/adr/0027-transport-and-connections-are-vertices.md)). A node is therefore **one path tree** — data endpoints, controllers, *and* transports — addressed and orchestrated uniformly. This is *not* a violation of the `:`-not-`/` rule: a connection is a **distinct identity with its own lifecycle**, not a control facet of a data vertex. The **default link direction is consumer-dials / producer-pushes** (the SSE shape that pairs with consumer-initiated subscription and lets a constrained leaf dial out through NAT); `role` makes it overridable (dial out to a **router** for a constrained producer or NAT-on-both-sides).
_Avoid_: "transport config is a `:settings` field on some vertex" (a connection is its own `/` vertex); "a connection's params are `/`-path nodes" (the connection is the identity; its scalars are `:settings`); "opening a link needs a transport-specific API" (it is a `:children[]` `SPEC` write, like any creation).

**Network formation / orchestrator (ephemeral admin peer)**:
Forming a graph across nodes is done with **ordinary vertex writes** — there is no orchestration-specific protocol. An **orchestrator** (typically a **web UI**) is just a peer the owner granted `WRITE_ACL`, which joins **temporarily**, then on other devices **creates** controllers + transport connections (`:children[]`) and **binds** data flows (consumer-initiated writes into producers' `:subscribers[]`), then **departs — leaving the wired devices talking to each other** (the patch-cable that outlives the hand). The end-to-end flow is [reference 13](docs/reference/13-network-formation.md). A planned **declarative** layer (a desired-state **network manifest** + a continuous **reconciler**) is **tooling over this wire model** — it diffs the manifest against live `read`s and converges by issuing exactly these create+bind writes; it adds no wire behavior.
_Avoid_: "the orchestrator is a special role / needs its own protocol" (it is a peer with admin doing vertex writes); "the web UI must stay connected for the link to persist" (the wiring persists after it departs); "the manifest/reconciler is part of the protocol" (it is tooling-domain).

**Access control (ACL) / subject-token**:
Access is **authorization** — a device-held list of `subject → rights` (read / write / subscribe / create / admin) on a vertex or field, enforced locally on each operation (`PermissionDenied` on failure). The **subject is a token**, and the token is **pluggable** — this separates authorization from identity-provenance. v1 uses the transport-authenticated **`origin_peer_id`** as the token (advisory on an unauthenticated bus, since the protocol does **authorization, not authentication** — identity authenticity is the transport / security module's job). A later security module may supply **asymmetric credentials** as a stronger token **without changing the ACL model** — the ratified form is **raw-key ed25519 with trust-on-first-use pairing** (the public key *is* the identity; certificate-authority X.509 PKI is rejected — [ADR-0045](docs/adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md)). So **ACL-lists and capabilities are not rival models** — they are the same `subject→rights` authorization over a weaker vs. stronger token; the **token exchange / key management** is a separate, deferred concern. An **owner peer** is the provisioned root that bootstraps a device's ACL and **delegates admin** to orchestrators (enabling third-party binding).
_Avoid_: "capabilities vs ACL is an either/or"; "ACL authenticates" (it authorizes; the transport authenticates the token); "the subject is always a peer_id" (it is a pluggable token); "stronger identity means X.509 PKI" (rejected — raw-key ed25519 TOFU, a deferred module).

**ACL entry (ACE, NFSv4-style) / inheritance**:
Each grant is an **ACE** (access control entry, NFSv4-style): `{type: ALLOW|DENY, flags, subject-token, access_mask, expires_ns?}`. The **`access_mask`** is a bitfield — `READ`/`WRITE`/`SUBSCRIBE`/`CREATE` (add child)/`DELETE`/`READ_ACL`/`WRITE_ACL`/`WRITE_OWNER` — so the **`admin` right is precisely `WRITE_ACL`** (may modify the ACL / delegate), distinct from acting and from `WRITE_OWNER`. ACEs **ALLOW or DENY** (ordered, first-match-per-bit), and special subjects `OWNER@` / `EVERYONE@` avoid enumeration. **Composite ACLs propagate by an `INHERIT` flag** (NFSv4 inheritance, riding the graph/address composition): an ACE on a composite applies to its whole subtree — `:acl` is **not** written per-leaf, the way `:subscribers[]` is not. A vertex's **effective ACL** = its own ACEs + inherited ancestor ACEs. The **wire layout is the full model**; the **required-modules MCU profile enforces a subset** (ALLOW-only, single `INHERIT` flag), with full DENY / ordered evaluation in the `security_acl` host module.
_Avoid_: "admin is a vague catch-all" (it is `WRITE_ACL`); "ACL is per-vertex only" (composites inherit via `INHERIT`); "a grant is just a subject→permission bitfield" (it is an ACE with type/flags/mask); "MCU must implement DENY ordering" (subset is ALLOW-only).

**Per-subscriber delivery policy**:
A subscriber's **delivery QoS** — carried as child fields of its **SUBSCRIBER** TLV (so it is **per-edge**, not per-vertex like `:settings`) and enforced **producer-side**, before fan-out. It is **byte-agnostic only**: delivery mode (every sample / time-throttled / on-change-by-byte-diff), `min_interval`, `keepalive`. **Numeric or semantic filtering — deadband, tolerance, unit-aware gating — is not a libtracer concern**; it is application logic, implemented as a schema-aware **filter vertex** (a [reference 11](docs/reference/11-vertex-roles-and-aggregation.md) computed role) between producer and consumer. This keeps L4 dispatch from ever numerically interpreting opaque payload (load-bearing claim 5).
_Avoid_: "deadband is a SUBSCRIBER or QoS field"; "L4 dispatch compares payload *values*" (it may compare *bytes* for on-change, never interpret them); "delivery policy is per-vertex".

**Lazy / on-demand source (subscriber-gated production)**:
A vertex that **produces only while it has subscribers** — the RTSP / camera-stream pattern (subscribe to an "empty" vertex ⇒ frames begin flowing). Because **subscribing is a field-write** into `:subscribers[]` (load-bearing claim 2), a handler-role vertex observes its own subscriber-count **edge**: `0→1` activates the source (begin producing/publishing), `1→0` tears it down. The graph runtime's only contribution is "a vertex **may observe writes to its own control fields**"; there is **no separate subscribe/unsubscribe wire primitive**.
_Avoid_: "subscribing returns the data directly" (it registers an edge; the vertex reacting is what produces); "a dedicated on-subscribe wire hook" (it is a field-write the vertex observes); "the source always runs" (a lazy source is gated on subscriber count).

**Array-whole read / atomic multi-field write** (LIST replacement):
An array-whole read like `read('/x:subscribers[]')` returns a `PL=1` reply whose children are the element TLVs (SUBSCRIBER `0x04` for subscribers). An atomic multi-field write is a **SETTINGS (`0x0B`)** TLV. Neither uses a generic container.
_Avoid_: "returns a LIST", "write a single LIST TLV".

**Fixed-stride array**:
An array-typed vertex field whose elements are all the same size, so `:field[N]` resolves by direct offset (`base + N × stride`, O(1)) on **contiguous** backing. Variable-size arrays — and arrays whose in-memory rope scatters elements across segments — resolve by **walking** children (O(n)). Array-ness is a **schema (L4)** property, never a wire bit; on the wire an array is just a `PL=1` TLV with homogeneous children (ADR-0008).
_Avoid_: "array type code", "`opt.ARRAY` bit", a wire-level array marker — none exist.

**Address-shift slicing**:
The application-level replacement for wire-level fragmentation: a logically large payload is split across N child endpoints `ep[0..N]` sharing one timestamp; the receiver reassembles. A **group** is identified by **`(origin_peer_id, ts)`** — the same in-flight identity as the cycle-dedup recent-set — with each slice's `[index]` giving its position. **Totality is opt-in** (`expected_count` or a `:manifest`): a dropped *trailing* slice is not guaranteed-detectable ([ADR-0011](docs/adr/0011-address-shift-totality-opt-in.md)), while a missing *interior* slice surfaces `tr::flow::address_shift_gap`.
_Avoid_: grouping by `ts` alone (collides across publishers); "fragmentation", "FRAGMENT type code".

**`origin_timestamp` (per-producer monotonic) / coherent sampling**:
The `ts` half of the `(origin_peer_id, ts)` identity is a **per-producer monotonic** value (HLC-style), **not literal wall-clock**: strictly increasing per origin, never regressing or colliding (wall-clock-*seeded* where available, bumped logically on coarse/low-res clocks or NTP backward jumps). This is what makes `(origin, ts)` a **collision-free identity** for cycle-dedup and slice-grouping even when node clocks diverge. Wall-clock meaning is **advisory** (display / coarse correlation); **cross-producer ordering is undefined by design** ([reference 04](docs/reference/04-communication-flows.md): no global clock / CRDT). A `ts` is **optional on any TLV** (`opt.TS`); its primary use is **coherent sampling** — endpoints stamped with the **same `(origin, ts)`** form one coherent sample-group/snapshot (the *same* group primitive as address-shift slices). Cross-producer coherence needs a coordinated trigger or external clock sync, never cross-origin `ts` comparison.
_Avoid_: "`origin_timestamp` is wall-clock"; "two nodes' timestamps are comparable"; "per-producer `ts` can regress/collide"; "coherent sampling needs synced clocks" (within one producer it does not).

**Cycle termination**:
On the wire, the FWD plane is **loop-free by construction** ([ADR-0040](docs/adr/0040-net-plane-is-explicit-source-routed-only.md)): a frame's forward route (`dst`) strictly shrinks per hop and a `dst` revisiting a node is malformed (`INVALID_PATH`), so no dedup state or hop counter exists anywhere. In-process, a subscriber re-dispatch chain is bounded by the dispatch-depth cap (32, [ADR-0015](docs/adr/0015-graph-runtime-concurrency-and-in-process-cycle-cap.md)).
_Avoid_: "the net plane needs a `hop_count`/dedup set" (explicit source routes cannot loop); "the in-process cap is a wire mechanism" (it bounds local fan-out only).

**Wildcard delivery metadata**:
How a wildcard subscriber learns which concrete path produced each delivered TLV. **Local** delivery passes it out-of-band (implementation-defined); **remote** delivery carries the matched concrete `PATH` (`0x06`) on the wire (proposed under [RFC-0003](docs/spec/rfcs/0003-bridged-wildcard-delivery-path.md)).

**Framing modes: full-TLV (full caps) vs header-elided (non-interactive bindings)**:
Two **complementary** on-wire framing modes, chosen per-transport (and mixable per-frame); the forwarder is **uniform across both** and never does an identity↔path lookup ("does not feel the difference" — load-bearing claim 4).
- **Full-TLV ("full caps")**: self-describing frames carry the full PATH + control surface — enabling discovery, dynamic paths, in-band creation/ACL (the full feature set). Used on capable transports (IP/WS) where a 4-byte header is negligible, and for occasional control frames everywhere.
- **Header-elided ("non-interactive bindings" / transport-native addressing)**: the transport keys on its **native frame identity** (CAN ID, WS channel) via a **dynamic identity↔path map held inside the transport** (e.g. `transport_can`), self-establishing decentrally via in-band advertise frames ([ADR-0030](docs/adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md)); the TLV header is synthesized on ingress / elided on egress, so it **never hits the constrained bus** (zero added overhead — existing CAN/WS frames unchanged). For high-rate data on constrained buses (e.g. 100 ksps over CAN).

They coexist (a) **per-deployment** — an elided CAN leaf joined to a full-TLV IP backbone, the **transport adapter being the stateless translation point**; and (b) **per-transport** — an occasional **full-TLV control frame *establishes* the elided binding** ("full caps" sets up "non-interactive bindings"), which *is* the **`discovery_static` (pre-config) vs `discovery_mdns` (dynamic announce)** split. *(Zero-copy for large elided payloads needs the M6 rope-delivering transport seam; small samples cost a negligible ingress-synthesis copy.)*
_Avoid_: "elided vs full-TLV is an either/or" (they coexist); "the forwarder maps CAN IDs" (the *transport adapter* does); "the TLV header rides the CAN bus" (synthesized host-side); "header elision makes the forwarder transport-aware" (the adapter uniforms first).

**Advertise + id-match → dynamic rope groups**:
The advertise+id-match mechanism generalizes from a **single-value** binding (`id ↔ path`; lean frames are values) to a **rope/group** binding (`group-id ↔ (path, slice structure)`): a full-TLV **advertise** frame carries a runtime **manifest** (N slices, layout, total), and the lean id-matched **slice** frames that follow are **chained into a rope** by id+index at the reassembly layer. This is **[ADR-0011](docs/adr/0011-address-shift-totality-opt-in.md) address-shift slicing made dynamic** — the advertise frame is the manifest the ADR otherwise carries as a static `expected_count`/`:manifest`. The *same* mechanism thus spans a 9-byte elided CAN sample → a GB advertised rope group. **Zero-copy of the assembled rope requires the transport to deliver each slice as an owning/borrowable `view_t` (the M6 view-delivering seam)** — so advertise+id-match (graph protocol) and M6 (transport capability) **compose**; the flat-span seam alone forces a per-slice copy.
_Avoid_: "advertise+id-match obviates the M6 rope seam" (it composes with it for zero-copy); "dynamic slicing is a different mechanism from elided binding" (same advertise+id-match, with a structure in the advertise).

### Errors

**`tr::` error namespace**:
The identity space for **protocol/stack** errors — `tr::<concept>::<error>`, keyed by stable protocol **concept** (`frame`, `tlv`, `path`, `schema`, `flow`, `access`, `transport`, `version`), never by an implementation module. Prefix-filterable like a path (`tr::flow::*`). Specified in [RFC-0002](docs/spec/rfcs/0002-protocol-error-model.md) (proposed; ratification pending) + [ADR-0009](docs/adr/0009-built-in-error-model-tr-concept-namespace.md).
_Avoid_: the flat byte registry (`0x01 NOT_FOUND`, …), `tr::<layer>::<module>` (module-keyed), a `0x80–0xFF` user-error range.

**`tr::` (two registers — error identities vs. C++ symbols)**:
`tr::` names **two disjoint things** that must never be conflated. (1) On the wire / in logs it is the **error-identity** namespace above, keyed by the eight protocol **concepts**. (2) In the C reference implementation it is the **root C++ namespace** for code symbols, whose sub-namespaces mirror the **layer model** (`tr::mem` = L0, `tr::view` = L1, … — never the concept words). The two never collide because error identities are concept-keyed string paths, never C++ symbols, and code sub-namespaces are layer-keyed, never concept-keyed. Seeing `tr::frame::*` ⇒ an error identity; seeing `tr::mem::pool_t` ⇒ a C++ symbol.
_Avoid_: a C++ sub-namespace named for an error concept (`tr::frame`, `tr::flow` as code) — that re-introduces exactly the concept-vs-module conflation the error model forbids.

**Registered code / string identity**:
An error's on-wire identity is either a compact **registered code** (a `u16` the frozen registry assigns to a built-in `tr::…` path) or the literal **string** path (for unbounded third-party stack extensions). Optional structured detail may attach to either. The split *is* the built-in-vs-extensible split.

**Severity / disposition**:
Per-error properties of the **registry entry**, never on the wire: `severity` ∈ `warn|error|critical`; `disposition` ∈ `transient` (retry) | `permanent` (don't retry this request) | `fatal` (tear down the peer). Derived at L4 on receipt.

**Closed error boundary**:
Applications **never** emit a protocol error; there is no user error range. An application failure is ordinary **data**, self-described by the application's schema — the same way the protocol defines no application data *types* ([ADR-0010](docs/adr/0010-closed-protocol-error-boundary.md)).

**`ERROR` (`0x08`)**:
The TLV that carries a `tr::` error identity (registered code or string) plus optional detail. Always `opt.PL=1`; the first child is the identity — a `VALUE` for a registered code, a `NAME` for a string. Byte layout in [RFC-0002 §C](docs/spec/rfcs/0002-protocol-error-model.md). Supersedes the withdrawn "code as a leading child `VALUE`" shape (RFC-0001 §C.1).

**`tr::version::mismatch`** (was `VERSION_MISMATCH 0x06`):
A discovery/link-level error — "peer advertised an incompatible protocol version." Not a frame-parse outcome (there is no per-frame version field to read).
_Avoid_: "`opt.VR` set higher than receiver supports", the old `0x06` byte code.

### Modules & memory substrate

**Required modules**:
The modules every conforming node links (frame codec, path resolver, view/refcount machinery, FWD forwarder/dispatcher when ≥2 transports) — equivalently conformance profile **P0**. They are not architecturally privileged.
_Avoid_: "Core" as a noun for a fixed privileged build (the `core/` *directory* and "core type codes `0x01–0x1F`" are unaffected).

**`io_dir_t`**:
The L0 backend cache-coherency hook direction enum (`enum class`, `SCREAMING_SNAKE` scoped enumerators): `io_dir_t::DEVICE_TO_CPU` (DMA-in / RX ⇒ invalidate cache before the CPU reads HW-written bytes) and `io_dir_t::CPU_TO_DEVICE` (DMA-out / TX ⇒ clean cache so HW reads the CPU's last writes). Consumed by the two `mem_backend_t` cache hooks **`before_io`** (prep the cache for the device, pre-transfer) and **`after_io`** (reconcile the cache for the next reader, post-transfer); the method carries *timing*, the enum carries *direction*, the backend maps the pair to clean/invalidate. No-ops on cacheless cores (Cortex-M0/M3/M4).
_Avoid_: the `IO_DIR_READ`/`IO_DIR_WRITE` spelling, the unscoped `IO_DIR_DEVICE_TO_CPU` form (now scoped), and any other integer-value set — one canonical spelling only.

**Memory-binding spectrum / transparent byte router**:
The L0/L1 substrate is a modular binding layer: an endpoint's bytes may be bound as a heap snapshot, a shadow vertex, or a live/raw view (MMIO register, program variable — no copy, no CRC, lock-free). In the live case libtracer is a **transparent byte router** — it imposes no snapshot/copy/CRC. Each **backend module** (`mem_backend_t`) owns and declares its per-architecture contract: allocation, cache hooks, ISR-safety, atomicity granularity, memory ordering, `destroy` thread-affinity. Safety (snapshot/shadow) is recommended, never mandated ([ADR-0012](docs/adr/0012-modular-memory-binding-transparent-router.md)).
_Avoid_: "the protocol forbids live/raw memory binding", "endpoints must snapshot/copy".

**Module ABI**:
The C contracts between modules (`mem_backend_t`, `transport_vtable_t`, `abi_version`) — **implementation-defined by design**, not a protocol property; semver-stable within one implementation. Two conforming nodes interoperate over the **wire**, never via a shared ABI ([ADR-0013](docs/adr/0013-v1-scope-boundaries.md)).
_Avoid_: "the protocol defines the module ABI", "modules are binary-portable across implementations".

**Segment / view**:
A **view** is a `{owner, offset, length}` window over a refcounted **segment** of backing memory (`tr::view::segment` in the reference impl). Distinct from a **NAME segment** — a single `/`-separated path component, encoded as one NAME TLV (`0x02`).
_Avoid_: using bare "segment" for a path component; prefer "NAME segment" there and "view" for the L1 window.

**Rope / assembly (reassembly)**:
A **rope** is an ordered chain of **views** over (possibly different) segments — the L1 representation of a logically-contiguous payload that physically lives in scattered backing memory. **Assembly** and **reassembly** mean *constructing a rope by chaining views* — pointer-linking, **zero-copy, never `memcpy`**. The rope is also the **transport-agnostic scatter-gather representation**: each transport *lowers* it to its native DMA (`iovec`/`sendmsg`, CAN descriptor chains, RDMA verbs). A contiguous **copy** occurs at exactly one place — a substrate boundary a transport's DMA cannot span (e.g. lwIP pbuf → CAN region), flattened by the transport at **egress**, never per-fanout and never as "reassembly."
_Avoid_: "reassemble = copy the slices into a contiguous buffer"; "the substrate chooses the DMA" (the *transport* does); calling a per-fanout or per-hop copy "reassembly."

**Two compositions (memory vs TLV)**:
The same bytes belong to **two orthogonal composite trees**, and conflating them is a category error. (1) **Memory composition** (L1) composes *storage* — where bytes physically live: the leaf is a **view** (one window over one segment), the composite is a **rope** (a chain of views across segments). (2) **TLV composition** (L3) composes *meaning* — what bytes are: the leaf is an **opaque TLV** (`opt.PL=0`), the composite is a **structured TLV** (`opt.PL=1`) whose **type code** says what the children mean. Both are the Composite pattern, over different axes; they are decoupled, and that decoupling **is** the zero-copy story. Consequences: a **rope is not a TLV list** (storage vs meaning); a view boundary may fall **anywhere**, including mid-TLV-header (hence rope-aware / link-walking decode); and a `FWD` **uses** a rope (the route and payload bytes it carries onward) but **is not** one (it composes meaning around them).
_Avoid_: "a rope is a list of TLVs"; "a memory split must align to a TLV boundary"; "the router is a rope" / "rope inherits TLV".

## Flagged ambiguities (resolved)

- **"version"** — protocol = **v1** (integer, conformance-bearing, discovery-carried); release = **0.0.x** (arbitrary, git/package). "v0.1 is the wire format" is a category error → "protocol v1 is the wire format".
- **"LIST"** — retired (`0x05`); nesting is `opt.PL=1` + a purpose type byte; array reads concat element TLVs, multi-field writes use SETTINGS.
- **"Core"** — not a privileged unit; it means the **required modules** (profile P0).
- **"segment"** — overloaded; pin **view** (L1 window) vs **NAME segment** (path component).
- **`io_dir_t`** — `DEVICE_TO_CPU` / `CPU_TO_DEVICE` is canonical across reference 08/09/10.
- **"error identity"** — was a flat byte registry (`0x01 NOT_FOUND`, …); now the `tr::<concept>::<error>` namespace, registered-code-or-string, **protocol-only** (RFC-0002 proposed; ADR-0009/0010).
- **"array indexing"** — array-ness is an **L4 schema** property (fixed-stride ⇒ O(1) on contiguous backing), never a wire bit (ADR-0008).

## Example dialogue

> **Dev:** CI says we're on 0.0.1 but the spec says v1 — which is the breaking-change boundary?
> **Maintainer:** Different axes. **Protocol v1** is the boundary — it freezes and becomes immutable; a wire-incompatible change is **protocol v2** on a new discovery name. **0.0.1** is just this library's **release**; we'll ship 0.1, 1.0, 2.0 that all still speak **protocol v1**. No version bit on the wire — a peer learns we're v1 because we answer on `_libtracer._tcp`.
>
> **Dev:** I'm reading `:subscribers[]` — do I get a LIST back?
> **Maintainer:** No LIST anymore — `0x05` is retired. You get a structured (`PL=1`) reply whose children are SUBSCRIBER TLVs. To set several QoS fields at once you write a SETTINGS. The rule is always "`PL=1` plus a type byte that says what the children are."
