# Arduino integration

Arduino library metadata for libtracer. **Not distributed via the Arduino Library
Manager** — see [Status](#status) below.

## Use

**Not available through the Library Manager** — libtracer has never been submitted
to the registry and searching for it there finds nothing. Use the
[PlatformIO package](../platformio/) or the
[ESP-IDF managed component](../esp-idf/) instead.

The `library.properties` here describes the intended Arduino packaging, but the
layout does not satisfy the registry either: Arduino requires `library.properties`
in the **repository root**, and this one lives in `integrations/arduino/`.

## Conformance profile

**P0 — in-process** + **`transport_uart`** (the Arduino `Serial` link maps to the
`transport_uart` module; "serial" is the board-facing name for UART). See the
[conformance profiles](../../docs/reference/00-overview.md#conformance-profiles) and the
[module catalog](../../docs/reference/10-module-catalog.md).

## Default modules

Required (P0) modules + `transport_uart`. No discovery, no executor — a single-link leaf
(e.g. an RC car or a sensor over USB-CDC).

## Security posture

**Unsafe by default in v0.1** — no `security_*` modules exist yet (`security_psk` for UART
is post-MVP per the module catalog). The serial link is unauthenticated and unencrypted;
use only on a physically trusted bus until `security_psk` lands.

## Files

- `library.properties` — Arduino library metadata.

## Status

**Not planned.** The Arduino Library Manager path is not on the roadmap — the
Arduino IDE toolchain and the AVR/no-STL targets it centers on are a poor fit for
the C++23 core. Use the **PlatformIO** package (which ships and compiles the core)
or the **ESP-IDF** managed component for embedded builds instead.
