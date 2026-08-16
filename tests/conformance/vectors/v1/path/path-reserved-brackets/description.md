# path/path-reserved-brackets

A structurally well-formed PATH whose second segment, `frame[7]`, carries the
reserved characters `[` and `]` (reference/03 §Reserved characters; normative via
spec/v1.md §3). Outer: type=0x06 PATH, opt=0x00 (PL MUST be 0 under the packed body of
[RFC-0018](../../../../../docs/spec/rfcs/0018-packed-path-segments.md)), length=16. Two
packed segment records: `06 "camera"` (7 bytes), `08 "frame[7]"` (9 bytes). 20 bytes total.

```
06 00 10 00 06 63 61 6D 65 72 61 08 66 72 61 6D 65 5B 37 5D
```

This is a **codec-tier** vector: the wire grammar places no constraint on segment payload
bytes, so every core MUST carry these bytes across `decode`→`encode` bit-for-bit — an
`ok` here means "the codec carries the bytes", nothing more (see HARNESS.md §what a
vector gates).

What the vector *pins* is the reserved-character set `/ : . [ ] * ?` (its
`expected.json` carries it as `reserved_characters`): each tier's **own** host suite
must additionally assert that its segment predicate rejects this vector's `frame[7]`
segment and accepts the `camera` control — that is where relaxing a predicate turns red
(#996; the pre-fix C++ predicate admitted the brackets while Rust/TS rejected them).
The binding tests are listed in HARNESS.md's behavioural-vector table.
