# An address is packed segment records (Rust)

`path_to_tlv("/sensor/temp")` builds the PATH TLV every request names its target with. Its
body is the RFC-0018 packed form: a length byte, then the segment's bytes, repeated.

## What to notice

- **No separators, no terminator.** `/sensor/temp` becomes `[6]sensor[4]temp`, twelve bytes.
  On a node that body is the vertex-map key, so one address has exactly one spelling.
- **Rendering gives the canonical string back.** `tlv_to_path` turns the decoded TLV into
  `/sensor/temp`, and a trailing slash splits to the same segments.
- **A relative path is not an address.** `path_to_tlv("sensor/temp")` answers
  `BuildError::NotRooted` instead of guessing a root.

## Source

```{literalinclude} /bindings/rust/examples/path_tlv.rs
:language: rust
:linenos:
```

Run it from `bindings/rust/`:

```console
$ cargo run --example path_tlv
```

See also: [packed PATH (C++)](wire-packed-path.md) · [addressing reference](../reference/03-addressing.md).
