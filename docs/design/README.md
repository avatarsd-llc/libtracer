# Design notes — reference-implementation programs

> **Status:** contributor material, published for reference. These are **not** the standard.
> The protocol is [`../spec/v1.md`](../spec/v1.md); the implementation-independent description
> is [`../reference/README.md`](../reference/README.md). What lives here is the *why* and the
> *measured cost* of the C++23 reference implementation's own choices — programs of work, cost
> budgets, and the ledgers of what turned out to be wrong.

Design rationale that constitutes a *decision* is an ADR ([`../adr/`](https://github.com/avatarsd-llc/libtracer/tree/main/docs/adr/)); a proposed change to the
normative surface is an RFC ([`../spec/rfcs/`](https://github.com/avatarsd-llc/libtracer/tree/main/docs/spec/rfcs/)). These notes sit between: too long
for an ADR, too specific to one implementation for the reference suite.

| Program | Topic |
| --- | --- |
| [Zero-copy & flatten](zero-copy-and-flatten.md) | Whether libtracer really needs to flatten: every `materialize` call site classified, the 4096-byte decode arena dissected, and the two flattens that are genuinely fundamental. |
| [Concurrency](concurrency/README.md) | Which serializers exist in the graph runtime and what each costs; the two independent limits on the read path; the cost budget; the ranked remaining levers; and a ledger of three mis-attributions in this area. |
| [Build configuration](config/README.md) | Which knobs exist, what each costs on which target, and which constants are deliberately off-limits — the integrator question the reference suite is not allowed to answer. |
| [RFC-0014 implementation](rfc-0014-implementation-plan.md) | Slice plan for the creator-endpoint and link-liveness work. |

```{toctree}
:caption: Design programs
:hidden:
:maxdepth: 2

Zero-copy & flatten <zero-copy-and-flatten>
Concurrency & scaling <concurrency/README>
Build configuration <config/README>
RFC-0014 implementation <rfc-0014-implementation-plan>
```
