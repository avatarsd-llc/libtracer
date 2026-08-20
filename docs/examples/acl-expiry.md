# `expires_ns` is evaluated against the caller's clock (L4 auth / ACL)

An ACE may carry an absolute deadline: `expires_ns`, nanoseconds since the UNIX epoch, with `0`
meaning *never*. It is not a timer and nothing sweeps it — the policy compares it to the `now`
its caller passed in, on every single check
([ADR-0050](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0050-acl-pure-policy-cached-effective-ace-merge.md)).

The example runs against the **pure** surface first, because `effective_acl_t` takes `now` as a
parameter — so it can step the clock without sleeping — and then confirms the identical rule at
the real `graph_t` door, which reads its own clock.

## What to notice

- **One merged list, four verdicts, zero invalidations.** The example asks the *same*
  `effective_acl_t` about several `now`s, including going back to an earlier one. That is why the
  graph caches the effective-ACE **merge** but never a verdict: a merge stays valid as the clock
  moves, so expiry needs no invalidation mechanism at all.
- **Expiry is `expires_ns <= now`, not `<`.** At the deadline the grant is already gone. The
  example pins both sides of that single nanosecond.
- **An expired ACE grants nothing — and still closes the vertex.** Presence is what closes
  ([open by default](acl-open-by-default.md)), and an expired entry is present. So a temporary
  grant that lapses does **not** restore the open state it replaced; it leaves the vertex shut to
  everyone. This is the trap: "the badge expired, so we are back to normal" is exactly wrong.
- **`0` means never expires, not expired at the epoch.** The example checks that too, because the
  two readings of a zero field differ by the entire lifetime of the grant.
- **The parser refuses an *empty* `expires_ns` for the same reason.** An empty payload would load
  as `0` — the permissive reading — so an absent value must not be allowed to mean "never
  expires" ([strict ACL parsing](acl-parse-strict.md)).
- **Nothing here is conditional, and nothing sleeps.** No timers, no threads, no wall-clock
  rendezvous: the deadline is a number the example passes in. The graph-level half uses grants
  that expired one nanosecond after the epoch and one that expires in the year 2262, so it is
  deterministic on any machine, on any CI leg.

## Source

```{literalinclude} /core/examples/acl_expiry.cpp
:language: cpp
:linenos:
```

See also: [security-acl module](../modules/security-acl.md) ·
[protocol TLVs](../reference/05-protocol-tlvs.md) ·
[open by default](acl-open-by-default.md) ·
[inheritance](acl-inherit.md) ·
[strict ACL parsing](acl-parse-strict.md).
