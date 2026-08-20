# No frame crosses until the Upgrade completes (transport plane, `ws`)

`ws` is the browser-reachable kind: a page cannot open a raw TCP socket, so libtracer frames
reach it inside RFC 6455 messages. The price is a phase no other stream kind has.

Before the first byte of a frame, the client sends `GET / HTTP/1.1` with `Upgrade: websocket`
and a fresh 16-byte nonce in `Sec-WebSocket-Key`; the server answers `101 Switching Protocols`
with `Sec-WebSocket-Accept` set to `base64(sha1(key ++ RFC-6455-GUID))`. The example drives that
from a raw POSIX socket and checks the answer against `ws::accept_key` — so the `101` is shown
to be **computed from the client's own nonce**, not a constant a stub could echo.

## What to notice

- **`ok()` on a WS transport is the handshake's verdict, not the socket's.** The TCP connect
  succeeding is not the link coming up. The example proves the point twice: once by hand, once
  behind `transport_ws_client`, which does exactly the exchange spelled out above.
- **One libtracer frame is one BINARY message.** The sink is handed the payload; the WS header
  never reaches it. Client→server frames are masked (§5.1), server→client are not — and both
  directions land in the same sink shape, because masking is the kind's business.
- **The handshake is also an attack surface, and it has its own budget.** The peer on that path
  has authenticated nothing and is making this node accumulate a header block, so `max_handshake`
  is a PRE-AUTH request-size bound (#934). The example shows both arms: a smaller budget is
  honoured, a larger one is **clamped back**.
- **Tighten-only is the general shape of a config-writable bound here.** A key an unauthenticated
  peer's deployment can reach may narrow what it costs the node and may never widen it — the
  same rule `max_frame` follows on every kind.
- **The multi-peer listener is the same object.** `transport_ws_server` shares its slot/poll
  machinery with `transport_tcp_server` (#871); what WS adds is the packaging. The slot side is
  [its own page](net-multi-peer-listener.md).
- **This target needs the WS transport.** It is built only when `LIBTRACER_TRANSPORT_WS` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/net_ws_upgrade.cpp
:language: cpp
:linenos:
```

See also: [transport module](../modules/transport.md) ·
[WebSocket session & auth reference](../reference/16-websocket-session-auth.md) ·
[connection config](../modules/connection-config.md) ·
[the raw stream underneath](net-tcp-stream-framing.md).
