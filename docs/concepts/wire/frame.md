# Frames

## In one paragraph

A frame is the outermost TLV a link carries: a header, a payload, and an optional trailer. The
header is `type`, `opt` and `length`, 4 bytes, or 6 when the payload needs a 32-bit length. The
payload is the region the application's bytes live in, and nothing is ever interleaved with it.
The trailer, after the payload, holds a wire time and a CRC when `opt` asks for them. A link adds
the trailer as it sends and strips it as it receives, so the payload is byte-identical at rest,
in transit and at rest again on the next node. That is what lets a forwarder or a recorder keep
the bytes it was handed instead of re-encoding them.

## The picture

```{mermaid}
:caption: "Legend: sky blue = record, blue = frame."
flowchart LR
    accTitle: The same payload at rest, in transit and at rest again
    accDescr: At rest on node A a value is header plus payload. Sending appends a trailer with wire time and CRC, giving a frame in transit. Receiving on node B verifies the CRC and strips the trailer, leaving the same header and payload at rest. A forwarder strips and re-appends a fresh trailer.
    R1["at rest on A: header, payload"]:::record -->|"send: append trailer"| W[/"in transit: header, payload, trailer"/]:::frame
    W -->|"receive: verify, strip"| R2["at rest on B: header, payload"]:::record
    R2 -->|"forward: fresh trailer"| W2[/"in transit to C"/]:::frame
```

## When you need it

You need the frame boundary whenever you move bytes: in a transport that finds where one frame
ends, in a recorder that stores frames, and in a forwarder that must re-time or re-check them. An
application sets the trailer bits only when its link has no integrity check of its own, or when
a receiver wants the time the frame left the sender.

## How it works

**Header.** `type` is one byte, the record's code. `opt` is one byte of flags
([the opt byte](flags.md)). `length` is the payload's byte count, little-endian, `u16` by default
or `u32` when `opt.LL` is set; the payload therefore starts at offset 4 or 6. The header is
packed, so a reader must not assume `length` is aligned.

**Payload.** `length` bytes, opaque or a run of child TLVs ([records](record.md)). Because the
length is fixed-width, a reader knows the payload's extent after reading the header and can
hand the whole region to DMA or skip it in one step.

**Trailer.** At most two fields, in a fixed order: a wire time, then a CRC.

| Field | Present when | Bytes |
| --- | --- | --- |
| wire time, absolute `u64` ns since the Unix epoch | `opt.TS`, `opt.TF` clear | 8 |
| wire time, relative `i32` ns from the parent's | `opt.TS` and `opt.TF` | 4 |
| CRC-32C | `opt.CR`, `opt.CW` clear | 4 |
| CRC-16-CCITT | `opt.CR` and `opt.CW` | 2 |

The CRC covers the payload and then the wire time, never the header. A corrupted time is
therefore a CRC failure, not a wrong time.

**Wire time is not sample time.** The trailer's time says when the frame left a sender, and it is
replaced at every hop. The time a sample was taken is a `TIME` record inside the payload, which
no hop touches.

**Validation is per level.** Ingress checks the top header and the CRC. A level of children is
checked where it is consumed, so a forwarder reads only the headers it needs.

## Worked example

The smallest frame is an empty `STATUS`, which reads as OK: a header and nothing else.

```{hex-walk} framing/empty-status-ok
:caption: The minimum frame.
hex: the empty payload, STATUS = OK
```

The same three-byte opaque `VALUE`, in transit with an absolute wire time and a CRC-32C. Open the
row to see the 12-byte trailer after the payload.

```{hex-walk} crc/value-ts-abs-crc32c
:caption: A value with both trailer fields.
hex: three opaque payload bytes
```

At rest on the receiver it is a header and a payload again, with the same three payload bytes.

## Cost

### Wire bytes

| Part | Bytes |
| --- | ---: |
| header, `u16` length (payload up to 65,535 bytes) | 4 |
| header, `u32` length | 6 |
| trailer, none to all | 0, 2, 4, 6, 8, 10 or 12 |

A 1 KiB payload in transit with an absolute time and a CRC-32C is 4 + 1,024 + 12 = 1,040 bytes;
a 16 KiB one is 16,400 bytes; a 64 KiB one needs the wide header and is 65,554 bytes.

### Allocations

Decoding a frame allocates nothing: the payload is a span into the received buffer. Appending
a trailer at egress writes 0 to 12 bytes after the payload; stripping one at ingress is a length
change. A receiver checks `length` against the bytes it holds before it reads or reserves any of
them.

### Latency

Not benched on this page. The CRC is the one per-byte cost: software CRC-32C is about 2 cycles a
byte on a Cortex-M with a 256-entry table, and the x86 and ARMv8 CRC instructions are used where
the CPU has them. `bench_codec` measures decode and encode over the conformance vectors.

### Flash and RAM per target

| Target | Flash | RAM |
| --- | --- | --- |
| NARROW | the codec is a required module of the gated Cortex-M0 node; CRC-32C table 1 KiB with `kCrc32cSlices = 1` | none of its own |
| MID | inside the reported ESP-IDF core; slice-by-8 tables 8 KiB by default | none of its own |
| WIDE | not measured | none of its own |

## What the library does not do

- It does not stamp a wire time from a clock of its own. A producer that stamps supplies the
  clock through `wire_clock_t`; a producer that does not pays nothing.
- It does not carry a frame's trailer onward. A forwarder strips it and adds its own.
- It does not version a frame or prioritise it. Priority belongs to the egress link.

## Pitfalls

- **Symptom:** every frame from a conforming peer fails its CRC. **Cause:** your CRC covers the
  header too. **Fix:** fold only the payload, then the wire time.
- **Symptom:** sample times include each hop's queueing delay. **Cause:** the application reads
  the trailer's wire time as acquisition time. **Fix:** carry acquisition time in a `TIME` record
  inside the payload.
- **Symptom:** a 60-byte datagram claiming a 4 GiB payload exhausts the receive pool. **Cause:**
  the buffer was sized from `length` before checking it. **Fix:** check `length` against the
  bytes actually received first.

## Where the rules live

**Normative source:** [v1 §3](../../spec/v1.md#3-wire-format), which incorporates
[reference 01 §Frame layout](../../reference/01-data-format.md#frame-layout), its
[CRC](../../reference/01-data-format.md#crc), its
[timestamp form](../../reference/01-data-format.md#timestamp-form) and its
[truncation handling](../../reference/01-data-format.md#truncation-handling).

**Exercised by:** `empty-status-ok`, `value-ts-abs-crc32c`, `value-crc16`, `value-crc32c`,
`value-ll-u32`, `frame_test`, `crc_soft_test`.

Next concept: [Records](record.md). Go deeper:
[wire format, bit by bit](../../modules/wire-format-bits.md), then the
[frame codec](../../modules/frame-codec.md), then
[reference 01](../../reference/01-data-format.md#frame-layout).
