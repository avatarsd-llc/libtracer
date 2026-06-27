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

## Files

- `library.json` — PlatformIO manifest (points at `core/include/` and `core/src/`).

## Releasing

`pio pkg publish` is run from CI on tag `v*` (see `.github/workflows/publish-pio.yml` once added).
