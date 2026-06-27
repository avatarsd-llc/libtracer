# ESP-IDF integration

Packages the libtracer C++ reference core as an [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) component, so an ESP-IDF project can depend on libtracer through the component manager.

## Use

### As a local component (vendored)

Add this directory as a component in your project — point `EXTRA_COMPONENT_DIRS` at it, or symlink/copy it under your project's `components/`:

```cmake
# top-level CMakeLists.txt
set(EXTRA_COMPONENT_DIRS "path/to/libtracer/integrations/esp-idf")
```

The component's `CMakeLists.txt` references the core sources in `../../core/`, so it must sit inside a checkout of the libtracer repo (it is the in-tree component, not a flattened copy).

### Via the component manager (planned)

Once published to the [ESP Component Registry](https://components.espressif.com/), add to `idf_component.yml`:

```yaml
dependencies:
  avatarsd-llc/libtracer: "^0.0.1"
```

## Conformance profile

**P0 — in-process** ([conformance profiles](../../docs/reference/00-overview.md#conformance-profiles)). This component packages the wire codec (L2/L3), the L0/L1 substrate, and the L4 graph runtime — `read`/`write`/`await` + field-write, all in-process, zero transports. This is exactly what an **in-process mirror** needs (e.g. the strawberry-fw migration Phase 1).

Transport modules — `transport_ws` ([#54](https://github.com/avatarsd-llc/libtracer/issues/54)), `transport_can` ([#55](https://github.com/avatarsd-llc/libtracer/issues/55)) — are added in later phases and bring their own ESP-IDF requirements (`lwip`, etc.); they are intentionally **not** in the P0 component.

## Requirements

- **ESP-IDF ≥ 5.3** — libtracer's core is **C++23** (`std::expected`, `std::span`), which needs the GCC 13 toolchain shipped with ESP-IDF 5.3+.
- `REQUIRES pthread` — the L4 graph uses `std::shared_mutex` / `std::mutex` (ESP-IDF's pthread support).

## Security posture

**Unsafe by default in v0.1** — no `security_*` modules exist yet (TLS/DTLS/PSK/ACL enforcement are post-MVP). The P0 profile has no transport at all, so there is no network exposure from this component on its own; add the matching `security_*` module when you add a transport.

## Status

**Component packaging is written but not yet built on-target.** There is no ESP-IDF toolchain in this repo's CI, so the CMake/manifest here are authored to ESP-IDF conventions but have **not** been compiled for an ESP32/-C6 yet. First on-silicon validation is expected via strawberry-fw (its migration Phase 1 smoke test on ESP32-C6) — see [#64](https://github.com/avatarsd-llc/libtracer/issues/64). Report build issues there.

## Files

- `CMakeLists.txt` — the ESP-IDF component definition (`idf_component_register`).
- `idf_component.yml` — component manifest for the ESP Component Registry.
