<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0021 — The frame of reference of a wire SUBSCRIBER's PATH target

| Field | Value |
| ---- | ---- |
| **RFC** | 0021 |
| **Title** | The frame of reference of a wire SUBSCRIBER's PATH target |
| **Status** | **proposed** — awaiting the maintainer's ruling on §3's fork. The comment window is waived for a sole maintainer (precedent: RFC-0020), but the *direction* here has not been ruled; this RFC exists to put the fork on the record, not to record a decision already taken. |
| **Author** | filed from the #491 refutation, 2026-08-01 |
| **Amends** | [RFC-0004](0004-remote-operation-addressing.md) §D (operation semantics), §E (delivery/fanout) |
| **Tracking** | [#491](https://github.com/avatarsd-llc/libtracer/issues/491) |
| **Related** | [ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md) (consumer-initiated subscription), [ADR-0061](../../adr/0061-per-transport-mount-routing-strip-k-l5-demux.md) (strip-K descent), [ADR-0073](../../adr/0073-naming-authority-the-application-mints-one-predicate-gates.md) §Consequences erratum, [#739](https://github.com/avatarsd-llc/libtracer/issues/739) (`subscribe_toward`, the host-local dual) |

## 1. Summary

A `SUBSCRIBER` TLV may carry a `PATH` child. The wire encoding for it exists and is pinned by a conformance vector; **its meaning is not defined by any normative text**, and the reference implementation discards it. This RFC asks the protocol to say what that `PATH` means — specifically, **whose frame of reference it is spelled in** — and proposes that when it routes through a transport mount it is the *delivery route in the producer's own frame*, binding the edge to that mount link rather than to the session the subscribe-write arrived on.

## 2. Motivation — a documented capability does not work

[CONTEXT.md](../../../CONTEXT.md) §Network formation describes an **orchestrator** that joins temporarily, wires data flows between *other* devices, and departs, "leaving the wired devices talking to each other — the patch-cable that outlives the hand." [ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md) makes the subscribe-write identical whether it comes from the consumer, from firmware, or from a third-party orchestrator.

A conformance test built to that model (issue #491, branch `test/491-orchestrate-and-depart`) **fails**, and the failure is structural rather than a bug:

1. The wire `:subscribers[]` door discards the `SUBSCRIBER`'s `PATH` child outright — `s.target_key.reset()` in `graph_t::subscribe_wire`, commented *"a PATH child names the consumer at ITS origin — never a local re-dispatch target."* The edge is instead bound to `(caller = the arrival session, return_route = the accumulated src)`.
2. Deliveries therefore ride back to **whoever wrote the subscription** — the orchestrator — not to the consumer it named.
3. When the orchestrator departs, `fwd_router_t::link_down` → `graph_t::evict_link_edges` removes that edge, so nothing survives at all.

The host-side dual that *does* resolve a mount-path target, `fwd_router_t::subscribe_toward` ([#739](https://github.com/avatarsd-llc/libtracer/issues/739)), is reachable only in-process — there is no wire door onto it.

**The precise error to avoid repeating** (recorded as an erratum on ADR-0073 §Consequences): ADR-0026 is correct that an orchestrator issues the *identical write*. What does not follow is that an identical write yields an identical **binding** — the binding is derived from the *arrival session*, which differs for a third party.

## 3. The fork this RFC exists to resolve

A `SUBSCRIBER`'s `PATH` child can be read two ways, and v1 never said which:

| reading | meaning | consequence |
| --- | --- | --- |
| **(a) consumer's own frame** — today's implementation comment | "this is who I am, at my node" — informational provenance | the field is inert on the wire; third-party origination is impossible; CONTEXT.md's orchestrator model is aspirational |
| **(b) producer's frame** — this RFC's proposal | "deliver to this address, resolved from *your* root" | third-party origination works; the edge outlives its writer |

Reading (b) is the same frame every other routed address on the wire already uses: a `FWD`'s `dst` is resolved from the receiver's root, and a subscription's delivery *is* a write (RFC-0004 §D). Under (a) the `PATH` is the only address on the wire spelled in the sender's frame — an inconsistency that is itself an argument for (b).

## 4. Proposed change

### A. The `PATH` child is the delivery target, in the producer's frame

A `SUBSCRIBER` carrying a `PATH` child MUST have that path resolved **from the receiving (producer) node's own root**, by the same resolution a `FWD` `dst` receives — the strip-K mount descent of ADR-0061.

### B. Resolution outcomes

1. **Routes through a transport mount** — the leading segments name a registered mount, with a residual below it. The edge binds to `(link = that mount's registry identity, return_route = the residual)`. This is exactly what `subscribe_toward` computes in-process today.
2. **Names a purely local vertex** — no mount is involved. The edge binds as a **local re-dispatch target**, identically to the in-process subscribe door.
3. **Resolves to nothing**, or names a mount exactly with no residual, or (per [RFC-0020](0020-bus-name-not-a-routable-next-hop.md)) names a bus link's own connection NAME — the subscribe-write is **rejected**; §F fixes the code.

### C. Lifetime — the property the whole change exists for

Because outcome B.1 stores the **mount** link, the edge is no longer keyed to the writer's session, so the writer's departure does not evict it: `evict_link_edges(<writer's session>)` no longer matches. The edge instead lives and dies with the *delivery* link, which is the correct lifetime — it is the link the data actually flows over.

### D. Absent `PATH` child — unchanged

A `SUBSCRIBER` with no `PATH` child keeps today's behaviour exactly: bind to the arrival session, deliver along the accumulated `src`. This is the consumer-subscribes-for-itself case, which is the overwhelmingly common one, and it is untouched. **No existing flow changes.**

### E. Access control — the open question this RFC most needs answered

The `SUBSCRIBE` gate on the producer's `:acl` continues to run under the *writer's* caller context (#81, ADR-0026): an orchestrator must hold `SUBSCRIBE` on the producer to wire anything at all.

But (b) grants a genuinely new power: **a third party holding `SUBSCRIBE` on a producer can direct that producer's data to any node the producer can reach.** Under (a) it could only direct data to itself. Candidate answers, none yet ruled:

1. `SUBSCRIBE` is sufficient — the right to subscribe already implies the right to route the data, and an orchestrator that can write a subscription can equally read the value and forward it.
2. A distinct right is required for a subscription whose target is not the writer (a "wire-elsewhere" right), so a merely-observing peer cannot redirect a stream.
3. The producer's node validates the target against its own policy (e.g. targets must be links the producer created).

Answer 1 is the smallest and is probably right — the forwarding argument is strong — but the asymmetry deserves to be stated deliberately rather than inherited.

### F. Errors

A `PATH` child that fails §B.3 answers `tr::path::invalid` (`0x0021`), the code RFC-0020 uses for an unroutable next-hop, carried in the ordinary `REPLY{kind=ERROR}`. It MUST NOT silently degrade to the arrival-session binding — a silent degradation is precisely the failure mode that made #491 look like it worked (the write answered `RESULT` while binding something else).

### G. Conformance vectors

1. `subscriber/target-mount-routed` — a `SUBSCRIBER{PATH …}` naming a path through a mount; the producer's subsequent write emits a `FWD{WRITE}` on the *mount* link with `dst` = the residual.
2. `subscriber/target-local` — a `PATH` naming a local vertex; delivery is a local re-dispatch.
3. `subscriber/target-unroutable` — rejected with `0x0021`, and **no edge appended**.
4. `subscriber/no-target` — the absent-`PATH` case still binds to the arrival session (the regression guard for §D).
5. An end-to-end vector for the departure property: after the writer's link is torn down, the producer still delivers to the target (the #491 recipe).

## 5. Compatibility

- **No new TLV, no new type code, no encoding change.** The `PATH` child already exists in the encoder and is pinned by the `tlv-types/subscriber-path` conformance vector — which is a **structural** vector ("structurally faithful", category `tlv-types`) and asserts nothing about the wire door's semantics, so it does not constrain this change.
- **It is a behaviour change** for any sender that sends a `PATH` today and relies on it being ignored. The reference implementation ignores it, so nothing in this tree relies on the old reading; the protocol is `DRAFT`.
- §D keeps every consumer-subscribes-for-itself flow byte-identical.

## 6. Alternatives considered

1. **Status quo, documented.** Say plainly that third-party origination is not supported, and correct CONTEXT.md and `reference/13` to match. Cheapest, and honest — but it deletes a capability the project's own model describes as central, and it leaves `subscribe_toward` as an in-process-only asymmetry.
2. **A new control verb** that exposes `subscribe_toward` over the wire. Strictly more surface (a new field or frame shape) for the same result the existing `PATH` child can express. Rejected on minimalism unless §3's fork is decided *against* (b), in which case this becomes the only route.
3. **Orchestrator proxies the deliveries.** Defeats the purpose — the orchestrator must be able to depart; this is the browser-relay the whole model exists to retire.

## 7. Open questions

1. §3's fork: reading (a) or (b)? Everything else follows.
2. §E: is `SUBSCRIBE` sufficient authority to direct a producer's data to a third node?
3. Should outcome B.2 (a local target through the wire door) be permitted at all, or is the wire door restricted to mount-routed targets? Permitting it makes the two doors uniform; restricting it keeps the wire door's blast radius smaller.
4. Does the delivery-compaction opt-in (RFC-0004 §E.1) interact with a mount-routed target? The route handle is per `(link, route)`, and both are now the mount's — expected to work unchanged, but it wants a vector.
