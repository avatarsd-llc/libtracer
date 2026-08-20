# `access_mask` is a bitfield, and every gate tests one bit (L4 auth / ACL)

There is no access *level* and no ordering among the rights. `acl_right_t` values are single
bits, a stored ACE may carry any OR of them, and each door in the graph asks for its own bit and
no other
([ADR-0020](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md),
[reference/05 §`0x0A`](../reference/05-protocol-tlvs.md)).

The example gives six subjects one single-bit grant each, on one vertex, and then runs every
gate twice — once for the subject that holds its bit and once for a subject that holds a
*different* one. So every allow is the bit under test, and every deny is a caller who is
authorized for something else rather than for nothing.

## What to notice

- **`WRITE_ACL` is precisely the admin right.** Not a role, not a flag beside the mask — one bit
  in the same bitfield, and the one that lets a subject rewrite the policy and so delegate to
  others. That is what makes *"the owner grants an orchestrator admin over a subtree, and the
  orchestrator then leaves"* an ordinary ACE write rather than a privileged mode
  ([reference/13](../reference/13-network-formation.md)).
- **Reading the policy is `READ_ACL`, not `READ`.** A subject that may read a vertex's *value*
  learns nothing about who else may. The two bits are independent in both directions.
- **`SUBSCRIBE` is the producer's fan-out gate.** Appending to `:subscribers[]` asks the
  **source** vertex "who may subscribe to me?", and a `WRITE` grant does not buy it. The dual
  question — "who may write into me?" — is the *target's* `WRITE` bit, evaluated at delivery
  ([reference/13 §Delivery and the two ACLs](../reference/13-network-formation.md)).
- **`CREATE` is its own bit because creation is a write.** In-band vertex creation goes through
  the same door as everything else
  ([ADR-0017](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0017-in-band-vertex-creation-controller-orchestration.md),
  superseded by
  [ADR-0059](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md)),
  so it needs a bit of its own or every writer would be a creator.
- **`DELETE` and `WRITE_OWNER` are reserved and have no core surface yet.** They are in the
  enum because the wire layout is the full NFSv4 model; nothing in `core/` gates on them.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/acl_right_bits.cpp
:language: cpp
:linenos:
```

See also: [security-acl module](../modules/security-acl.md) ·
[protocol TLVs](../reference/05-protocol-tlvs.md) ·
[network formation](../reference/13-network-formation.md) ·
[open by default](acl-open-by-default.md) ·
[inheritance](acl-inherit.md).
