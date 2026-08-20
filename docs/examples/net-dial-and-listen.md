# DIAL and LISTEN are two constructors, not two types (transport plane)

The minimal pair, spelled with `tcp_transport_t` because it is the kind that makes the point
plainest: one class, two constructors, and nothing downstream can tell which one built the
object it holds.

- `tcp_transport_t(bind_port)` **listens**. Pass `0` and the kernel picks the port;
  `local_port()` reports which.
- `tcp_transport_t(peer_host, peer_port)` **dials**, synchronously, inside the constructor.

Past bring-up the role is gone. The example's `exchange()` helper takes a `transport_t&` and
drives either end in either direction, which is the whole claim in one signature.

## What to notice

- **`ok()` is the CAME-UP predicate and it is role-specific** (#1059): on LISTEN it answers *did
  the bind succeed*, on DIAL *did the connect succeed*. It is answered once, right after
  construction, and never reverts.
- **`link_up()` is the other question** — is this connection alive *now*. After a teardown the
  two diverge: `ok()` stays true (the link did come up), `link_up()` goes false. Confusing them
  is how a dead link gets treated as healthy.
- **The failed bring-up is provoked deterministically.** A second LISTEN on the port the first
  one already holds cannot succeed on any machine. Dialling a port nobody is *expected* to
  answer would be a guess about the host rather than a demonstration, and would flake.
- **Direction is not a property of the link.** What makes a reply routable is the route the FWD
  plane grew into the frame's `src`, not which end opened the socket — see
  [the src you accumulated is the way home](route-reply-home.md).
- **The ephemeral port is how the two halves rendezvous** without a hard-coded number, which is
  also why every socket example in this tree binds `0`: nothing here can collide with whatever
  else is running on the CI machine.
- **This target needs the TCP transport.** It is built only when `LIBTRACER_TRANSPORT_TCP` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/net_dial_and_listen.cpp
:language: cpp
:linenos:
```

See also: [transport module](../modules/transport.md) ·
[connection config](../modules/connection-config.md) ·
[transports are vertices](../reference/19-transports-are-vertices.md) ·
[the stream framing this kind adds](net-tcp-stream-framing.md) ·
[the multi-peer listener](net-multi-peer-listener.md).
