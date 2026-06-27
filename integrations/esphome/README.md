# ESPHome integration

Packages libtracer as an ESPHome [external component](https://esphome.io/components/external_components.html). ESPHome has no central registry — users reference the component by git URL, so the repo is the distribution channel.

## Use

In your ESPHome YAML:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/avatarsd-llc/libtracer
      ref: main
    components: [libtracer]

libtracer:
  # component config goes here
```

## Conformance profile

**P1 — single-transport leaf** + **`discovery_mdns`**. The ESP32 Wi-Fi link uses
**`transport_tcp`** (there is no separate `transport_wifi` module — Wi-Fi is the physical
layer beneath TCP; see the [module catalog](../../docs/reference/10-module-catalog.md) and
the ESP32-over-Wi-Fi row in [reference 12](../../docs/reference/12-deployment-profiles.md)).
See the [conformance profiles](../../docs/reference/00-overview.md#conformance-profiles).

## Default modules

Required modules + `transport_tcp` (over Wi-Fi) + `discovery_mdns` (so a board is found on
the LAN without static config).

## Security posture

**Unsafe by default in v0.1** — no `security_*` modules exist yet. **TLS is not available**:
`security_tls` (which pairs with `transport_tcp`/`transport_ws`) is post-MVP per the module
catalog. **Do not expose a libtracer ESPHome node to an untrusted network** until
`security_tls` lands; keep it on a trusted VLAN or tunnel it.

## Files

- `components/libtracer/` — the ESPHome component (Python `__init__.py`, C++ glue).

## Status

Stub. The component is not yet implemented.
