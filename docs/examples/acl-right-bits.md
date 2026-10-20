# `access_mask` is a bitfield, and every gate tests one bit (L4 auth / ACL)

There is no access *level* and no ordering among the rights. `acl_right_t` holds two single
bits, READ and WRITE, a stored ACE may carry either or both, and each door in the graph asks for
one of them
([RFC-0036](../spec/rfcs/0036-two-rights-fields-are-paths-under-their-vertex.md),
[reference/05 §`0x0A`](../reference/05-protocol-tlvs.md)).

The example gives subjects READ, WRITE or both on one vertex and runs every door for each, so
every allow is the bit under test and every deny is a caller authorized for the other bit rather
than for nothing.

## What to notice

- **A `:` field is a path under its vertex.** It has no ACL of its own: reading any field, `:acl`
  included, needs READ on the vertex, and writing one needs WRITE.
- **Subscribing is a read.** Appending to `:subscribers[]` needs READ on the **source** vertex,
  because a subscription delivers what a read would. The dual question, "who may write into
  me?", is the *target's* WRITE bit, evaluated at delivery
  ([reference/13 §Delivery and the two ACLs](../reference/13-network-formation.md)).
- **Creating is a write to the parent.** `:children[]` creation, a creation hook and the creator
  endpoint all ask for WRITE.
- **Writing `:acl` is not a right.** Only the owner, the empty local caller, may do it, so a
  subject holding both bits still cannot rewrite the policy. Remote administration goes through
  an owner-app vertex ([reference/13](../reference/13-network-formation.md)).
- **A `:subscribers[N]` slot is its holder's.** A subject with READ may clear or replace its own
  slot, never another's or the owner's.
- **A bit outside READ|WRITE is refused at install** with `TYPE_MISMATCH`, so a retired right is
  never stored and left inert.
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
