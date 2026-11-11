# noise/ws-text-opcode

A WebSocket-carrier case of [reference/23](../../../../../../docs/reference/23-noise-link-binding.md)
§13.12 (RFC-0033 Amendment 2, A2.12). A valid first message sent as a text message is refused.

A valid first message sent as a text message (opcode `0x1`). Expected: the responder drops it and counts it in `malformed_rx`, sends nothing, counts no Noise failure, and keeps the connection open.

Every Noise byte is from [`nnpsk0-appendix-a`](../nnpsk0-appendix-a/), so that case's third-party
checks gate them. `ws.json` is written by [`gen_ws_vectors.py`](../gen_ws_vectors.py), which adds
only RFC 6455 framing; `--check` fails if the file differs. Every frame is also parsed by the
third-party `websockets` library ([`independent_check_websockets.py`](../independent_check_websockets.py),
result in `independent_check_websockets.txt`). The case has no `transcript.json` and no `input.bin`,
so neither the codec harnesses nor the Noise transcript replay walk it; the WebSocket carrier's
implementation ([#2296](https://github.com/avatarsd-llc/libtracer/issues/2296)) replays it.
