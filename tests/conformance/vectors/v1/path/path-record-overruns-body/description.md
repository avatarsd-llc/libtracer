# path/path-record-overruns-body

A packed `PATH` (RFC-0018) whose **last record runs past the body**:

```
06 00 0C 00                       PATH, opt = 0x00, body 12 bytes
   06 73 65 6E 73 6F 72              record: len 6, "sensor"          (7 bytes)
   09 74 65 6D 70                    record: len 9, only 4 bytes left  (5 bytes)
      ^^
      declares nine payload bytes; the body ends after four
```

## The rule this pins

RFC-0018 §5's body grammar is *self-delimiting*: the walk is `p += 1 + body[p]`, and it must
land exactly on the body's end. A run that does not tile the body is **ragged framing**, not a
shorter address:

- a resolver MUST NOT resolve the well-framed prefix (`/sensor`) and discard the tail — that
  would make two different byte strings address one vertex, which is the injectivity failure
  RFC-0018 §4 exists to close;
- it MUST NOT pad, clamp, or read past the body — the length is attacker-chosen, and reading
  past it is the buffer overrun the bounds compare exists to prevent;
- it answers `ERROR{tr::path::invalid}` (`0x0021`), the same status every other malformed
  address answers.

`key_view_t::record_end` reports the ragged record as `0` and every walk built on it stops
there, so `split_levels` / `for_each_level` fail and no partial key is ever produced. The same
rule holds at the frame door: a forwarder whose `dst` walk hits a ragged record stops the
address there and falls through to its terminus arm, where the resolver refuses.

## The companion case

The **over-long** direction has no representation to test: a record's length field is a `u8`, so
255 is the largest value it can express and RFC-0018 §5 freezes the per-segment maximum there
forever. A segment longer than that is unspellable rather than illegal — which is the point of
the note in §5 that a LEB128 length is explicitly *not* the escape hatch. What remains testable
is exactly this: a length the encoding CAN express and the body does not honour.

## Why this is an `input.bin` case and not a `reject.bin` one

`reject.bin` means **decode** must fail (HARNESS.md §negative cases). A packed `PATH` body is
`opt.PL = 0` — opaque bytes to the codec — so a codec has nothing to refuse and correctly
round-trips these bytes. The requirement is a **resolver** requirement. See
`path/path-escape-in-key-context`, its sibling, for the other half of what
`path-value-children-illegal` used to cover.
