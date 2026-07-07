# Vertex creation is an in-band, ACL-gated field-write; controllers are created from a device-known catalog and wired by a separate binding step

Status: accepted

Reference 11 §"what this document does NOT specify" states that *"the C-API for declaring a vertex's role is implementation-specific"* and that *"roles are intentionally invisible to peers."* Registration was an out-of-band, local act. Substituting the strawberry-fw `io_layer` (and the controller/logic-executor it carries) with a libtracer graph breaks that assumption: a remote **orchestrator** must be able to **create a controller on a device** and wire it to other devices' endpoints. This ADR records that creation becomes a first-class, in-band graph operation, and how it stays inside the read/write-only API and the facade — **superseding the "registration is out-of-band / roles invisible" stance of reference 11**.

## Decision

**Creating a vertex is an in-band field-write** — an orchestrator writes a **controller-spec** `{type, path, config}` into a device's **creation field** (`:children[]` / `:controllers[]`). This adds **no new wire primitive**: it is an ordinary field-write, so the read/write/await API and load-bearing claim 2 are intact.

**The device instantiates one of its *own* known controller types.** The orchestrator does not inject a role or code — it *selects a type* from the device's **controller-type catalog**, which is the creation field's **schema** (a POINT enumerating accepted `type`s and each one's config schema). So roles remain invisible (reference 11's facade survives); what becomes visible is the device's *catalog of types*, never the internal role. This bounds the operation — a device can only be asked to make things it already knows how to make.

**Creation and binding are separate steps.** Creation merely instantiates the controller, which **exposes its own port vertices** (input-port and output-port endpoints). A *distinct* **binding** step then wires those ports to other vertices with **SUBSCRIBER** edges (producer-holds; ACL-gated by the source — [ADR-0018](0018-access-control-authorization-pluggable-subject-token.md)). A controller therefore **subscribes to nothing at creation**; what it consumes and produces is decided entirely by the separate binding step — a patch-cable / dataflow model that mirrors strawberry-fw's wiring diagram.

**Creation is ACL-gated** ([ADR-0018](0018-access-control-authorization-pluggable-subject-token.md)): writing the creation field requires `create` rights, held by the device's **owner peer** or an orchestrator it has delegated admin to.

## Considered options

- **Keep registration out-of-band (provisioning / config side-channel).** Rejected: a remote orchestrator could then never create a controller — the entire io_layer-substitution use case (a fleet orchestrator standing up controllers across devices) is impossible. The graph would be statically provisioned only.
- **A new `create` wire primitive.** Rejected: violates the read/write-only API (load-bearing claim 2). Creation as a field-write reuses the existing control surface.
- **Self-wiring controller-spec (bindings embedded in `config`).** Rejected (this was the initially-recommended form): it couples creation and binding into one atomic write, but the wiring is then *implicit* (hidden in config) rather than inspectable on the wire as ordinary SUBSCRIBER edges, and it conflates two genuinely separate concerns. The separate-binding model makes the patch graph first-class and re-wireable without re-creating the controller.
- **Let the orchestrator inject an arbitrary role/handler.** Rejected: that would make roles visible and injectable, breaking reference 11's facade and opening arbitrary-code execution. The device-known-catalog keeps it bounded.

## Consequences

- Orchestration is a **first-class graph citizen** expressed through the existing API; the io_layer-substitution's "a fleet orchestrator creates and wires controllers" maps directly onto field-writes.
- **Reference 11 is amended**: its §"does NOT specify" no longer claims registration is purely out-of-band; creation is an in-band, ACL-gated field-write, while *roles* (as opposed to the *type catalog*) remain invisible.
- A device's **controller-type catalog** is a new schema surface it must publish (the creation field's `:schema`).
- **Dynamic vertex lifecycle** now has a wire-driven path (create/destroy), which raises handle-invalidation (a stale path handle to a destroyed vertex must fail safely) — an implementation concern for the graph runtime, analogous to strawberry-fw's generation-counter handles.
- This is an **L4 graph-semantics** decision, not a new wire format: the controller-spec is a structured TLV built from existing type codes (or a forward-extension code in `0x0E–0x7F`), and "a write to `:children[]` instantiates a child" is L4 behavior ([reference 04](../reference/04-communication-flows.md), [11](../reference/11-vertex-roles-and-aggregation.md)), so no RFC against the immutable wire spec is required.
