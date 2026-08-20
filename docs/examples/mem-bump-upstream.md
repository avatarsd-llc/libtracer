# The upstream decides what the buffer means (L0 substrate)

`tr::mem::bump_source_t` takes a second argument nobody has to pass, and it is the one that
changes what the source *means*. Once the caller's buffer cannot fit a request, the bump draws
from its **upstream** instead — so the same buffer is either a fast path or the node's hard
ceiling, depending on one word at the construction site.

| upstream | an oversize request | what the buffer is |
| --- | --- | --- |
| `heap_source()` (the default) | succeeds, off the process heap | a fast path |
| `null_source()` | `nullptr` | the bound |

## What to notice

- **This is what makes the bump a capability-*preserving* substitution.** A
  `std::pmr::monotonic_buffer_resource` also spills past its buffer — but it spills to a
  **throwing** resource, which under `-fno-exceptions` is the `abort()` the failable seam exists
  to remove. Same shape, different ending.
- **`used()` is how you tell where a block came from.** The example serves an oversize request
  through the default upstream and finds `used() == 0`: nothing was carved from the caller's
  buffer, so a node that believed its buffer was the bound was quietly wrong. Watch this number,
  not the success of the call.
- **`null_source()` is the honest form of `std::pmr::null_memory_resource()`.** Both mean "serve
  nothing"; only one of them says so by value.
- **A spilled block is still returned properly.** `release` on a block that came from the
  upstream routes back to the upstream (only a block from the *buffer* is the no-op), so the
  composition leaks nothing — which is why the ASan leg is a real check on this example.
- **A refusal is not a broken source.** After the oversize `nullptr`, the hard-bounded source
  still serves what fits and the earlier block is untouched. Exhaustion is backpressure, not a
  terminal state.
- **The shipped example of the bounded composition is the arena decode.**
  [`wire_arena_decode`](wire-arena-decode.md) names `null_source()` so a frame that outgrows the
  stack slab is refused rather than quietly served from the heap.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/mem_bump_upstream.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) ·
[memory substrate reference](../reference/09-memory-substrate.md) ·
[a bump source](mem-bump-source.md) · [`decode_into`: a flat arena](wire-arena-decode.md).
