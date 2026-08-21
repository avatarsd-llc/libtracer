# conn/spec-name-in-use

RFC-0014 §2's `spec-name-in-use`: a `SPEC` written to `/net/<module>/conn` naming a connection
that **already exists** is refused `ERROR{tr::path::in_use}` (`0x0022`) — and the live
connection is left exactly as it was.

```
SPEC{ NAME "name"   NAME "up",
      NAME "config" SETTINGS{ NAME "addr" NAME "10.0.0.9",
                              NAME "port" VALUE u16=9000 (LE) } }
```

```
0e403e00020004006e616d6502000200757002000600636f6e6669670b402200
02000400616464720200080031302e302e302e3902000400706f727401000200
2823
```
(one byte string, wrapped here for width; the canonical bytes are `input.bin`.)

## Why the config differs from `conn/create-via-spec`

This vector is **not** a byte-for-byte replay of the create. It names the same connection
(`up`) with a *different* address and port on purpose, because RFC-0014's ruling (i) has two
halves and a replayed create would only pin one of them:

1. **The create is refused**, with `tr::path::in_use` (`0x0022`) — never a silent second
   connection and never a silent overwrite. The refusal is deliberately idempotent-safe: a
   retrying orchestrator reads `PATH_IN_USE` as *"already exists"* and carries on, so a lost
   reply costs a retry rather than a duplicate. A declarative reconciler diffs live-vs-desired
   and `SPEC`s only absent names.
2. **Re-`SPEC` is not a reconfiguration door.** The live `up` still dials `127.0.0.1:8080`
   afterwards; nothing from these bytes reaches it. Connection config is creation-time and
   transport-private, and there is no post-creation write door for it (RFC-0014 §Erratum
   2026-08-13, [#1070](https://github.com/avatarsd-llc/libtracer/issues/1070)) — changing a
   key means `NAME`-retire plus `SPEC`-re-create, and the routes under the connection do **not**
   survive that.

A core that treated a repeated `SPEC` as an update would pass a byte-for-byte replay of the
create vector and fail this one, which is the reason the two differ.

## What this vector gates, and where the behaviour is bound

Codec only, per [HARNESS.md](../../../../HARNESS.md). Bound in
`core/tests/transport_vertex_test.cpp`, `test_conformance_vectors`: the second write answers
`PATH_IN_USE`, the router's demux table still holds exactly one link, and the live
connection's parsed `conn_settings_t` still reads `127.0.0.1:8080` rather than `10.0.0.9:9000`.
