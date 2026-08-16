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
`subscribers[]`, `subscribers[3]`) for the field-write surface. `path_key_t` +
`path_key_hash_t` (FNV-1a over the bytes) key the `unordered_map`.

## Interface

```cpp
struct field_step_t { std::string name; bool indexed, append, wildcard; std::uint16_t index; };
struct field_path_t { std::vector<field_step_t> steps; };

class path_t {
    explicit path_t(std::string_view);               // known-good LITERAL: parses once, aborts if malformed
    static result_t<path_t> parse(std::string_view);  // RUNTIME string; result_t = expected<T, status_t>
    std::span<const std::byte> key() const;          // canonical PATH payload bytes
    const field_path_t& field() const;               // the :field.sub[N] tail
    std::size_t segment_count() const;
};
class path_key_t { /* owned key bytes; ≤16 B inline, else one heap block */ };
struct path_key_hash_t { /* FNV-1a over the key bytes */ };
```

The two entry points differ in what failure means. `path_t::parse` is for a string whose
validity is itself a runtime condition, and returns `status_t::INVALID_PATH`
(`core/src/path.cpp:97-105,118,123`). The `explicit` constructor is for a compile-site
literal, where a malformed path is a source bug: it hard-aborts rather than yielding a
`result_t` the caller would only `*`-deref unchecked (`path.hpp:160`, defined
`:231-235`). Neither uses
exceptions, so both hold under `-fno-exceptions`.

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
  and ≤8 field steps (`core/include/libtracer/path.hpp:32,34,36,38`) let a
  component size fixed scratch instead of allocating per frame — the mount-prefix walk's
  stitch buffer is two segments' worth, `std::array<std::byte, kMaxSegmentBytes * 2>`
  (`core/src/fwd_router.cpp`), and no longer scales with how wide a mount is (#523).
- **Ordinary names cost no heap block.** `path_key_t` holds records up to 16 bytes inline
  (`path_key_t::kInlineBytes`, `core/include/libtracer/path.hpp:252`) — a packed segment
  record is a 1-byte length prefix plus the segment text, so a name of up to 15 characters
  never allocates; longer records spill to a single owned block.

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

### The owned key

A path is looked up by an owned copy of its canonical bytes rather than by a
string. `path_key_t` is that copy, with a small-buffer optimization sized so that
a packed segment record — a 1-byte length prefix plus the segment text — fits inline
for names up to fifteen characters, which is the overwhelming norm; longer records spill to
one heap block. It is immutable after construction, matching its use: a vertex's
name never changes. `path_key_hash_t` and `path_key_eq_t` are the hash-map
bindings over it, and `target_key_t` is the delivery-target key `try_make_target_key`
builds.

```{doxygenclass} tr::graph::path_key_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::path_key_hash_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::graph::path_key_eq_t
:project: libtracer
:members:
```

```{doxygentypedef} tr::graph::target_key_t
:project: libtracer
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
