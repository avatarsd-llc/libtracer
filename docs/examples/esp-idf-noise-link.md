# ESP-IDF: a request and its reply over Noise

A Noise link ([RFC-0033](../spec/rfcs/0033-noise-nnpsk0-datagram-link-binding.md)) carries
frames inside a `Noise_NNpsk0_25519_ChaChaPoly_SHA256` session over UDP. This example builds two
of them in one image, a DIAL link and a LISTEN link talking over `127.0.0.1`, and sends one
request through them. The LISTEN side echoes it as the reply. The request is the session's
confirmation and the reply its acknowledgement, so the initiator reports `UP` after two round
trips.

## What to notice

- **One Kconfig option turns it on.** `CONFIG_LIBTRACER_NOISE_CRYPTO_PSA=y` in `sdkconfig.defaults`
  makes the component define the PSA backend and selects the mbedTLS ciphers ESP-IDF leaves off.
  The module is header-only, so an image without the option compiles none of it.
- **The application sizes mbedTLS's key-slot table.** `main/psa_slots.h` is passed as
  `MBEDTLS_USER_CONFIG_FILE` from the project `CMakeLists.txt`. The rule is 6 slots per Noise
  link, so these two links need 12, each 32 B. Without static slots `security_noise_psa.hpp`
  refuses to build.
- **The library holds no key, no entropy and no clock.** The host type gives it the key store
  (`resolve_psk`), the random source (`ephemeral`, PSA's, backed by the hardware RNG) and the
  initiator counter (`next_counter`, a `noise::boot_counter_t`). The clock reaches the links as
  the `now` passed to `sweep`. A real node keeps its PSK in its own secure storage and persists
  the boot number once per boot before its first attempt.
- **The session state is one block.** Each link draws one block from its `memory.state`, the net
  sub-pool by default. The example prints its size next to the link object's.
- **lwIP serves `127.0.0.1` only with its loopback interface**, so `sdkconfig.defaults` sets
  `CONFIG_LWIP_NETIF_LOOPBACK=y`.
- **Board-only, named and not run:** to reach a host or a second board instead, give the DIAL link
  that peer's address and run a LISTEN link with the same PSK there, once the board has joined a
  network (Wi-Fi or Ethernet bring-up, which is the application's).

## What it costs on an ESP32-C6

Measured with ESP-IDF 6.0 (mbedTLS 4) and GCC 15.2 at `-Os`. The flash rows compare this image
with the same two-link echo over plain UDP, with no Noise link and no PSA.

| | bytes |
| --- | --- |
| per link: the `noise_link_t` object | 632 |
| per link: the session-state block (PSK state, handshake cipher, both session slots of 48 B each, the handshake slot and its stored messages) | 496 |
| per image: mbedTLS's PSA key-slot table at 12 slots of 32 B (`.bss`) | 820 |
| per image: other static RAM (`.data`, `.bss`) | 396 |
| flash: mbedTLS's PSA crypto (X25519, ChaCha20-Poly1305, SHA-256, HKDF) | 100,291 |
| flash: what else the image grows by in ESP-IDF's components (NVS, which PSA's key storage uses, the SHA and DMA drivers, eFuse, stdio) | 21,917 |
| flash: the link code, the host type and the example app | 11,318 |

The UDP carrier under each link costs what the [one-link example](esp-idf-one-link.md) shows.
Handshake time on silicon is not measured yet: no board was attached.

## Source

```{literalinclude} /integrations/esp-idf/examples/concepts/noise_link/main/app_main.cpp
:language: cpp
:linenos:
```

## Build and run

```console
$ cd integrations/esp-idf/examples/concepts/noise_link
$ idf.py set-target esp32c6 build
$ idf.py flash monitor        # board-only
```

CI builds it for `esp32c6`, and also builds and runs it on the ESP-IDF `linux` target, where the
two links bind host UDP ports. See also: [the Noise module](../modules/security-noise.md) ·
[one link, and its `max_frame`](esp-idf-one-link.md).
