# The `:` field plane is the vertex's `ioctl`: an optional, protocol-defined-plus-device-private control surface on one identity

Load-bearing claim 2 says the control surface — subscriptions, QoS, ACLs, liveness — is expressed as writable `:`-addressed fields, not as separate primitives. Designing the byte layout (and weighing a "make creation/ACL device-private endpoints instead" alternative) forced three questions the claim did not answer: must *every* vertex carry this machinery? does splitting control into sub-addresses **dissolve** the vertex's consistency? and how does a *generic* orchestrator act across heterogeneous devices? This ADR resolves all three with one framing.

## Decision

**A vertex is a Linux `fd`, and `:` is its `ioctl`.** The three operation classes map exactly, all on **one identity**:

| Linux on one `fd` | libtracer on one vertex | plane |
| --- | --- | --- |
| `read` / `write` | `read(/v)` / `write(/v)` | **data** (the value) |
| `ioctl(fd, cmd, arg)` | `write(/v:field, val)` (`:settings`, `:acl`, `:children`) | **control** |
| `poll` / `epoll` | `await(/v)` / `write(/v:subscribers[])` | **readiness / notify** |

Three rules follow:

1. **`:` keeps control as facets of one vertex — it does not dissolve it.** `/v:acl` is a field *of* `/v` (same identity, atomic with it); it is **not** a `/v/acl` child vertex. The `:` vs `/` separator is exactly the anti-dissolution mechanism — `ioctl` does not spawn a new `fd`, and `:field` does not spawn a new vertex. This preserves per-vertex atomicity/consistency.

2. **Fields are optional.** A vertex implements only the fields its role needs; an unsupported `:field` returns `SCHEMA_NOT_FOUND` — the `ENOTTY` of an unsupported ioctl. A minimal endpoint (a 9-byte control input, a GB shared buffer) is a char-device with read/write only and **zero** fields; the machinery is pay-for-what-you-use.

3. **Fields are standard *and* device-private — like ioctls.** **Protocol-defined standard** fields (`:subscribers`, `:acl`, `:settings`, `:children`) have uniform meaning so a *generic* orchestrator works across any device that supports them; a device may *also* expose **device-private** `:fields` (its own control ops, device-bounded). The protocol owns the **addressing**; the device owns the **catalog** of what each field accepts. This subsumes the "device-bounded creation endpoint" alternative without leaving the protocol.

**Creation is one such optional standard field.** `:children[]` is unified creation + composition membership (write a `SPEC` `0x0E` naming a device-catalog type → instantiates a child; read → the subtree members). Per-subscriber QoS is `:subscribers[].qos_settings`. The byte layouts are in [reference 05](../reference/05-protocol-tlvs.md); they are *optional capabilities*, not mandatory machinery.

## Considered options

- **Mandatory control fields on every vertex.** Rejected: a 9-byte input or a GB buffer would carry ACL/subscriber machinery it never uses — bloating the minimal MCU case the protocol exists to serve (load-bearing claim 5).
- **Fully device-private control endpoints (no protocol-defined fields).** Rejected: a generic fleet orchestrator could not exist — it would need per-device adapters to learn each device's "add controller" / "subscribe" convention. The standard-fields layer is what makes ADR-0017 orchestration generic. (The device-private *extension* is kept — as private `:fields`.)
- **Control surfaces as sub-vertices (`/v/acl`, `/v/subscribers`).** Rejected: dissolves the vertex into many sub-vertices, losing one-identity atomicity/consistency — the precise failure the `:` separator was designed to prevent.

## Consequences

- A **9-byte control input**, a **GB shared buffer**, and a **richly-orchestrated controller** are the *same* vertex model lighting up different optional `:` fields — one model spans the whole range.
- **Generic cross-device orchestration** (standard fields) and **device-specific extensibility** (private fields) coexist on one vertex, like standard vs driver-private ioctls.
- Per-vertex **consistency is preserved** — control is `:` facets, never `/` sub-vertices.
- This **sharpens load-bearing claim 2**: the field-write control surface is *optional* and *two-tier* (standard + device-private); reference 05 specifies the standard fields' bytes (`0x0E SPEC`, `SUBSCRIBER.qos_settings`, the `ACL 0x0A` ACE of [ADR-0020](0020-acl-nfsv4-style-aces-with-inheritance.md)).
