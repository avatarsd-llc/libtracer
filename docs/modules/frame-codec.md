# frame-codec — the TLV wire codec (L2/L3)

```{admonition} In one paragraph
:class: tip
The codec turns wire bytes into a borrowed `tlv_t` tree and back. A TLV is a 4- or
6-byte header (type, an `opt` bitfield, a length) + payload + an optional trailer
(timestamp, CRC). **`decode`** never copies payloads — they are `std::span`s into
the input buffer; **`encode`** serializes a `tlv_t` and recomputes the CRC. The
[bit-level walkthrough](wire-format-bits.md) shows every bit.
```

## Decode and encode

`decode(bytes) → std::expected<tlv_t, err_t>` parses exactly one TLV that fills the
input: it reads the header, rejects reserved bits and bad structure, verifies the
trailer CRC, and — when `opt.PL=1` (payload-is-structured) — walks child TLVs
**iteratively**, never recursively. The result borrows the input, so holding it
requires keeping the bytes alive (that is what [views](views.md) provide).
`encode(tlv)` does the reverse, recomputing the CRC over the body when `opt.CR` is
set. It does **not** take the length width from the model verbatim: a body over
`0xFFFF` widens to the u32 `LL` form whatever `tlv.opt.ll` says, because `encode`
emits through `emit_tlv` (below) — the one home of the length-width policy. A
programmatically built tree therefore cannot serialize a length truncated to
`size & 0xFFFF`; bodies at or under `0xFFFF` are unchanged and `opt.ll` is never
cleared. Decode failure is one of `FRAME_TRUNCATED`, `FRAME_INVALID`, `FRAME_CRC_FAIL`
or `TLV_NESTING_TOO_DEEP` — the RFC-0002 registry codes, not a decode-only error
vocabulary (`core/include/libtracer/frame.hpp:26-31`).

```{mermaid}
flowchart TD
    H["read header:<br/>type · opt · length"] --> V{"bounds ok?<br/>reserved bits zero?"}
    V -->|no| E["err_t"]
    V -->|yes| P{"opt.PL?"}
    P -->|"1 — structured"| C["push children region<br/>onto the open-node stack"]
    P -->|"0 — opaque"| O["payload = span into input"]
    C --> T["verify trailer CRC"]
    O --> T
    T --> N{"more bytes<br/>in region?"}
    N -->|yes| H
    N -->|no| D["tlv_t tree (borrowed)"]
```

The `opt` byte is the protocol's compactness lever: six 1-bit flags select
structure, trailer contents, and field widths, so the common frame is just **4
bytes** of header. Bits 7 and 0 are reserved-MUST-be-zero; a set reserved bit makes
the frame invalid (`opt_t::kReservedMask`, `core/include/libtracer/tlv.hpp`).

### Trailer CRCs

Trailer CRCs are **CRC-32C** (Castagnoli, reflected poly `0x82F63B78`, the default)
or **CRC-16-CCITT (FALSE)** (poly `0x1021`, init `0xFFFF`, no final xor) when
`opt.CW=1`. Both tables are `constexpr` and built at compile time — `crc32c_table`
and `crc16_table`, `core/include/libtracer/crc.hpp:38,51` — so a constant-evaluated
CRC needs no runtime table build.

Compile-time tables are not the whole runtime story. CRC-32C dispatches once, on
first use, to the SSE4.2 `_mm_crc32_*` or ARMv8 `__crc32c*` instruction where the
CPU carries it, and folds through a portable slice-by-8 table otherwise; all three
paths — hardware, slice-by-8, byte-at-a-time — produce byte-identical checksums,
which is what lets frozen test vectors hold across targets
(`crc32c_update_runtime`, `crc.hpp:168-180`). The intrinsics are confined to a
`target`-attributed function so the translation unit stays runnable on a CPU
without the extension. The slice-by-8 tables are 8 × 256 × `u32` = 8 KiB of rodata
that a hardware-CRC CPU never touches (`crc32c_slice_tables`, `crc.hpp:71-83`) —
the one footprint line a constrained target should know about here.

