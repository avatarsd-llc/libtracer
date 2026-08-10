# `field_write` is the single SUBSCRIBER admission door: sugar, wire, and firmware subscriptions take one code path with uniform gate and latch

Status: accepted. Resolves [#59](https://github.com/avatarsd-llc/libtracer/issues/59); makes load-bearing claim 2 ("subscribing IS writing a SUBSCRIBER into `:subscribers[]`") true in code, completing [ADR-0026](0026-consumer-initiated-subscription-client-write.md)'s single-primitive claim at the implementation level; maintainer-ratified 2026-07-04.

## Context

The graph stores subscriptions in one place (`vertex_t::subs_`, read back through the `:subscribers[]` control plane), but **four append paths** reach it and only one is the control plane: the two local `subscribe()` sugars push directly; the wire remote-subscribe bypasses `field_write` too (`op_resolver_t` detects the `:subscribers[]` append and calls `add_remote_subscriber` directly); only local string field-writes take the `field_write` branch. The costs are not cosmetic: the SUBSCRIBER `delivery_compact` parse is duplicated across `graph.cpp` and `op_resolve.cpp` with an admitted "so the two never drift" comment; the transient-local durability latch fires **only** on the remote path; and whether the SUBSCRIBE ACL gate applies depends on which door an edge entered through. `subscriber_t` is constructed at four sites with slightly different field sets.

## Decision

`field_write`'s `:subscribers[]` branch becomes **the** admission door, implemented as one internal admit step (parse once → SUBSCRIBE ACL gate → arity/fan-in check → durability latch → slot append):

- The local `subscribe(src, target)` / `subscribe(src, callback)` sugars **encode a SUBSCRIBER TLV and call the door** — subscribe-time is control-plane-cold; the encode/parse round-trip is irrelevant.
- `op_resolver_t` routes the wire append **through the same door**, passing a small write-context `{subject, delivery-binding}` (return-route + link + origin) instead of calling a parallel API; `add_remote_subscriber` leaves the public surface.
- Unsubscribe (`:subscribers[N]` clear) is the same door's removal branch, as today.

**Deliberate behavior alignment** (this is the point, not a side effect): the SUBSCRIBE ACL gate and the transient-local durability latch now apply **uniformly** across all doors. Local callers resolve to an `OWNER@`-equivalent subject by default, so in-process use without a subject resolver is unaffected; a local subscriber that requests durability now receives the latched LKV exactly as a remote one does — previously a remote-only behavior with no principled reason for the asymmetry. Firmware-baked, NVS-restored, orchestrator-issued, and SDK-sugar subscriptions are now the *same client write* in code, exactly as ADR-0026 states they are on the wire.

> **Erratum (2026-08-01), [RFC-0022](../spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md):** this ADR's "durability=1 edge" is a per-*vertex* predicate (`settings.durability`). RFC-0022 §3.A moved it to the SUBSCRIBER: the latch fires when **that subscription** set `durability_request` (bit 5 of its `delivery_policy`), not when the producer carries a flag. Nothing about the ONE-DOOR decision changes — the latch still fires at the single admission step, for every door, which is what made the move a one-line predicate change. What changes is which subscribers are latched: a producer's flag used to replay to every subscriber of that vertex, including the ones that never asked, and there was no way to decline.

## Considered options

- **Extract only a shared `admit_subscriber` function, keep four doors.** Rejected: kills the parse duplication but leaves the doors — claim 2 stays true only at the storage level, and every future admission concern (QoS validation, quota) must remember all four entry points.
- **Status quo + shared parse helper.** Rejected: the latch and gate asymmetries remain unprincipled.
- **Defer behind #59.** Rejected by maintainer: this pass is the moment the surrounding code (ACL policy, key_view) is already moving.

## Consequences

- One place implements admission; the `delivery_compact` parse exists once; `subscriber_t` has one construction site.
- Behavior change, called out for release notes: local subscribers gain the durability latch; local sugar subscriptions become ACL-gated (no-op without a resolver installed).
- `graph.hpp` loses `add_remote_subscriber` from its public surface; the write-context type is the same one the ACL subject resolution already threads.
- Tests asserting door-specific behavior (the remote-only latch) are updated to assert the uniform semantics; a new test pins "all doors produce byte-identical `:subscribers[]` read-back."

## Erratum (2026-08-10): "`OWNER@`-equivalent subject" names a subject that does not exist

The Decision section says local callers *"resolve to an `OWNER@`-equivalent subject by default,
so in-process use without a subject resolver is unaffected."*

**The behaviour is right; the name is not.** There is no `OWNER@` principal and never was — no
evaluator special-cases the string, and ADR-0020's erratum
([#1033](https://github.com/avatarsd-llc/libtracer/issues/1033)) withdraws it outright. The
actual mechanism is simpler and has no subject in it: with **no subject resolver installed**,
`graph_t::acl_allows` does not gate at all, so a local caller is trusted by construction rather
than by resolving to a privileged token. Read the sentence as *"local calls are ungated until a
resolver is installed."* Nothing about the decision changes.
