# Examples

Worked, **compile-tested** examples of the C++ reference implementation. Every example
on these pages is a real source file under [`core/examples/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/examples)
that CI **builds and runs as a smoke test** on every change — the code shown is included
verbatim from that file, so it cannot drift from what actually compiles.

| Example | Layer | What it shows |
| ------- | ----- | ------------- |
| [In-process pub/sub](in-process-pubsub.md) | L4 graph | `read`/`write`/`await`, three delivery styles, zero-copy fan-out |
| [Wire codec round-trip](wire-roundtrip.md) | L2/L3 codec | `encode` / `decode`, the CRC trailer, and zero-copy borrowed payloads |

:::{admonition} Build & run them yourself
:class: tip

The examples build by default with the core (`LIBTRACER_BUILD_EXAMPLES`, on when
libtracer is the top-level project):

```console
$ cmake -S core -B build -DBUILD_TESTING=ON
$ cmake --build build
$ ./build/examples/in_process_pubsub
$ ./build/examples/wire_roundtrip
```

Or run them the way CI does — as ctest smoke tests that self-check and fail on any
mismatch: `ctest --test-dir build -R example_`.
:::