`crc32c` and `crc16_ccitt` each take one span or two, and a `crc32c_state` /
`crc16_ccitt_state` accumulator takes any number of chunks: a CRC over a
payload-plus-trailer region, or over a rope crossing link boundaries, is computed
without concatenating the pieces into a fresh buffer. Trailer CRC placement is
ADR-0004, [CRC in optional trailer][adr-0004].

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

## Nesting depth

Nesting depth is bounded by the **receiver's decode resources, never by a
constant**. No depth constant exists in the codec, and an implementation that
hardcodes one is not implementing the rule (RFC-0006, [resource-bounded nesting
depth][rfc-0006]).

Both decoders share one structural descent, `grammar::walk`
(`core/include/libtracer/grammar.hpp`, ADR-0048, [one wire grammar][adr-0048]).
Recursion is forbidden there: a malicious deep frame must not overflow a small MCU
call stack, so the walk keeps one open-node record per open level in a
`walk_stack_t`. That stack starts in a caller-supplied inline span and, once those
slots are used, relocates into geometrically grown blocks drawn from a spill
source. **The inline span is a tuning knob, not a limit — overflowing it changes
cost, not behaviour** (`grammar.hpp:363-369`). Exhausting the spill source rejects
the frame with `TLV_NESTING_TOO_DEEP`, which means exactly "exceeds this receiver's
decode resources" (`grammar.hpp:461-465`).

The two decoders differ only in what they spill to, and therefore in what bounds
them:

| decoder | inline slots | spill source | the depth bound is |
| --- | --- | --- | --- |
| `decode` → owning `tlv_t` | 8 (`core/src/frame.cpp:126`) | the nothrow heap source (`frame.cpp:127`) | the heap — an owning-tree decode allocates there regardless |
| `decode_into` → `tlv_arena_t` | 8 (`core/src/tlv_arena.cpp:134`) | the caller's `mem::block_source_t` (`tlv_arena.cpp:135`) | whatever resource the caller injected |

The 8 is the typical FWD nesting (three to four levels) with headroom, not a
ceiling: the arena test decodes a frame nested 100 deep (`core/tests/tlv_arena_test.cpp:324`).
A receiver that wants a hard bound gets one by injecting a small source: a
stack-buffer `mem::bump_source_t` makes that buffer the whole decode budget
(`mem_source.hpp`), and exhaustion is then a returned `err_t` rather than an
allocation failure.

## Interface

```cpp
enum class type_t : std::uint8_t { VALUE=0x01, NAME=0x02, /*…*/ STATUS=0x09, ROUTER=0x0D };

struct opt_t {                                   // the 6 option bits
    bool pl, ts, cr, ll, cw, tf;
    static constexpr std::uint8_t kReservedMask = 0b1000'0001;
    static constexpr bool  reserved_set(std::uint8_t);      // bit 7 or 0 set ⇒ invalid
    static constexpr opt_t decode(std::uint8_t);
    constexpr std::uint8_t encode() const;
    constexpr opt_t without_trailer() const;               // clears TS/CR/CW/TF
};

struct tlv_t {
    type_t type;  opt_t opt;
    std::span<const std::byte> payload;                // opaque TLVs (borrowed)
    std::vector<tlv_t> children;                       // structured TLVs (opt.PL=1)
    std::optional<trailer_t> trailer;                  // {timestamp_t?, crc_t?}
};

std::expected<tlv_t, err_t> decode(std::span<const std::byte>);  // borrowed
std::expected<tlv_t, err_t> decode(const view::view_t&);         // the L1→L2 cast
std::vector<std::byte>      encode(const tlv_t&);                // recomputes CRC
std::vector<std::byte>      path_key(const tlv_t& path);         // canonical PATH key
bool                        equal(const tlv_t&, const tlv_t&);   // spans by content

// tlv_emit.hpp — bytes without a model object
void emit_header(std::vector<std::byte>&, type_t, opt_t, std::size_t body_len);
void emit_tlv   (std::vector<std::byte>&, type_t, opt_t, std::span<const std::byte> body);
void emit_name  (std::vector<std::byte>&, std::span<const std::byte>);
void emit_name  (std::vector<std::byte>&, std::string_view);

// tlv_arena.hpp — the terminus decoder
std::expected<tlv_arena_t, err_t> decode_into(std::span<const std::byte>,
                                              mem::block_source_t&);

namespace tr::crc { constexpr std::uint32_t crc32c(...);        // 1 or 2 spans
                    constexpr std::uint16_t crc16_ccitt(...);
                    struct crc32c_state; struct crc16_ccitt_state; }  // n chunks
```

