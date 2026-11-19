# fwd/fwd-op-undefined

FWD{ op=0x04, dst=/sensor/temp, src=/reply-ep }: opcode 4, the first value RFC-0004 §B leaves undefined.

```
0f402200010001000406000c000673656e736f720474656d7006000900087265706c792d6570
```

```
0F 40 22 00                          FWD, PL = 1, length 34
   01 00 01 00 04                      VALUE op = 0x04
   06 00 0C 00 06 "sensor" 04 "temp"    PATH dst = /sensor/temp
   06 00 09 00 08 "reply-ep"            PATH src = /reply-ep
```

38 bytes total.

## The rule this pins

annex 05 §Reserved range: the `op` byte's opcode is `op & 0x3F`, and bits 7 and 6 are reserved
and MUST be zero. RFC-0004 §B defines opcodes 0 to 3. A forwarder masks the byte and never
switches on it, so it routes this frame on unchanged. The terminus owns the verdict: a byte that
names no defined operation answers `ERROR{tr::schema::type_mismatch}` (`0x0030`) along `src`.
That is the answer annex 01 §Handling unknown type codes gives an unknown code when a return path
exists. It is never a silent drop, because silence looks like a dead link to the origin.

## Why this is an `input.bin` case

The frame decodes, so a codec has nothing to refuse. The manifest declares
`"malformed_input": true`, because the C++ legality pre-pass reads the raw `op` byte and refuses
any value above 3.

## Bound by

C++ `core/tests/op_resolve_test.cpp`, `test_undefined_opcode_answers_addressed_error`, which
feeds these bytes to the resolver and checks for `kind = ERROR` with code `0x0030`, against a
plain `READ` control.
