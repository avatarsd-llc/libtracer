# A remote write is one FWD frame (Rust)

The Rust crate is a codec, so a write to another node is a frame you build:
`FWD{ op=WRITE, dst, src, VALUE }` (RFC-0004 §B). The receiving node reads the same children
back with `decode_fwd`.

## What to notice

- **Four children, in a fixed order.** `op`, the `dst` route, the `src` reply route, then the
  payload. `encode_fwd` assembles them and `decode_fwd` reads them back by position.
- **There is no correlation id.** The frame carries `src = /client`, the route its reply
  travels home on, and that is all a reply needs ([the way home](route-reply-home.md)).
- **The value is an ordinary VALUE TLV.** `value_u32(21_500)` is the same four little-endian
  bytes any core writes.

## Source

```{literalinclude} /bindings/rust/examples/fwd_write.rs
:language: rust
:linenos:
```

Run it from `bindings/rust/`:

```console
$ cargo run --example fwd_write
```

See also: [read and write (C++)](graph-read-write.md) · [protocol TLVs reference](../reference/05-protocol-tlvs.md).
