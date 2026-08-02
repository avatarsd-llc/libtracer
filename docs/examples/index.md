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

Two of the seven targets need the FWD routing plane and exist only when
`LIBTRACER_NET_PLANE` is on: `two_node_fwd` and `tree_of_ropes` are declared inside
`if(LIBTRACER_NET_PLANE)` blocks (`core/examples/CMakeLists.txt:58,73`), and so are
their test registrations (`core/examples/CMakeLists.txt:87-96`). The option defaults to
`ON` (`core/CMakeLists.txt:63-65`), so the recipe above builds all seven. Configured with
`-DLIBTRACER_NET_PLANE=OFF`, those two binaries are never produced and
`ctest --test-dir build -R example_` runs **five of seven** — a pass with two examples
absent rather than skipped, which no ctest output distinguishes from a clean full run.
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
```
