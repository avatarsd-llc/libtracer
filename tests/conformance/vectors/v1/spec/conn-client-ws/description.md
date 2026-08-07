# spec/conn-client-ws

A connection-formation SPEC — the payload of the in-band
`write /net:children[] += SPEC{…}` that brings a transport link up (ADR-0027,
ADR-0043 §5, `docs/reference/13` §2). A DIAL `client` over `ws` to `127.0.0.1:8080`.

```
SPEC{ NAME "type"   NAME "client",
      NAME "name"   NAME "up",
      NAME "config" SETTINGS{ NAME "role" VALUE u8=0  (DIAL),
                              NAME "port" VALUE u16=8080 (LE),
                              NAME "kind" NAME "ws",
                              NAME "addr" NAME "127.0.0.1" } }
```

```
0e406c00020004007479706502000600636c69656e74020004006e616d650200
0200757002000600636f6e6669670b403e0002000400726f6c65010001000002
000400706f727401000200901f020004006b696e640200020077730200040061
646472020009003132372e302e302e31
```
(one byte string, wrapped here for width; the canonical bytes are `input.bin`.)

| Bytes | Meaning |
| --- | --- |
| `0e 40 6c 00` | SPEC, `opt.PL=1`, body length 108 |
| `02 00 04 00 74797065` / `02 00 06 00 636c69656e74` | NAME `"type"` → NAME `"client"` |
| `02 00 04 00 6e616d65` / `02 00 02 00 7570` | NAME `"name"` → NAME `"up"` |
| `02 00 06 00 636f6e666967` | NAME `"config"` — the key preceding the SETTINGS |
| `0b 40 3e 00` | SETTINGS, `opt.PL=1`, body length 62 |
| `02 00 04 00 726f6c65` / `01 00 01 00 00` | NAME `"role"` → VALUE u8 `0` (DIAL) |
| `02 00 04 00 706f7274` / `01 00 02 00 901f` | NAME `"port"` → VALUE u16 `8080` (LE) |
| `02 00 04 00 6b696e64` / `02 00 02 00 7773` | NAME `"kind"` → NAME `"ws"` |
| `02 00 04 00 61646472` / `02 00 09 00 3132372e302e302e31` | NAME `"addr"` → NAME `"127.0.0.1"` |

## Two value types in one record, and neither is optional

`config` is the same positional `(NAME key, value)` grammar as the SPEC body, but its
values are **mixed by type on purpose**:

- integers and flags (`role`, `port`) are a **`VALUE`** (`0x01`), little-endian;
- strings (`kind`, `addr`) are a **`NAME`** (`0x02`).

The reader is typed on both sides — a string key is found only as a `NAME` child and an
integer key only as a non-empty `VALUE` child — so the two forms are not
interchangeable. A core whose settings builder emits only one of them cannot express
half of this record: with `VALUE` alone there is no way to say `addr = "127.0.0.1"`, and
the link is never formed. `addr` also shows why the string form is a raw `NAME` payload
and not an address segment: a dotted quad contains `.`, which the addressing grammar
reserves — the emitter writes the bytes and validates nothing.

Key order is free (the reader is order-insensitive and skips unknown keys whole for
forward compatibility); the order here is the one the reference emitter writes.

## What this vector gates, and where the behaviour is bound

Per [HARNESS.md](../../../HARNESS.md), a vector gates the codec only. The behaviour —
that these bytes reach a child factory and that the factory reads `kind`/`addr` back as
strings and `role`/`port` back as integers — is bound in
`core/tests/children_test.cpp` (`test_conformance_vectors`), and the byte-identity of
the TypeScript builder in `bindings/typescript/packages/client/test/conn-spec.test.mjs`.
