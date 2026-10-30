# The illustrated frame

## In one paragraph

This page walks every byte of one real frame: a remote write of the `u32` value 1234 to
`/sensor/temp`, asking for a reply at `/reply-ep`. It is 46 bytes, and each of them belongs to
one of the pieces the other wire pages explain. The outer record is a `FWD`, the envelope of every
remote operation. Its four children, in a fixed order, are the operation, the destination, the
return route and the payload. Two of them are `PATH` addresses made of segment records, and two
are `VALUE` records holding plain little-endian bytes. The bytes below are read from the
conformance vector `fwd/fwd-write-value` when the site builds, so they are the bytes a conforming
encoder produces, not a drawing of them.

## The picture

```{mermaid}
:caption: "Legend: blue = frame, sky blue = record."
flowchart LR
    accTitle: The four children of one FWD frame
    accDescr: A 46-byte FWD frame with a 4-byte header holds four child records in order. A VALUE of one byte holds the op, 1 for WRITE. A PATH holds dst, the segment records sensor and temp. A PATH holds src, the segment record reply-ep. A VALUE of four bytes holds the payload, 1234.
    F[/"FWD 0x0F, 46 bytes"/]:::frame -->|"bytes 4 to 8"| O["VALUE: op = WRITE"]:::record
    F -->|"bytes 9 to 24"| D["PATH dst: sensor, temp"]:::record
    F -->|"bytes 25 to 37"| S["PATH src: reply-ep"]:::record
    F -->|"bytes 38 to 45"| V["VALUE: u32 1234"]:::record
```

## When you need it

Use this page to check an encoder in another language against real bytes, to learn to read a
capture, or to see how the pieces fit before reading the spec. Every row in the walk below is a
record you can open; the header fields, the path's records and the value are shown for each.

## How it works

**Bytes 0 to 3, the frame header.** `0f` is `FWD`. `40` is `opt` with only `PL` set: the payload
is child records, there is no trailer, and the length is 16 bits. `2a 00` is the payload length,
42, little-endian. Header plus payload is the whole frame: 4 + 42 = 46.

**Bytes 4 to 8, the operation.** A `VALUE` (`01`) with `opt` `00` and length `01 00`, holding one
byte: `01`, `WRITE`. Its opcode is the low six bits; the top two are reserved. `READ` is `00`,
`AWAIT` `02` and `REPLY` `03`.

**Bytes 9 to 24, the destination.** A `PATH` (`06`) with `opt` `00`, since an address is never
structured, and length `0c 00`, 12. Its body is two segment records: `06` and the six bytes of
`sensor`, then `04` and the four bytes of `temp`. On the receiving node those 12 bytes are the
vertex's lookup key as they stand.

**Bytes 25 to 37, the return route.** Another `PATH`, length `09 00`, holding one segment record:
`08` and `reply-ep`. A forwarding hop would prepend its own part of the route here, so a reply
can retrace the frame. An empty `src` would make the write unacknowledged: applied, and answered
with nothing.

**Bytes 38 to 45, the payload.** A `VALUE` with length `04 00` and the bytes `d2 04 00 00`: 1234,
little-endian. The payload is whatever record the operation carries; a write of a structured
value would put a `POINT` here instead.

**What a node does with it.** This frame's `dst` names a vertex on the receiving node, so that
node is the terminus: it decodes the four children into a flat arena of spans, finds the vertex by
the `dst` body, checks the caller's write right, stores the value, and replies to `src`. Had `dst`
begun with a link's mount run, the node would have forwarded it instead: consume that run from
`dst`, prepend its own inbound link to `src`, and send the rest of the bytes unchanged.

## Worked example

Open a row to see its header fields; the path rows list their records. The plain hex of the whole
vector follows the walk.

```{hex-walk} fwd/fwd-write-value
:caption: A remote write of 1234 to /sensor/temp, replying to /reply-ep.
u8: op, 1 = WRITE
path: dst, the target vertex
path: src, the route back
u32: the value written
```

