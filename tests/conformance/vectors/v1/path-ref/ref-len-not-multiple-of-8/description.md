# path-ref/ref-len-not-multiple-of-8

A `PATH_REF` whose `length` is **not** a whole number of elements: 12 bytes, one and a half
elements.

```
14 00 0C 00     <- type=PATH_REF(0x14), opt=0x00, length=12   <- 12 % 8 = 4, invalid
   07 00 00 00 03 00 00 00      <- element 0
   AA BB CC DD                  <- four bytes that are not an element
```

`decode` MUST fail with `tr::frame::invalid`.

## Why this is a `reject.bin` case and not an `input.bin` one

Contrast `path/path-escape-in-key-context` and `path/path-record-overruns-body`, where an
illegally-spelled canonical `PATH` still round-trips because its body is opaque bytes to the
codec and the constraint is a **resolver** rule. (Before
[RFC-0018](../../../../../docs/spec/rfcs/0018-packed-path-segments.md) the same contrast was drawn
against `path/path-value-children-illegal`, now retired.) This
one is different in kind: the count of a `PATH_REF` is not carried on the wire at all —
RFC-0024 §4.3 removed a count field as a byte restating what the length already says — so
the count **is** `length / 8`. A length of 12 does not describe a body any decoder can
frame. It is a shape error, checkable from the header alone, which is exactly what makes
it a codec rule.

The four trailing bytes are deliberately not zero: a decoder that silently truncated to the
nearest whole element would produce a frame that re-encodes to different bytes, so a lenient
implementation fails the round-trip rather than passing quietly.
