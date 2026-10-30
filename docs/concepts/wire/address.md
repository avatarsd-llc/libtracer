# Addresses

## In one paragraph

An address is a `PATH` record whose body is a list of path elements, read head first. A path
element is a NAME or a PAIR. A NAME is one segment of the string path, encoded as a segment
record: a length byte and that many bytes of UTF-8, so `/sensor/temp` is `06 sensor 04 temp`. A
PAIR is one node's owner-issued `(index, generation)` for a vertex, encoded as an 11-byte escape
record that starts with a zero byte. The string path is the truth and a PAIR chain is a learned
cache of it, re-learned whenever it goes stale. Because a canonical body has exactly one spelling,
its bytes are the graph's lookup key: a node finds a vertex by comparing the frame's own bytes,
without building a string.

## The picture

```{mermaid}
:caption: "Legend: sky blue = record."
flowchart LR
    accTitle: The two kinds of path element
    accDescr: A PATH body is a list of path elements. Each one is either a NAME, encoded as a segment record of a length byte and UTF-8, or a PAIR, encoded as an escape record of zero, kind 0x16, length 8, a u32 index and a u32 generation. A canonical key holds only NAMEs; a frame path may mix both.
    P["PATH, opt.PL = 0"]:::record -->|"body: elements, head first"| E{"element kind?"}
    E -->|"first byte 1 to 64"| N["NAME: segment record, len then UTF-8"]:::record
    E -->|"first byte 0, kind 0x16"| R["PAIR: escape record, index then generation"]:::record
    N -->|"canonical key"| K["vertex lookup by byte compare"]:::record
    R -->|"frame path only"| F["read by exactly one node"]:::record
```

## When you need it

Every operation has an address, and every remote one has two: `dst`, the route to the target,
and `src`, the route back. Read this page when you encode paths by hand, read routes in a
capture, place pre-encoded paths in flash on an MCU, or wonder why a frame shrank between hops.

## How it works

**Segment records.** A NAME is `[u8 len][len bytes of UTF-8]`, `len` from 1 to 64, with no
terminator and no separator. The walk over a body is one load and one add per record. An empty
body is the root, `/`. A path has at most 255 segments and at most 1,024 body bytes; for short
segments the count binds first.

**Escape records.** A record whose length byte is zero is an escape: `00`, a kind byte, a length
byte, then that many bytes. Kind `0x16` is the PAIR, always 8 bytes: a `u32` index into the
owning node's vertex table and a `u32` generation that moves on when the vertex retires. A node
that does not implement a kind steps over the record by its declared length. Kind `0x16` shares a
number with no type code; the two are separate namespaces.

**Canonical key or frame path.** A canonical key, such as a stored subscription target or a
static path handle, holds only NAMEs, and an escape in one is refused. A frame path, the `dst` or
`src` of a frame in flight, may mix NAMEs and PAIRs in any order. A malformed element refuses the
address, not the frame that carries it.

**A PAIR is an address, not a capability.** It means something only on the node whose table it
indexes, and only that node reads it. The node checks the index's bounds, compares the generation
and checks registration; a PAIR it cannot validate is answered with `tr::path::not_found`, and
the origin falls back to the canonical string it still holds. Access is checked against the
vertex the PAIR reaches, so a matching generation authorizes nothing.

**Hop by hop.** Each forwarding hop consumes its own head element of `dst`, so the frame shrinks
toward the target, and prepends its own part of the route to `src`, so a reply can retrace it. A
hop prepends a PAIR when it can issue one and its NAME run otherwise. Learning a chain adds no
frame and no flag: the origin reads it off a success reply's `src`.

