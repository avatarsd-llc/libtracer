# A stream has no boundaries, so the kind supplies them (transport plane, `tcp`)

TCP delivers bytes, not messages. Whatever `write` calls a sender makes, the receiver may see
them merged, split, or both — so a stream transport that handed its reader's buffer straight up
would deliver half frames and double frames.

`tcp_transport_t` prefixes each frame with `u32-LE length` and reads it back in two steps: read
four bytes, then read exactly that many. The prefix is **transport** framing — it is on the
wire, it is not in the TLV, and nothing above the transport ever sees it.

## What to notice

- **Why a FIXED-width prefix and not the TLV's own length field.** A variable-width header
  cannot be read without first buffering an unknown number of bytes, which is the problem the
  prefix exists to solve. Four bytes, always, then the body.
- **Coalesced.** Two whole records in one `write` arrive as **two** frames, split at the right
  byte and in order.
- **Split.** One record dribbled out in three `write`s — the four-byte prefix itself torn in
  half — arrives as **one** frame, and no fourth frame is invented from the fragments. This is
  the case the fixed width has to survive: the reader cannot even know the length until all four
  bytes are in hand.
- **The prefix is demonstrated, not asserted.** The example reads a transport-sent frame off a
  raw socket and decodes the length itself, so the framing is visible on the wire rather than
  described.
- **The peer is a raw POSIX socket on purpose.** A second `tcp_transport_t` would be the
  realistic peer and exactly the wrong tool: it would choose the write boundaries itself and
  hide the thing being shown.
- **The fourth stream hazard is deliberately elsewhere.** A prefix above the effective cap tears
  the connection down, because a stream that has lost framing sync cannot be resynchronized —
  that is a lifecycle concept, and `tcp_test`'s oversize-prefix case owns it.
- **This target needs the TCP transport.** It is built only when `LIBTRACER_TRANSPORT_TCP` is on
  (the default). Nothing in it is conditional at run time.

## Source

```{literalinclude} /core/examples/net_tcp_stream_framing.cpp
:language: cpp
:linenos:
```

See also: [transport module](../modules/transport.md) ·
[frame codec](../modules/frame-codec.md) ·
[data format reference](../reference/01-data-format.md) ·
[the datagram family, which needs none of this](net-udp-datagram.md) ·
[dial and listen](net-dial-and-listen.md).
