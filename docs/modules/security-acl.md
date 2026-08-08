# security & ACL — access control on the graph

```{admonition} In one paragraph
:class: tip
Access control is a **pure policy over typed entries**. A vertex stores a list of
`tr::graph::ace_t` — subject, granted-rights mask, flags, expiry. The graph does
the walking: it merges a vertex's own entries with the `INHERIT`-flagged entries
of its ancestors into an `effective_acl_t`, then hands that list plus the
check-time clock to the policy the build selected. The policy touches no graph
state, takes no locks and reads no clock of its own, which is what makes ACE
edge cases — expiry, inheritance, ordering — unit-testable with no live graph.
```

## What it does

An ACE (access-control entry) grants a *subject* a set of *rights* on a vertex.
The subject is an opaque token, not a name the protocol interprets: how a
deployment maps a peer to a token is the deployment's business, and the special
token `EVERYONE@` matches any resolved subject. The rights are bits in
`acl_right_t`. An entry may carry an absolute expiry, after which it grants
nothing, and the `kAceInherit` flag, which is what makes it visible to
descendants.

`EVERYONE@` is **reserved**, and the core enforces the reservation rather than
describing it ([#908](https://github.com/avatarsd-llc/libtracer/issues/908)). The
wire has one spelling for a subject token — the `acl/acl-aces` vector sends
`peer-a` and `EVERYONE@` as the same opaque VALUE — so a deployment whose
resolver passes a caller-supplied identity through (a username, a certificate
CN, a peer name) could otherwise mint a principal that *is* the wildcard, and an
entry meant for that one principal would grant everyone. A subject token equal
to `kEveryoneSubject` therefore matches nothing in either policy, and
`graph_t::acl_allows` refuses such a caller outright — at every gate, on a
guarded vertex and on a bare one, the same fail-closed arm the resolver's own
error return takes. `is_reserved_subject` is public so a resolver can refuse it
at its own door too. `OWNER@`, the other special subject ADR-0020 names, is *not*
reserved here: no evaluator special-cases it today, so it is an ordinary opaque
token until one does.

Evaluation is split in two on purpose:

- **The graph owns the walk.** It collects the vertex's own entries in stored
  order, then appends each ancestor's `INHERIT`-flagged entries, producing one
  merged list in evaluation order. `effective_acl_t` is that list plus the
  verdict call over it.
- **The policy owns the decision.** Given a merged list, a subject, one right
  bit and the current time, it answers `ALLOW`, `DENY`, or `NO_MATCH`.
  `NO_MATCH` means no applicable entry decided the bit — the caller applies the
  open-by-default rule itself rather than the policy guessing.

Two policies ship, and the build picks one (see [config](config.md)):

| Policy | Profile | Semantics |
|---|---|---|
| `allow_only_policy_t` | the default, MCU-class | ALLOW entries only, with a single `INHERIT` flag; there is no ordering question to answer |
| `full_acl_policy_t` | host | ordered first-match-per-bit, DENY included |

Because the choice is per-target configuration and the check runs on the data
plane, it is made at compile time — a runtime branch on every access would be a
cost paid by the target that does not need the feature.

## The wire side

An ACL lives on the wire as the `:acl` ACL TLV described in
[reference §protocol TLVs](../reference/05-protocol-tlvs.md) §`0x0A`, and the
typed parse/build pair lives with the policy rather than in the codec:
`parse_acl` turns a decoded ACL TLV into `ace_t` values, `encode_acl` turns them
back into bytes. Keeping them here is why an ACE test needs no hand-rolled byte
builder.

Parse-time validation is policy-gated: under the ALLOW-only profile a `DENY`
entry or an unrecognized flag is rejected with `TYPE_MISMATCH` at write time, so
stored entries never carry a semantic the running policy cannot evaluate.

**Parsing an ACL is strict, and that is a security property, not fussiness**
([#906](https://github.com/avatarsd-llc/libtracer/issues/906)). A lenient read of
an access-control document does not lose a field — it changes what the document
grants. A `type` sent as a big-endian `u16` `0x0001` (DENY) has `0x00` in its low
byte, so a width-tolerant load turned a refusal into a grant; a dropped
`expires_ns` turned a time-limited grant permanent; an ignored unknown key
dropped whatever restriction a newer writer meant to add. So `parse_acl` rejects
a numeric field whose payload is empty or **wider** than the field, a known key
whose value TLV is the wrong type, an unknown key, a repeated key, and a body
whose `(NAME key, value)` pairing does not hold. A payload *narrower* than the
field is fine: little-endian zero-extension names the same integer, which is why
the `acl/acl-aces` conformance vector's two-byte `access_mask` keeps parsing.

The walk is pair-consuming, the same mechanics `net::config_reader_t` uses — and
the exact opposite ruling on unknown keys, deliberately. Config is where a newer
peer legitimately sends more than the receiver understands; an ACL is not.

## Pitfalls

- **`NO_MATCH` is not `DENY`.** A policy that collapses the two takes the
  open-by-default decision away from the caller and changes behaviour on every
  vertex with no applicable entry.
- **Expiry is absolute, in nanoseconds since the UNIX epoch.** The clock is
  passed *in* by the caller; a policy that read a clock itself could not be
  tested deterministically and would disagree with the rest of one check.
- **Inheritance is a flag, not a mode.** Only `kAceInherit`-flagged entries of an
  ancestor reach a descendant; an entry without it is local no matter where it
  sits in the tree.
- **The subject token is opaque.** Comparing it is a byte comparison; the
  protocol never parses it, so an implementation that reads structure into it has
  invented a private extension.
- **A resolver may not return `EVERYONE@`.** It is the one string carved out of
  the otherwise opaque token space, and a resolver that hands it back names no
  principal — the core refuses that caller instead of letting an identity
  impersonate the wildcard.

## API reference

```{doxygenstruct} tr::graph::ace_t
:project: libtracer
:members:
```

```{doxygenenum} tr::graph::ace_type_t
:project: libtracer
```

```{doxygenenum} tr::graph::acl_right_t
:project: libtracer
```

```{doxygenvariable} tr::graph::kAceInherit
:project: libtracer
```

```{doxygenvariable} tr::graph::kEveryoneSubject
:project: libtracer
```

```{doxygenfunction} tr::graph::is_reserved_subject
:project: libtracer
```

```{doxygenenum} tr::graph::acl_verdict_t
:project: libtracer
```

```{doxygenclass} tr::graph::effective_acl_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::graph::parse_acl
:project: libtracer
```

```{doxygenfunction} tr::graph::encode_acl
:project: libtracer
```

The two selectable policies are documented with the rest of the build
configuration, on [config](config.md).

See: [graph](graph.md) (which owns the effective-ACE walk),
[config](config.md) (which selects the policy),
[reference §protocol TLVs](../reference/05-protocol-tlvs.md) (the wire layout),
[reference §host embedding](../reference/07-host-embedding.md).
