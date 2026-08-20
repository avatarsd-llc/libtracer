# One seam, every wire technology (transport plane)

`tr::net::transport_t` is the entire contract a transport kind has to satisfy. Three calls —
`send(span)`, the optional scatter-gather `send(iov)`, and `set_receiver` /
`set_rope_receiver` — and no fourth.

There is no `send_read`, no `send_reply`, and no TLV type anywhere in it. That absence is the
design: the router owns addressing (the `dst` source route), the codec owns structure, and a
transport owns exactly one question — *how do these bytes cross this wire*. It is why `udp`,
`tcp`, `ws`, `can`, `quic`, `webtransport` and the in-process loopback are interchangeable to
everything above them, and why writing a new kind is writing this one class.

The example writes a complete kind in about twenty lines, then drives the shipped
`loopback_channel_t` with the identical calls.

## What to notice

- **A transport never sees TLV semantics.** It is handed `std::span<const std::byte>` and hands
  back the same. Callback-plus-recv-thread is an implementation choice about *this* C++ seam,
  not a protocol property
  ([ADR-0013](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0013-v1-scope-boundaries.md)) —
  two conforming nodes need not share it, only the wire.
- **The iovec overload is optional, and the base pays for the kinds that skip it.** A kind that
  cannot `writev` inherits a gather into one block drawn from the link's egress source, so a
  rope's `to_iovec()` reaches every kind. Overriding it elides that copy — never changes the
  contract; the bytes on the wire are one record either way.
- **Declaring one `send` hides the other.** Ordinary C++ name hiding, and a kind that omits
  `using transport_t::send;` silently loses the entry point it meant to inherit. The example
  carries the line and says why.
- **The sink goes in before frames flow, and that is not advice.** A kind whose receive thread
  starts in its own constructor is draining the wire while the owner is still wiring; a frame
  landing in an empty slot is dropped with no counter moving. Dialling kinds offer `defer_recv`
  plus `start_receiving()` for exactly that window (#1025 / #1045).
- **Owning delivery is a capability, not an assumption.** `delivers_ropes()` is `false` by
  default and the example's own kind leaves it there. There is deliberately no adapter that
  wraps a borrowed span in a rope whose refcounts would lie
  ([ADR-0042](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0042-refcounted-receiver-seam-view-delivery.md) §1).
- **Nothing here is conditional.** `transport_t` and the loopback channel are the required core,
  so this target builds and runs under every CI leg — including the minimal module set with the
  net plane and all four transports off.

## Source

```{literalinclude} /core/examples/net_transport_seam.cpp
:language: cpp
:linenos:
```

See also: [transport module](../modules/transport.md) ·
[module catalog reference](../reference/10-module-catalog.md) ·
[transports are vertices](../reference/19-transports-are-vertices.md) ·
[a kind is a name](net-kind-catalog.md).
