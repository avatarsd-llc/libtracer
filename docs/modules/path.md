# path — addressing (L4)

```{admonition} In one paragraph
:class: tip
A **`Path`** parses `/sensor/temp` (with an optional `:field.sub[N]` tail) into the
**canonical PATH-TLV payload bytes** — the concatenated `NAME` children. Those
bytes, not the string, are the vertex-map key: dispatch is a byte compare, never a
string parse on the hot path.
```

## What it does

`Path::parse` validates and canonicalizes per the addressing rules (`reference/03`):
strip a trailing `/`, reject empty segments (`//`) and unrooted paths, enforce the
limits (≤64 B/segment, ≤1024 B total, ≤32 segments, ≤8 field steps). It emits the
canonical key — e.g. `/sensor/temp` → `02 00 06 00 'sensor' 02 00 04 00 'temp'`
(18 bytes) — and parses the `:`-tail into a `FieldPath` (`settings.deadline_ns`,
`subscribers[]`, `subscribers[3]`) for the field-write surface. `PathKey` +
`PathKeyHash` (FNV-1a over the bytes) key the `unordered_map`.

## Interface

```cpp
struct FieldStep { std::string name; bool indexed, append; std::uint16_t index; };
struct FieldPath { std::vector<FieldStep> steps; };

class Path {
    static Result<Path> parse(std::string_view);     // Result = expected<T, Status>
    std::span<const std::byte> key() const;          // canonical PATH payload bytes
    const FieldPath& field() const;                  // the :field.sub[N] tail
    std::size_t segment_count() const;
};
struct PathKey { std::vector<std::byte> bytes; };  struct PathKeyHash { /* FNV-1a */ };
```

## String → bytes, once

```{mermaid}
flowchart LR
    S["/sensor/temp:settings.deadline_ns"] --> P[Path::parse]
    P --> K["key bytes<br/>NAME sensor · NAME temp"]
    P --> F["field<br/>settings → deadline_ns"]
    K --> M{{"vertex map<br/>(byte-keyed)"}}
    classDef e fill:#dbeafe,stroke:#1e40af; class M e;
```

## Benefits

- **No strings on the hot path** — parse once at registration; every read/write
  compares canonical bytes. A build-time PATH literal needs no parse at all.
- **One key everywhere** — the same bytes key the local map and travel on the wire
  (a PATH TLV), so local and remote addressing are byte-identical.
- **Validated** — malformed paths fail at parse with a typed `Status`, not deep in
  dispatch.

See: [graph](graph.md), [wire-format-bits](wire-format-bits.md).
