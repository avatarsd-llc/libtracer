# noise/nnpsk0-appendix-a

The `Noise_NNpsk0_25519_ChaChaPoly_SHA256` transcript of
[RFC-0033](../../../../../../docs/spec/rfcs/0033-noise-nnpsk0-datagram-link-binding.md)
Appendix A, as a machine-readable case (RFC-0033 §12.2). It is a transcript, not a frame: the
case has no `input.bin`, so the codec harnesses and the cross-core `--tap` matrix do not walk
it.

`transcript.json` holds:

- **inputs:** the prologue, the PSK, both ephemeral private keys, both handshake payloads
  (msg1: flags `01`, counter 4294967297 = boot 1, attempt 1, as u64 LE), the two frames, and
  the nonce each transport datagram is sealed at;
- **derived:** both ephemeral public keys and `DH(e_i, e_r)`;
- **steps:** `ck`, `h` and `k` after each token (Appendix A.3);
- **outputs:** both handshake datagrams (58 B and 50 B, type byte first), the handshake hash,
  the two transport keys after `Split()`, and the three transport datagrams: the empty
  confirmation (initiator, n = 0, 25 B), `VALUE "ping"` (initiator, n = 1, 33 B) and
  `VALUE "pang"` (responder, n = 0, 33 B).

Every value is copied from the RFC's text, and every value is reproduced by:

- `conformance_runner`, on the build's Noise crypto backend (`-DLIBTRACER_NOISE_CRYPTO=openssl`,
  `sodium` or `psa`), token by token on `symmetric_state_t` and datagram by datagram through
  both `handshake_t` sides and `transport_cipher_t`;
- `independent_check_noiseprotocol.py`, with the third-party Python `noiseprotocol` library,
  every value including the per-token states (result in
  `independent_check_noiseprotocol.txt`);
- `independent_check_snow/`, with the third-party Rust `snow` crate over its own primitives
  (curve25519-dalek, RustCrypto ChaCha20-Poly1305 and SHA-256), every value but the per-token
  states, which snow does not expose (result in `independent_check_snow.txt`).
