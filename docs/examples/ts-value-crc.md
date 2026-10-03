# A VALUE frame and its CRC trailer (TypeScript)

The TypeScript core package on its own: `encode` a `VALUE` with `opt.cr` set, `decode` it,
and watch the trailer catch a flipped bit. The bytes match the [Rust example](rust-value-crc.md)
exactly.

## What to notice

- **The encoder computes the trailer.** The literal passes `trailer: null`; `opt.cr` is the
  request, and `decode` hands back `trailer.crc` with its width and value.
- **Errors carry a wire-named code.** The refusal is a thrown `CodecError` whose `.code` is
  `ERROR.FRAME_CRC_FAIL`.
- **Without the trailer, the damage passes.** Integrity is the sender's per-frame choice.

## Source

```{literalinclude} /bindings/typescript/examples/value-crc.mjs
:language: javascript
:linenos:
```

Run it from `bindings/typescript/`, after `npm ci && npm run build`:

```console
$ node examples/value-crc.mjs
```

See also: [the trailer (C++)](wire-trailer.md) · [data format reference](../reference/01-data-format.md).
