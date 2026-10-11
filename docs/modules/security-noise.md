# security & Noise — the Noise link and its crypto backend

```{admonition} In one paragraph
:class: tip
The Noise link binding ([RFC-0033](../spec/rfcs/0033-noise-nnpsk0-datagram-link-binding.md))
carries frames inside a `Noise_NNpsk0_25519_ChaChaPoly_SHA256` session. This module is its
cryptographic half, the handshake and transport steps written once over a **crypto backend
chosen at compile time** (OpenSSL, libsodium or mbedTLS through PSA), and the link built on
them, `noise_link_t`, which carries one request and its reply in two round trips. The core stays
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

## The link

`transport_noise.hpp` is the link half, built on these types: `noise_link_t<B, Host, Carrier>`
is an ordinary `transport_t` to the router, and the frames it carries are byte-identical to the
ones its carrier would carry without it (reference/23 §1). It owns its carrier, a
`udp_transport_t` on a host, and takes three compile-time choices:

| parameter | what | |
| --- | --- | --- |
| `B` | the crypto backend | `default_crypto_t` |
| `Host` | the application's seams (`noise::link_host`) | `resolve_psk(ref, key)`, the key store; `ephemeral(key)`, a fresh random private key per call; `next_counter()`, strictly increasing across attempts |
| `Carrier` | the datagram link underneath (`noise::datagram_carrier`) | `send_to(address, iov)`, `inbound_source()`, `peer_address()` |

The PSK is named by reference (`noise_config_t::psk_ref`) and resolved once, when the link is
built. The library holds no key, no entropy and no clock.

**One request and its reply.** DIAL initiates and LISTEN answers. A send on a link with no
session fails at once, counted in `dropped_tx`, and on DIAL it starts a handshake (`dial()`
demands without a frame). Once the second message arrives, `poll` reports `session`: the
request sent then is the confirmation, the responder's reply is the acknowledgement, and the
link reports `UP` after it. Four datagrams, two round trips:

```cpp
link.dial();                                  // or the first send, which fails and dials
while (!link.poll(app_now()).session) wait(); // the app's clock, the app's wait
link.send(request);                           // the confirmation
// ... the reply arrives on the receiver: the acknowledgement; link.state() == UP
```

**Time.** `poll(now)` runs every deadline `now` has passed and returns the next one
(`noise_poll_t::next`): the initiator's retry (the stored first message, or an empty
confirmation while unacknowledged, the interval doubling to `retry_cap`) and attempt deadlines,
and the responder's pending expiry. A deadline of 0 is never due, and deadlines count from the
last `now` the link was given, so an application polls once before its first demand.

**Memory.** Everything the link holds about its sessions (the PSK state, the handshake cipher,
the two session slots, the handshake slot and its stored messages) is one block from the
link's `memory.state`, drawn when the link is built. On x86-64 it is 504 B with PSA Crypto,
520 B with OpenSSL 3 (where each cipher is a pointer to a library context the backend builds
with it) and 664 B with libsodium. A handshake fills slots that already
exist, so a first message under a wrong PSK, or a replayed one, costs no memory. A transport
message is decrypted in place in the carrier's receive buffer, and a send seals into one block
of `frame + 25` bytes from `memory.io`, returned after the send.

**Counters.** Reference/23 §11's ten `noise_*` nouns follow the universal three in the link's
`:stats.link.<child>` block (`transport_t::kind_stats`). This slice keeps no previous session,
so a message in a replaced session's phase is `noise_stale`. Rekey and the generation rule are
[#2074](https://github.com/avatarsd-llc/libtracer/issues/2074), persisting the counter and the
mark is [#2075](https://github.com/avatarsd-llc/libtracer/issues/2075), and ESP-IDF is
[#2076](https://github.com/avatarsd-llc/libtracer/issues/2076).

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
| `psa` | `psa_crypto_t` | mbedTLS 3.6.1+ or 4.x through PSA | the ESP-IDF backend; needs a sized static key-slot table (below) |

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
  `security_noise_psa.hpp` refuses to build without it.
- A slot table sized by the app, because every PSA user in the image shares it. The rule:

  | setting | at least |
  | --- | --- |
  | `MBEDTLS_PSA_KEY_SLOT_COUNT` | 6 per Noise link (two sessions of two keys during a rekey, the handshake cipher, the ephemeral key), plus the peak of the image's other PSA users |
  | `MBEDTLS_PSA_STATIC_KEY_SLOT_BUFFER_SIZE` | the largest key any of them imports; Noise's keys are all 32 B |

  A Noise-only image needs 8 slots of 32 B: 360 B of `.bss` on an ESP32-C6, and the
  configuration the CI psa leg tests (8 is the fewest the two suites pass with, holding both
  ends of a session in one process). Rough figures for a mixed image at 16 slots: about 2 KB
  with TLS that verifies only ECDSA P-256 or X25519 peers, about 5 KB with RSA-2048 server
  certificates, about 20 KB if the image holds its own RSA-2048 private key. Left unsized,
  mbedTLS sizes 32 slots for the largest key type enabled: 76,616 B at ESP-IDF's defaults,
  which enable RSA-4096 key pairs. That figure is the unsized default, not the cost of Noise.
  A table that is too small fails at run time, when an import is refused, not at build.
  ESP-IDF has no option for any of this: the Kconfig help of
  `CONFIG_LIBTRACER_NOISE_CRYPTO_PSA` shows the header an app passes as
  `MBEDTLS_USER_CONFIG_FILE`.
- **A NARROW image that needs PSA for nothing else should use libsodium** (ESP-IDF's
  component registry has a port). It allocates nothing, has no global table, and needs no
  mbedTLS change. The backend is a compile-time choice, so the swap is one define.
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
| handshake, initiator | 34 | 0 | 6200 | 6244 |
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

```{doxygenclass} tr::net::noise_link_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::noise_config_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::noise_poll_t
:project: libtracer
:members:
```

```{doxygenconcept} tr::net::noise::link_host
:project: libtracer
```

```{doxygenconcept} tr::net::noise::datagram_carrier
:project: libtracer
```

```{doxygenenum} tr::net::noise::counter_t
:project: libtracer
```

```{doxygenstruct} tr::net::noise::session_t
:project: libtracer
:members:
```
