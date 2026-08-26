# conn/create-via-spec

RFC-0014 §2's `create-via-SPEC`: the payload of a `write /net/<module>/conn` that
materializes the connection vertex `/net/<module>/up`, dialing `127.0.0.1:8080`.

```
SPEC{ NAME "name"   NAME "up",
      NAME "config" SETTINGS{ NAME "addr" NAME "127.0.0.1",
                              NAME "port" VALUE u16=8080 (LE) } }
```

```
0e403f00020004006e616d6502000200757002000600636f6e6669670b402300
0200040061646472020009003132372e302e302e3102000400706f7274010002
00901f
```
(one byte string, wrapped here for width; the canonical bytes are `input.bin`.)

| Bytes | Meaning |
| --- | --- |
| `0e 40 3f 00` | SPEC, `opt.PL=1`, body length 63 |
| `02 00 04 00 6e616d65` / `02 00 02 00 7570` | NAME `"name"` → NAME `"up"` |
| `02 00 06 00 636f6e666967` | NAME `"config"` — the key preceding the SETTINGS |
| `0b 40 23 00` | SETTINGS, `opt.PL=1`, body length 35 |
| `02 00 04 00 61646472` / `02 00 09 00 3132372e302e302e31` | NAME `"addr"` → NAME `"127.0.0.1"` |
| `02 00 04 00 706f7274` / `01 00 02 00 901f` | NAME `"port"` → VALUE u16 `8080` (LE) |

## What is NOT in these bytes is the point

The creator endpoint's `SPEC` carries `{ name, config }` and nothing else:

- **No `type`.** The module segment in the path (`/net/<module>/conn`) already selects the
  transport, so there is no catalog child type left to name. The superseded global
  `:children[]` door did need one — it carried `type` and `role` on top of these fields — and
  RFC-0014 Amendment 4 (S7) retired that door and its `spec/conn-client-ws` vector with it.
  There is no longer a second spelling to contrast with; these bytes are the only ones a
  conforming core emits to create a connection.
- **No `role`.** RFC-0014 §1/§3 make the role **positional**: it is fixed by *which module*
  the endpoint belongs to (`ws-client` = `DIAL`, `ws-server` = `LISTEN`), never by a payload
  field. An implementation that read a `role` pair here would let a peer create a listener
  through a dialer's endpoint. Since S7 the reference implementation does not read a `role`
  key at all: it is an ordinary unknown pair, ignored like any other.

## The config's values are mixed by TYPE, and the two are not interchangeable

`addr` is a textual `NAME`; `port` is an opaque `VALUE` u16. A string key is found only as a
`NAME` child, so a core that can emit only one of the two forms cannot express half the
record. This claim used to be carried by `spec/conn-client-ws`; it lives here now, because
these bytes carry the same mix and this is the door that survives.

`name` is required and stays required (ADR-0073 §5): a creator-chosen name is what makes a
retried create idempotent — the retry answers `tr::path::in_use` (see `conn/spec-name-in-use`)
instead of minting a second connection.

## What this vector gates, and where the behaviour is bound

Per [HARNESS.md](../../../../HARNESS.md), a vector gates the codec only — these bytes decode and
re-encode to themselves in every core, and that says nothing about what a node does with them.
The behaviour (the write executes rather than assigns; `/net/ws-client/up` appears; the link
is wired into the router's demux table; the `config` reaches `conn_settings_t`) is bound in
`core/tests/transport_vertex_test.cpp` — `test_conformance_vectors`.
