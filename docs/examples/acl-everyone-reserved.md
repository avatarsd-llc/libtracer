# `EVERYONE@` is reserved in both directions (L4 auth / ACL)

The subject-token space is opaque bytes with exactly one spelling the evaluators know:
`kEveryoneSubject` — `"EVERYONE@"`. An ACE carrying it applies to every resolved subject, which
is how a vertex is opened to all comers for one right without enumerating anybody
([ADR-0020](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md)).

The half that is easy to miss is the other direction, and it is the reason this gets its own
example.

## What to notice

- **The wire has ONE spelling for a subject token.** The ACE's subject and the resolver's output
  are the same opaque bytes — the `acl/acl-aces` conformance vector sends `peer-a` and the
  wildcard as the same opaque VALUE. So a *principal* that could BE those bytes would be
  indistinguishable from the wildcard ACE.
- **The core refuses the resolver's OUTPUT, rather than trusting every integrator to blacklist
  the string.** A caller that resolves to `EVERYONE@` is denied at every gate
  ([#908](https://github.com/avatarsd-llc/libtracer/issues/908)). The deployments at risk are
  exactly the ones whose resolver passes a caller-supplied identity through — usernames,
  certificate CNs, peer names — which is the most ordinary resolver there is.
- **It is refused on a BARE vertex too, and that is the discriminating case.** The refusal
  happens *before* the open-by-default rule, so it is not "the wildcard subject matches no ACE";
  it is "this is not a principal at all". The example checks the bare vertex with an ordinary
  caller as the ablation, so the deny is not just a vertex that happened to be closed.
- **`is_reserved_subject()` is public for a reason.** A resolver can refuse the token at its own
  door as well; the core checks it in the two places a subject reaches an ACE comparison.
- **The wildcard grant is still one bit.** `EVERYONE@` with `READ` is not `EVERYONE@` with
  access — see [`access_mask` is a bitfield](acl-right-bits.md).
- **`OWNER@` is not a special subject and never was.** ADR-0020 named it once, no evaluator ever
  special-cased it, so an `OWNER@` ACE matched nobody while still *closing* the vertex it was
  written to delegate. The erratum
  ([#1033](https://github.com/avatarsd-llc/libtracer/issues/1033)) withdraws the name rather than
  reserving it: with no document telling an operator to write that ACE, there is nothing for an
  impersonated `OWNER@` principal to match either. The example asserts it is an ordinary token.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/acl_everyone_reserved.cpp
:language: cpp
:linenos:
```

See also: [security-acl module](../modules/security-acl.md) ·
[protocol TLVs](../reference/05-protocol-tlvs.md) ·
[the subject resolver](acl-subject-resolver.md) ·
[open by default](acl-open-by-default.md).