`decode(const view::view_t&)` is the L1→L2 cast — "a TLV is a cast from a view." It
lives at L2 because it produces a `tlv_t`, and consumes an L1 [view](views.md); the
returned tree borrows the view's bytes, so the view and its segment must outlive
it.

## Byte emission without a model object

`tlv_emit.hpp` appends one TLV — `<type> <opt> <length> <body>` — straight into a
`std::vector<std::byte>`, with no intermediate `tlv_t`. It is the one
representation of the header byte layout (ADR-0048 §3): `encode` and every
structural byte-builder in the tree share it instead of each hand-rolling
`type`/`opt`/little-endian length. `emit_tlv` is also the one home of the
**length-width policy** — `encode` goes through it rather than calling
`emit_header` itself, so there is no second widen rule to drift (#924).

| function | what it appends |
| --- | --- |
| `emit_header(out, type, opt, body_len)` | the header alone; length is `u16` LE, or `u32` LE when `opt.ll` is set. The width follows `opt.ll` verbatim — this writes a header, it does not decide one |
| `emit_tlv(out, type, opt, body)` | header + body, auto-setting `opt.ll` when `body` exceeds `0xFFFF`. `encode` routes through here; the forward plane's `fwd_frame_view` / `stack_writer` tiers carry their own copy of the widen rule |
| `emit_name(out, bytes)` / `emit_name(out, sv)` | a `NAME` TLV with default `opt` — the metadata-tag workhorse (SETTINGS keys, `:schema` labels, `:children[]` members). It is NO LONGER the PATH-segment emitter: RFC-0018 packed a PATH body into `[u8 len][utf8]` records, which `wire::emit_path_segment` (`packed_path.hpp`) writes. The `string_view` form needs no temporary buffer |
| `emit_path_segment(out, seg)` | one packed PATH segment record — `[u8 len][utf8]`, RFC-0018 §5. Returns `false`, appending nothing, on an empty segment (it would spell the §5.4 escape) or one past 255 bytes |
| `emit_path_element(out, element)` | ONE path element, whatever its kind — the byte-exact inverse of `path_element_at` (`path_element.hpp`, [#1347](https://github.com/avatarsd-llc/libtracer/issues/1347)). A `PATH` is a list of **path elements**; an element's kind is NAME or LABEL; a NAME element is encoded as a **segment record**, a LABEL element as an **escape record** — and this switches over the three record emitters so a caller re-spelling a walked body never crosses the two grammars. It lives beside the reader rather than in `packed_path.hpp`, which is deliberately kind-agnostic. Returns `false`, appending nothing, for a `MALFORMED` element and for a LABEL carrying the reserved zero generation |
| `emit_path_ref(out, elements[, type])` | a `PATH_REF` TLV — the 4-byte envelope plus the bare 8-byte element array (RFC-0024 §4). `type` selects which bound-path code heads it: `PATH_REF` (`0x14`, the default) or `PATH_REF_REVERSE` (`0x15`, RFC-0024 §7.1 amendment 2), whose body grammar is identical. Returns `false`, emitting nothing, past the 255-element bound: a route that long has no bound spelling and falls back to the canonical `PATH` |

Building a PATH is `emit_path_segment` per segment into one buffer
(`packed_path.hpp`); that concatenation of packed `[u8 len][utf8]` records is exactly
the canonical PATH key `path_key` produces from a decoded PATH, and — since
[RFC-0018](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0018-packed-path-segments.md)
gave an address exactly one spelling — the body a resolver can key on **in place,
unconditionally**, where a flag on the arena node used to say whether it could. The
PATH's own header carries `opt_t{}`: a packed body is not a child run, so `PL` stays
clear. Building an FWD request is `emit_tlv` for the outer frame over a body built the
same way. Pass `opt_t{.pl = true}` for a structured payload.

These live in `tr::wire` (L2/L3) because they produce wire bytes from wire types;
the layer-free little-endian byte helper they build on stays in `tr::detail`
(`byteorder.hpp`). For decoding, and for emitting a full `tlv_t` value with
payload, children and trailers, `frame.hpp`'s `decode`/`encode` are the entry
points.

## The BATCH record — folding a flush into one written value

`batch.hpp` is the one canonical spelling of
[RFC-0025](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0025-stream-class-values.md)'s
batch convention: `BATCH` is `type_t::BATCH` (`0x80`), the single code assigned inside the
user range, and a batch is otherwise an ordinary structured TLV — **zero new grammar**, so
every conforming decoder above already decodes one and the graph never interprets it.

| function | what it does |
| --- | --- |
| `emit_batch(out, base_ns, samples[, offsets_ns])` | FOLDS N already-encoded sample frames into one record: a `TIME{u64 LE ns}` base child, optionally the non-uniform stream's packed `i32` offset run, then the sample frames' own bytes **verbatim**. A fold is a concatenation under one header, never a re-encode |
| `batch_wire_bytes(samples_bytes[, offsets])` | the whole size function, so a caller sizes the buffer before filling it |
| `read_batch(tlv, dt_ns)` | the reader half: a `batch_view_t` over the base, the offsets and the sample frames |
| `batch_view_t::sample_time_ns(i)` | the SAMPLE clock of frame `i`, **derived**: `base + i × dt_ns` on a uniform stream, `base + offsets[i]` on a non-uniform one |

Two properties are worth stating where a reader will look for them.

**A uniform stream spends 0 bytes per sample on time.** `dt_ns` is the nominal sample period
the stream's *descriptor* declares — a `SETTINGS` LKV beside the data vertex, negotiated once
and never repeated per batch — so nothing per-sample is transmitted and a 4-byte sample costs
4 bytes. That is also why `dt_ns` is a parameter of `read_batch` and not something the record
is asked for: whether the child after the `TIME` is an offset array or the first sample frame
is a fact about the descriptor. A non-uniform stream (`dt_ns == 0`) carries one packed `i32`
run in a single child — 4 bytes per sample, contiguous, no per-child TLV header, no anchor
walk.

**A batch carries no trailer.** Wire/TX time is `opt.TS` on the **outermost** frame only and
is always `TF=0`; a written value is an inner TLV. Sample time is the payload `TIME` child;
playout time is never transmitted at all (the three-clock model of
[01-data-format.md](../reference/01-data-format.md)). `read_batch` declines bytes that do not
spell the convention — a reader's refusal, never the codec's: the same bytes still decode,
still forward and still round-trip.

## The terminus arena decoder

Alongside the owning `tlv_t` model, the codec ships a second decoder for the FWD
terminus: **`wire::decode_into(span, tr::mem::block_source_t&) → tlv_arena_t`**
(public header `tlv_arena.hpp`). It parses the same frames with the same
validation — bounds, reserved bits, type `0x00`, the bound-path (`0x14`/`0x15`) body shape, trailer
CRC, trailing bytes ⇒ `FRAME_INVALID` — but the result is a **flat, pre-order array of `arena_tlv_t`
span-nodes**: `{type, opt, wire (trailer-excluded), body, end}`.
Every span borrows the input frame; every node is drawn from the injected
`block_source_t`.

**The arena contract is structure only, never bytes** (ADR-0041, [terminus arena
decode span contract][adr-0041]). The decode holds structure — node types, option
bits, subtree extents — and the payload bytes are never copied. A borrowed span may
be read, copied once to its owner, or sub-viewed off a refcounted owner; it may
never be stored as a borrowed span. The arena is a resolve-scoped view: read it,
take the ownership copies, drop it.

Subtree navigation is index arithmetic rather than pointer chasing. `end` is one
past the last descendant, so node `i`'s children start at `i + 1` and siblings walk
`j = i + 1; while (j < node[i].end) { visit(j); j = node[j].end; }`. An opaque
node's `end` is its own index + 1.

`decode_into` does not replace `decode`. The owning `tlv_t` model remains the
general codec — it materializes, it outlives its input by copying, and it is what
`encode` round-trips; the arena form is a resolve-scoped view over an inbound
frame, chosen where a decode must not touch the heap. Both are `tr::wire`, both
share `grammar::walk`, and an equivalence test decodes every conformance vector
through each and requires them to agree (`core/tests/tlv_arena_test.cpp`).

Every draw `decode_into` makes is nothrow and guarded, because it runs on the wire
RX path behind no ACL and a peer chooses both the nesting depth and the node count.
Exhaustion answers `TLV_NESTING_TOO_DEEP`, never `std::bad_alloc` — which on a
`-fno-exceptions` node is a link-wrapped `abort()`
(`core/include/libtracer/tlv_arena.hpp:130-134`).

## Consequences

- **Self-describing and compact** — one 4-byte header covers the common case; the
  `opt` bits opt into width and trailer only when needed.
- **Decode is a cast, not a copy** — payloads are spans; structured TLVs are
  sub-spans of the same buffer. Pairs with [views](views.md) for zero-copy.
- **Bounded and safe** — fixed-width length (no varint ambiguity), an iterative
  parse with no recursion and no depth constant, CRC verified before the bytes are
  trusted.
- **The receiver sets its own ceiling** — depth and node count are bounded by the
  resource the receiver injects, so the same codec serves a heap-backed host and a
  stack-slab MCU terminus without a build flag.

## Pitfalls

- **A decoded tree outliving its buffer.** `tlv_t::payload` and every child payload
  point into the decode input. Freeing or reusing that buffer while the tree is
  live is a dangling read; a `view_t` — and thus its segment — is what keeps the
  bytes alive.
- **Storing an arena span.** `arena_tlv_t::wire` and `::body` point into an inbound
  frame that the transport recycles. An implementation that stores one instead of
  copying it once to an owner reads another peer's later frame.
- **Copying `wire` without clearing the trailer bits.** `wire` excludes the trailer
  by construction, so a whole-TLV copy that keeps the source `opt` byte advertises
  a trailer it does not carry. `opt_t::without_trailer()` is the typed fix.
- **Hardcoding a nesting-depth limit.** A decoder that rejects at a fixed depth
  rejects frames a conforming sender may emit, and reports a resource condition it
  does not have. The limit is the decode resource; the inline slot count is a
  tuning knob.
- **Ignoring the reserved bits.** Bits 7 and 0 of `opt` must be zero. A decoder
  that masks them off accepts frames no conforming sender emits and spends the
  protocol's extension point.
- **Treating trailing bytes as a second frame.** `decode` and `decode_into` each
  require the input to be exactly one TLV; trailing bytes are `FRAME_INVALID`.
  Splitting a stream into frames is the transport's job
  (`length_prefix_framer.hpp`).

## API reference

Headers: `frame.hpp`, `tlv.hpp`, `tlv_emit.hpp`, `tlv_arena.hpp`, `batch.hpp`,
`path_ref.hpp`, `crc.hpp` — all under `core/include/libtracer/`.

`tr::wire::opt_t` — the option bits carried in every header — is documented once, on
[wire format bits](wire-format-bits.md), alongside the bit layout it names.

```{doxygenstruct} tr::wire::tlv_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::wire::trailer_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::wire::decode(std::span<const std::byte>, mem::block_source_t&)
:project: libtracer
```

```{doxygenfunction} tr::wire::decode(const view::view_t&, mem::block_source_t&)
:project: libtracer
```

```{doxygenfunction} tr::wire::encode
:project: libtracer
```

```{doxygenfunction} tr::wire::emit_tlv
:project: libtracer
```

The bound-path element codec (RFC-0024 §4) — the element's layout and the purely STRUCTURAL
rules the grammar enforces. What an element *means* — the bounds check into a host's vertex
map, the generation compare, the ACL re-check at the dereferenced vertex — is L4 routing and
lives in [fwd-router.md](fwd-router.md) §the bound hop.

```{doxygenstruct} tr::wire::path_ref_element_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::wire::path_ref_element_count
:project: libtracer
```

```{doxygenfunction} tr::wire::path_ref_body_valid
:project: libtracer
```

```{doxygenfunction} tr::wire::path_ref_element_at
:project: libtracer
```

```{doxygenfunction} tr::wire::path_ref_store_element
:project: libtracer
```

```{doxygenfunction} tr::wire::emit_name(std::vector<std::byte>&, std::span<const std::byte>)
:project: libtracer
```

```{doxygenfunction} tr::wire::emit_name(std::vector<std::byte>&, std::string_view)
:project: libtracer
```

```{doxygenstruct} tr::wire::arena_tlv_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::wire::decode_into
:project: libtracer
```

```{doxygenclass} tr::wire::tlv_arena_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::wire::crc_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::wire::timestamp_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::wire::check_frame
:project: libtracer
```

```{doxygenfunction} tr::wire::validate_rope
:project: libtracer
```

### The lazy tier — `tlv_view_t`

A rope-delivered frame does not have to become an owning tree to be read. A
`tlv_view_t` is one TLV whose bytes stay in the rope: it holds the parsed header
facts plus a refcounted subrope, and **nothing that is not accessed is ever
decoded**. Children are materialized one header at a time by stepping
`children_t`; a payload handed onward stays the subrope it already is; and
`materialize()` into a `tlv_t` is the single, explicit copy point.

Validation is lazy in the same sense. Anchoring a view checks the root header and
the exact total, with the CRC walk deferred; child headers are grammar-checked as
they are stepped over; and integrity — the per-TLV CRC trailer — is checked by
whichever consumer accesses a TLV, through `verify()`. An endpoint whose members
form one transaction verifies all of them before mutating any state, so a
partially-applied frame is not a reachable outcome.

```{doxygenclass} tr::wire::tlv_view_t
:project: libtracer
:members:
```

### Keys — `key_view_t`

A vertex-map key is the concatenated packed segment records of its path — each
`[u8 len][utf8]`, [RFC-0018](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0018-packed-path-segments.md)
— so every ancestor / descendant / child relation is a byte operation and no
string form is ever materialized. `key_view_t` is the navigation over those bytes
and the single home of that walking.

Why a byte prefix means "ancestor": records are self-delimiting and parsed
left-to-right, so a shared byte prefix parses identically in both keys and every
prefix boundary lands on a record boundary — a differing length byte breaks the
byte match one record earlier. `/a` (`01 'a'`) is a prefix of `/a/b`
(`01 'a' 01 'b'`); `/ab` (`02 'a' 'b'`) correctly is not. A strict byte-prefix of
a valid key is therefore exactly a strict ancestor of it. This is why an escape
record is refused in key context: a key must stay pure-string for the property to
be stated over its bytes at all.

```{doxygenclass} tr::wire::key_view_t
:project: libtracer
:members:
```

### The grammar core

The header/trailer rules — the type-`0x00` reject, the reserved-bit reject, the
`LL` length width, trailer sizing, the two-span CRC — are parsed and validated in
**one** place, read through a small chunk cursor so the same rules serve every
byte source. Both materializing decoders funnel through it. The cursor is the
byte-*source* seam: `span_cursor` is the contiguous case and `rope_cursor` walks
links, stitching a straddled header into a bounded scratch.

#### Cursor window containment — what holds in a RELEASE build (#986)

The cursors' bounds contracts are **not** uniformly debug-only, and the split is
deliberate rather than incidental. It rests on one asymmetry: a `span_cursor`'s
window *is* its whole object, so overshooting it forms an out-of-range `subspan`
— undefined behaviour that the fuzz and sanitizer CI reports. A `rope_cursor`'s
window is a **soft** bound inside a longer link chain, so an overshooting read
walks bytes the chain genuinely holds. Nothing faults, no sanitizer can see it,
and the caller is handed real bytes from the wrong place and told it succeeded.

| reader | violation in a release build |
| --- | --- |
| `rope_cursor::for_each_span` | **truncated to the window** and the cursor latches (`poisoned()`); `parse_header` answers `FRAME_TRUNCATED`, the forward-plane gather drops the frame |
| `span_cursor::for_each_span` | UB — caught by fuzz/ASan CI, the backstop the rope case lacks |
| `rope_cursor::byte_at` / `load_le` | debug-asserted only; callers bounds-check per read, and a wrong point read is one byte where a wrong feed is an unbounded run |
| `region` (both) | debug-asserted only |

The guarantee is spent on the bulk reader alone **because it was priced**. Giving
the contiguous cursor the same clamp-and-latch measured `compact-forward` at
x0.66 deliveries/s and `compact-terminus` at x0.82 — reproduced in 4/4 and 3/4
interleaved pairs with disjoint ranges — since a `min()`-derived `subspan` length
costs the CRC feed loop what a directly-derived one gives it, and carrying a latch
byte takes the cursor past two registers. A latency regression on the delivery
path is an automatic reject here, so `span_cursor::poisoned()` is a
`static constexpr false` that folds the shared grammar's check away entirely.

What the shipped shape costs, measured: **zero** on the pinned symbols
(`bench/symbol_ratchet.json`, including `route_fwd_forward<rope_cursor>`), **zero**
on the Cortex-M0 P0 flash footprint — a span-only MCU never instantiates the rope
cursor at all — and x1.00 on the interleaved delivery-path A/B. Targets that link a
rope-delivering transport pay +50 B in `parse_header<rope_cursor>` (+3.0%) on rv32.

`rope_cursor_assert_test` gates the debug half; `rope_cursor_release_guard_test`
gates this one, compiled with `NDEBUG` forced **on** so it cannot pass vacuously.

```{doxygenstruct} tr::wire::grammar::header_t
:project: libtracer
:members:
```

```{doxygenenum} tr::wire::grammar::crc_check_t
:project: libtracer
```

```{doxygenfunction} tr::wire::grammar::parse_header
:project: libtracer
```

```{doxygenfunction} tr::wire::grammar::walk
:project: libtracer
```

```{doxygenstruct} tr::wire::grammar::span_cursor
:project: libtracer
:members:
```

```{doxygenclass} tr::wire::grammar::rope_cursor
:project: libtracer
:members:
```

```{doxygenstruct} tr::wire::grammar::walk_frame_t
:project: libtracer
:members:
```

```{doxygenclass} tr::wire::grammar::walk_stack_t
:project: libtracer
:members:
```

### Stream framing

Splitting a byte stream back into frames is the transport's job, and it is one
state machine rather than one per transport. `length_prefix_framer` reassembles
u32-LE length-prefixed frames from arbitrary chunks: each complete frame lands in
**one exactly-sized refcounted segment** drawn from the caller's backend, so
there is no library-owned buffer and exactly one copy off the wire. An allocation
failure is backpressure — the frame is drained so framing sync survives, and
counted — while an oversize prefix is malformed, because a desynchronized stream
cannot be re-framed and the caller must tear the connection down. The state
machine names no transport type, which is why it is tested directly with no live
connection.

```{doxygenclass} tr::net::length_prefix_framer
:project: libtracer
:members:
```

See the [reference data-format](../reference/01-data-format.md) for the normative
rules and [wire-format-bits](wire-format-bits.md) for worked byte dumps.

[adr-0004]: https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0004-crc-in-optional-trailer.md
[adr-0041]: https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0041-terminus-arena-decode-span-contract.md
[adr-0048]: https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md
[rfc-0006]: https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0006-resource-bounded-nesting-depth.md
