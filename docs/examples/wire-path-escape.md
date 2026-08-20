# The escape record: skippable in a frame, refused as a key (L2/L3 codec)

`len == 0` cannot spell a [segment record](wire-packed-path.md) — path syntax has no empty
segment, so `/a//b` is invalid rather than a spelling of `/a/b`. That makes zero free to
reserve, and
[RFC-0018](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0018-packed-path-segments.md)
§5.4 spends it on the **escape record**: `00 <u8 kind> <u8 len> <len bytes>`.

Its whole value is an asymmetry ([CONTEXT.md](../../CONTEXT.md) §Segment / view). In a **frame
path** an escape is *admissible*: a forwarder that does not implement `kind` steps over it by
the record's declared length rather than dropping a frame it is only relaying. In **canonical
/ key** context it is *rejected*: the vertex-map key must stay pure-string, so that a byte
prefix still implies an ancestor.

## What to notice

- **The skip needs no knowledge of the kind.** The example escapes with kind `0x7F`, which
  this build assigns no meaning to at all, and `packed_record_span` still steps over it and
  lands exactly on the next literal record. The node least likely to implement a kind is
  precisely the node that must step over it, which is why the record is self-delimiting.
- **The two contexts are two functions, not a bool argument.** `packed_record_span` (frame
  path) admits the escape; `packed_path_valid_key` (canonical) refuses it, and `wire::path_key`
  answers `nullopt` for the decoded `PATH`. No call site has to decide the question with a flag
  it might pass the wrong way round.
- **The frame is still well-formed.** The escape-bearing `PATH` decodes cleanly — it is a
  *valid frame* that is *not a key*. Those are different verdicts and the example asserts both.
- **`kPackedEscapeKindLabel` (`0x16`) is a kind this layer never mints.** It is the kind
  [RFC-0027](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0027-label-switched-path-compression.md)
  uses for its path-label element, and RFC-0027 is implemented — but minting lives in the
  forwarder and only on a node given a mint table (off by default), never here:
  `packed_path.hpp` stays kind-agnostic by design, and emitting an escape is not minting a label.
  In this example the escape is simply bytes that arrived.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/wire_path_escape.cpp
:language: cpp
:linenos:
```

See also: [path module](../modules/path.md) · [frame codec](../modules/frame-codec.md) ·
[addressing reference](../reference/03-addressing.md).
