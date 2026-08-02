# Design notes

> **Scope:** these notes are **not** the standard. The protocol is
> [`../spec/v1.md`](../spec/v1.md); the implementation-independent description is
> [`../reference/README.md`](../reference/README.md). What lives here is the measured cost of the
> C++23 reference implementation's own choices, and how cost measurement goes wrong here.

Design rationale that constitutes a *decision* is an ADR ([`../adr/`](https://github.com/avatarsd-llc/libtracer/tree/main/docs/adr/)); a proposed change to the
normative surface is an RFC ([`../spec/rfcs/`](https://github.com/avatarsd-llc/libtracer/tree/main/docs/spec/rfcs/)). These notes sit between: too long
for an ADR, too specific to one implementation for the reference suite.

The toctree below is the order of record; this table adds the topic of each area.

| Area | Topic |
| --- | --- |
| [Concurrency](concurrency/README.md) | Which serializers exist in the graph runtime and what each costs; the two independent limits on the read path; the cost budget; the remaining serializers; and how measurement goes wrong in this area. |
| [Zero-copy and flatten](zero-copy-and-flatten.md) | Where a copy still lands on the data plane: every `materialize` call site classified single-link vs multi-link, the 4096-byte decode arena, and the structural copies. |
| [Build configuration](config/README.md) | Which knobs exist, what each costs on which target, and which constants are deliberately off-limits — the integrator question the reference suite is not allowed to answer. |
| [Failable allocation and backpressure](allocation-and-backpressure.md) | Why no peer-provokable allocation can abort under `-fno-exceptions`, which seam each allocation draws from, and how exhaustion surfaces as `BACKPRESSURE`. |

```{toctree}
:caption: Design notes
:hidden:
:maxdepth: 2

Concurrency & scaling <concurrency/README>
Zero-copy and flatten <zero-copy-and-flatten>
Build configuration <config/README>
Failable allocation and backpressure <allocation-and-backpressure>
```
