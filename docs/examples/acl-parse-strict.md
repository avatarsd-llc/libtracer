# An ACL is a security document, so the parser refuses (L4 auth / ACL)

Everywhere else in this codebase a decoder is forgiving in one specific way: an unknown key is
*skipped*, because a newer peer legitimately sends more than the receiver understands
(`config_reader_t`). `parse_acl` takes the **opposite** ruling on the same shape, and the reason
is that leniency here does not lose a field — it inverts or widens a grant
([#906](https://github.com/avatarsd-llc/libtracer/issues/906),
[reference/05 §`0x0A`](../reference/05-protocol-tlvs.md)):

- a dropped `expires_ns` turns a time-limited grant permanent;
- a dropped `flags` turns a vertex-local ACE into an inherited one;
- a `type` sent big-endian as u16 `0x0001` truncates from DENY to ALLOW;
- an unknown key is a restriction a newer writer meant to apply and this reader would ignore.

So the rule is: any shape `encode_acl` would never emit is `TYPE_MISMATCH` at **write** time,
where an operator finds out, rather than a quietly weaker policy at check time, where nobody
does.

## What to notice

- **The example builds its ACLs by hand, deliberately.** The point is precisely the shapes the
  typed builder cannot produce; a test that could only construct valid ACLs would be testing
  nothing. Each rejection is one edit away from the canonical ACE at the top, which is checked
  first so no refusal below is incidental.
- **Narrower than the field is the one safe leniency.** Little-endian zero-extension is exact, so
  a two-byte `access_mask` names the same integer as the canonical u32 — a pre-RFC-0026 spelling
  stays readable. Wider is truncation, and truncation is how DENY became ALLOW.
- **An empty numeric payload is a refusal, not a zero.** `0` is `ALLOW` for `type` and "never
  expires" for `expires_ns`: an absent value must not read as the permissive one.
- **A flag bit beyond `kAceInherit` is refused, not weakened.** `INHERIT_ONLY` / `NO_PROPAGATE`
  would be silently mis-evaluated by the merge, so richer NFSv4 flags gate on the merge honouring
  them first.
- **The walk is pair-consuming.** It steps one whole `(NAME key, value)` pair at a time, so a
  value can never be resynchronized onto as the next key — which a `subject` sent as a `NAME` (an
  accepted spelling, for `EVERYONE@`) previously could be
  ([#927](https://github.com/avatarsd-llc/libtracer/issues/927)). An odd child count means an
  unpaired trailing key, and is refused.
- **A read of `:acl` re-encodes the parsed ACEs.** It does not echo the written bytes, so what
  comes back can never describe a different policy from the one being enforced.
- **Nothing here is conditional** — the target builds and runs under every CI leg, and every case
  it parses is `ALLOW`-typed, so the verdicts do not depend on which policy the target binds
  ([the two profiles](acl-policy-profiles.md) covers the case that does).

## Source

```{literalinclude} /core/examples/acl_parse_strict.cpp
:language: cpp
:linenos:
```

See also: [security-acl module](../modules/security-acl.md) ·
[protocol TLVs](../reference/05-protocol-tlvs.md) ·
[what `decode` refuses](wire-decode-refusals.md) ·
[the two policy profiles](acl-policy-profiles.md) ·
[expiry](acl-expiry.md).
