# ESP-IDF: one task, no pool locks

The component's default root locks its arena and each of its sub-pools with the build's
`guard_t`, an interrupt-masked critical section on a chip, because a default must be safe for any
number of tasks. A node whose graph is touched by **one task only** declares its own root with
`tr::no_guard_t` as the lock and injects it into its `graph_t`. The lock is a template argument,
so every lock and unlock compiles to nothing.

## What to notice

- **It is a type, not a flag.** `tr::mem::arena_root_t<tr::no_guard_t, N>` is the default root's
  own template with a different lock. A `static_assert` in the example pins that the lock is an
  empty type and that the root is never larger than `tr::mem::mcu_root_t`; the app prints both
  sizes. On a chip the build's lock holds a `portMUX_TYPE` per sub-pool and per arena, so the
  difference shows; on the `linux` target's unicore build both locks are one byte.
- **The root is the app's.** Its region and free-list heads are the app's `constinit` arrays, so
  the root is constant-initialized and the linker map shows it as the app's `.bss`. The graph is
  built over it (`graph_t g(g_root)`), and the default arena is not touched.
- **An injected root serves every purpose itself.** The graph derives no sub-pools from it
  (`derives_sub_pools()` is `false`), so `:stats.mem.values`, `.tables` and `.net` answer
  `SCHEMA_NOT_FOUND` on this node; the root's own census is the number to read.
- **This covers the pool, not the whole build.** The graph's vertex locks and value slot still
  use the build's `guard_t`. A single-threaded core build binds that to `tr::no_guard_t` in its
  `libtracer/config_override.hpp`; the ESP-IDF component generates that file and offers no
  Kconfig option for it.
- **One task means one task.** Anything else that reaches the graph, a link's receive task
  included, makes `tr::no_guard_t` a data race. This node has no link.

## Source

```{literalinclude} /integrations/esp-idf/examples/concepts/single_threaded/main/app_main.cpp
:language: cpp
:linenos:
```

## Build and run

```console
$ cd integrations/esp-idf/examples/concepts/single_threaded
$ idf.py set-target esp32c6 build
$ idf.py flash monitor        # board-only
```

CI builds it for `esp32c6`, and also builds and runs it on the ESP-IDF `linux` target. See also:
[a shared seam needs a thread-safe backend](view-sync-pool.md).
