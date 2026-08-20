# A `PATH` body is packed segment records (L2/L3 codec)

An address on the wire is a `PATH` (`0x06`) whose body is `opt.PL = 0` and holds a
self-delimiting run of **segment records** — each `[u8 len][len bytes of UTF-8]`, in order
([RFC-0018](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0018-packed-path-segments.md),
[CONTEXT.md](../../CONTEXT.md) §Segment / view). No per-segment type byte, no per-segment
option byte, hence exactly **one spelling per address** — which is what lets the vertex map be
keyed on the body bytes directly rather than on a materialized string.

[Register a vertex](graph-register.md) shows the other end of this: `path_t` parses its string
once into exactly these bytes. Here they are built by hand and compared.

## What to notice

- **The body *is* the key.** `wire::path_key` on the decoded `PATH` and `path_t("/sensor/temp").key()`
  produce byte-identical output. One is a peer's frame, the other a local literal; the
  dispatcher cannot tell them apart, which is the point.
- **A `PATH` has zero children.** That is RFC-0018 in one assertion. The pre-RFC spelling used
  a `NAME` TLV per component, and its option byte admitted several legal spellings of one
  address — so a byte key was a hope rather than a guarantee. `NAME` (`0x02`) itself is not
  retired; it survives for SETTINGS keys, `:schema` labels and `:children[]` members.
- **The walk is one byte load and one add.** `p += 1 + body[p]`, with the bounds compare the
  load already needed. `packed_path_valid_key` is exactly that walk, asking whether the body
  tiles cleanly into literal records.
- **`emit_path_segment` refuses what it cannot spell.** An empty segment would collide with
  the [escape record](wire-path-escape.md), and anything past 255 bytes will not fit the
  length field; both append **nothing** and return false rather than emitting a half-record.
- **The key bytes belong to the `path_t`.** `key()` hands out a span into the path object, so
  the object has to outlive it — the example names it rather than calling `key()` on a
  temporary.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/wire_packed_path.cpp
:language: cpp
:linenos:
```

See also: [path module](../modules/path.md) · [frame codec](../modules/frame-codec.md) ·
[addressing reference](../reference/03-addressing.md).
