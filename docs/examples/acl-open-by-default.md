# Open by default, and the first ACE is the lock (L4 auth / ACL)

A fresh `graph_t` has no subject resolver, and a fresh vertex has no `:acl`. Both are open
states, and they are **independent**: installing a resolver does not close a bare vertex, and
writing an `:acl` does not close a graph that cannot name its callers. Only the two together
enforce anything
([ADR-0018](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0018-access-control-authorization-pluggable-subject-token.md),
[ADR-0020](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md)).

## What to notice

- **Enforcement is opt-in twice over, and each opt-in is separate.** With no resolver the ACL is
  still parsed, still validated and still stored — it is simply never evaluated, because there is
  nothing to evaluate it *against*. With a resolver but no ACE, every vertex answers as it always
  did. The example shows each half failing to enforce on its own.
- **There is no "deny" to write. The first grant is the lock.** An *empty* effective ACL is open;
  any present ACE closes the vertex to every subject that ACE does not name. That is the rule
  that surprises people: `peer-a` is refused although nothing anywhere mentions `peer-a`.
- **A grant is one bit, not a door.** The subject the ACE *does* name is refused for every right
  the mask does not carry — the example's `peer-z` may read and may not write. See
  [`access_mask` is a bitfield](acl-right-bits.md).
- **The empty caller context is the trusted local channel.** The in-process host API passes no
  caller, and that context is settled as trusted *before* the resolver is consulted at all
  ([#905](https://github.com/avatarsd-llc/libtracer/issues/905)) — which is how the `:acl` in the
  example gets written in the first place. A remote operation always carries its inbound link
  NAME, so it cannot spell the trusted context.
- **This is why an ACL is not a deployment checklist item you can defer.** A node that ships
  without a resolver is not "using default permissions"; it is not enforcing at all.
- **Nothing here is conditional** — the target builds and runs under every CI leg, with no net
  plane, no sockets and no threads.

## Source

```{literalinclude} /core/examples/acl_open_by_default.cpp
:language: cpp
:linenos:
```

See also: [security-acl module](../modules/security-acl.md) ·
[graph module](../modules/graph.md) ·
[network formation](../reference/13-network-formation.md) ·
[the subject resolver](acl-subject-resolver.md) ·
[`access_mask` is a bitfield](acl-right-bits.md).
