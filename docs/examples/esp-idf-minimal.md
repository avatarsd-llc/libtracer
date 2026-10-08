# ESP-IDF: the component, and nothing else

The smallest ESP-IDF app that uses libtracer: a `main` component that says `REQUIRES libtracer`,
and a `graph_t` built with no argument. On an ESP-IDF build that graph draws every byte it holds
from **one static arena** in `.bss`, the MCU default root
([ADR-0083](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0083-one-allocation-seam.md)
Decision 4). The app registers one vertex, writes it, reads it back, and prints what the arena
carved for it.

## What to notice

- **No source is passed, and none is needed.** The component binds `kSlabPool = false`, so
  `tr::mem::default_root()` is the static arena of `CONFIG_LIBTRACER_ARENA_BYTES` (32 KiB on a
  chip). The graph's tables and the value it stores both come from it; the root's census
  (`stats()`) shows the bytes carved.
- **The value is built on the graph's own backend.** `over_bytes(bytes, g.value_backend())`
  copies the app's bytes into a segment drawn from the arena's value sub-pool, the same place the
  graph keeps what it stores.
- **No transport is compiled.** `sdkconfig.defaults` turns `CONFIG_LIBTRACER_TRANSPORT_UDP`,
  `_TCP` and `_WS` off: an in-process node sheds those translation units from `libtracer.a`.
- **"No heap" is checked at link time, not here.** CI's no-heap link check reads `libtracer.a`
  with `nm` and fails on any heap call not pinned in `tools/no_heap_baseline.json`, a list that
  only shrinks.

## Source

```{literalinclude} /integrations/esp-idf/examples/concepts/minimal/main/app_main.cpp
:language: cpp
:linenos:
```

## Build and run

```console
$ cd integrations/esp-idf/examples/concepts/minimal
$ idf.py set-target esp32c6 build
$ idf.py flash monitor        # board-only
```

CI builds it for `esp32c6`, and also builds and runs it on the ESP-IDF `linux` target, where it
exits non-zero on a failed check. See also: [the ESP-IDF one-concept set](index.md) ·
[backends](../modules/backends.md).
