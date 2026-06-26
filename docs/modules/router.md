# router — the ROUTER envelope (L4)

```{admonition} In one paragraph
:class: tip
A bridge wraps a data TLV in a **ROUTER** envelope when it crosses a wire, and
sheds it on the other side. `router_wrap` builds the envelope (origin peer id,
origin timestamp, hop count, then the data TLV last); `router_unwrap` returns the
metadata + a **zero-copy span** over the wrapped data. `(origin, timestamp)` is the
dedup key; `hop_count` is the cycle-termination guarantee ([ADR-0014]).
```

## What it does

ROUTER (`0x0D`) is a structured TLV whose `NAME`-tagged children carry the routing
metadata, with `NAME "data"` followed by the wrapped data TLV as the **last** child.
`router_wrap` emits it via the [frame-codec](frame-codec.md) byte emitters
(LL-aware, so large payloads still fit); `router_unwrap` walks the children to fill
`router_meta_t` and returns a `span` over the data — no copy. To downstream
subscribers the ROUTER is invisible (the [bridge](bridge.md) strips it); to other
bridges it carries the dedup key and `hop_count`.

## Interface

```cpp
struct router_meta_t { peer_id_t origin; std::uint64_t ts; std::uint8_t hop; };
struct unwrapped_t  { router_meta_t meta; std::span<const std::byte> data; };  // borrows the frame

std::vector<std::byte> router_wrap(std::span<const std::byte> data, const router_meta_t&);
std::expected<unwrapped_t, error_t> router_unwrap(std::span<const std::byte> frame);
```

## Envelope layout

```text
ROUTER (type 0x0D, opt.PL=1)
 ├─ NAME "origin_peer_id"   VALUE  <16 bytes>     ┐
 ├─ NAME "origin_timestamp" TIME   <u64 ns>       ├ dedup key = (origin, ts)
 ├─ NAME "hop_count"        VALUE  <u8>           ┘  termination = hop_count
 └─ NAME "data"             <the wrapped data TLV, verbatim, LAST>
```

## Benefits

- **Append-only at egress, strip-only at ingress** — the data TLV's bytes (and its
  own trailer) survive the wrap/unwrap untouched; the payload is never rewritten.
- **Zero-copy unwrap** — the wrapped TLV is returned as a span into the received
  frame; the bridge copies once only when re-injecting into the local graph.
- **Two independent loop defenses** — a recent-set on `(origin, ts)` (best-effort)
  and `hop_count`/`MAX_HOPS` (the guarantee), so dedup memory is a free knob.

See: [bridge](bridge.md), [wire-format-bits](wire-format-bits.md#a-router-frame).
