# `tlv_view_t`: a frame whose bytes are scattered (L1 + L2/L3)

This is the hinge between the two domains. Memory composition and TLV composition are
orthogonal ([CONTEXT.md](../../CONTEXT.md) §Two compositions), so a **rope** link boundary may
fall *anywhere* — including in the middle of a TLV header — and the decoder must not care. A
CAN reassembly group and a fragmented WebSocket message both arrive exactly like this.

`tlv_view_t::over`
([ADR-0053](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md))
adopts the rope as one lazy TLV: it parses the root header with the CRC walk **deferred** and
requires the declared total to match the rope's length. Nothing that is not accessed is ever
decoded.

## What to notice

- **The split is deliberately mid-header.** The example cuts the frame at byte 6, inside the
  first child's header, and the child still materializes. A frame arriving as the links a
  transport happened to receive it in is the normal case, not the awkward one — and it crosses
  the receiver seam as the rope it already is, never flattened at ingress.
- **Children come one header at a time.** `children().next()` parses exactly one child header
  per call and yields it as its own `tlv_view_t` over a subrope. A sibling nobody looks at has
  its payload walked by nobody.
- **Validation is staged.** `over` anchors the bounds; child headers are grammar-checked as
  they are stepped over; `verify()` is the deferred CRC walk, run by whichever consumer wants
  the integrity guarantee. An endpoint applying several members as one transaction verifies
  first and applies second.
- **`materialize()` is the single explicit copy point.** Everything the lazy tier deferred is
  paid there, once, by the consumer that asked: one contiguous copy plus the full grammar walk.
  A hop that only forwards never calls it — it hands `wire()` or a `body()` subrope onward and
  the links stay refcount-alive across the hop.
- **Nothing here is conditional** — the target builds and runs under every CI leg, net plane
  on or off.

## Source

```{literalinclude} /core/examples/wire_lazy_view.cpp
:language: cpp
:linenos:
```

See also: [views module](../modules/views.md) · [frame codec](../modules/frame-codec.md) ·
[views & ownership reference](../reference/08-views-and-ownership.md) ·
[rope scatter-gather](rope-scatter.md).
