# ESPHome integration

Packages libtracer as an ESPHome [external component](https://esphome.io/components/external_components.html). ESPHome has no central registry — users reference the component by git URL, so the repo is the distribution channel.

## Use

In your ESPHome YAML:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/avatarsd/libtracer
      ref: main
    components: [libtracer]

libtracer:
  # component config goes here
```

## Files

- `components/libtracer/` — the ESPHome component (Python `__init__.py`, C++ glue).

## Status

Stub. The component is not yet implemented.
