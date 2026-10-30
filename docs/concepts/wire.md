# The wire format

## In one paragraph

Everything libtracer puts on a link has one shape, the TLV. A `type` byte says what it is, an
`opt` byte says which optional parts it carries, a fixed-width `length` says how long its
payload is, and an optional trailer after the payload carries a wire time and a CRC. A payload
is either opaque bytes or a run of child TLVs, so a whole remote operation is TLVs inside one
TLV. A frame is the outermost TLV a link carries, and an address is a `PATH` TLV whose body is a
run of small records, one per path element. The same bytes are the frame on the wire, the
value in memory and the key in the graph: nothing is decoded into a second form. The child
pages take each piece in turn, and [the illustrated frame](wire/illustrated-frame.md) walks
every byte of one real frame.

```{toctree}
:hidden:
:maxdepth: 1

Frames <wire/frame>
Records <wire/record>
The opt byte <wire/flags>
Addresses <wire/address>
The illustrated frame <wire/illustrated-frame>
```

## The picture

```{mermaid}
:caption: "Legend: blue = frame, sky blue = record."
flowchart LR
    accTitle: One frame, the pieces it is made of
    accDescr: A frame is a header, a payload and an optional trailer. The header is type, opt and length. The opt byte's PL bit chooses whether the payload is opaque bytes or child records. A child record can be a PATH, whose body is segment records. The trailer carries a wire time and a CRC when opt asks for them.
    F[/"frame"/]:::frame -->|starts with| H["header: type, opt, length"]:::record
    F -->|then| P{"opt.PL?"}
    P -->|"0: opaque"| O["payload bytes"]:::record
    P -->|"1: structured"| C["child records"]:::record
    C -->|"one of them"| A["PATH: segment records"]:::record
    F -.->|"opt.TS, opt.CR"| T["trailer: wire time, CRC"]:::record
```

## When you need it

Read this area when you write an encoder or decoder in another language, a dissector, or a
transport that must find frame boundaries, or when you read a capture byte by byte. An
application that uses the C++ API never builds bytes by hand: `read`, `write` and the graph's
calls produce and consume them. It still pays their cost, which is why the Cost section below
is worth a look before you size an MCU's buffers.

## How it works

| Piece | What it is | Bytes | Page |
| --- | --- | --- | --- |
| frame | the outermost TLV: header, payload, optional trailer | 4 and up | [Frames](wire/frame.md) |
| record | one TLV, opaque or structured, named by its type code | 4 and up | [Records](wire/record.md) |
| `opt` | the bit field that says which optional parts are present | 1 | [The opt byte](wire/flags.md) |
| address | a `PATH`: one record per path element, NAME or PAIR | `1 + len` per NAME, 11 per PAIR | [Addresses](wire/address.md) |

**One layout, read in place.** The header has fixed offsets and a fixed-width length, so a
reader knows where the next TLV starts after reading two or four bytes, without a varint scan.
A structured payload is only child TLVs, so the reader walks it with the same loop and an
explicit work stack, never by recursion. Decoding produces spans into the buffer it was given;
the [frame codec](../modules/frame-codec.md) builds nothing.

**Optional parts cost only when set.** Wide length, wire time and CRC each have a bit in `opt`.
A frame that sets none of them is header plus payload, and the smallest frame is 4 bytes.

**The trailer moves, the payload does not.** A link adds a trailer when it sends and strips it
when it receives, so a forwarder or a recorder keeps the payload bytes exactly as they arrived.

## Cost

Each child page carries the numbers for its piece. In summary:

### Wire bytes

A TLV costs 4 bytes of header (6 with `opt.LL`), plus 0 to 12 bytes of trailer. A remote write
of one `u32` to `/sensor/temp` with a return address is 46 bytes, walked byte by byte in
[the illustrated frame](wire/illustrated-frame.md). The overhead is flat as the payload grows:

| Opaque payload | Header | Frame, no trailer | Frame with CRC-32C | Overhead |
| ---: | ---: | ---: | ---: | ---: |
| 64 B | 4 B | 68 B | 72 B | 6.3% / 12.5% |
| 1 KiB | 4 B | 1,028 B | 1,032 B | 0.4% / 0.8% |
| 4 KiB | 4 B | 4,100 B | 4,104 B | 0.1% / 0.2% |
| 64 KiB | 6 B | 65,542 B | 65,546 B | under 0.01% |

The last row needs `opt.LL`, because a `u16` length stops at 65,535 bytes.

### Allocations

None to decode. A decoded TLV is a set of spans into the received buffer, and a value handed to
N subscribers is N reference-count increments, not N copies. The terminus, the one node that
reads a whole `FWD` tree, decodes into a flat arena drawn from the node's injected block
source; when that source is exhausted the frame is refused, not the node. A forwarding hop
rebuilds two headers and gathers the rest of the frame as windows into the bytes it received.

### Latency

Not benched on this page. `bench_codec` times a decode plus an encode of every conformance
vector, and its figures are on the generated
[performance page](../performance.md).

### Flash and RAM per target

| Target | Flash | RAM |
| --- | --- | --- |
| NARROW | inside the gated Cortex-M0 node; not split out per module | one work-stack entry per open level, from the injected source |
| MID | inside the reported ESP-IDF core; not split out | same |
| WIDE | not measured | same |

The Cortex-M0 sentinel in `tools/cortexm0_footprint.py` links the frame codec, the terminus
arena and the path module as required modules, so their flash is gated there, but only as part
of the whole node. The one line a constrained target should know: the default slice-by-8
CRC-32C tables are 8 KiB of read-only data, and `config_t::kCrc32cSlices = 1` replaces them
with a 1 KiB table.

## What the library does not do

- It does not version frames. There is no version field; an incompatible wire is a different
  service at discovery time.
- It does not fragment. A payload too large for one TLV is split by address, not by the codec.
- It does not cap nesting with a constant. Depth and breadth are bounded by the receiver's own
  decode budget.
- It does not decode into a second representation. The bytes it received are the bytes it keeps.

## Pitfalls

- **Symptom:** a peer refuses frames your encoder thinks are fine. **Cause:** a reserved `opt`
  bit is set, often by an `opt` mask shifted by one. **Fix:** compare your bytes against the
  conformance vectors; [the opt byte](wire/flags.md) shows the two pairs that catch this.
- **Symptom:** a parser crashes or loops on a deep frame. **Cause:** it recurses per nesting
  level. **Fix:** walk with an explicit work stack and refuse what the stack cannot hold.

## Where the rules live

**Normative source:** [v1 §3](../spec/v1.md#3-wire-format), which incorporates the frame
layout of [reference 01 §Frame layout](../reference/01-data-format.md#frame-layout), the type
registry of [reference 05](../reference/05-protocol-tlvs.md#type-code-allocation-summary) and
the path syntax of [reference 03](../reference/03-addressing.md#path-syntax).

**Exercised by:** `empty-status-ok`, `pl-bit-structured`, `opt-all-bits-nested`,
`path-sensor-temp`, `fwd-write-value`, `frame_test`, `conformance_runner`.

Next concept: [Frames](wire/frame.md). Go deeper:
[wire format, bit by bit](../modules/wire-format-bits.md), then the
[frame codec](../modules/frame-codec.md), then [v1 §3](../spec/v1.md#3-wire-format).
