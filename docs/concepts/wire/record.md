# Records

## In one paragraph

A record is one TLV: a type code that says what it means, a header, and a payload. The payload
is opaque bytes or, when `opt.PL` is set, a run of child records and nothing else. There is no
list type and no generic container: a structured record's own type code says what its children
are, so `POINT` holds a tree of values, `STATUS` holds an outcome, and `FWD` holds an operation, a
destination, a return route and a payload. Codes `0x01` to `0x1F` are the core protocol, `0x20`
to `0x7F` are held back for future core types, and `0x80` to `0xFF` belong to applications. A
receiver that meets a code it does not implement steps over it by its length, so new codes can be
added without breaking deployed nodes.

## The picture

```{mermaid}
:caption: "Legend: sky blue = record."
flowchart LR
    accTitle: Opaque and structured records
    accDescr: A record's opt.PL bit chooses its payload. With PL clear the payload is opaque bytes whose meaning is the type code's and the application's. With PL set the payload is child records only, each of which is again opaque or structured.
    R["record: type, opt, length"]:::record --> P{"opt.PL?"}
    P -->|"0"| O["opaque payload: bytes"]:::record
    P -->|"1"| C1["child record"]:::record
    P -->|"1"| C2["child record"]:::record
    C2 -->|"opt.PL = 1"| G["grandchild record"]:::record
```

## When you need it

When you read a capture, write a decoder, or define your own application record in the user
range. The type code tells you whether a record's children are meaningful to the protocol; a
user-range code is the application's to define, opaque or structured.

## How it works

**The type code.** One byte at offset 0. Its ranges are in the registry; `0x00` is never a
record, so a run of zeros reads as corruption rather than as a frame.

| Range | Holds | Examples |
| --- | --- | --- |
| `0x01`–`0x1F` | the core protocol | `VALUE` `0x01`, `NAME` `0x02`, `PATH` `0x06`, `POINT` `0x07`, `STATUS` `0x09`, `TIME` `0x0C`, `FWD` `0x0F` |
| `0x20`–`0x7F` | future core types | none assigned |
| `0x80`–`0xFF` | applications | `BATCH` `0x80`, the one assigned code ([batching](../batching.md)) |

The full list, with each record's layout, is
[reference 05](../../reference/05-protocol-tlvs.md#type-code-allocation-summary); it is not
copied here.

**Opaque or structured.** `opt.PL` is the only thing that says whether a payload is child
records. The same 12 payload bytes are two children with the bit set and one opaque blob with it
clear, so a reader never guesses from the bytes. Which core types are structured is listed in
[reference 05 §Structured TLVs](../../reference/05-protocol-tlvs.md#structured-tlvs). No address
form is structured: a `PATH` body is a packed run of small records of its own
([addresses](address.md)).

**Walking children.** A structured payload is walked with the same header read as the frame:
read 4 or 6 bytes, skip the payload and trailer, repeat until the parent's length is used up. A
nested record pushes one entry on an explicit work stack instead of recursing. The wire sets no
nesting limit; a receiver refuses what its own decode budget cannot hold, as
`tr::tlv::nesting_too_deep`, and the protocol's own record shapes nest at most five deep, so
every conforming receiver reads every protocol frame.

**Unknown codes.** A record whose code a receiver does not implement is skipped by its length,
and its CRC is still checked. Nested, it is opaque bytes to that receiver; as the outer addressed
record, it is answered with `tr::schema::type_mismatch` when there is a return route. A
forwarder may pass it through unchanged. The exceptions are the retired codes, which carry
rules of their own.

## Worked example

The pair below differs in one bit. With `opt.PL` set, the 12 payload bytes are a `VALUE` and a
`NAME`:

```{hex-walk} framing/pl-bit-structured
:caption: Twelve bytes read as two children.
hex: two opaque value bytes
str: a name
```

With `opt.PL` clear, the very same 12 bytes are one opaque payload:

```{hex-walk} framing/pl-bit-opaque
:caption: The same twelve bytes, opaque.
hex: twelve bytes, no children
```

An encoder whose `opt` mask is shifted by one bit turns one of these into the other, which is
why the pair is a conformance vector.

## Cost

### Wire bytes

Every record pays its own 4-byte header, children included. A structured record with N children
costs `4 + Σ(4 + child payload)` before trailers. Ten one-byte values cost 54 bytes framed as
children of a `POINT`, against 14 bytes for the same ten bytes as one opaque payload. One opaque
1 KiB payload spends under 0.4% on its header, while 1 KiB of 2-byte children spends 4 bytes on
each of 512 headers: 3,076 bytes in all. The packed sample block of a
[batch](../batching.md) is the protocol's answer for many small fixed-width values.

### Allocations

None to walk. A child is a span into its parent's payload; the work stack holds one entry per
open level and is drawn from the receiver's injected source. A materializing decoder, such as
the terminus arena, also holds one node per record, so a wide frame costs nodes as a deep one
costs stack.

### Latency

Not benched on this page. A structured payload's cost grows with its record count, not its byte
count, because each child is one header read; `bench_codec` measures it over the structured
conformance vectors.

### Flash and RAM per target

| Target | Flash | RAM |
| --- | --- | --- |
| NARROW | part of the gated Cortex-M0 codec | one work-stack entry per open level, one node per record when materialized |
| MID | part of the reported ESP-IDF core | same |
| WIDE | not measured | same |

## What the library does not do

- It does not offer a generic list or container type. A structured record's type code states its
  purpose.
- It does not interpret a user-range record. Applications define `0x80` to `0xFF`, and a node that
  does not know one moves it unchanged.
- It does not limit nesting with a constant. The bound is each receiver's own decode budget.

## Pitfalls

- **Symptom:** a parser rejects frames a conforming peer sends, but passes its own round trips.
  **Cause:** it hardcodes a nesting limit, often 32. **Fix:** bound the work stack by the decode
  budget you inject and report `tr::tlv::nesting_too_deep`.
- **Symptom:** a wide but shallow frame runs a decoder out of memory half way through.
  **Cause:** the budget covered open levels but not one node per record. **Fix:** budget for
  both, and refuse before committing anything.
- **Symptom:** a new peer's frames break an old node. **Cause:** the old node rejects unknown codes
  instead of stepping over them. **Fix:** skip by `length` and keep parsing.

## Where the rules live

**Normative source:** [v1 §3](../../spec/v1.md#3-wire-format), which incorporates the
[type code registry](../../reference/01-data-format.md#type-code-registry), the
[unknown-code rules](../../reference/01-data-format.md#handling-unknown-type-codes) and the
[iterative parsing requirement](../../reference/01-data-format.md#iterative-parsing-requirement)
of reference 01, and the per-type layouts of
[reference 05](../../reference/05-protocol-tlvs.md#type-code-allocation-summary).

**Exercised by:** `pl-bit-structured`, `pl-bit-opaque`, `description-utf8`, `tlv_children_test`,
`tlv_arena_test`.

Next concept: [The opt byte](flags.md). Go deeper:
[wire format, bit by bit](../../modules/wire-format-bits.md), then
[reference 05](../../reference/05-protocol-tlvs.md#type-code-allocation-summary), then
[v1 §3](../../spec/v1.md#3-wire-format).
