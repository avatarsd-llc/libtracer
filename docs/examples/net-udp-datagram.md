# A datagram already has boundaries (transport plane, `udp`)

Every stream kind in the tree — `tcp`, `ws`, `quic` — has to *invent* message boundaries,
because a stream has none. UDP already has them, so `udp_transport_t` adds nothing: a `send` is
one `sendto`, an inbound datagram is one frame, and there is no reassembler, no length prefix
and no partial-frame state anywhere in the kind.

That single difference is what the whole page is about, and it has two consequences worth
seeing before wiring a datagram link.

## What to notice

- **One datagram is one frame, with zero bytes of framing.** What the sender handed to `send` is
  what the receiver's callback sees, and its *length* came from the datagram rather than from
  anything in the bytes. Two sends are two frames — never one coalesced read the receiver has to
  split apart, which is exactly the work [the TCP page](net-tcp-stream-framing.md) shows.
- **The bound is hard, and `max_frame` may only tighten it.** `kMaxDatagram` is what a datagram
  can physically be, so a configured cap above it is inert rather than loosening. A frame larger
  than a datagram is not a UDP frame; that is what a streaming kind is for.
- **The listener has no peer until one talks to it.** Constructed with no peer address, the
  transport LEARNS its peer from the source address of the first inbound datagram. That is what
  lets a config-created `role=listener` reply to a dialer whose ephemeral source port could not
  have been known in advance.
- **A send before the peer is known is a no-op** — not an error, and not a queued frame. There
  is no address to send to and UDP has nowhere to hold it. The example asserts it rather than
  leaving it to be discovered.
- **The two counters mean different things.** `dropped_rx` is *this node's* resources
  (RX-backend exhaustion — backpressure, never an OOM); `malformed_rx` is *the peer's fault* (a
  datagram over the cap). UDP is connectionless, so neither tears anything down: the next
  datagram is served normally.
- **Both sockets bind port 0.** Real loopback sockets on kernel-chosen ports, the way
  `udp_test` does, so nothing here can collide with what else is running.
- **This target needs the UDP transport.** It is built only when `LIBTRACER_TRANSPORT_UDP` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/net_udp_datagram.cpp
:language: cpp
:linenos:
```

See also: [transport module](../modules/transport.md) ·
[connection config](../modules/connection-config.md) ·
[module catalog reference](../reference/10-module-catalog.md) ·
[the stream family's answer](net-tcp-stream-framing.md).
