# ESP-IDF: sizing the arena

The MCU default root is one `.bss` array of `config_t::kArenaBytes`, which the component binds
from `CONFIG_LIBTRACER_ARENA_BYTES`. Its size is a compile-time number, so the linker map shows
exactly what libtracer holds, and it never grows: a node that outgrows it is **told so**, by value
at run time and by a sizing message at init
([ADR-0083](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0083-one-allocation-seam.md),
[ADR-0056](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0056-vertex-handle-infallible-register.md)).

## What to notice

- **Size it against the node's peak.** The app sets the arena to 8 KiB in `sdkconfig.defaults`,
  registers and writes the eight vertices it is built to hold, and reads the root's census:
  `capacity` is the arena, `in_use` the bytes carved so far. Their difference is the headroom.
  The arena never gives bytes back to itself (a returned block is reused by its class), so the
  carved figure is a high-water mark.
- **At run time, a refusal is a value.** `try_register_vertex` is the failable form: past the
  arena it answers an error, the root's census counts the refusal and the size of the largest
  refused request, and the node keeps running.
- **At init, a refusal is a sizing message.** `register_vertex` is infallible (ADR-0056): a
  literal path at startup is not a condition the app can handle, so libtracer reports and
  aborts. The line names the call, the sub-pool and the bytes it needed:

  ```text
  libtracer: register_vertex: the "tables" memory source refused an allocation at initialization (88 bytes needed, 6400 bytes in use): a sizing bug, give it more room (ADR-0056, ADR-0083)
  ```

  That is the line from a `linux`-target run of this example; the run before it printed
  `8 sensors: 3504 of 8192 bytes carved` and `try_register_vertex refused after 39 more`. The
  "in use" figure is the sub-pool's, not the arena's.

  The example runs that last step only with `CONFIG_EXAMPLE_SHOW_INIT_EXHAUSTION=y`
  (`idf.py menuconfig`, *arena_sizing example*). It is off by default because on a board the
  abort is a reset. The line goes through the build's `fault_sink_t`; on ESP-IDF that is the
  console.
- **The numbers to size by are on the node.** [`:stats.mem.*`](esp-idf-stats-subpools.md)
  reports each sub-pool's `peak`, so a running node can tell you the arena it needs.
- **No arena at all is a build choice.** An app that injects its own source into every
  graph, router, link and transport vertex can set `CONFIG_LIBTRACER_DEFAULT_ROOT_HEAP=y`: no
  arena region, heads table or root object is linked (about 5.2 KB of static RAM at a 4 KiB
  arena), and any default it missed draws from the system heap instead, unbounded. With an
  arena, a root census `peak` of 0 is the sign that nothing reached it
  ([#2090](https://github.com/avatarsd-llc/libtracer/issues/2090)).

## Source

```{literalinclude} /integrations/esp-idf/examples/concepts/arena_sizing/main/app_main.cpp
:language: cpp
:linenos:
```

## Build and run

```console
$ cd integrations/esp-idf/examples/concepts/arena_sizing
$ idf.py set-target esp32c6 build
$ idf.py flash monitor        # board-only
```

CI builds it for `esp32c6`, and also builds and runs it on the ESP-IDF `linux` target with the
init step off. See also: [the component, and nothing else](esp-idf-minimal.md).
