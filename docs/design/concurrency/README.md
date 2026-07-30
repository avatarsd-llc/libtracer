# Concurrency and scaling — reference-implementation design notes

> **Status:** design (post-implementation record, not pre-ADR). **Target:** the C++23 reference
> implementation under [`../../../core/`](../../../core/), on a many-core host. **Companion:**
> [`../../reference/15-concurrency-and-scaling.md`](../../reference/15-concurrency-and-scaling.md)
> is the standard-level half — the obligations any implementation must meet and the four
> hardware regimes. Everything here is specific to this codebase and one host, and none of it
> is normative.

Where the reference suite says *what* must hold, these notes say *what it costs here*: which
serializers exist, what each one measures, which shapes currently scale, and what is left.

| File | Topic |
| --- | --- |
| [`00-scaling-and-serialization.md`](00-scaling-and-serialization.md) | The serializer inventory with file:line; the two independent limits on the read path; what #647 and #654 each bought, measured; the cost budget; the ranked remaining levers; the diagnostic recipe; and the ledger of this document's own corrections. |

## Instruments

Three, and it matters which one a number came from:

| instrument | what it measures | committed? |
| --- | --- | --- |
| [`bench/bench_contention`](../../../bench/bench_contention.cpp) | the **machine** — what one shared cache line costs, libtracer absent | yes |
| [`bench/bench_lkv_slot`](../../../bench/bench_lkv_slot.cpp) | model slot policies (`slot` mode) and the real `graph_t::read`/`write` shapes (`graph` mode) | yes |
| the `has_registered_child` ablation | the ceiling a lock removal could reach | **no** — a two-line hack that breaks the composed branch read and can never merge |

The model arms in `bench_lkv_slot slot` are named `model_*` in the source precisely because two
of them once shared names with real library types, and this project's documentation has already
been corrected twice for quoting a model arm as a path cost. Never quote a `slot`-mode number
as a forecast for a real path.

```{toctree}
:caption: Concurrency program
:hidden:
:maxdepth: 1

Scaling and serialization <00-scaling-and-serialization>
```
