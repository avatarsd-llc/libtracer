# security & Noise — the Noise link's crypto backend

```{admonition} In one paragraph
:class: tip
The Noise link binding ([RFC-0033](../spec/rfcs/0033-noise-nnpsk0-datagram-link-binding.md))
carries frames inside a `Noise_NNpsk0_25519_ChaChaPoly_SHA256` session. This module is its
cryptographic half: the handshake and transport steps, written once over a **crypto backend
chosen at compile time** (OpenSSL, libsodium or mbedTLS through PSA). The core stays
crypto-free. A build that does not name a backend compiles none of this and links no crypto
library. Everything works on caller memory: no allocation of its own, no clock and no
randomness. On every backend, nothing allocates before a first message's PSK tag is proven,
and nothing allocates per frame.
```

## What it does

`security_noise.hpp` holds the protocol, written once over a backend type `B`:

- `symmetric_state_t<B>` is Noise's SymmetricState (`ck`, `h` and the handshake cipher key),
  a plain value that copies. `psk_state` builds the state after the prologue and the `psk`
  token. That state is the same for every handshake on a link, so a link computes it once
  and starts each handshake from a copy (RFC-0033 §5.8).
- `handshake_t<B>` is one side of the handshake over RFC-0033's datagrams: `write_first` and
  `read_second` for the initiator, `read_first` and `write_second` for the responder, then
  `split`. `read_first` checks the PSK tag with no Diffie-Hellman and no key generation. The
  caller checks the counter against its mark, and only `write_second` runs `ee`. So a
  message under a wrong PSK, or a replayed one, costs one `MixHash`, one HKDF and one tag
  check, and no memory. The tag is checked with the link's handshake cipher, a backend cipher
  the link builds once beside its PSK state and passes to each `handshake_t`, which re-keys
  it per message. An all-zero X25519 result is refused (`LOW_ORDER`) whatever the backend
  does.
- `transport_cipher_t<B>` holds a session's two transport keys, keyed into the backend once
  at `split`. `seal` writes the type byte, the nonce, the ciphertext and the tag, in place
  when the frame already sits at its payload offset. `parse_transport_header` reads the type
  and the nonce before any decryption, which is where the replay pre-check goes, and `open`
  then decrypts in place. Nonces at or above 2^62 are refused on both sides. A cipher takes
  one call at a time: the link serialises each direction's senders, or keeps one session
  cipher per sending thread.

Every refusal is a `refusal_t`, which the link maps onto RFC-0033 §5.9's counters.

