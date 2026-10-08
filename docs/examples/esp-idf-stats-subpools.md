# ESP-IDF: reading the sub-pools' `:stats`

The default root is one arena, and the graph derives three sub-pools from it: **values** (what
vertices store), **tables** (registrations and the graph's own containers) and **net** (the
router's and the links' defaults). Each is a `:stats` seam on any vertex
(RFC-0010 Amendment 3): one READ of `:stats.mem.values`, `.tables` or `.net` answers that
sub-pool's census as one `SETTINGS` block of `NAME` / u64 pairs.

## What to notice

- **One READ, one block.** The counters arrive together, sampled in one call: `capacity`,
  `in_use`, `peak`, `refused` and `largest_refused`. A reader looks them up by name and ignores a
  name it does not know.
- **Read in place, with nothing allocated.** The example walks the block with
  `tr::wire::tlv_node_t::over(bytes, tr::mem::null_source())`, which validates the frame and
  walks its children without building a tree.
- **`peak` is the number to size by.** The app writes values of 16, 200 and 700 bytes to one
  vertex. Each write replaces the last, so `in_use` holds only the 700-byte value, while `peak`
  still counts the moment the old and the new value were both alive.
- **A node with no link still has a net sub-pool.** Its census answers, with nothing refused.
- **An injected root answers none of the three.** A graph over the app's own root derives no
  sub-pools, and each name answers `SCHEMA_NOT_FOUND`
  ([one task, no pool locks](esp-idf-single-threaded.md)).

## Source

```{literalinclude} /integrations/esp-idf/examples/concepts/stats_subpools/main/app_main.cpp
:language: cpp
:linenos:
```

## Build and run

```console
$ cd integrations/esp-idf/examples/concepts/stats_subpools
$ idf.py set-target esp32c6 build
$ idf.py flash monitor        # board-only
```

CI builds it for `esp32c6`, and also builds and runs it on the ESP-IDF `linux` target. See also:
[sizing the arena](esp-idf-arena-sizing.md) ·
[Rust: reading a node's `:stats`](rust-fwd-read-stats.md).
