# Rope scatter-gather (L1 views)

A [`rope_t`](../modules/views.md) chains several `view_t` windows into one logical
payload **without ever copying the bytes** (ADR-0053). This example composes a
16-link rope over independently-allocated segments, checks the logical bytes match a
hand-built reference, and contrasts the two ways to hand that payload to a consumer.

## What to notice

- **Composition is chaining, not copying** — 16 `append`s produce a 16-link rope; only the
  chain metadata (a single vector once the inline capacity spills) is ever allocated, never
  the payload bytes.
- **`to_iovec()` is the zero-copy egress path** — one `std::span` per link, each pointing
  *into* its original segment; this is exactly what you hand to `writev`/`sendmsg`.
- **`flatten()` is the one copy you can measure** — the single contiguous `memcpy`, taken
  only at a boundary that cannot scatter-gather. The `RESULT` line prints both so the
  trade-off is explicit.
- **It self-checks** — `total_length`, the iovec coverage, and byte-for-byte equality of the
  flattened bytes against the scatter-gather order are all asserted.

```{note}
The absolute nanoseconds come from whatever build ran (CI builds the examples in a debug
configuration); treat them as a *shape*, not a spec number. The canonical, release-build,
CI-published figures live on the [performance page](../performance.md).
```

## Source

```{literalinclude} /core/examples/rope_scatter.cpp
:language: cpp
:linenos:
```

See also: [views module](../modules/views.md) ·
[views & ownership reference](../reference/08-views-and-ownership.md).
