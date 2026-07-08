# integrations — Platform and framework wrappers

Each subfolder packages the libtracer reference implementation for a specific platform or framework. They are thin: they configure build files, register the library with the host's package format, and provide a minimal example.

| Integration | Distribution | Status |
|-------------|--------------|--------|
| [esp-idf/](esp-idf/) | ESP Component Registry (`idf_component.yml`) | managed component; P0 in-process profile built in CI for esp32c6 + esp32c3 (`.github/workflows/esp-idf.yml`) via the [`inprocess_mirror`](esp-idf/examples/inprocess_mirror/) example |
| [platformio/](platformio/) | `registry.platformio.org` (`pio pkg publish`) | library ships + compiles the default core source set (codec + graph + tcp/udp/ws/can; CUDA/QUIC/WebTransport are opt-in and filtered out). On esp32 a `build.extraScript` hook compiles the ESP-IDF TWAI CAN driver so `transport_can` has a real bus — **best-effort, not yet board/CI-verified** |
| [esphome/](esphome/) | `external_components:` git URL — no central registry | stub |
| [arduino/](arduino/) | Arduino Library Manager | not planned |

## Bridges

A *bridge* is an integration that translates a foreign protocol (Modbus, Z-Wave, vendor-X RPC, …) into libtracer. Bridges may live in-tree under `integrations/bridges/<protocol>/` or out-of-tree as separate repos. See [CONTRIBUTING.md](../.github/CONTRIBUTING.md#bridges-smart-device-adapters-for-incompatible-protocols).
