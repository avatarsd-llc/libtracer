# fwd/fwd-op-reserved-bit6

FWD{ op=0x40, dst=/sensor/temp, src=/reply-ep }: op byte 0x40, whose low six bits read READ, with reserved bit 6 set.

```
0f402200010001004006000c000673656e736f720474656d7006000900087265706c792d6570
```

```
0F 40 22 00                          FWD, PL = 1, length 34
   01 00 01 00 40                      VALUE op = 0x40
   06 00 0C 00 06 "sensor" 04 "temp"    PATH dst = /sensor/temp
   06 00 09 00 08 "reply-ep"            PATH src = /reply-ep
```

38 bytes total.

## The rule this pins

annex 05 §Reserved range (RFC-0029 §5.3): the `op` byte's opcode is `op & 0x3F`, and bits 7 and
6 are reserved and MUST be zero. RFC-0004 §B defines opcodes 0 to 3. A forwarder masks the byte
and never switches on it, so it routes this frame on unchanged.

This byte sets a reserved bit, so no conformant origin sends it.

The answer is the reference terminus's behaviour, not normative text: it answers
`ERROR{tr::schema::type_mismatch}` (`0x0030`) along `src` (#904, #2245), the answer annex 01
§Handling unknown type codes gives an unknown code when a return path exists, rather than a
silent drop.

## Why this is an `input.bin` case

The frame decodes, so a codec has nothing to refuse. The manifest declares
`"malformed_input": true`, because the C++ legality pre-pass reads the raw `op` byte and refuses
any value above 3, a set reserved bit included.

## Bound by

C++ `core/tests/op_resolve_test.cpp`, `test_undefined_opcode_answers_addressed_error`, which
feeds these bytes to the resolver and checks for `kind = ERROR` with code `0x0030`, against a
plain `READ` control.
