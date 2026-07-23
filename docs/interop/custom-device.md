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

## The running example: a "vendor widget"

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
  (load-bearing claim 2); the device's apply seam turns the stored bytes into a PWM
  duty, a relay state, whatever the hardware does.
- **Wire it to another device** — an orchestrator subscribes this widget's
  `temperature` to another vendor's controller input, then departs. Neither vendor
  ever heard of the other.

The vendor's proprietary logic — filtering, safety interlocks, calibration — lives
entirely *behind* the vertices, in the owner's handlers. Nothing of it leaks into
the protocol surface, which is why it never has to be disclosed or standardized.

### The legibility part (what makes it integrable by a stranger)

Names alone make the tree navigable; descriptors make it unambiguous. The vendor
installs an owner field-descriptor table
([RFC-0010](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0010-owner-app-fields-and-schema.md))
whose descriptor bytes the runtime serves **verbatim** from `:schema` — the ideal
carrier for a semantic tag plus a natural-language brief:

```cpp
/** @brief Legibility table for /actuator/level — served verbatim via :schema. */
static constexpr auto kLevelFields = std::to_array<tr::graph::app_field_static_t>({
    // name        access  descriptor (opaque to the runtime, legible to the reader)
    {"unit",       ro,     "ratio 0..1, f32 LE"},
    {"purpose",    ro,     "output level of the main actuator; 0=off, 1=full"},
    {"tag",        ro,     "com.example.widget.actuator.level.f32"},
});
graph.set_app_fields_static(level_vertex, kLevelFields);
```

`set_app_fields_static` stores **views** over the caller's static data — the whole
legibility layer can live in `.rodata` and cost zero heap. A reader (human or
agent) that reads `:schema` now knows the type, range, direction and purpose
without any out-of-band contract.

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
| **Creator endpoint + type catalog** (ADR-0059) | orchestrators instantiate your controllers/connections in-band | fixed function; wiring baked at build |
| **Write-creates** | peers materialize vertices `mkdir -p`-style under CREATE ACL | static tree only |
| **Multiple transports + FWD** | the device becomes a forwarder — one address space across CAN + IP | leaf node on one link |
| **Header-elided framing** (e.g. CAN) | zero protocol overhead on constrained buses; the TLV header never hits the wire | full-TLV frames everywhere |
| **Address-shift slicing** | payloads beyond one frame, grouped by `(origin, ts)` | payloads bounded by transport frame |
| **Lazy production** | produce only while `:subscribers[]` is non-empty (the RTSP pattern) | always-on producers |
| **Discovery module** (mDNS static/dynamic) | peers find you; versioning rides the service name | peers are configured with your address |

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
   (RFC-0007); a write to your vertex is applied locally and never auto-relayed
   onward.
4. **Safe handling of unknown type codes** — ignore gracefully; new core codes may
   appear in `0x0E–0x7F` within v1.
5. **FWD hop logic — only if ≥ 2 transports are loaded.** A single-transport leaf
   never forwards.

## What a custom device MUST NOT do

These are the interop-breakers — each one either poisons the shared byte-plane or
silently forks the protocol:

| Anti-feature | Why it breaks interop |
| ---- | ---- |
| **Semantic wire types outside the user range** | a generic forwarder must route your frames without knowing you; core codes `0x01–0x7F` are registry-owned. User records use `0x80–0xFF` with `opt.PL=1` |
| **Meaning encoded in framing** | payload semantics belong in data (VALUE bytes, app fields) — never in `opt` bits, lengths, or private trailer contents |
| **Emitting protocol errors from application logic** | the error boundary is closed (ADR-0010): app failures are ordinary *data*, self-described by your schema. `tr::*` identities are for the stack only |
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
- [ ] Route every remote actuation through the owner apply seam; validate there
      (the runtime deliberately does not).
- [ ] Pick transports by role; load only what the deployment uses.
- [ ] Keep proprietary logic behind the vertices; publish nothing but bytes,
      names, and legible descriptions.
- [ ] Never touch the MUST-NOT table above.

For a complete, resource-budgeted embedded application of this checklist, continue
to the [production ESP32 node example](esp32-production-node.md).
