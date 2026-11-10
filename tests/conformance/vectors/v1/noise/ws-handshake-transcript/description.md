# noise/ws-handshake-transcript

A WebSocket-carrier case of [reference/23](../../../../../../docs/reference/23-noise-link-binding.md)
§13.12 (RFC-0033 Amendment 2, A2.12). Appendix A over WebSocket: each datagram is one binary frame, client frames masked.

Appendix A's six datagrams, each as one binary frame: the client's first message (64 B, `82 BA` then its mask), the server's second message (52 B, `82 32`), the empty confirmation and `VALUE "ping"` from the client, then the empty acknowledgement and `VALUE "pang"` from the server. Client frame *k* is masked with `a<k> b<k> c<k> d<k>`. Expected: every frame byte for byte, and Appendix A's handshake hash and transport keys.

Every Noise byte is from [`nnpsk0-appendix-a`](../nnpsk0-appendix-a/), so that case's third-party
checks gate them. `ws.json` is written by [`gen_ws_vectors.py`](../gen_ws_vectors.py), which adds
only RFC 6455 framing; `--check` fails if the file differs. Every frame is also parsed by the
third-party `websockets` library ([`independent_check_websockets.py`](../independent_check_websockets.py),
result in `independent_check_websockets.txt`). The case has no `transcript.json` and no `input.bin`,
so neither the codec harnesses nor the Noise transcript replay walk it; the WebSocket carrier's
implementation ([#2296](https://github.com/avatarsd-llc/libtracer/issues/2296)) replays it.