In transit with a CRC-32C, the same frame sets `CR` in the outer `opt` (`50`) and gains 4 trailer
bytes after byte 45. The children are not touched, because the trailer belongs to the frame.

## Cost

### Wire bytes

Of the 46 bytes, 4 are the value. The other 42 are the envelope: 20 bytes of headers, 1 of op,
and 21 of address bodies. The envelope does not grow with the value:

| Value | Frame | Envelope share |
| ---: | ---: | ---: |
| 4 B | 46 B | 91% |
| 64 B | 106 B | 40% |
| 1 KiB | 1,066 B | 3.9% |
| 4 KiB | 4,138 B | 1.0% |
| 16 KiB | 16,426 B | 0.3% |
| 64 KiB | 65,582 B | under 0.1% |

The last row widens both the value's and the frame's length to 32 bits, 4 bytes more in all.
Spelling `dst` as one PAIR makes the frame 45 bytes; an empty `src` makes it 37.

### Allocations

None to read: every child is a span into the 46 bytes. The terminus draws its arena nodes from
the node's injected block source, stores the value with at most one copy of the `VALUE` record,
and emits its reply into one exactly-sized segment. A forwarding hop allocates no copy of the
payload; it rebuilds two headers and gathers the rest.

### Latency

Not benched on this page. `bench_codec` times a decode plus an encode of this vector among the
others; its figures are on the generated [performance page](../../performance.md).

### Flash and RAM per target

| Target | Flash | RAM |
| --- | --- | --- |
| NARROW | the frame codec, terminus arena and path module are required modules of the gated Cortex-M0 node | arena nodes for 5 records, from the injected source |
| MID | inside the reported ESP-IDF core | same |
| WIDE | not measured | same |

## What the library does not do

- It does not wrap a `FWD` in anything. Routing is explicit in `dst` and `src`; there is no
  router header and no correlation id.
- It does not keep state per request on a forwarding hop. Everything a reply needs is in `src`.
- It does not re-encode the payload on the way. The value's bytes arrive as they were sent.

## Pitfalls

- **Symptom:** a peer answers a well-formed write with an undefined-opcode error. **Cause:** a
  reserved bit of the `op` byte is set. **Fix:** keep bits 7 and 6 of `op` clear.
- **Symptom:** a forwarder misroutes a subscribe request that a hop has already relayed.
  **Cause:** it found the trailing reverse-route child by position. **Fix:** find an added child
  by its type code, and mask `op` with `0x3F` rather than switching on the raw byte.
- **Symptom:** a write is never acknowledged. **Cause:** `src` was empty, which asks for no reply.
  **Fix:** give `src` a return route whenever you want an answer.

## Where the rules live

**Normative source:** [v1 §3](../../spec/v1.md#3-wire-format), which incorporates the `FWD`
envelope in [reference 05 §Reserved range](../../reference/05-protocol-tlvs.md#reserved-range-0x0f--0x1f),
the path in [reference 05 §0x06 PATH](../../reference/05-protocol-tlvs.md#0x06--path) and the
header in [reference 01 §Frame layout](../../reference/01-data-format.md#frame-layout).

**Exercised by:** `fwd-write-value`, `fwd-read`, `fwd-src-accumulated`, `fwd_frame_view_test`,
`fwd_multihop_test`.

## History

:::{dropdown} How this frame got its shape
- [RFC-0004](../../spec/rfcs/0004-remote-operation-addressing.md) (accepted) defined `FWD` and its
  four children.
- [RFC-0018](../../spec/rfcs/0018-packed-path-segments.md) packed `PATH` bodies into segment
  records; under the old `NAME`-child body this frame was 55 bytes.
- [RFC-0029](../../spec/rfcs/0029-one-path-primitive.md) (accepted 2026-09-30) made the PAIR the
  one compact path element and reserved the top two `op` bits.
:::

Next concept: [The graph](../graph.md). Go deeper:
[wire format, bit by bit](../../modules/wire-format-bits.md#5--a-fwd-frame-the-remote-operation-envelope),
then the [frame codec](../../modules/frame-codec.md), then
[v1 §3](../../spec/v1.md#3-wire-format).
