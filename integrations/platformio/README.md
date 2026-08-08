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

## Files

- `library.json` (repo root) — PlatformIO manifest (points at `core/include/` and
  `core/src/`, and lists what `pio pkg publish` ships in `export.include`).
- `pio_esp32_can.py` — the `build.extraScript` hook described above.

## Releasing

`pio pkg publish` is run from CI on tag `v*` (see `.github/workflows/publish-pio.yml` once added).
