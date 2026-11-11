# noise/ws-oversize-fragments

A WebSocket-carrier case of [reference/23](../../../../../../docs/reference/23-noise-link-binding.md)
§13.12 (RFC-0033 Amendment 2, A2.12). After proof, a fragmented message passes max_frame + 25 at its third fragment.

After the proof (the handshake, the empty confirmation and acknowledgement) with `max_frame` 16, so the bound is 41 B, a 70-byte message arrives in four fragments of 20, 20, 20 and 10 bytes. Expected: refused at the third fragment, where the running total passes 41, counted once in `malformed_rx`; the fourth fragment is discarded through its FIN; the connection stays open, and the following `VALUE "ping"` is delivered.

Every Noise byte is from [`nnpsk0-appendix-a`](../nnpsk0-appendix-a/), so that case's third-party
checks gate them. `ws.json` is written by [`gen_ws_vectors.py`](../gen_ws_vectors.py), which adds
only RFC 6455 framing; `--check` fails if the file differs. Every frame is also parsed by the
third-party `websockets` library ([`independent_check_websockets.py`](../independent_check_websockets.py),
result in `independent_check_websockets.txt`). The case has no `transcript.json` and no `input.bin`,
so neither the codec harnesses nor the Noise transcript replay walk it; the WebSocket carrier's
implementation ([#2296](https://github.com/avatarsd-llc/libtracer/issues/2296)) replays it.
