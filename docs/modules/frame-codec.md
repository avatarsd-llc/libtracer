# frame-codec — the TLV wire codec (L2/L3)

```{admonition} In one paragraph
:class: tip
The codec turns wire bytes into a borrowed `tlv_t` tree and back. A TLV is a 4- or
6-byte header (type, an `opt` bitfield, a length) + payload + an optional trailer
(timestamp, CRC). **`decode`** never copies payloads — they are `std::span`s into
the input buffer; **`encode`** serializes a `tlv_t` and recomputes the CRC. The
[bit-level walkthrough](wire-format-bits.md) shows every bit.
```

## What it does

`decode(bytes) → std::expected<tlv_t, error_t>` parses exactly one TLV that fills the
input: it reads the header, rejects reserved bits and bad structure, verifies the
trailer CRC, and — when `opt.PL=1` (payload-is-structured) — walks child TLVs
**iteratively** with a depth cap of 32 (no recursion blow-ups). The result borrows
the input, so holding it requires keeping the bytes alive (that is what
[views](views.md) provide). `encode(tlv)` does the reverse, recomputing the CRC
over the body when `opt.CR` is set.

```{mermaid}
flowchart TD
    H["read header:<br/>type · opt · length"] --> V{"bounds ok?<br/>reserved bits zero?"}
    V -->|no| E["error_t"]
    V -->|yes| P{"opt.PL?"}
    P -->|"1 — structured"| C["push children region<br/>(depth cap 32)"]
    P -->|"0 — opaque"| O["payload = span into input"]
    C --> T["verify trailer CRC"]
    O --> T
    T --> N{"more bytes<br/>in region?"}
    N -->|yes| H
    N -->|no| D["tlv_t tree (borrowed)"]
```

The `opt` byte is the protocol's compactness lever: six 1-bit flags select
structure, trailer contents, and field widths, so the common frame is just **4
bytes** of header. CRCs are **CRC-32C** (default) or CRC-16-CCITT, both
`constexpr` tables built at compile time.

## Interface

```cpp
enum class type_t : std::uint8_t { VALUE=0x01, NAME=0x02, /*…*/ STATUS=0x09, ROUTER=0x0D };
struct opt_t { bool pl, ts, cr, ll, cw, tf;            // the 6 option bits
             std::uint8_t encode() const; static opt_t decode(std::uint8_t); };

struct tlv_t {
    type_t type;  opt_t opt;
    std::span<const std::byte> payload;                // opaque TLVs (borrowed)
    std::vector<tlv_t> children;                       // structured TLVs (opt.PL=1)
    std::optional<trailer_t> trailer;                  // {timestamp_t?, crc_t?}
};

std::expected<tlv_t, error_t> decode(std::span<const std::byte>);   // borrowed, depth-capped
std::vector<std::byte>    encode(const tlv_t&);                 // recomputes CRC

namespace crc { std::uint32_t crc32c(...); std::uint16_t crc16_ccitt(...); }   // constexpr
```

## Frame shape

```text
 byte:  0      1        2   3            4 … (4+len-1)        … trailer …
       ┌──────┬────────┬───────────────┬───────────────────┬─────────────────┐
       │ type │  opt   │ length (u16)  │ payload           │ [timestamp][crc]│
       └──────┴────────┴───────────────┴───────────────────┴─────────────────┘
                  │       (u32 if LL=1)   opaque bytes, OR        TS? then CR?
                  │                       concatenated child
                  │                       TLVs when PL=1
        opt bits (MSB→LSB):  R · PL · TS · CR · LL · CW · TF · R
                             (bits 7 and 0 are reserved-must-be-zero)
```

## The terminus arena sibling

Alongside the owning `tlv_t` model, the codec ships a second decoder for the FWD
terminus: **`wire::decode_into(span, std::pmr::memory_resource&) → tlv_arena_t`**
(public header `tlv_arena.hpp`). It parses the same frames with the same
validation (bounds, reserved bits, depth cap, trailer CRC), but the result is a
**flat, pre-order array of `arena_tlv_t` span-nodes** — `{type, opt, wire
(trailer-excluded), body, end, canonical_path}` — every span borrowing the input
frame, every node drawn from the injected pmr resource. Zero-copy, no owning
vectors; a resolve-scoped view, never stored. The codec's `tlv_t` model,
`decode`, and `encode` are unchanged alongside it.

## Benefits

- **Self-describing & compact** — one 4-byte header covers the common case; the
  `opt` bits opt into width/trailer only when needed.
- **Decode = cast, not copy** — payloads are spans; structured TLVs are sub-spans
  of the same buffer. Pairs with [views](views.md) for true zero-copy.
- **Bounded & safe** — fixed-width length (no varint ambiguity), iterative parse
  with a depth cap, CRC verified before you trust the bytes.

## API reference

Generated from `core/include/libtracer/frame.hpp` by Doxygen.

```{doxygenstruct} tr::wire::tlv_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::wire::trailer_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::wire::decode
:project: libtracer
```

```{doxygenfunction} tr::wire::encode
:project: libtracer
```

See the [reference data-format](../reference/01-data-format.md) for the normative
rules and [wire-format-bits](wire-format-bits.md) for worked byte dumps.
