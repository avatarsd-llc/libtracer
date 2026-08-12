# path/path-reserved-brackets

A structurally well-formed PATH whose second NAME payload, `frame[7]`, carries the
reserved characters `[` and `]` (reference/03 §Reserved characters; normative via
spec/v1.md §3). Outer: type=0x06 PATH, opt=0x40 (PL=1), length=22. Two NAME children:
NAME `camera` (02 00 06 00 + utf8), NAME `frame[7]` (02 00 08 00 + utf8). 26 bytes total.

```
06 40 16 00 02 00 06 00 63 61 6D 65 72 61 02 00 08 00 66 72 61 6D 65 5B 37 5D
```

This is a **codec-tier** vector: the wire grammar places no constraint on NAME payload
bytes, so every core MUST carry these bytes across `decode`→`encode` bit-for-bit — an
`ok` here means "the codec carries the bytes", nothing more (see HARNESS.md §what a
vector gates).

What the vector *pins* is the reserved-character set `/ : . [ ] * ?` (its
`expected.json` carries it as `reserved_characters`): each tier's **own** host suite
must additionally assert that its segment predicate rejects this vector's `frame[7]`
NAME and accepts the `camera` control — that is where relaxing a predicate turns red
(#996; the pre-fix C++ predicate admitted the brackets while Rust/TS rejected them).
The binding tests are listed in HARNESS.md's behavioural-vector table.
