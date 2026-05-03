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
    https://github.com/avatarsd/libtracer.git
```

## Files

- `library.json` — PlatformIO manifest (points at `core/include/` and `core/src/`).

## Releasing

`pio pkg publish` is run from CI on tag `v*` (see `.github/workflows/publish-pio.yml` once added).
