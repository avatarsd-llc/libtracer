# Two evaluators, and DENY exists only where one can see it (L4 auth / ACL)

The wire layout is the full NFSv4-style model; the required-modules MCU profile enforces a
**subset** of it
([ADR-0020](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md)).
ACE evaluation is therefore a pure per-target policy
([ADR-0050](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0050-acl-pure-policy-cached-effective-ace-merge.md))
with two adapters, both always compiled:

| policy | rule | `kAcceptsDeny` |
| ------ | ---- | -------------- |
| `allow_only_policy_t` (default) | any applicable ACE grants; order is irrelevant | `false` |
| `full_acl_policy_t` | ordered first-match-per-bit; the first applicable ACE decides by its type | `true` |

## What to notice

- **The same two ACEs mean the opposite thing under the two evaluators.** Hand both a DENY
  followed by an ALLOW, same subject and same bit: the full policy answers DENY, the ALLOW-only
  policy answers ALLOW — because it never looks at the type at all.
- **Which is precisely why `parse_acl` refuses to store a DENY under the ALLOW-only profile.**
  Not a limitation to work around: a stored restriction the running evaluator cannot see would be
  read as a *grant*, and a security document must never be interpreted more broadly than it was
  written. `kAcceptsDeny` is the constant that decides it, and the parser is its only reader.
- **Stored order is semantic under the full policy, and meaningless under the other.** Reversing
  the two ACEs reverses the full policy's verdict. An ALLOW-only list has nothing to order, which
  is exactly what lets the MCU subset skip the whole concept.
- **`effective_acl_t` is where the policy is chosen, and it adds the open-by-default rule** on
  top: no effective ACE ⇒ allowed; any present ACE ⇒ `NO_MATCH` denies.
- **Both arms run in every build.** The policies are named explicitly as template arguments
  rather than left to the target's binding, so this example is not a run-time skip: the ALLOW-only
  and the full policy are both exercised whatever the target selected. The bound choice —
  `acl_policy_t`, plain C++ in `config.hpp` and rebindable by a `config_override.hpp` fragment
  ([ADR-0068](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0068-build-configuration-is-plain-cpp-config-header.md))
  — is only **printed**, on the last line.
- **Nothing here is conditional** — the target builds and runs under every CI leg, and it touches
  no graph at all: the policies are pure functions over ACE lists.

## Source

```{literalinclude} /core/examples/acl_policy_profiles.cpp
:language: cpp
:linenos:
```

See also: [security-acl module](../modules/security-acl.md) ·
[config module](../modules/config.md) ·
[deployment profiles](../reference/12-deployment-profiles.md) ·
[strict ACL parsing](acl-parse-strict.md) ·
[inheritance](acl-inherit.md).
