# Concurrency and scaling

> **Scope:** this implementation's own serializers and their measured cost on one many-core
> host. Not normative. **Target:** the C++23 reference implementation under
> [`core/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/), on a many-core host. **Companion:**
> [`../../reference/15-concurrency-and-scaling.md`](../../reference/15-concurrency-and-scaling.md)
> is the standard-level half — the obligations any implementation must meet and the four
> hardware regimes. Everything here is specific to this codebase and one host, and none of it
> is normative.

Where the reference suite states *what* must hold, these pages state *what it costs here*: which
serializers exist, what each one guards, which shapes scale on this host, and which serializers
remain on the hot paths.

| File | Topic |
| --- | --- |
| [`00-scaling-and-serialization.md`](00-scaling-and-serialization.md) | The serializer inventory with file anchors; the two independent limits on the read path; the measured effect of removing the per-read map lock and of binding the hazard-pointer slot; the cost budget; the remaining serializers; the diagnostic recipe; and how measurement goes wrong here. |
| [`01-write-and-delivery-path.md`](01-write-and-delivery-path.md) | The write and delivery half: the snapshot-under-lock split, the vertex lock stripes taken on every delivery, fan-out and scatter-gather delivery cost, and the forward-demux mount scan. Its forward-hop and mount-scan figures come from [`bench/bench_forward_demux.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/bench_forward_demux.cpp), the instrument that settled the acceptance condition of [ADR-0061, per-module mount routing](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md). |

## The three instruments behind the figures here

`bench/` holds several dozen bench sources. This table is not an inventory of them: it names the three
that produce the numbers in [`00-scaling-and-serialization.md`](00-scaling-and-serialization.md),
because it matters which one a figure came from. The write and delivery path quotes a fourth,
named in its row above.

| instrument | what it measures | committed? |
| --- | --- | --- |
| [`bench/bench_contention`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/bench_contention.cpp) | the **machine** — what one shared cache line costs, libtracer absent | yes |
| [`bench/bench_lkv_slot`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/bench_lkv_slot.cpp) | model slot policies (`slot` mode) and the real `graph_t::read`/`write` shapes (`graph` mode) | yes |
| the `has_registered_child` ablation | the ceiling a lock removal could reach | not committed; it short-circuits the fork check (`vertex_t::has_registered_child`, `core/include/libtracer/vertex.hpp:1460`, called from `core/src/graph.cpp:964`), which breaks the composed branch read |

A `slot`-mode arm models a slot policy in isolation and is not the cost of any code path. The
arms are named `model_*` in the source (`bench/bench_lkv_slot.cpp:193,342,456`) so that a model
policy and a real library type cannot be confused. Never quote a `slot`-mode number as a forecast
for a real path.

```{toctree}
:caption: Concurrency
:hidden:
:maxdepth: 1

Scaling and serialization <00-scaling-and-serialization>
Write and delivery path <01-write-and-delivery-path>
```
