# fwd/fwd-read-empty-src

FWD{ op=READ, dst=/sensor/temp, src=<zero-length PATH> }: a `READ` with the unacknowledged
write's empty `src`.

```
0f401900010001000006000c000673656e736f720474656d7006000000
```

```
0F 40 19 00                          FWD, PL = 1, length 25
   01 00 01 00 00                       VALUE op = READ
   06 00 0C 00 06 "sensor" 04 "temp"    PATH dst = /sensor/temp
   06 00 00 00                          PATH src, zero-length
```

29 bytes total.

## The rule this pins

annex 05 §Reserved range, RFC-0004 Amendment 2: only a `WRITE` may go unacknowledged. A `READ`
asks for a result and gives it nowhere to go, so it is malformed. The terminus drops it and emits
no frame, because no route could carry a NACK. An `AWAIT` or a `:subscribers[]` subscribe with an
empty `src` is malformed in the same way. The sibling `fwd/fwd-write-unacknowledged` is the legal
form of these bytes.

## Why this is an `input.bin` case

The frame decodes, and only a terminus can tell a `READ` from a `WRITE`. The manifest declares
`"malformed_input": true`, because the C++ legality pre-pass refuses a `READ` or an `AWAIT` whose
`src` is empty.

## Bound by

C++ `core/tests/empty_src_unacked_test.cpp`, `test_conformance_vectors`, which feeds these bytes
to a router: zero frames go out, and nothing is read. The same frame with a one-segment `src` is
the positive control and draws exactly one reply.
