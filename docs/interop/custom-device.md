# Building a custom interoperable device

A worked, generic example of a vendor device — and the detailed matrix of what a
custom device **can**, **may**, and **must not** implement while staying
interoperable. This is the practical companion to the
[interoperability model](../interoperability.md): that page says *why* interop is a
legibility discipline; this page says exactly *what to build*.

> **Audience**: a vendor (or a vendor's coding agent) designing its own libtracer
> device from scratch — its vertex tree, its self-description, its transports, and
> the seams where its proprietary logic attaches.

---

## The running example: a vendor widget

Take a deliberately ordinary device: a mains-powered widget with one ambient
sensor, one actuator, and a push-button. Its entire public identity is one vertex
tree:

```text
/                        (root)
├── sensor/
│   └── temperature      stored value, f32 °C, published on the owner's cadence
├── actuator/
│   └── level            stored value, f32 0..1 — REMOTE-WRITABLE (this is the seam)
├── button/
│   └── gesture          stream vertex, u8 event code — subscribe-only in practice
└── net/                 transport vertices (one per loaded transport/listener)
```

Everything a third party can do with this device follows from the uniform surface:

- **Discover** — walk the tree; `read /sensor/temperature:schema` returns the
  vertex's NAME, its protocol knobs, and (if the vendor installed a descriptor
  table) the legible `app` part.
- **Observe** — `write /sensor/temperature:subscribers[] SUBSCRIBER{target=...}`
  (consumer-initiated; the producer holds the edge and fans out).
- **Actuate** — `write /actuator/level VALUE{0.7}`. Delivery *is* a write
  (load-bearing claim 2); the device's apply seam turns the written bytes into a PWM
  duty, a relay state, whatever the hardware does.
- **Wire it to another device** — an orchestrator subscribes this widget's
  `temperature` to another vendor's controller input, then departs. Neither vendor
  ever heard of the other.

The vendor's proprietary logic — filtering, safety interlocks, calibration — lives
entirely *behind* the vertices, in the owner's handlers. Nothing of it leaks into
the protocol surface, which is why it never has to be disclosed or standardized.

### The apply seam

A write to a `STORED_VALUE` vertex stores bytes and does nothing else. Hardware
moves only where the owner attached a seam:

| Written surface | Owner seam | Registration |
| --- | --- | --- |
| The vertex **value** (`write /actuator/level VALUE{…}`) | `handlers_t::on_write` — receives the written rope **and a `write_ctx_t`** carrying the writer's `subject` (empty ⇒ the local host); the value is **consumed, not stored** | `register_vertex(path, role_t::HANDLER, handlers)` |
| A declared **app field** (`write /actuator/level:settings.app.ramp_ms VALUE{…}`) | `handlers_t::on_app_field_write` — fires after the bytes are stored, with the field key and the written TLV | `register_vertex(…, handlers)`, or alongside a descriptor table |

