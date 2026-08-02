# fwd/fwd-mint-request

`FWD{ op = READ | mint-request, dst = /sensor/temp, src = /reply-ep }` — an ordinary READ
that also asks every host on its route to answer with its own vertex ref (RFC-0024 §7.5).

This frame is [`fwd/fwd-read`](../fwd-read/description.md) with **one bit changed**, and
the two vectors are deliberately kept that way: the whole claim of the request side is that
a mint request costs **zero added bytes**.

```
0F 40 2B 00                       FWD, opt=0x40 (PL=1), length=43
   01 00 01 00 80                 VALUE op = 0x80  ← READ (0x00) | mint request (bit 7)
   06 40 12 00 …                  PATH /sensor/temp
   06 40 0C 00 …                  PATH /reply-ep
```

## Why the flag lives in the `op` byte

There was nowhere cheaper to put it, and one place that looked cheaper and is not:

- **Not a TLV `opt` bit.** All six defined bits are assigned, and bits 7 and 0 are
  reserved-MUST-be-zero — a frame that sets one is `tr::frame::invalid`. There is no free
  option bit, and this is not a reason to take a reserved one.
- **Not a child.** A dedicated presence child costs a 4-byte TLV header to express one bit.
- **The `op` byte had six free bits.** It carries `READ=0, WRITE=1, AWAIT=2, REPLY=3` — two
  bits of eight in use. Bit 7 is the mint request; **the opcode is `op & 0x3F`**.

## The masking rule this vector exists to gate

`op & 0x3F` is not an implementation convenience — it is a MUST on every forwarder
(RFC-0024 §9.3, incorporated at
[`05-protocol-tlvs.md`](../../../../../../docs/reference/05-protocol-tlvs.md) §`0x14`
§routing semantics). A core that switches on the raw byte sees opcode `0x80` here, calls it
unknown, and rejects — a clean error rather than a mis-execution, but an error, and at
*every* hop rather than only at the one that would have minted. Masking is what makes a flag
additive: an unrecognised one degrades to the plain opcode, and the operation still runs.

## What this vector gates

The codec, and only the codec (HARNESS.md §"What a vector gates"). Round-tripping these
bytes proves nothing about whether a core masks the op byte or mints anything. The
behaviour is bound by the C++ host suite — `core/tests/bound_path_test.cpp`,
`test_mint_round_trip` and `test_unmasked_op_byte_still_routes`.
