# Wire format, bit by bit

A hands-on tour of the actual bytes — like a protobuf encoding guide, but for
libtracer's TLV. Every example is a **real frame** you can reproduce with
`encode()`. Read [frame-codec](frame-codec.md) for the API; this page is the bits.

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
  STATUS, `0x0B` SETTINGS, `0x0C` TIME, `0x0D` ROUTER … (`0x80–0xFF` is yours).
- **opt** — eight flag bits (below).
- **length** — payload size, **fixed-width** little-endian: `u16` normally, `u32`
  when `opt.LL=1`. Fixed width means a parser jumps `header + length` to the next
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
**CR=1** (CRC trailer). You pay bytes only for the options you set; the default
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
*your* schema says so. libtracer never interprets application data (just like JSON
doesn't know your field is a temperature).

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
lets a recorder or bridge *attach* integrity at egress and *strip* it at ingress
**without touching the payload bytes**: at rest a value is `header+payload`; in
transit it grows a trailer; the payload is byte-identical through both.

### 4 · a structured `PATH` `/sensor/temp` (22 bytes)

`opt.PL=1`, so the payload is **child TLVs concatenated** — two `NAME`s.

```text
 06 40 12 00 │ 02 00 06 00 73 65 6E 73 6F 72 │ 02 00 04 00 74 65 6D 70
 └──┬───────┘ └────────────┬──────────────┘ └──────────┬──────────┘
    │           NAME "sensor" (10 bytes)        NAME "temp" (8 bytes)
    │           02=NAME 00=opt 0006=len  s e n s o r
    │
    type=0x06 PATH · opt=0x40 (PL=1) · length=0x0012=18  (= 10 + 8 child bytes)
```

Walking it is the **same loop** as the outer frame: read a 4-byte header, jump
`length`, repeat. No special list type, no nesting markers — structure *is*
concatenation. And those 18 payload bytes are **exactly** the vertex-map key
([path](path.md)): the address on the wire and the address in memory are the same
bytes.

### 5 · a `ROUTER` frame (the bridge envelope)

A data TLV wrapped for the wire ([router](router.md)). `NAME`-tagged metadata, then
the wrapped TLV last:

```text
 0D 40 LL LL │ <NAME "origin_peer_id"> <VALUE 16B>
             │ <NAME "origin_timestamp"> <TIME u64>     ← dedup key = (origin, ts)
             │ <NAME "hop_count"> <VALUE u8>            ← termination = hop_count
             │ <NAME "data"> <… the wrapped data TLV, verbatim …>
   type=0x0D ROUTER · opt=0x40 (PL=1)
```

The wrapped TLV (example 2, 3, or 4) is copied in **verbatim, trailer and all** — so
`router_unwrap` hands it back as a zero-copy span and the bridge re-injects the
exact original bytes.

## The same bytes, three ways

This is the payoff the byte layout buys: there is no separate "decode into a struct"
step. The wire bytes, the in-memory value, and the graph node are one buffer.

```{mermaid}
flowchart LR
    B["bytes:<br/>06 40 12 00 02 00 …"]:::b
    B --> W["on the wire<br/>(a frame)"]
    B --> M["in memory<br/>(a View → Tlv, borrowed)"]
    B --> G["in the graph<br/>(the vertex's value / key)"]
    classDef b fill:#dbeafe,stroke:#1e40af;
```

## Where the benefits live, in the bytes

| You see in the bytes… | …which buys |
| --- | --- |
| 4-byte header (`type opt len`) | tiny per-message overhead; fits MCU MTUs |
| **fixed-width** length | jump to the next TLV with no varint scan → an *iterative*, bounded, recursion-free parser |
| `opt` flag bits | pay for timestamp/CRC/wide-length **only when set**; default frame is 4 bytes |
| **trailer**-positioned CRC/TS | attach/strip integrity & time without rewriting the payload (rest ⇄ transit) |
| `PL=1` = concatenated children | structure with no list type; a structured value is parsed in place as sub-spans |
| payload = opaque bytes | the protocol is a transparent carrier; *your* schema gives bytes meaning |
| the key bytes = the PATH payload | one address for wire and memory; dispatch is a byte compare |

Net: **the bytes you receive are the bytes you keep** — a decoded value is a set of
`std::span`s into the received buffer ([views](views.md)), so reading a field is a
pointer load and handing a value to N subscribers is N refcount bumps, not N copies.
That is the entire performance argument, visible in the layout.

See: [frame-codec](frame-codec.md) · the normative
[data-format reference](../reference/01-data-format.md) · the
[TLV catalog](../reference/05-protocol-tlvs.md).
