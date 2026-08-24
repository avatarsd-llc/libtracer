# 21 — ROS 2 integration (`rmw_tracer`)

*Descriptive reference. This section describes how ROS 2 binds to libtracer: what the
adapter is, what maps one-for-one, where the two models genuinely differ, and what is not
built. It is **not** part of the normative wire protocol — `rmw_tracer` adds no TLV, no
`opt` bit and no error code, and a node that never hears of ROS is unaffected. The
normative surface is [`../spec/v1.md`](../spec/v1.md).*

*Most of this page describes **direction** rather than shipped code. The binding is one
translation unit long today. Every claim below says which it is, in the sentence that makes
it.*

*The suite [is not a roadmap](README.md#what-this-suite-is-not) and this page does not
become one: the "State" and "Built?" columns record what is true of the tree at the time of
writing, and the sequencing, priority and ownership of the work live in the issue tracker.*

---

## The adapter is an RMW implementation, not a bridge

ROS 2 is layered `rclcpp`/`rclpy` → `rcl` → **`rmw`** (the ROS MiddleWare abstraction) → a
concrete middleware. libtracer's binding is pinned at the `rmw` seam:
**`rmw_tracer`**, an ament package under
[`bindings/ros2/`](https://github.com/avatarsd-llc/libtracer/tree/main/bindings/ros2)
selected with `RMW_IMPLEMENTATION=rmw_tracer`, so an existing ROS 2 node runs over a
libtracer graph with no node code changes
([ADR-0023](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0023-ros2-binding-via-rmw-tracer.md)).

That choice reverses an earlier recommendation, and the reversal is on record. The research
note
[`docs/research/ros2-adapter.md`](https://github.com/avatarsd-llc/libtracer/blob/main/docs/research/ros2-adapter.md)
compared the two integration models Zenoh also offers — an `rclcpp`-level topic **bridge**
(`zenoh-plugin-ros2dds`-shaped) and a **native rmw** (`rmw_zenoh`-shaped) — and recommended
"bridge first, native rmw maybe never". Its own superseding admonition records that
ADR-0023 and
[ADR-0025](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0025-rmw-tracer-end-to-end-zero-copy-rcl-over-rdma.md)
overrode that recommendation, and that two of the note's premises (an egress-batching
prerequisite, and a small-message throughput deficit) no longer hold. The note remains the
architecture background and the record of the interface as first proposed; it is **not** the
current mapping, and the note retracts its own `:settings` QoS sketch — see
[QoS](#qos-the-settled-direction) below.

ADR-0023's reasons for rejecting the bridge as *the* integration: it is not transparent
(every application needs wiring), it double-buffers every message, it defeats intra-process
zero-copy, and it cannot carry ROS QoS faithfully. A bridge remains viable as a *tool* for
mixing two middlewares, which is a different job.

The roadmap position is explicit rather than implied. The tracker that carries this work —
[#92](https://github.com/avatarsd-llc/libtracer/issues/92), *"[roadmap, end-of-queue] Direct
browser-to-robot binding via rmw_tracer + WebTransport (ADR-0031)"*, labelled
`ready-for-human` — was grilled on 2026-08-06 and confirmed as an end-of-queue roadmap item
with no design call due. Its WebTransport half has since landed
([ADR-0043](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0043-quic-webtransport-optional-module-msquic.md)
Phase B); the `rmw_tracer` half is what remains under it.

---

## What maps one-for-one

The mapping below is ADR-0023's, as narrowed by the errata it carries and by
[`bindings/ros2/README.md`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/ros2/README.md),
which is the checklist of record for the implementation.

| ROS 2 (`rmw`) | libtracer | Built? |
| --- | --- | --- |
| topic `/ns/foo` | vertex at path `/ns/foo`; the ROS namespace is the path tree | no |
| message (CDR bytes from `rosidl`) | opaque `VALUE` payload — libtracer never parses it | n/a (the substrate is built) |
| message type name | the `:schema` POINT, for introspection only | no |
| `rmw_publish` | `write(path, VALUE)` — the payload is the bytes `rcl` already serialized | no |
| `rmw_create_subscription` | a SUBSCRIBER edge on the **producer** vertex's `:subscribers[]` | no |
| `rmw_take` | pop the retained history, copying into the user buffer | no |
| loaned message | a `view_t` — a borrowed view into the segment, no copy | no |
| `rmw_wait` + guard conditions | the graph's `await` plus subscription callbacks signalling readiness | no |
| service / client | a `…/_request` + `…/_response` path pair | no |
| `rmw_get_node_names`, graph guard conditions | the graph's structural feed (a parent's `:children[]`) | no |

Two rows carry most of the weight.

**The message stays opaque.** A ROS message is CDR bytes, and libtracer's load-bearing
claim 5 is that the graph imposes no shape on user data
([00-overview.md](00-overview.md)) — so a ROS message is a `VALUE` payload and nothing on
the data path needs `rosidl`. `:schema` records the type *name* for tools, not for parsing.
This is what makes the binding a binding rather than a protocol change: two libtracer nodes,
one running through `rmw_tracer` and one native, interoperate over the wire because the
ROS-ness is confined to the opaque payload and a type string
([ADR-0023](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0023-ros2-binding-via-rmw-tracer.md)).

**A `view_t` is a loaned message.** The `rmw` layer is thin — it forwards publish and take
to the middleware, so the middleware decides the copies — and the one place `rmw` can force
a copy is `rmw_take`, which by default copies into a user buffer. ADR-0023 therefore makes
the loaned-message API (`rmw_borrow_loaned_message` / `rmw_publish_loaned_message` /
`rmw_take_loaned_message` / `rmw_return_loaned_message`) **mandatory**, not an optimization:
omitting it hands the take-side latency back to DDS. None of those entry points is written.

---

## Where the models genuinely differ

These are not gaps in the adapter. They are places where a libtracer ruling and a DDS
assumption disagree, so `rmw_tracer` has to choose a behaviour and live with it.

### 1. A ROS topic name is location-independent; a libtracer path is a route

libtracer addresses a remote vertex by its **full path from the caller's own root, walking
through transport vertices** — the path-suffix below a transport vertex *is* the address on
its peer, and there is no separate global name
([CONTEXT.md](../../CONTEXT.md) §Path-as-route;
[ADR-0027](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0027-transport-and-connections-are-vertices.md),
[ADR-0061](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)).
The glossary states the negative directly: *"the path is location-independent"* is on its
_Avoid_ list.

A DDS topic name is the same string on every participant. A libtracer path is not: the robot's
own `/scan` is reached from a workstation as `/net/ws-client/<name>/scan`. So the topic↔path
map `rmw_tracer` holds is **per node and per link**, not a global table, and two ROS nodes on
opposite sides of a forwarder do not spell the same topic the same way.

This is cheap at the NARROW end and expensive at the WIDE end. A constrained node with one
upstream link has exactly one prefix to prepend; a multi-hop fabric has one per route, and
the adapter must decide whether ROS's flat topic namespace is reconstructed (by rewriting
prefixes) or exposed (by letting the ROS name carry the route).

### 2. Subscribing is a write to the producer, and the producer's ACL can refuse it

DDS matches readers and writers anonymously by topic name and QoS compatibility. libtracer
has no matching step: a SUBSCRIBER edge lives on the **source** vertex's `:subscribers[]`
and carries the delivery target, the write is issued by the consumer acting as a client, and
**the source's `:acl` gates it**
([CONTEXT.md](../../CONTEXT.md) §SUBSCRIBER direction;
[ADR-0026](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0026-consumer-initiated-subscription-client-write.md);
the ACE model is
[ADR-0020](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md)).

So `rmw_create_subscription` becomes an addressed, authorized, **refusable** operation
against a named producer. Two consequences the adapter must answer, neither of which DDS
poses: which `rmw_ret_t` a permission denial becomes, and what a subscription means before
the producer is reachable at all (DDS lets a reader exist unmatched indefinitely; a
libtracer subscribe either lands on a vertex or does not).

Whose session the edge belongs to is itself ruled: a wire SUBSCRIBER whose `PATH` target
routes through a transport mount binds the edge to *that mount's link*, so a third party can
wire two peers together and depart without the edge dying with it
([RFC-0021](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0021-wire-subscriber-target-frame-of-reference.md);
the flow is [13-network-formation.md](13-network-formation.md)).

### 3. Depth is declared at each end, never negotiated

ROS's `KEEP_LAST(depth)` is a property of an endpoint, and each reader gets its own. libtracer
keeps that shape, by a different mechanism.

Delivery to a subscriber **is an ordinary write to that subscription's target vertex**, and the
target lives on the *consumer's* node
([RFC-0007](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0007-delivery-terminates-at-target.md);
[CONTEXT.md](../../CONTEXT.md) §SUBSCRIBER direction). Fan-in resolves by the **target's** role —
overwrite for a stored value, **append for a stream** — so a consumer that declares its target
STREAM gets its own bounded history ring, trimmed to a depth its own application declares through
its own `graph_t::set_history_depth`: *"Role 2: the CONSUMER's bounded history ring"*
(`core/include/libtracer/vertex.hpp:208`). Each reader does get its own queue, sized by the reader
— in BYTES, against the source that reader injects through `graph_t::set_ring_source`
([RFC-0025](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0025-stream-class-values.md) §4.6.1).
That ring is what `rmw_take` pops.

What has no analogue is **negotiation**. `set_history_depth` has no wire surface and nothing is
inherited
([RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§3.C/§3.F), so each end declares its own retention and neither can read or constrain the other's.
A remote subscriber cannot choose the **producer's** depth, and no DDS-style QoS-compatibility
check between two endpoints has any wire basis to run on.

Per-subscription delivery policy *does* exist, and is **per-edge rather than per-vertex** —
`reliability`, `priority` and `durability_request`, packed in the SUBSCRIBER's `SETTINGS` child and
enforced producer-side before fan-out (RFC-0022 §3.A; [CONTEXT.md](../../CONTEXT.md)
§Per-subscriber delivery policy, which names *"delivery policy is per-vertex"* as a thing to
avoid saying). What it deliberately excludes is **magnitudes**: a depth or a queue bound is never
packed into policy bits, and would arrive as a full-width field in the subscription's cold half if
something ever implemented one (§3.A, §3.E).

So for `rmw_tracer` the depth mapping is direct — `KEEP_LAST(depth)` is the reader-side target
vertex's ring depth, set locally at `rmw_create_subscription`. What stays genuinely open is only
the *reporting* question: what `rmw_get_subscriptions_info_by_topic` should say about a publisher
whose depth is, by construction, not observable from the reader.

### 4. A publisher creates its own topic; a remote write does not

A ROS node's own topics are vertices it owns, and a **local** data write to a nonexistent
path creates it `mkdir -p` style, gated by the CREATE bit on the nearest existing ancestor
([RFC-0005](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md)
§Write-creates). So `rmw_create_publisher` for a node's own topic has a direct analogue: the
publisher brings its vertex into being on its own node, and remote peers then subscribe to it by
writing a SUBSCRIBER into its `:subscribers[]` ([CONTEXT.md](../../CONTEXT.md) §SUBSCRIBER
direction). That is the ordinary formation path, and nothing below narrows it.

What has no DDS analogue is the remote arm: a remote fieldless `FWD{WRITE}` to an unresolved
`dst` **answers `not_found` and creates nothing** (RFC-0005 §D amendment 1). Creating a
vertex on another node is an explicit, ACL-gated write to that node's **creator endpoint**
([ADR-0059](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md),
[RFC-0014](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)).
In DDS, a publisher's mere existence is enough to bring a topic into being on the fabric;
here, creation authority is local-or-governed-channel, and the asymmetry is the ruling rather
than an oversight.

### 5. Delivery terminates at the target, and every subscription is a subtree subscription

Two libtracer rules have no ROS counterpart, one subtractive and one additive.

- **Subtractive.** A delivery applies the target-local write effects and never re-dispatches
  to the target's own `:subscribers[]`
  ([RFC-0007](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0007-delivery-terminates-at-target.md)).
  A chain of plain vertices does not relay; re-emission is the logic behind the target. A ROS
  node that expects a "republish by wiring" topology has to write that node.
- **Additive.** Every subscription observes writes at its vertex **and at any descendant**,
  and a write bubbles to subscribers at each ancestor
  ([RFC-0005](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md);
  [CONTEXT.md](../../CONTEXT.md) §Subtree subscription). Subscribing to a ROS *namespace*
  with one edge is therefore free in the substrate — and is **not expressible in the `rmw`
  C API**, which knows only topics. It is reachable from a libtracer-native peer, or as an
  `rmw_tracer`-local extension that stock ROS tools would not see.

`await` is deliberately *not* subtree-scoped — it observes stores at its own vertex only —
so a wait set built on `await` and a subscription built on `:subscribers[]` do not see the
same events, and the adapter must not assume they do.

### 6. Discovery is not a flood

ROS 2 over DDS assumes Simple Discovery: participants find each other and learn every topic
without being told where to look. libtracer's net plane is **explicit-source-routed only**
([ADR-0040](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0040-net-plane-is-explicit-source-routed-only.md)),
peer enumeration is stateless
([ADR-0044](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)),
and discovery proper is an optional module rather than a wire behaviour
([10-module-catalog.md](10-module-catalog.md)).

What `rmw_get_node_names` and the graph guard conditions would be built on is the graph's own
structural feed — subscribing a parent's `:children[]` — which reports **what is reachable
through the links this node has**, not what exists on a LAN. `ros2 topic list` under
`rmw_tracer` would therefore answer a different question than it does under a DDS RMW, and
that difference is structural, not a missing feature.

### 7. Fan-in is decided by an ACL, not by a name

A DDS topic accepts N writers and M readers because they share a string. In libtracer many
subscriptions MAY fan into one target, and they are gated by the **target's** own write ACL
plus any firmware arity — a single-input sink refuses a second writer device-locally, with no
orchestrator present ([CONTEXT.md](../../CONTEXT.md) §SUBSCRIBER direction). "Two publishers
on `/cmd_vel`" is admissible, but it is an authorization outcome rather than a naming one.

---

## QoS: the settled direction

### The history, briefly

ADR-0023 originally mapped `rmw_qos_profile_t` onto a per-vertex `:settings` container, and
so did the research note's interface sketch. **That surface no longer exists.** RFC-0022
deleted `settings_t` outright and removed the `:settings.<knob>` write surface; a write to
any of those names answers `SCHEMA_NOT_FOUND`. The correction is recorded in ADR-0023's two
errata (2026-08-01 and 2026-08-03), in `bindings/ros2/README.md` §QoS (the 2026-08-04
retraction), and in the research note's second admonition. **Nothing shipped against the old
mapping** — `qos.c` was never written.

### The ruling

The direction is settled, and this page records it rather than reopening it. The
2026-08-19 ruling on [#92](https://github.com/avatarsd-llc/libtracer/issues/92) is a
**hybrid**: rmw-local emulation by default, plus per-edge subscription options for the one
thing a peer must cooperate with.

| `rmw_qos_profile_t` field | Where it goes | Status |
| --- | --- | --- |
| `reliability`, `durability` | the subscription's packed 16-bit `delivery_policy`, carried in the SUBSCRIBER's existing `qos_settings` SETTINGS child (RFC-0022 §3.A) | the carrier is built and round-trips; nothing in `rmw_tracer` sets it |
| `history` + `depth` | **rmw-local emulation** against the producer vertex's owner-sized STREAM ring — no wire surface | direction only; no code |
| `lifespan` | **rmw-local emulation** — no wire surface | direction only; no code |
| `deadline`, `liveliness` | **deliberately deferred** to one future RFC | that RFC does not exist |

Three things the ruling fixes, all of them direction rather than shipped behaviour:

1. **Emulation is the default.** Everything emulable is implemented inside `rmw_tracer`, with
   no wire surface — which is what the 2026-08-04 retraction already implied once the
   `:settings` container was gone.
2. **`SUBSCRIBER.qos_settings` is the only sanctioned wire carrier**, on the precedent of
   `delivery_compact` ([RFC-0004](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md)
   §E.1, described in [05-protocol-tlvs.md](05-protocol-tlvs.md) §SUBSCRIBER) — and today it
   needs to carry nothing new, because `delivery_policy` already lives there.
3. **No new wire surface may be added for QoS outside this shape** without the deferred RFC.

`delivery_policy` being *built* is not the same as it being *honoured*: the reference
implementation stores and reads back `reliability` and `priority` while the transport work
that would act on them is outstanding
([`core/include/libtracer/subscriber.hpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/include/libtracer/subscriber.hpp)
says so at the point of definition; the gap is priced in
[`docs/research/horizon.md`](https://github.com/avatarsd-llc/libtracer/blob/main/docs/research/horizon.md)
§B.3).

### Why `deadline` and `liveliness` are one RFC with something else

They are deferred to a **single** future RFC that answers both ROS deadline QoS and
time-aware / TSN-class delivery — the horizon gap recorded in
[#1385](https://github.com/avatarsd-llc/libtracer/issues/1385) and written up in
`docs/research/horizon.md` §B.3. They are two names for one design and must not be solved
twice.

That note also pins the shape any such proposal must respect, and both constraints are
libtracer rulings rather than preferences:

- **No magnitudes in the packed policy bits** (RFC-0022 §3.A). A deadline is a full-width
  field in the subscription's cold half or it is nothing; spending the reserved bits on a
  duration contradicts the rule that a bit-width on a magnitude is a synthetic limit.
- **Cross-producer ordering is undefined by design**
  ([ADR-0019](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0019-per-producer-monotonic-origin-timestamp.md)).
  A per-subscription deadline measured against the *receiving* node's clock needs no shared
  time base; anything sold as TSN-class determinism does, and adopting one is an ADR-level
  reversal, not an RFC detail. The two asks are separable and must stay separated.

`liveliness` is a different animal again and the horizon note recommends splitting it off
rather than smuggling it in.

### What has no mapping at all, stated plainly

Until that RFC exists, `deadline`, `liveliness` and `lifespan` have **no libtracer wire
mapping**, and `rmw_tracer` must either emulate them locally or decline to offer them. That
is the honest state and it was already the effective one — the deleted `deadline_ns` knob was
inert: writable, readable, visible in `:schema`, and honoured by nothing (RFC-0022 §2).

---

## What exists today

The whole package, as of this writing:

| Path | What it is |
| --- | --- |
| [`bindings/ros2/package.xml`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/ros2/package.xml), [`CMakeLists.txt`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/ros2/CMakeLists.txt) | the ament package — built with `colcon`, **not** by `core/`'s CMake or `ctest` |
| [`bindings/ros2/src/rmw_tracer/identity.c`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/ros2/src/rmw_tracer/identity.c) | the only real translation unit: `rmw_get_implementation_identifier` and `rmw_get_serialization_format` |
| [`tools/build-ros.sh`](https://github.com/avatarsd-llc/libtracer/blob/main/tools/build-ros.sh) | build-verify in `ros:jazzy` — libtracer, the ament package and the ROS `rmw` headers compile and link together |
| [`bindings/ros2/README.md`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/ros2/README.md) | the concept mapping, the QoS carriers and the phased entry-point checklist — the record of implementation intent |
| [`bindings/ros2/CHANGELOG.md`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/ros2/CHANGELOG.md) | the package's public-API log |

Three properties of that list matter more than its contents:

- **It is verified, but not in CI.** `tools/build-ros.sh` needs the ROS 2 / ament toolchain,
  which the runners do not have, so the check is a local developer step by design
  (ADR-0023 §Consequences). Nothing about `rmw_tracer` is gated on a pull request.
- **The package is not published.** It ships to no registry, `tools/sync-version.py`
  deliberately does not stamp it, and its `package.xml` reads `0.0.0` on purpose
  (`bindings/ros2/CHANGELOG.md`).
- **The project says so everywhere it lists implementations.** The root README and
  [the capability matrix](../capability-matrix.md) both record the ROS 2 binding as an early
  stub, not as a supported target.

### The work ahead

The full RMW C ABI is roughly 198 entry points. `bindings/ros2/README.md` stages them so that
each milestone is `colcon`-built and loadable, with every unimplemented function returning
`RMW_RET_UNSUPPORTED` so the library always links. That README is the checklist of record —
per-phase translation unit lists live there, not here, so they cannot drift apart.

| Phase | Milestone it unlocks | State |
| --- | --- | --- |
| R0 — loads | `rmw_tracer` loads; `ros2 doctor` sees it | `identity.c` only |
| R1 — pub/sub, copy path | a `talker`/`listener` pair over `rmw_tracer` | not started |
| R2 — zero-copy | the loaned-message API; a `view_t` *is* the loaned message | not started |
| R3 — QoS + graph | the mapping above; `ros2 topic list` | not started |
| R4 — services / actions | request/response path pairs, actions composed on top | not started |
| R5 — transport differentiators | ROS over CAN/UART, ROS into GPU memory | not started |

Each phase is validated in the `ros:jazzy` (and a CUDA image for R5) container, never on the
runners.

---

## The differentiators, and what each still needs

ADR-0023 and ADR-0025 name what `rmw_tracer` would reach that a DDS or Zenoh RMW does not.
Read them as a target list with a cost attached, not as a feature list.

**Graph-wide zero-copy loaned messages** — MID/WIDE. A `view_t` handed to `rmw_take_loaned_message`
is the take-side answer, and it is R2 work. ADR-0025 adds the honest constraint on the publish
side: only a **loanable** (fixed-size, pointerless) `rosidl` type can be constructed in place;
strings and unbounded sequences fall back to a single CDR serialize, which is a universal
ROS/DDS/Zenoh limitation rather than a libtracer one. `:schema` is where loanability would be
surfaced.

**ROS 2 over CAN or UART with header elision** — NARROW, and the sharpest of the three.
Framing is a transport-adapter concern
([ADR-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0022-transport-framing-modes-elided-full-tlv-advertise.md),
described in [14-can-transport.md](14-can-transport.md)), so `rmw_tracer` inherits it without
knowing which bus it is on. This is the rung-4 composition of
[12-deployment-profiles.md](12-deployment-profiles.md), and the reach it claims — a ROS node
on a 16 KB MCU — is the one differentiator that does not depend on an unbuilt module.

**ROS messages into GPU memory** — WIDE. `mem_cuda` backs a `VALUE` payload inside a
heterogeneous host+device rope
([ADR-0024](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md));
the module is catalogued at v1, opt-in ([10-module-catalog.md](10-module-catalog.md)).

**"rcl over RDMA"** — WIDE, and **the one place a reader should discount the ADR against the
catalog.** ADR-0025 composes end-to-end zero-copy from a loaned segment plus a zero-copy
transport: `mem_shared` / `transport_shm` intra-host, `transport_rdma` + `mem_rdma`
inter-host. The module catalog rates `transport_shm` **post-MVP** and `transport_rdma`
**future**, and it further records that v1 `mem_shared` is single-publisher / multi-reader,
with the cross-process case treated as MMIO and *copied*
([10-module-catalog.md](10-module-catalog.md) §hard integrations). ADR-0025 is a decision
about the shape of a path, not a description of a built one; the ADR itself lists those
modules as what the decision *needs*. Do not quote its latency framing as a measured result.

The downstream use case these compose into — a browser binding directly to a ROS 2 robot — is
[ADR-0031](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0031-direct-browser-to-robot-binding-and-webtransport.md),
which is candid that implementation is roadmap and that `rmw_tracer` is parked. Its
browser-facing half is landed and its `rmw_tracer` half is not, which is what keeps
[#92](https://github.com/avatarsd-llc/libtracer/issues/92) open.

---

## Pitfalls

- **Reading the phase table as a schedule.** It is a decomposition, not a plan with dates.
  The tracker is titled `[roadmap, end-of-queue]` and labelled `ready-for-human`; nothing
  here is committed to a release.
- **Expecting `ros2 topic list` to enumerate a LAN.** It would enumerate what is reachable
  through this node's links. There is no participant flood to enumerate (ADR-0040).
- **Expecting a subscriber to choose its own depth.** Ring depth is the owner's, declared
  through the host API with no wire surface (RFC-0022 §3.B). Per-subscription `depth` is
  something `rmw_tracer` must emulate.
- **Reading ADR-0025's chain as shipped.** Two of its three transports are post-MVP or future
  in the module catalog, and the intra-host SHM case copies at the process boundary in v1.
- **Treating any `:settings.*` QoS mapping as live.** Every document that shows one — the
  research note, ADR-0023's original text — is superseded in place and says so. A write to
  those names answers `SCHEMA_NOT_FOUND`.
- **Assuming the binding changes the wire.** It does not. If it ever needs to, that is a
  spec change and needs an RFC, which is precisely the boundary the QoS ruling above draws.

---

## See also

- [12-deployment-profiles.md](12-deployment-profiles.md) §Rung 4 — where a ROS 2 node sits in
  the deployment spectrum, and which modules that rung adds.
- [02-graph-model.md](02-graph-model.md) §cross-walk — the concept-by-concept comparison
  against ROS 2, DDS, MQTT and Zenoh.
- [13-network-formation.md](13-network-formation.md) — the discover / delegate / create / bind
  / depart sequence a ROS node's subscriptions would ride.
- [`bindings/ros2/README.md`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/ros2/README.md)
  — the implementation checklist of record.
- [ADR-0023](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0023-ros2-binding-via-rmw-tracer.md),
  [ADR-0025](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0025-rmw-tracer-end-to-end-zero-copy-rcl-over-rdma.md),
  [ADR-0031](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0031-direct-browser-to-robot-binding-and-webtransport.md)
  — the decisions, with their errata.
- [`docs/research/ros2-adapter.md`](https://github.com/avatarsd-llc/libtracer/blob/main/docs/research/ros2-adapter.md)
  — the superseded research note, kept as the dated record of the bridge-first analysis.
