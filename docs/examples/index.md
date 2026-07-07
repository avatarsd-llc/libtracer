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

Several examples print a `RESULT …` line with **latency and throughput** numbers. Those are
informational (measured on whatever build ran — CI builds the examples in debug), so CI never
flakes on timing; the canonical release-build figures live on the
[performance page](../performance.md).

:::{admonition} Build & run them yourself
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
```

Or run them the way CI does — as ctest smoke tests that self-check and fail on any
mismatch: `ctest --test-dir build -R example_`.
:::
