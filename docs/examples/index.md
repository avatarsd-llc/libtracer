# Examples

Worked, **compile-tested** examples of the C++ reference implementation. Every example
on these pages is a real source file under [`core/examples/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/examples)
that CI **builds and runs as a smoke test** on every change — the code shown is included
verbatim from that file, so it cannot drift from what actually compiles.

| Example | Layer | What it shows |
| ------- | ----- | ------------- |
| [In-process pub/sub](in-process-pubsub.md) | L4 graph | `read`/`write`/`await`, three delivery styles, zero-copy fan-out |
| [Pub/sub fan-out & dispatch cost](pubsub-fanout.md) | L4 graph | per-delivery latency as fan-out scales 1 → 8 → 64; `:schema` discovery |
| [Wire codec round-trip](wire-roundtrip.md) | L2/L3 codec | `encode` / `decode`, the CRC trailer, and zero-copy borrowed payloads |
| [Wire codec deep-dive & throughput](wire-codec.md) | L2/L3 codec | frame anatomy + encode/decode/round-trip throughput |
| [Rope scatter-gather](rope-scatter.md) | L1 views | compose a multi-link `rope_t`; `to_iovec` (zero copy) vs `flatten` (one copy) |
| [Two nodes over a wire — FWD delivery](two-node-fwd.md) | L4 + transport | `fwd_router_t` source-routing across a channel; cross-wire latency |
| [Composition axes](tree-of-ropes.md) | L1 + L4 + transport | why a node is a tree of ropes and not a rope of ropes; rope over two backends; mount = identity, not memory |
| [Register a vertex, and address it](graph-register.md) | L4 graph | `path_t` parses once; the vertex map is keyed on PATH bytes; `PATH_IN_USE`; placeholders |
| [Read and write](graph-read-write.md) | L4 graph | one store per vertex, last-writer-wins; `read` returns a reference to the published value |
| [`await`](graph-await.md) | L4 graph | the readiness plane: single-shot, at its own vertex only, `TIMEOUT` on the deadline |
| [Write-creates](graph-write-creates.md) | L4 graph | a LOCAL data write materializes its target and its missing intermediates |
| [`:children[]`](graph-children.md) | L4 graph | enumerate a parent's members, one level, through the `:` control plane |
| [Retirement](graph-retire.md) | L4 graph | logically absent, not erased: `NOT_FOUND`, live handles, the generation stamp |
| [A HANDLER vertex](graph-handler-vertex.md) | L4 graph | the role decides what a write means: `on_write` executes, `on_read` computes |
| [A STREAM vertex](graph-stream.md) | L4 graph | the bounded history ring and its owner-declared depth (no wire surface) |
| [Subscribe to one vertex](sub-callback.md) | L4 graph | `subscribe(src, callable)`, the `subscription_t` handle, and the inline delivery contract |
| [One edge, a whole subtree](sub-subtree.md) | L4 graph | RFC-0005 vertical bubbling; `own_subs` vs `has_subscribers`; provenance rides in the data |
| [Delivery terminates at the target](sub-terminal-delivery.md) | L4 graph | RFC-0007: `A → B` plus `B → C` does not relay; a cycle cannot loop |
| [The delivery policy is per subscription](sub-durability-latch.md) | L4 graph | RFC-0022 §3.A `delivery_policy_t`; `durability_request` latches the last value on join |
| [Unsubscribe & the release hook](sub-unsubscribe.md) | L4 graph | `unsubscribe(sub, release)`; when the `{fn, ctx}` pair is safe to free |
| [Unsubscribing from inside a delivery](sub-unsubscribe-from-dispatch.md) | L4 graph | ADR-0080's deferred grace point; `reclaim_local` vs `reclaim_strict` |
| [Retire drops a producer's subscriptions](sub-retire.md) | L4 graph | RFC-0009 §B re-virginize; what survives a retire, and what liveness does not do yet |

The toctree below is the order of record; this table adds the layer and the summary.
Each example's layer column names the module that owns the types it uses — the
[C++ API reference](../modules/index.md) is where those declarations are rendered
from the headers.

Several examples print a `RESULT …` line with **latency and throughput** numbers. Those are
informational (measured on whatever build ran — CI builds the examples in debug), so CI never
flakes on timing; the canonical release-build figures live on the
[performance page](../performance.md).

:::{admonition} Build and run the examples
:class: tip

The examples build by default with the core (`LIBTRACER_BUILD_EXAMPLES`, on when
libtracer is the top-level project):

```console
$ cmake -S core -B build -DBUILD_TESTING=ON
$ cmake --build build
$ ./build/examples/in_process_pubsub
$ ./build/examples/pubsub_fanout
$ ./build/examples/wire_roundtrip
$ ./build/examples/wire_codec
$ ./build/examples/rope_scatter
$ ./build/examples/two_node_fwd
$ ./build/examples/tree_of_ropes
```

Or run them the way CI does — as ctest smoke tests that self-check and fail on any
mismatch: `ctest --test-dir build -R example_`.

Two targets need the FWD routing plane and exist only when
`LIBTRACER_NET_PLANE` is on: `two_node_fwd` and `tree_of_ropes` are declared inside
`if(LIBTRACER_NET_PLANE)` blocks (`core/examples/CMakeLists.txt:59,74`), and so are
their test registrations (`core/examples/CMakeLists.txt:87-96`). The option defaults to
`ON` (`core/CMakeLists.txt:63-65`), so the recipe above builds every example. Configured with
`-DLIBTRACER_NET_PLANE=OFF`, those two binaries are never produced and
`ctest --test-dir build -R example_` runs **two fewer** — a pass with two examples
absent rather than skipped, which no ctest output distinguishes from a clean full run.

The eight `graph_*` targets are pure in-process L4 and are built unconditionally; run them
together with `ctest --test-dir build -R example_graph_`.
:::

```{toctree}
:caption: Worked examples
:hidden:
:maxdepth: 1

In-process pub/sub <in-process-pubsub>
Pub/sub fan-out & dispatch cost <pubsub-fanout>
Wire codec round-trip <wire-roundtrip>
Wire codec deep-dive & throughput <wire-codec>
Rope scatter-gather <rope-scatter>
Two nodes over a wire — FWD delivery <two-node-fwd>
Composition axes <tree-of-ropes>
Register a vertex, and address it <graph-register>
Read and write <graph-read-write>
await <graph-await>
Write-creates <graph-write-creates>
:children[] <graph-children>
Retirement <graph-retire>
A HANDLER vertex <graph-handler-vertex>
A STREAM vertex <graph-stream>
Subscribe to one vertex <sub-callback>
One edge, a whole subtree <sub-subtree>
Delivery terminates at the target <sub-terminal-delivery>
The delivery policy is per subscription <sub-durability-latch>
Unsubscribe & the release hook <sub-unsubscribe>
Unsubscribing from inside a delivery <sub-unsubscribe-from-dispatch>
Retire drops a producer's subscriptions <sub-retire>
```