The link half (the slots, the counter and the mark, the replay window, the key phase, the
`now` deadlines and the counters) is the next slice of
[#2064](https://github.com/avatarsd-llc/libtracer/issues/2064), built on these types.

## Choosing a backend

One CMake cache variable, `LIBTRACER_NOISE_CRYPTO`, picks the backend. Any value but `none`
makes the header-only target `libtracer_noise`, which carries the library and a
`LIBTRACER_NOISE_CRYPTO_<BACKEND>` define. `security_noise.hpp` turns that define into the
alias `tr::net::noise::default_crypto_t`. There is no runtime switch.

| value | backend | library | notes |
| --- | --- | --- | --- |
| `none` (default) | — | — | the module is not compiled |
| `openssl` | `openssl_crypto_t` | libcrypto 3 | the fastest AEAD on a host at 1 KiB and above; needs the deprecated `SHA256_*` calls (not a `no-deprecated` build) |
| `sodium` | `sodium_crypto_t` | libsodium | no allocation at all, handshake included |
| `psa` | `psa_crypto_t` | mbedTLS 3.6 or 4.x through PSA | the ESP-IDF backend |

A backend is a type that meets the `crypto_backend` concept (`security_noise_crypto.hpp`):
SHA-256, Noise's HKDF, an X25519 key pair and a ChaCha20-Poly1305 cipher. `hash`, `hkdf` and
the cipher's `set_key`, `seal` and `open` must not allocate; building a cipher and the X25519
calls may. A test can wrap a backend in another compile-time policy, as `security_noise_test`
does to count the X25519 calls a refused first message makes (none).

The PSA backend needs three things from the mbedTLS build:

- `MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS`, or every PSA call copies its buffers through the
  heap. ESP-IDF's port sets it.
- `MBEDTLS_PSA_STATIC_KEY_SLOTS` without `MBEDTLS_PSA_KEY_STORE_DYNAMIC` (mbedTLS 3.6.1 and
  later), or every key import allocates, a first message's handshake key included.
  `security_noise_psa.hpp` refuses to build without it. ESP-IDF has no option for it: the
  app passes both lines in an `MBEDTLS_USER_CONFIG_FILE`, as the Kconfig help of
  `CONFIG_LIBTRACER_NOISE_CRYPTO_PSA` shows. It costs
  `MBEDTLS_PSA_KEY_SLOT_COUNT * MBEDTLS_PSA_STATIC_KEY_SLOT_BUFFER_SIZE` of static RAM:
  76,616 B of `.bss` at ESP-IDF's defaults on an ESP32-C6, where the slots are sized for an
  RSA key pair. The same header can lower either value.
- On ESP-IDF, `CONFIG_MBEDTLS_CHACHA20_C` and `CONFIG_MBEDTLS_CHACHAPOLY_C`, which are off by
  default. The Kconfig option selects them.

## What it costs

Allocation calls, counted process-wide (`malloc` and its family) by `security_noise_test`
on the host (GCC 13, glibc). The library's own allocations are included. The zero rows are
asserted.

| | openssl (3.0.13) | sodium (1.0.18) | psa (mbedTLS 4.1.1) | psa (mbedTLS 3.6.5) |
| --- | --- | --- | --- | --- |
| per link: PSK state and handshake cipher | 3 | 0 | 0 | 0 |
| per session: the two transport ciphers | 6 | 0 | 0 | 0 |
| handshake, initiator | 34 | 0 | 6200 | 6245 |
| handshake, responder | 34 | 0 | 6200 | 6244 |
| a first message read, refused (wrong PSK) or accepted (a replay) | 0 | 0 | 0 | 0 |
| per frame (seal + open), 0 B to 65519 B | 0 | 0 | 0 | 0 |

The handshake rows are the X25519 calls: `EVP_PKEY` objects on OpenSSL, and mbedTLS's bignum
arithmetic on PSA, paid once per session and only after the PSK is proven. The #2065
harness measures the time of each step on each backend.

## Conformance

`conformance_runner` replays every `tests/conformance/vectors/v1/noise/<case>/transcript.json`
on the build's backend and compares each recorded value: the symmetric state after every
token, both handshake messages, the handshake hash, the transport keys and the transport
datagrams. The first case is RFC-0033 Appendix A. Two third-party Noise implementations
reproduce it too, as the RFC's §15 Q6 ruling requires: `noiseprotocol` (Python) and `snow`
(Rust, over its own primitives). Their scripts and results sit next to the transcript.

## API reference

```{doxygenconcept} tr::net::noise::crypto_backend
:project: libtracer
```

```{doxygenclass} tr::net::noise::symmetric_state_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::net::noise::psk_state
:project: libtracer
```

```{doxygenclass} tr::net::noise::handshake_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::noise::transport_cipher_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::net::noise::parse_transport_header
:project: libtracer
```

```{doxygenenum} tr::net::noise::refusal_t
:project: libtracer
```

```{doxygenenum} tr::net::noise::msg_type_t
:project: libtracer
```

```{doxygenvariable} tr::net::noise::kFirstMessageBytes
:project: libtracer
```

```{doxygenvariable} tr::net::noise::kSecondMessageBytes
:project: libtracer
```

```{doxygenvariable} tr::net::noise::kTransportHeaderBytes
:project: libtracer
```

```{doxygenvariable} tr::net::noise::kTransportOverhead
:project: libtracer
```

```{doxygenvariable} tr::net::noise::kMaxFrameBytes
:project: libtracer
```

```{doxygenvariable} tr::net::noise::kNonceLimit
:project: libtracer
```

```{doxygenstruct} tr::net::noise::first_payload_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::noise::transport_header_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::noise::openssl_crypto_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::noise::sodium_crypto_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::noise::psa_crypto_t
:project: libtracer
:members:
```
