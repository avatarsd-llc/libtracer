# The opt byte

## In one paragraph

Every record's second byte is `opt`, eight bits that say which optional parts the record carries
and how to read its payload. Six bits have a meaning: `PL` says the payload is child records,
`LL` widens the length to 32 bits, `TS` and `TF` add an absolute or relative wire time to the
trailer, and `CR` and `CW` add a 32-bit or 16-bit CRC. Bits 7 and 0 are reserved for the life of
v1, and a record with either set is invalid. Each part costs bytes only when its bit is set, so a
record with `opt` zero is a 4-byte header and its payload. The byte has no version bit, no
priority bit and no room to grow: new meaning goes into new type codes, never into `opt`.

## The picture

```{mermaid}
:caption: "Legend: sky blue = record."
flowchart LR
    accTitle: What each opt bit adds
    accDescr: The opt byte, bits 7 to 0, R PL TS CR LL CW TF R. PL makes the payload child records. LL widens the length field from 2 to 4 bytes. TS adds a wire time to the trailer, which TF makes relative. CR adds a CRC to the trailer, which CW makes 16-bit. The two R bits are reserved and refuse the record.
    O["opt: R PL TS CR LL CW TF R"]:::record -->|"PL"| P["payload = child records"]:::record
    O -->|"LL"| L["length u32: header 6 bytes"]:::record
    O -->|"TS, TF picks the form"| T["trailer time: u64, or i32 relative"]:::record
    O -->|"CR, CW picks the width"| C["trailer CRC: CRC-32C, or CRC-16"]:::record
    O -->|"bit 7 or bit 0"| X["record invalid"]:::record
```

## When you need it

Set `opt` bits when you build records by hand, in a binding or a test. The C++ API sets them
for you: `opt_t::encode` and `opt_t::decode` turn the bits into a struct and back, and the
encoder picks `LL` from the payload's size. Read this page when a peer refuses your frames, or
when you choose between CRC-32C and CRC-16 for a link.

## How it works

| Bit | Name | Clear (the default) | Set |
| ---: | --- | --- | --- |
| 7 | R | required | the record is invalid |
| 6 | `PL` | payload is opaque bytes | payload is child records only |
| 5 | `TS` | no wire time | the trailer starts with a wire time |
| 4 | `CR` | no CRC | the trailer ends with a CRC |
| 3 | `LL` | `u16` length, header 4 bytes | `u32` length, header 6 bytes |
| 2 | `CW` | CRC-32C, 4 bytes | CRC-16-CCITT, 2 bytes |
| 1 | `TF` | absolute `u64` time, 8 bytes | relative `i32` time, 4 bytes |
| 0 | R | required | the record is invalid |

**Pairs.** `CW` means something only with `CR`, and `TF` only with `TS`. With its partner clear
the bit changes nothing about the bytes.

**Per record, not per frame.** Each record in a frame has its own `opt`. A structured parent can
carry one absolute time while each child carries a 4-byte relative one, read from the nearest
ancestor that has a time. A relative time with no such ancestor names no time; the party that
reads it as a time refuses it, while a decoder and a relay carry it as they found it. The
reference writer does not mint relative times today: the form is specified and relayed, kept for
later use.

**Reserved means refused.** A receiver does not mask the two reserved bits off and carry on; it
refuses the record. Because the bits stay reserved for all of v1, a receiver can make that check
at compile time.

**The CRC choice.** CRC-32C misses about one corruption in 2<sup>32</sup>, CRC-16 about one in
65,536, and CRC-16 saves 2 bytes per record. It suits short records on a bus that checks its own
frames, such as CAN or a UART with framing; elsewhere CRC-32C is the default.

## Worked example

One frame with every meaningful bit set somewhere. The outer `POINT` has `opt` `0x60`: `PL` and
an absolute `TS`. Its child `POINT` has `opt` `0x7e`: every meaningful bit, so a 6-byte header,
a relative time and a CRC-16 in its trailer. Open each row to see its header fields and trailer.

```{hex-walk} framing/opt-all-bits-nested
:caption: Every meaningful opt bit, in one frame.
str: a one-letter name
```

## Cost

### Wire bytes

| Bits set | Adds |
| --- | ---: |
| none | 0 |
| `LL` | 2 (header) |
| `TS` | 8 (trailer) |
| `TS` and `TF` | 4 (trailer) |
| `CR` | 4 (trailer) |
| `CR` and `CW` | 2 (trailer) |
| all six | 2 + 4 + 2 = 8 |

The largest addition is 14 bytes (`LL`, absolute `TS`, CRC-32C). On a 1 KiB payload that is 1.4%;
on 16 KiB it is under 0.1%. `LL` is not optional above 65,535 bytes of payload.

### Allocations

None. `opt` is one byte read in place; the trailer it describes is written into the bytes after
the payload at egress and dropped by a length change at ingress.

### Latency

Not benched on this page. Reading `opt` is one load and a mask; the only per-byte cost it switches
on is the CRC, described on [Frames](frame.md).

### Flash and RAM per target

| Target | Flash | RAM |
| --- | --- | --- |
| NARROW | part of the gated Cortex-M0 codec; `kCrc32cSlices = 1` keeps the CRC-32C table at 1 KiB | none |
| MID | part of the reported ESP-IDF core | none |
| WIDE | not measured | none |

## What the library does not do

- It does not carry a version or a priority in `opt`. Versions live at discovery; priority lives on
  the egress link.
- It does not let applications borrow the reserved bits. Extensions use type codes.
- It does not set `TS` unless the producer supplies a clock and asks for a stamp.

## Pitfalls

- **Symptom:** every record you send is refused as invalid. **Cause:** a shifted `opt` mask sets
  bit 7 or bit 0. **Fix:** test against `reserved-bit7-reject` and `reserved-bit0-reject`.
- **Symptom:** a structured payload reads as opaque, or the other way round. **Cause:** the same
  shifted mask moved `PL`. **Fix:** test against the `pl-bit-structured` and `pl-bit-opaque` pair.
- **Symptom:** times read back near 1970. **Cause:** a relative time with no anchor was read as an
  offset from zero. **Fix:** refuse an anchorless relative time where you read it as a time.

## Where the rules live

**Normative source:** [v1 §3](../../spec/v1.md#3-wire-format), which incorporates
[reference 01 §Options bitfield](../../reference/01-data-format.md#options-bitfield) and its
[length encoding](../../reference/01-data-format.md#length-encoding).

**Exercised by:** `opt-all-bits-nested`, `reserved-bit7-reject`, `reserved-bit0-reject`,
`relative-ts-nested`, `tf1-reserved`, `value-crc16`, `frame_test`.

Next concept: [Addresses](address.md). Go deeper:
[the opt byte, bit by bit](../../modules/wire-format-bits.md#the-opt-byte-bit-by-bit), then the
[frame codec](../../modules/frame-codec.md), then
[reference 01 §Options bitfield](../../reference/01-data-format.md#options-bitfield).
