# integrations — Platform and framework wrappers

Each subfolder packages the libtracer reference implementation for a specific platform or framework. They are thin: they configure build files, register the library with the host's package format, and provide a minimal example.

| Integration | Distribution | Status |
|-------------|--------------|--------|
| [platformio/](platformio/) | `registry.platformio.org` (`pio pkg publish`) | stub |
| [esphome/](esphome/) | `external_components:` git URL — no central registry | stub |
| [arduino/](arduino/) | Arduino Library Manager | stub |

## Bridges

A *bridge* is an integration that translates a foreign protocol (Modbus, Z-Wave, vendor-X RPC, …) into libtracer. Bridges may live in-tree under `integrations/bridges/<protocol>/` or out-of-tree as separate repos. See [CONTRIBUTING.md](../CONTRIBUTING.md#bridges-smart-device-adapters-for-incompatible-protocols).
