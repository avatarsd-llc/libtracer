# `field_write` is the single SUBSCRIBER admission door: sugar, wire, and firmware subscriptions take one code path with uniform gate and latch

Status: accepted. Resolves [#59](https://github.com/avatarsd-llc/libtracer/issues/59); makes load-bearing claim 2 ("subscribing IS writing a SUBSCRIBER into `:subscribers[]`") true in code, completing [ADR-0026](0026-consumer-initiated-subscription-client-write.md)'s single-primitive claim at the implementation level; maintainer-ratified 2026-07-04.

## Context

The graph stores subscriptions in one place (`vertex_t::subs_`, read back through the `:subscribers[]` control plane), but **four append paths** reach it and only one is the control plane: the two local `subscribe()` sugars push directly; the wire remote-subscribe bypasses `field_write` too (`op_resolver_t` detects the `:subscribers[]` append and calls `add_remote_subscriber` directly); only local string field-writes take the `field_write` branch. The costs are not cosmetic: the SUBSCRIBER `delivery_compact` parse is duplicated across `graph.cpp` and `op_resolve.cpp` with an admitted "so the two never drift" comment; the transient-local durability latch fires **only** on the remote path; and whether the SUBSCRIBE ACL gate applies depends on which door an edge entered through. `subscriber_t` is constructed at four sites with slightly different field sets.

## Decision

`field_write`'s `:subscribers[]` branch becomes **the** admission door, implemented as one internal admit step (parse once → SUBSCRIBE ACL gate → arity/fan-in check → durability latch → slot append):

- The local `subscribe(src, target)` / `subscribe(src, callback)` sugars **encode a SUBSCRIBER TLV and call the door** — subscribe-time is control-plane-cold; the encode/parse round-trip is irrelevant.
- `op_resolver_t` routes the wire append **through the same door**, passing a small write-context `{subject, delivery-binding}` (return-route + link + origin) instead of calling a parallel API; `add_remote_subscriber` leaves the public surface.
- Unsubscribe (`:subscribers[N]` clear) is the same door's removal branch, as today.

**Deliberate behavior alignment** (this is the point, not a side effect): the SUBSCRIBE ACL gate and the transient-local durability latch now apply **uniformly** across all doors. Local callers resolve to an `OWNER@`-equivalent subject by default, so in-process use without a subject resolver is unaffected; a local subscriber on a durability=1 edge now receives the latched LKV exactly as a remote one does — previously a remote-only behavior with no principled reason for the asymmetry. Firmware-baked, NVS-restored, orchestrator-issued, and SDK-sugar subscriptions are now the *same client write* in code, exactly as ADR-0026 states they are on the wire.

## Considered options

- **Extract only a shared `admit_subscriber` function, keep four doors.** Rejected: kills the parse duplication but leaves the doors — claim 2 stays true only at the storage level, and every future admission concern (QoS validation, quota) must remember all four entry points.
- **Status quo + shared parse helper.** Rejected: the latch and gate asymmetries remain unprincipled.
- **Defer behind #59.** Rejected by maintainer: this pass is the moment the surrounding code (ACL policy, key_view) is already moving.

## Consequences

- One place implements admission; the `delivery_compact` parse exists once; `subscriber_t` has one construction site.
- Behavior change, called out for release notes: local subscribers gain the durability latch; local sugar subscriptions become ACL-gated (no-op without a resolver installed).
- `graph.hpp` loses `add_remote_subscriber` from its public surface; the write-context type is the same one the ACL subject resolution already threads.
- Tests asserting door-specific behavior (the remote-only latch) are updated to assert the uniform semantics; a new test pins "all doors produce byte-identical `:subscribers[]` read-back."