Both seams are declared on the same `handlers_t`; their signatures and the
lock/re-entrancy rules are in
[graph — declaring owner fields](../modules/graph.md#declaring-owner-fields) and
[graph — interface](../modules/graph.md#interface). The runtime validates
*addressing* only — declared or undeclared, writable or not. Range, dtype and
interlock checking is the owner's, in the seam.

`on_write`'s second argument is the **write context**: `write_ctx_t::subject` is the
writer's resolved subject token — exactly what the vertex's `:acl` was just evaluated
against — and is **empty** for a local host write (`is_local_owner()`). It is *borrowed for
the call*, the same contract as the rope beside it, so **copy it if you retain it**. A seam
that needs per-writer behaviour (an auth endpoint, a per-operator interlock) keys off it
instead of inventing an out-of-band channel.

### The legibility part (what makes it integrable by a stranger)

Names alone make the tree navigable; descriptors make it unambiguous. The vendor
installs an owner field-descriptor table
([RFC-0010 — owner-writable application property fields](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0010-owner-app-fields-and-schema.md))
whose descriptor bytes the runtime serves **verbatim** from `:schema` — the ideal
carrier for a semantic tag plus a natural-language brief:

```cpp
/** @brief Legibility table for /actuator/level — descriptors served verbatim via :schema. */
static constexpr std::array<std::byte, kRampDescLen> kRampDesc = encode_ramp_descriptor();

static constexpr std::array<tr::graph::app_field_static_t, 1> kLevelFields{{
    // key below settings.app.  remote access                 descriptor bytes
    {"ramp_ms",                 tr::graph::app_access_t::RW,  kRampDesc},
}};

graph.set_app_fields_static(level_vertex, kLevelFields);
```

A descriptor is a structured TLV the runtime never parses. Its vocabulary —
`dtype`, `unit`, `min`/`max`, `label` — is a SHOULD-level convention so that
generic consumers converge, and owner-defined extras ride alongside opaquely: a
semantic tag such as `com.example.widget.actuator.ramp_ms` is exactly such an
extra. `access` is the one member an owner must **not** supply: the runtime
projects it from the table, so a descriptor cannot claim `rw` on a field the table
declares `ro`. The bytes themselves are emitted at build time — a `constexpr`
encoder, a code generator, or a hand-written byte literal — so the whole legibility
layer can live in `.rodata` and cost zero heap.

The `static` above is load-bearing: the runtime views **the array as well as the
bytes it points at**, so both must outlive the vertex. The parameter type
(`tr::graph::borrowed_fields_t`) accepts the array spellings a `constexpr` table
takes and rejects a `std::vector`, so the usual way to get this wrong is a compile
error rather than a use-after-free. A table whose size is only known at run time
needs storage you keep alive yourself, installed via
`tr::graph::borrowed_fields_t::unchecked(...)`; if you would rather the runtime own
a copy, use the owning `set_app_fields` instead.

---

## What a custom device CAN implement (all optional, all conforming)

Every row here is a free choice. A device may implement any subset — including
none — and remains a conforming node that any forwarder routes and any peer reads.

| Capability | What it buys | Cost when skipped |
| ---- | ---- | ---- |
| **Any vertex layout** | The graph imposes no shape (claim 5): one boolean vertex, a 10 GB frame across `ep[0..N]` slices, an MMIO register as a live view | none — layout is yours |
| **`:schema` descriptors** (RFC-0010) | unassisted integration by a stranger's reader | integrators need your datasheet |
| **Remote-writable actuators** | third parties drive your hardware through the apply seam | device is observe-only |
| **`:subscribers[]` fan-out** | push delivery, subtree subscriptions, lazy sources | peers poll with `read` |
| **`:acl` (ALLOW-only MCU subset)** | device-local authorization: who may read/write/subscribe/create | open device (fine on a trusted bus) |
| **In-band creation** (a `SPEC` write — to a creator endpoint for connections, to `:children[]` for your own registered types) | orchestrators instantiate your connections and controllers at run time, bounded by your own catalog | fixed function; wiring baked at build |
| **Vertex retirement** | a dynamic child can be withdrawn: its address answers `PATH_NOT_FOUND`, its subscriber edges are evicted, and a later revive inherits nothing of the old owner | the tree only grows; a withdrawn child stays addressable and keeps delivering |
| **Write-creates** | your own local writes materialize vertices `mkdir -p`-style under CREATE ACL (a *peer* creates through the creator endpoint — a remote write to an unresolved address is `PATH_NOT_FOUND`) | static tree only |
| **Multiple transports + FWD** | the device becomes a forwarder — one address space across CAN + IP | leaf node on one link |
| **Header-elided framing** (e.g. CAN) | zero protocol overhead on constrained buses; the TLV header never hits the wire | full-TLV frames everywhere |
| **Address-shift slicing** | payloads beyond one frame, grouped by `(origin, ts)` | payloads bounded by transport frame |
| **Lazy production** | produce only while `:subscribers[]` is non-empty (the RTSP pattern) | always-on producers |
| **Discovery module** (mDNS static/dynamic) | peers find you; versioning rides the service name | peers are configured with your address |

### In-band creation: the surface to build against

Creation is not a new verb. It is an **append of a `SPEC` TLV to a parent's
`:children[]` field**, gated by that parent's `CREATE` right
(`core/src/graph.cpp:3598-3602`;
[ADR-0020 — NFSv4-style ACEs with inheritance](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md)).
The SPEC's `type` member names one of the device's registered child types and its
optional `config` SETTINGS carries the instantiation parameters; an unregistered
`type` answers `SCHEMA_NOT_FOUND`, the `ENOTTY` of an unsupported field
(`graph_t::create_child`, `core/src/graph.cpp:3649-3676`). Reading `:children[]`
returns the parent's **members**, never SPECs.

**The `/net` plane is the exception, and it is now a different door.** A connection is
*not* created through `:children[]`: it is created by writing
`SPEC{ NAME "name" NAME <name>, NAME "config" SETTINGS{…} }` to the module's own
**creator endpoint** `/net/<module>/conn` — one self-contained module per
*(transport, role)*, so both the transport and the role are positional in the path and
the SPEC carries no `type` and no `role`
([RFC-0014 — creator endpoint: connection lifecycle and link liveness](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)).
The `config` member `kind` selects which transport factory builds the link
(`core/src/transport_vertex.cpp:55`, factories registered through
`register_transport_type` at `:257`, catalogued at `:281`) and cross-checks the module's declaration. The
created connection is mounted and routed at **`/net/<module>/<name>`**, where `module`
is **declared by the application** through `register_module` — modules are declared-only
(ADR-0073 §4); an undeclared `(kind, role)` pair fails creation with `SCHEMA_NOT_FOUND`
(`core/src/transport_vertex.cpp:284`, refused at `:341`). One *(kind, role)*
pair is declared once: a second module claiming a pair another module already declared
is refused `PATH_IN_USE` rather than silently renaming the first.

The global `:children[]` spelling — `SPEC{type = "client"|"listener", …}` written to
`/net:children[]` — was **retired** at RFC-0014 S7: those two child types are no longer
registered, so that write answers `SCHEMA_NOT_FOUND`. A vendor builds against the
endpoint, not beside it. `:children[]` *creation* remains available for a device's own
registered types, and `:children[]` as an enumeration is untouched on every plane.

Removal has no wire spelling on the `:children[]` surface: a `[N]` clear of `:children[]` is
not implemented, and `graph_t::retire` is an owner-side call with no wire operation
behind it (`core/include/libtracer/graph.hpp:707-711`). A connection is the exception:
`NAME{<name>}` to its module's `conn` endpoint retires it, the other half of that one
control. Retirement empties the
vertex in place rather than freeing it — the handle stays dereferenceable and a
holder that caches a resolution re-checks `retire_generation` before use — and it
re-virginizes the address, clearing the previous owner's `:acl`, value seam, stored
value, history, app fields and subscribers
([RFC-0009 — vertex removal and subscriber eviction](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md)).
A device that exposes in-band creation without a retirement policy grows its tree
monotonically.

Two capabilities deserve emphasis because vendors habitually assume they are
mandatory:

- **A device with zero transports is conforming** (profile P0 — in-process only).
- **A device that describes nothing is conforming.** It reads, writes, awaits and
  forwards like any other node; it has simply pushed 100 % of the legibility burden
  onto out-of-band documentation. See the
  [budget table](../interoperability.md#enforcement-is-optional--drop-it-when-ramflash-is-expensive)
  — under RAM/flash pressure this is the designed degradation path, not a violation.

---

## What a custom device MUST implement

The irreducible floor — the required modules of conformance profile P0:

1. **The wire format, byte-exact** — TLV header (4-byte default / 6-byte `LL=1`),
   trailer-positioned TS + CRC, fixed-width little-endian lengths
   ([spec v1](../spec/v1.md)). No dialects.
2. **Path syntax and `:` field addressing** — including treating unsupported
   `:fields` as `SCHEMA_NOT_FOUND` (the `ENOTTY` default), never as a crash.
3. **read / write / await semantics** — delivery terminates at the target
   ([RFC-0007 — SUBSCRIBER delivery terminates at the target](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0007-delivery-terminates-at-target.md));
   a write to your vertex is applied locally and never auto-relayed onward.
4. **Safe handling of unknown type codes** — decode structurally, skip safely, do
   not crash. Core codes `0x01`–`0x1F` are assigned or reserved by this document
   set (`0x01`–`0x04`, `0x06`–`0x0C` and `0x0E` are assigned; `0x05` and `0x0D` are
   reserved codepoints with no mechanism), `0x0F`–`0x1F` is the v1 fast-track range
   and `0x20`–`0x7F` the long-term core registry — so a new core code may appear
   anywhere in `0x0F`–`0x7F` within v1
   ([reference 05 — protocol TLVs](../reference/05-protocol-tlvs.md)).
5. **FWD hop logic — only if ≥ 2 transports are loaded.** A single-transport leaf
   never forwards.

## What a custom device MUST NOT do

These are the interop-breakers — each one either poisons the shared byte-plane or
silently forks the protocol:

| Anti-feature | Why it breaks interop |
| ---- | ---- |
| **Semantic wire types outside the user range** | a generic forwarder must route your frames without knowing you; core codes `0x01–0x7F` are registry-owned. User records use `0x80–0xFF` with `opt.PL=1` |
| **Meaning encoded in framing** | payload semantics belong in data (VALUE bytes, app fields) — never in `opt` bits, lengths, or private trailer contents |
| **Emitting protocol errors from application logic** | the error boundary is closed ([ADR-0010 — the protocol error namespace is closed](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0010-closed-protocol-error-boundary.md)): app failures are ordinary *data*, self-described by your schema. `tr::*` identities are for the stack only |
| **A per-frame version bit** | v1 carries none, ever; incompatibility is versioned at discovery (`_libtracer-v2._tcp`), not per frame |
| **Wire-level fragmentation / reassembly metadata** | the wire format has none by design; large payloads address-shift across `ep[0..N]` |
| **Interpreting payload values in dispatch** | delivery policy may compare *bytes* (on-change), never interpret them; numeric filtering is an application filter-vertex |
| **Inventing new wire verbs** | there is no `connect`/`subscribe`/`create` primitive to extend — every control action is a field-write or a creator-endpoint write. SDK sugar is fine; wire verbs are not |
| **Relaying deliveries past the target** | chained auto-relay is forbidden (RFC-0007); re-emission is your application logic, explicitly |

The pattern behind all eight: **your device's meaning may be arbitrarily rich, but
it must ride *inside* the protocol's opaque payloads, never *as* protocol.** That
is the same byte-agnostic seam that keeps the core lean, and it is precisely what
lets two vendors who never met interoperate through a third party's reader.

---

## Checklist for the vendor's agent

A condensed contract a coding agent can execute against:

- [ ] Model the device as a vertex tree; one identity per independently-observable
      datum (promote to a `/` vertex what needs its own subscribers; keep bare
      attributes as `:` fields).
- [ ] Name vertices the way you would want them read (`temperature`, not `t7`).
- [ ] Install RFC-0010 descriptor tables with unit + purpose + tag on every vertex
      a stranger might integrate — static tables, `.rodata`, zero heap.
- [ ] Route every remote actuation through the owner apply seam — `on_write` for
      the value, `on_app_field_write` for a declared field — and validate there
      (the runtime deliberately does not).
- [ ] Pick transports by role; load only what the deployment uses.
- [ ] If the tree is dynamic, pair every creation path with a retirement policy.
- [ ] Keep proprietary logic behind the vertices; publish nothing but bytes,
      names, and legible descriptions.
- [ ] Never touch the MUST-NOT table above.

For a complete, resource-budgeted embedded application of this checklist, continue
to the [production ESP32 node example](esp32-production-node.md).
