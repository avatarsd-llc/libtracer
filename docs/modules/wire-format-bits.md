# Wire format, bit by bit

A byte-level tour of libtracer's TLV — the protobuf-encoding-guide view of the
format. Every example is a **real frame**, reproducible with `tr::wire::encode`
(`core/include/libtracer/frame.hpp`).

This page is **implementation-independent**. It describes the bytes any conformant
implementation puts on the wire, in any language; it lives under `docs/modules/`
for URL stability, not because anything here is specific to the C++ reference
implementation. The normative text is the
[data-format reference](../reference/01-data-format.md) and the
[TLV catalog](../reference/05-protocol-tlvs.md);
[frame-codec](frame-codec.md) is the C++ API that reads and writes these bytes.

The whole protocol is **one shape, recursively**: a *Type-Length-Value*. There are
no field tags, no varints, no schema needed to walk the bytes — the header tells
you everything, and a structured value is just **more TLVs concatenated**.

## The header (4 bytes, or 6)

```text
 ┌────────┬────────┬────────────────┐         ┌────────┬────────┬────────────────────────────────┐
 │  type  │  opt   │  length (u16)  │   or    │  type  │  opt   │        length (u32)            │
 │  u8    │  u8    │  little-endian │  LL=1   │  u8    │  u8    │        little-endian           │
 └────────┴────────┴────────────────┘         └────────┴────────┴────────────────────────────────┘
   byte 0   byte 1   bytes 2..3                  byte 0   byte 1   bytes 2..5
```

- **type** — one byte. `0x01` VALUE, `0x02` NAME, `0x06` PATH, `0x07` POINT, `0x09`
  STATUS, `0x0B` SETTINGS, `0x0C` TIME, `0x0F` FWD … (`0x80–0xFF` is yours).
- **opt** — eight flag bits (below).
- **length** — payload size, **fixed-width** little-endian: `u16` normally, `u32`
  when `opt.LL=1`. It counts payload bytes only: neither the header nor a trailer is
  included. Fixed width means a parser jumps `header + length` to the next
  TLV with **no scanning** — the basis of the iterative (non-recursive) walk.

## The `opt` byte, bit by bit

```text
   bit:   7      6      5      4      3      2      1      0
        ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
        │  R   │  PL  │  TS  │  CR  │  LL  │  CW  │  TF  │  R   │
        └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
          │      │      │      │      │      │      │      │
  reserved┘      │      │      │      │      │      │      └reserved  (both MUST be 0;
   (=0)          │      │      │      │      │      │                  non-zero ⇒ invalid)
                 │      │      │      │      │      │      └ TF  timestamp form: 0=abs u64 ns, 1=rel i32
                 │      │      │      │      │      └ CW  CRC width: 0=CRC-32C, 1=CRC-16-CCITT
                 │      │      │      │      └ LL  length width: 0=u16, 1=u32
                 │      │      │      └ CR  trailer carries a CRC
                 │      │      └ TS  trailer carries a timestamp
                 │      └ PL  payload is structured (children), not opaque bytes
                 └ (reserved)
```

So `opt = 0x40` is `0b0100_0000` → **PL=1** (structured). `opt = 0x10` →
**CR=1** (CRC trailer). A frame pays bytes only for the options it sets; the default
`opt = 0x00` is a bare opaque value with a 4-byte header.

## Worked frames

### 1 · empty `STATUS` = OK (4 bytes)

The smallest frame — a write acknowledgement.

```text
 09 00 00 00
 │  │  └──┴── length = 0x0000 = 0  (no payload)
 │  └─────── opt    = 0x00         (no flags)
 └────────── type   = 0x09 STATUS
```

An empty STATUS *is* "OK". No body, no enum — absence is the signal.

### 2 · a `VALUE` carrying one byte (5 bytes)

A boolean `true`.

```text
 01 00 01 00 01
 │  │  └──┴─ │  length = 0x0001 = 1
 │  │        └─ payload[0] = 0x01   ← the value 'true'
 │  └────────── opt = 0x00
 └───────────── type = 0x01 VALUE
```

The payload bytes are **opaque** to the protocol — `0x01` means `true` only because
the application's schema says so. libtracer never interprets application data (just
like JSON does not know a field is a temperature).

### 3 · a `VALUE` with a CRC trailer (13 bytes)

Same VALUE, payload `AA BB CC DD EE`, integrity-checked with CRC-32C.

```text
 01 10 05 00 AA BB CC DD EE  B6 C9 12 23
 │  │  └──┴─ └──────────────┘ └──────────┘
 │  │   len=5    payload          trailer: CRC-32C(payload) = 0x2312C9B6,
 │  │                                       stored little-endian
 │  └─ opt = 0x10  → CR=1 (trailer has a CRC)
 └──── type = 0x01 VALUE
```

The CRC lives in the **trailer**, after the payload — not the header. That is what
lets a recorder or forwarder *attach* integrity at egress and *strip* it at ingress
**without touching the payload bytes**: at rest a value is `header+payload`; in
transit it grows a trailer; the payload is byte-identical through both.

