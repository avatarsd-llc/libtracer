# ESP-IDF integration

Packages the libtracer C++ reference core as an [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) component, so an ESP-IDF project can depend on libtracer through the component manager.

## Use

### As a local component (vendored)

The component is the `libtracer/` subfolder (its folder basename is the ESP-IDF component name, so dependents `REQUIRES libtracer`). Point `EXTRA_COMPONENT_DIRS` straight at it, or symlink/copy it under your project's `components/`:

```cmake
# top-level CMakeLists.txt
set(EXTRA_COMPONENT_DIRS "path/to/libtracer/integrations/esp-idf/libtracer")
```

The component's `CMakeLists.txt` references the core sources in `../../../core/`, so it must sit inside a checkout of the libtracer repo (it is the in-tree component, used in place, not a flattened copy). The bundled [`examples/inprocess_mirror/`](examples/inprocess_mirror/) wires it exactly this way.

### Via the component manager (planned)

Once published to the [ESP Component Registry](https://components.espressif.com/), add to `idf_component.yml`:

```yaml
dependencies:
  avatarsd-llc/libtracer: "^0.0.1"
```

## Conformance profile

**P0 — in-process** ([conformance profiles](../../docs/reference/00-overview.md#conformance-profiles)). This component packages the wire codec (L2/L3), the L0/L1 substrate, and the L4 graph runtime — `read`/`write`/`await` + field-write, all in-process, zero transports. This is exactly what an **in-process mirror** needs (e.g. the strawberry-fw migration Phase 1).

Transport modules — `transport_ws` ([#54](https://github.com/avatarsd-llc/libtracer/issues/54)), `transport_can` ([#55](https://github.com/avatarsd-llc/libtracer/issues/55)) — are added in later phases and bring their own ESP-IDF requirements (`lwip`, etc.); they are intentionally **not** in the P0 component.

## Example

[`examples/inprocess_mirror/`](examples/inprocess_mirror/) is a tiny but real ESP-IDF app that depends on this component and drives the in-process mirror surface end to end — register a path, `write` a value, `read` it back, and `await` the next write — on single-core FreeRTOS. It is what the CI gate builds. To build it yourself:

```bash
cd integrations/esp-idf/examples/inprocess_mirror
idf.py set-target esp32c6
idf.py build      # produces build/inprocess_mirror.{elf,bin}
```

Or in the official Docker image, no local toolchain needed:

```bash
docker run --rm -v "$PWD:/p" -w /p/integrations/esp-idf/examples/inprocess_mirror \
  espressif/idf:release-v5.3 bash -c "idf.py set-target esp32c6 build"
```

## Requirements

- **ESP-IDF ≥ 5.3** — libtracer's core is **C++23** (`std::expected`, `std::span`), which needs the GCC 13 toolchain shipped with ESP-IDF 5.3+.
- `PRIV_REQUIRES pthread` — a **private** link dependency: libtracer's public headers expose only libstdc++ headers (`<atomic>`, `<mutex>`, `<shared_mutex>`, `<thread>`), never `<pthread.h>`, so pthread is not propagated to dependents. The pthread symbols are pulled in by libstdc++'s gthread threading, which the core's `.cpp` objects need at link time. (Earlier revisions used `REQUIRES pthread`, which over-declared it as a public dependency.)
- **Exceptions / RTTI** stay at the ESP-IDF default (**OFF**): the core's data path is exception-free and RTTI-free (it returns `std::expected`, never throws; no `typeid`/`dynamic_cast`), so the P0 profile links clean under `-fno-exceptions -fno-rtti`.

## Security posture

**Unsafe by default in v0.1** — no `security_*` modules exist yet (TLS/DTLS/PSK/ACL enforcement are post-MVP). The P0 profile has no transport at all, so there is no network exposure from this component on its own; add the matching `security_*` module when you add a transport.

## Status

**Built in CI.** The `inprocess_mirror` example compiles and links the P0 in-process core as a managed component in the `espressif/idf:release-v5.3` image, for **esp32c6** (the required single-core RISC-V target) and **esp32c3** — see [`.github/workflows/esp-idf.yml`](../../.github/workflows/esp-idf.yml) and [#64](https://github.com/avatarsd-llc/libtracer/issues/64). A clean `idf.py build` (a produced `.elf`/`.bin`) is the gate. Transports (`transport_ws` #54, `transport_can` #55) are still later-phase and not in this component. Report build issues on [#64](https://github.com/avatarsd-llc/libtracer/issues/64).

## Files

- `libtracer/CMakeLists.txt` — the ESP-IDF component definition (`idf_component_register`); its folder basename is the component name.
- `libtracer/idf_component.yml` — component manifest for the ESP Component Registry.
- `examples/inprocess_mirror/` — the CI-built smoke app (register / write / read / await).
