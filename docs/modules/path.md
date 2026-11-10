# path — addressing (L4)

```{admonition} In one paragraph
:class: tip
A **`path_t`** parses `/sensor/temp` (with an optional `:field.sub[N]` tail) into the
**canonical PATH-TLV payload bytes** — the concatenated packed segment records. Those
bytes, not the string, are the vertex-map key: dispatch is a byte compare, never a
string parse on the hot path.
```

## What it does

`path_t::parse` validates and canonicalizes per the addressing rules (`reference/03`):
strip a trailing `/`, reject empty segments (`//`) and unrooted paths, enforce the
limits (≤64 B/segment, ≤1024 B total, ≤255 segments, ≤8 field steps). It emits the
canonical key — e.g. `/sensor/temp` → `06 'sensor' 04 'temp'`
(12 bytes; each record is `[u8 len][utf8]`, RFC-0018) — and parses the `:`-tail into a `field_path_t` (`settings.app.setpoint`,
`subscribers[]`, `subscribers[3]`) for the field-write surface. The graph resolves those
bytes one record at a time down its vertex tree (ADR-0057).

## Interface

```cpp
struct field_step_t { std::string_view name; bool indexed, append, wildcard; std::uint16_t index; };
struct field_path_t { field_steps_t steps; };        // ≤ kMaxFieldDepth steps in place, no growth

class path_t {
    explicit path_t(std::string_view);               // known-good LITERAL: parses once, aborts if malformed
    static result_t<path_t> parse(std::string_view);  // RUNTIME string; result_t = expected<T, status_t>
    static result_t<path_t> parse(std::string_view, mem::block_source_t&);  // block from that source
    std::span<const std::byte> key() const;          // canonical PATH payload bytes
    const field_path_t& field() const;               // the :field.sub[N] tail
    std::size_t segment_count() const;
};
```

The two entry points differ in what failure means. `path_t::parse` is for a string whose
validity is itself a runtime condition, and returns `status_t::INVALID_PATH`
(`core/src/path.cpp:result_t<path_t> path_t::parse(std::string_view text, tr::mem`). The `explicit` constructor is for a compile-site
literal, where a malformed path is a source bug: it hard-aborts rather than yielding a
`result_t` the caller would only `*`-deref unchecked (`path.hpp:explicit path_t(std::string_view`, defined
`path.hpp:p`). Neither uses
exceptions, so both hold under `-fno-exceptions`.

A path owns no standard container (#2286): its key, its field steps and their names are one
block sized exactly at parse, drawn from the source given to `parse` (else the table source),
and a step's `name` views that block. The root path with no field tail draws nothing.

## String → bytes, once

```{mermaid}
flowchart LR
    S["/sensor/temp:settings.app.setpoint"] --> P[path_t::parse]
    P --> K["key bytes<br/>06 &quot;sensor&quot; · 04 &quot;temp&quot;"]
    P --> F["field<br/>settings → app → setpoint"]
    K --> M{{"vertex map<br/>(byte-keyed)"}}
    classDef e fill:#dbeafe,stroke:#1e40af
    class M e
```

## Consequences

- **No string work reaches dispatch.** A path parses at one visible construction site and
  the graph API takes `const path_t&`, so a held handle cannot re-parse; every subsequent
  read, write and subscribe on that path is a byte compare against the map key. The cost
  of the parse is paid once, at registration or at the literal, not per call.
- **Local and remote addressing are the same bytes.** `key()` returns the PATH-TLV payload
  that travels on the wire, so a forwarded frame carries the key it was matched on and a
  remote address needs no translation into a local one.
- **A malformed address fails at the boundary.** Empty segments (`//`), unrooted paths,
  reserved characters and every limit overrun reject at parse with a typed `status_t`
  rather than surfacing as a miss deep in dispatch.
- **The limits are a receiver's buffer budget.** ≤64 B per segment, ≤1024 B total,
  ≤255 segments (RFC-0023; the byte cap binds first under the current encoding, at 204)
  and ≤8 field steps (`core/include/libtracer/path.hpp:kMaxSegmentBytes`, `core/include/libtracer/path.hpp:kMaxPathBytes`, `core/include/libtracer/path.hpp:kMaxSegments`, `core/include/libtracer/path.hpp:kMaxFieldDepth`) let a
  component size fixed scratch instead of allocating per frame — the mount-prefix walk's
  stitch buffer is two segments' worth, `std::array<std::byte, kMaxSegmentBytes * 2>`
  (`core/src/fwd_router.cpp`), and no longer scales with how wide a mount is (#523).
- **A name costs its bytes and nothing else.** A vertex keeps only a reference to its own
  segment record (#2672); the bytes live in the graph's name block, chunks drawn from the
  graph's table source and kept until the graph goes, so no name takes a heap block of its
  own and an 8-character name costs 9 bytes plus its share of a chunk header.

## API reference

Generated from `core/include/libtracer/path.hpp` by Doxygen.

```{doxygenclass} tr::graph::path_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::field_path_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::field_step_t
:project: libtracer
:members:
```

### The delivery-target key

`target_key_t` is the delivery-target key `try_make_target_key` builds. A vertex's own name
is not a key type any more: since #2672 the vertex references its record in the graph's name
block (`vertex_t::name()`).

```{doxygenclass} tr::graph::target_key_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::graph::try_make_target_key
:project: libtracer
```

```{doxygenfunction} tr::graph::valid_segment
:project: libtracer
```

```{doxygenvariable} tr::graph::kMaxPathBytes
:project: libtracer
```

```{doxygenvariable} tr::graph::kMaxSegments
:project: libtracer
```

```{doxygenvariable} tr::graph::kMaxSegmentBytes
:project: libtracer
```

```{doxygenvariable} tr::graph::kMaxFieldDepth
:project: libtracer
```

See: [graph](graph.md), [wire-format-bits](wire-format-bits.md),
[reference §addressing](../reference/03-addressing.md).