**Static handles.** Because a canonical body has one spelling, a path can be encoded at build time
and kept in read-only memory. A write through such a handle is byte-identical on the wire to a
write through the string. The [static path handles](../../reference/03-addressing.md#static-path-handles-mcu-friendly-addressing)
section shows how.

## Worked example

`/sensor/temp` as a canonical `PATH`: two segment records, 12 body bytes, 16 bytes in all.

```{hex-walk} path/path-sensor-temp
:caption: A canonical path, two NAME elements.
path: the address
```

A remote `READ` whose `dst` is one PAIR, the receiving node's own handle for that vertex. It is
the last element, so this node is the terminus. The frame is 37 bytes, against 38 for the same
operation spelled `/sensor/temp`; the saving grows with every segment a PAIR replaces.

```{hex-walk} fwd/pair-terminus
:caption: A destination spelled as a PAIR.
u8: op, 0 = READ
path: dst, one PAIR element
path: src, the route back
```

## Cost

### Wire bytes

| Element | Bytes |
| --- | ---: |
| NAME of `len` bytes | `1 + len` |
| PAIR | 11 |
| `PATH` header | 4 |

A PAIR saves bytes once the local part it stands for is longer than 11 bytes, such as a mount run
`net/<module>/<name>`. Addresses do not grow with the payload: a write to `/sensor/temp` with
the return route `/reply-ep` spends 29 bytes on its two addresses whatever the value, under 3% of
the frame for a 1 KiB value and under 0.2% for a 16 KiB one.

### Allocations

None to resolve a canonical path: the body is the lookup key, compared in place. A PAIR is a
bounds check and two `u32` reads. A forwarding hop rebuilds the two route headers and gathers
everything else as windows into the received frame.

### Latency

Not benched on this page. A PAIR replaces a key lookup with an index, so its gain grows with the
number of segments it replaces. `bench_chain_delivery_pair` prices stream delivery over a PAIR
chain at 1 and 3 hops; no bench sets the PAIR spelling of one address against its NAME spelling,
and a pair of arms in `bench_codec` would.

### Flash and RAM per target

| Target | Flash | RAM |
| --- | --- | --- |
| NARROW | the path module is required in the gated Cortex-M0 node; static handles live in flash | none per path in flight |
| MID | inside the reported ESP-IDF core | same |
| WIDE | not measured | same |

## What the library does not do

- It does not use a global device name or a correlation id. A reply's route is the `src` the
  request accumulated.
- It does not keep per-request or hard state in a forwarding hop. The route is in the frame.
- It does not withdraw, lease or time out a PAIR. A stale one fails its generation check and the
  origin falls back to the string.
- It does not treat a PAIR as permission. Access is checked at the vertex it reaches.

## Pitfalls

- **Symptom:** a stored key or a static handle is refused. **Cause:** it contains an escape
  record. **Fix:** keep canonical keys to NAMEs; PAIRs belong in frame paths only.
- **Symptom:** an address is refused even though the bytes add up. **Cause:** the records do not
  tile the body: one runs past its end. **Fix:** count `1 + len` per record and match `length`.
- **Symptom:** a path's byte limit is hit earlier or later than expected. **Cause:** it was counted
  as segments plus separators. **Fix:** count the encoded body, which is the `PATH` `length` field.

## Where the rules live

**Normative source:** [v1 §3](../../spec/v1.md#3-wire-format), which incorporates
[reference 05 §0x06 PATH](../../reference/05-protocol-tlvs.md#0x06--path), its
[PAIR routing semantics](../../reference/05-protocol-tlvs.md#path-element-pair-escape-kind--0x16--routing-semantics)
and [reference 03 §Path syntax](../../reference/03-addressing.md#path-syntax); handles are
[v1 §3.1](../../spec/v1.md#31-static-path-handle-conformance).

**Exercised by:** `path-sensor-temp`, `path-deep-255-packed`, `pair-terminus`, `pair-hop-egress`,
`pair-stale-generation-not-found`, `path_test`, `path_pair_test`.

Next concept: [The illustrated frame](illustrated-frame.md). Go deeper:
[reference 03 §One path form, two element kinds](../../reference/03-addressing.md#one-path-form-two-element-kinds),
then [reference 05 §0x06 PATH](../../reference/05-protocol-tlvs.md#0x06--path), then
[v1 §3](../../spec/v1.md#3-wire-format).
