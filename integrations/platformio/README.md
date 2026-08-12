# PlatformIO integration

Packages the libtracer reference implementation as a PlatformIO library.

## Use

In your `platformio.ini`:

```ini
lib_deps =
    libtracer
```

Or pin to a git revision while pre-1.0:

```ini
lib_deps =
    https://github.com/avatarsd-llc/libtracer.git
```

## Conformance profile

**P1 — single-transport leaf** (the generic embedded profile). The concrete transport is
chosen per board (`transport_uart`, `transport_tcp`, `transport_can`, …); PlatformIO ships
the core + lets the project select its transport module. See the
[conformance profiles](../../docs/reference/00-overview.md#conformance-profiles) and the
[module catalog](../../docs/reference/10-module-catalog.md).

## Default modules

Required modules + one project-selected transport. No discovery/executor/security by
default — add them explicitly per deployment.

## Security posture

**Unsafe by default in v0.1** — no `security_*` modules exist yet (TLS/DTLS/PSK/ACL
enforcement are post-MVP). Run on a trusted link until the matching `security_*` module
for your transport lands.

## ESP32 CAN (TWAI)

On an `espressif32` target the build hook below also compiles the ESP-IDF TWAI
`can_link_t` and exposes its header, so `transport_can` gets a real on-chip CAN 2.0
bus. Construct a `tr::net::twai_link_t{{tx_gpio, rx_gpio, bitrate}}`, hand it to a
**CLASSIC** `transport_can` (TWAI is classic-only, no CAN-FD), and register
`can_transport_factory()`. On every other platform the hook is a no-op.

This glue is gated at **compile + link** by the `pio-esp32-can` workflow, which packs
the package and `pio run`s the `framework = espidf` consumer in
[`tests/packaging/pio_esp32_can/`](../../tests/packaging/pio_esp32_can/). Moving frames
on a real bus is a separate, still-open sign-off — the job energises no pin.

## No WebSocket on `espressif32`

**A PlatformIO `espressif32` build ships no WebSocket transport at all** (#984, applying
the #947 maintainer ruling: *ESP-IDF WebSocket must never use POSIX sockets*).

- The portable `transport_ws_server` / `transport_ws_client` pair is **excluded
  per-environment** by the build hook: on lwIP its scatter-gather egress is rejected
  (`lwip_sendmsg` returns `EOPNOTSUPP` for `MSG_NOSIGNAL`), so every data frame was
  silently dropped while the handshake and PING/PONG still worked (#948) — it was a
  working-looking transport that never delivered data on silicon. The hook also swaps
  core's full-node `register_builtin_transports` (udp+tcp+ws) for the udp+tcp-only
  dispatcher in `builtin_transports_udp_tcp.cpp`, so no dangling reference drags the
  pair back in. TU selection, no feature macros.
- The IDF-native replacements (`httpd_ws_link_t` / `esp_ws_client_link_t`) are **not
  packaged for PlatformIO**: they depend on `esp_http_server` / `esp_websocket_client`,
  and the latter is an IDF managed component whose availability under PlatformIO's
  `framework-espidf` is not established. Packaging them is the sanctioned follow-up on
  #984 once that dependency verifies — and it must update the CI gate alongside.

Need WS on an ESP32 today? Use the [ESP-IDF component](../esp-idf/) packaging, which
ships the native links. Every other transport this package compiles (udp, tcp, can)
is unaffected, on every platform; non-`espressif32` platforms keep the portable WS
pair.

The state is CI-gated on the linked fixture image by the same `pio-esp32-can` workflow:
`tools/check_esp_ws_plane.py --ws-plane none` asserts zero portable-WS **and** zero
native-link symbols via `nm` (with a symbol-table floor so a stripped ELF cannot pass
vacuously), plus the object-tree check.

## Files

- `library.json` (repo root) — PlatformIO manifest (points at `core/include/` and
  `core/src/`, and lists what `pio pkg publish` ships in `export.include`).
- `pio_esp32_can.py` — the `build.extraScript` hook described above. It also owns the
  package's **source filter** (a manifest `build.srcFilter` would take precedence over
  the script's and cannot express the per-environment `espressif32` WS exclusion).
- `builtin_transports_udp_tcp.cpp` — the udp+tcp `register_builtin_transports`
  dispatcher the hook compiles on `espressif32` in place of core's full-node one.

## Releasing

`pio pkg publish` is run from CI on tag `v*` (see `.github/workflows/publish-pio.yml` once added).
