# noise/ws-preproof-bound

A WebSocket-carrier case of [reference/23](../../../../../../docs/reference/23-noise-link-binding.md)
§13.12 (RFC-0033 Amendment 2, A2.12). Before proof, a 59-byte message is refused at byte 59, whole or fragmented.

Before any proof, a 59-byte message (a first message plus one byte), whole and then split into 30 and 29 bytes, each variant on a fresh connection. Expected: refused at byte 59 and counted in `malformed_rx`; the valid first message that follows is answered, so the refusal did not close the connection.

Every Noise byte is from [`nnpsk0-appendix-a`](../nnpsk0-appendix-a/), so that case's third-party
checks gate them. `ws.json` is written by [`gen_ws_vectors.py`](../gen_ws_vectors.py), which adds
only RFC 6455 framing; `--check` fails if the file differs. Every frame is also parsed by the
third-party `websockets` library ([`independent_check_websockets.py`](../independent_check_websockets.py),
result in `independent_check_websockets.txt`). The case has no `transcript.json` and no `input.bin`,
so neither the codec harnesses nor the Noise transcript replay walk it; the WebSocket carrier's
implementation ([#2296](https://github.com/avatarsd-llc/libtracer/issues/2296)) replays it.
