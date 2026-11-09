# One seam, every wire technology (transport plane)

`tr::net::transport_t` is the contract a transport kind has to satisfy. One call is required
today, the one-span `send(span)`. The scatter-gather `send(iov)` and the retained send are
overridable and have default bodies, and frames come back through `set_receiver` /
`set_rope_receiver`. The remaining members (`drop_stats`, `start_receiving` and the like) have
defaults too.

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
- **The span `send` is the pure virtual; the iovec overload is the one worth overriding.** The
  router sends forwards through `send(iov)`, while some short answers, such as a refusal or a
  bare `ERROR`, go through the span `send`. The base `send(iov)` gathers the spans into a
  temporary and calls the span `send`, dropping and counting a gather it cannot complete. A kind
  with `writev` overrides it to avoid that copy, and one without can join the spans into memory
  it already owns, as the example's kind does; the bytes on the wire are one record either way.
  [ADR-0089](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0089-scatter-gather-is-the-one-required-transport-send.md)
  makes `send(iov)` the required call and moves the span `send` into the base.
- **Declaring one `send` hides the others.** Ordinary C++ name hiding, and a kind that omits
  `using transport_t::send;` silently loses the retained send it meant to inherit. The example
  carries the line and says why.
- **The sink goes in before frames flow, and that is not advice.** A kind whose receive thread
  starts in its own constructor is draining the wire while the owner is still wiring; a frame
  landing in an empty slot is dropped with no counter moving. The built-in dialling kinds start
  receiving only at `start_receiving()`, which closes that window (#1025 / #1045 / #2247).
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
