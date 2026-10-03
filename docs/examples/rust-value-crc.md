# A VALUE frame and its CRC trailer (Rust)

The smallest whole frame the Rust core produces: a `VALUE` with the opt-in CRC-32C trailer.
`encode` computes the trailer from `opt.CR`; `decode` recomputes it and refuses a mismatch.

## What to notice

- **The trailer is opt-in, and so is the protection.** The same one-bit flip that `decode`
  refuses as `FrameCrcFail` with the trailer decodes silently without it. Integrity is a
  per-frame choice the sender makes, not a property of the codec
  ([reference 01](../reference/01-data-format.md)).
- **Twelve bytes: 4 + 4 + 4.** Header, payload, CRC. The C++ and TypeScript cores produce the
  same bytes, and the shared conformance vectors keep all three in step.
- **The error names are the wire names.** `Error::FrameCrcFail.name()` prints
  `FRAME_CRC_FAIL`, the spelling the C++ and TypeScript cores use.

## Source

```{literalinclude} /bindings/rust/examples/value_crc.rs
:language: rust
:linenos:
```

Run it from `bindings/rust/`:

```console
$ cargo run --example value_crc
```

See also: [the trailer (C++)](wire-trailer.md) · [data format reference](../reference/01-data-format.md).
