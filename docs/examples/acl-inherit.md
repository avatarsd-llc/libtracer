# Effective ACL = own ACEs + INHERIT-flagged ancestor ACEs (L4 auth / ACL)

`:acl` is not written per leaf, the way `:subscribers[]` is not. An ACE on a composite carries
the `kAceInherit` flag and applies to that composite's whole subtree, so an owner grants an
orchestrator admin over `/dev` once instead of over every endpoint below it — NFSv4 inheritance
riding the address composition
([ADR-0020](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md)).

## What to notice

- **Inheritance is per ACE, not per list.** One `:acl` holds both kinds side by side, and the
  example's composite does: a flagged `READ` that covers the subtree, and an unflagged `WRITE`
  that applies to `/dev` and nowhere else.
- **It covers the subtree, not the child.** The flagged ACE reaches `/dev/temp/raw` as readily as
  `/dev/temp`. The walk is over strict ancestors, nearest first.
- **An inherited grant is still one bit.** An inherited `READ` is not a `WRITE`; nothing widens
  on the way down.
- **Own ACEs come first, and combine rather than replace.** Writing an `:acl` on the child does
  not shadow what it inherits — the example's `app` gains `WRITE` while `fleet` keeps its
  inherited `READ`. Ordering (own before ancestors) only *matters* under the full policy, but the
  merge is built that way regardless
  ([ADR-0050](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0050-acl-pure-policy-cached-effective-ace-merge.md)).
- **The result worth internalising: a closed parent can sit over an open child.** A descendant
  whose only candidate ACE is an ancestor's *unflagged* one has an **empty** effective ACL, and
  an empty effective ACL is open. The example writes an unflagged ACE on `/site`, watches a
  stranger be refused there — and watches the same stranger write `/site/leaf`. If you meant to
  protect the subtree, the flag is not optional.
- **Only the merge is cached, never a verdict.** An `:acl` write marks the subtree dirty and the
  next check rebuilds, so a revoked grant takes effect on the very next operation — and expiry,
  which is evaluated against the caller's clock, needs no invalidation at all
  ([expiry](acl-expiry.md)).
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/acl_inherit.cpp
:language: cpp
:linenos:
```

See also: [security-acl module](../modules/security-acl.md) ·
[graph model](../reference/02-graph-model.md) ·
[network formation](../reference/13-network-formation.md) ·
[`access_mask` is a bitfield](acl-right-bits.md) ·
[the two policy profiles](acl-policy-profiles.md).