### 4 · a packed `PATH` `/sensor/temp` (16 bytes)

`opt.PL=0` — the payload is **not** child TLVs. It is a self-delimiting run of
**segment records**, each `[u8 len][utf8]`
([RFC-0018](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0018-packed-path-segments.md)).

```text
 06 00 0C 00 │ 06 73 65 6E 73 6F 72 │ 04 74 65 6D 70
 └──┬───────┘ └──────────┬────────┘ └───────┬──────┘
    │           "sensor" (7 bytes)    "temp" (5 bytes)
    │           06=len  s e n s o r
    │
    type=0x06 PATH · opt=0x00 (PL=0) · length=0x000C=12  (= 7 + 5 record bytes)
```

Walking it is *cheaper* than the outer frame's loop, not the same one: read one
length byte, jump `1 + len`, repeat — no option decode and no header to construct.
A record carries no type byte and no option byte, which is the whole point: an
address has exactly **one** spelling. And those 12 payload bytes are **exactly** the
vertex-map key ([path](path.md); `path_t::key`,
`core/include/libtracer/path.hpp`): the address on the wire and the address in
memory are the same bytes. `len == 0` is the reserved escape record
(`00 <kind> <len> <bytes>`) — a forwarder steps over one, a key rejects it.

### 5 · a `FWD` frame (the remote-operation envelope)

A remote write carried by the source-routed `FWD` (`0x0F`,
[reference/05 §reserved range](../reference/05-protocol-tlvs.md)): the op code, the
explicit route to the target (`dst`), the accumulated way back (`src`), then the
payload TLV — `FWD{ op=WRITE, dst=/b/temp, src=(empty), VALUE 0x2A }`, 29 bytes:

```text
 0F 40 19 00                              ← FWD · opt=0x40 (PL=1) · length=0x0019=25
 │ 01 00 01 00 01                         ← VALUE op: 1 byte, WRITE=0x01
 │ 06 00 07 00                            ← PATH dst (PL=0), 7 body bytes
 │   01 62                                ←   record "b"    (the next-hop link)
 │   04 74 65 6D 70                       ←   record "temp" (the target on the peer)
 │ 06 00 00 00                            ← PATH src (PL=0), empty — grows per hop
 │ 01 00 01 00 2A                         ← VALUE payload: the byte 0x2A
```

Every child is one of the shapes above — the frame is examples 2 and 4,
concatenated. A forwarding hop reads just the three leading headers **by offset**:
it strips the record `01 62` (`"b"`) from `dst` (shrinking it toward the target),
prepends its own segment record for the inbound link to `src` (the return route), and sends the rest of the
frame onward **untouched** — the payload bytes are never copied or re-encoded
(`rebuild_fwd_forward`, `core/include/libtracer/fwd_frame_view.hpp`, emits two
rebuilt headers and gathers every other region as an offset window into the source).
When `dst` no longer starts with a link name, the frame has arrived: the terminus
decodes it and applies the op.

Nothing wraps a FWD: routing is explicit and source-routed, and `0x0D` ROUTER is a
reserved, decodable codepoint with no implemented mechanism
([reference/05 §`0x0D`](../reference/05-protocol-tlvs.md)).

## The same bytes, three ways

There is no separate "decode into a struct" step. The wire bytes, the in-memory
value, and the graph node are one buffer.

```{mermaid}
flowchart LR
    B["bytes:<br/>06 00 0C 00 06 &quot;sensor&quot; …"]:::b
    B --> W["on the wire<br/>(a frame)"]
    B --> M["in memory<br/>(a borrowed view, no copy)"]
    B --> G["in the graph<br/>(the vertex's value / key)"]
    classDef b fill:#dbeafe,stroke:#1e40af;
```

## Consequences of the layout

| In the bytes | Consequence |
| --- | --- |
| 4-byte header (`type opt len`) | tiny per-message overhead; fits MCU MTUs |
| **fixed-width** length | jump to the next TLV with no varint scan → an *iterative*, bounded, recursion-free parser |
| `opt` flag bits | timestamp/CRC/wide-length cost bytes **only when set**; the default frame is 4 bytes |
| **trailer**-positioned CRC/TS | attach/strip integrity & time without rewriting the payload (rest ⇄ transit) |
| `PL=1` = concatenated children | structure with no list type; a structured value is parsed in place as sub-spans |
| payload = opaque bytes | the protocol is a transparent carrier; the application's schema gives bytes meaning |
| the key bytes = the PATH payload | one address for wire and memory; dispatch is a byte compare |

The bytes received are the bytes kept: a decoded value is a set of spans into the
received buffer ([views](views.md)), so reading a field is a pointer load and handing
a value to N subscribers is N refcount bumps rather than N copies.

## API reference

Generated from `core/include/libtracer/tlv.hpp` by Doxygen.

```{doxygenstruct} tr::wire::opt_t
:project: libtracer
:members:
```

```{doxygenenum} tr::wire::type_t
:project: libtracer
```

See: [frame-codec](frame-codec.md) · the normative
[data-format reference](../reference/01-data-format.md) · the
[TLV catalog](../reference/05-protocol-tlvs.md).
