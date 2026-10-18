# stream/batch-packed-malformed

[RFC-0025](../../../../../../docs/spec/rfcs/0025-stream-class-values.md) §7 vector 17
(Amendment 5, §4.1.4) — **a packed block whose length is not a multiple of the width is
malformed, and a malformed batch delivers none of its samples**:

```
BATCH (PL=1, type=0x80) {
  TIME  (PL=0) { u64 LE = 1700000000000000000 }   ; the batch BASE
  VALUE (PL=0) { FFFF0000 FF00FF00 FF00 }         ; 10 bytes against width = 4
}
```

The descriptor declares `width = 4` and `dt_ns = 1000`. Ten bytes is two whole frames and half
of a third; Amendment 5 makes `len % width != 0` malformed, and a malformed batch is refused
**whole**. A consumer that delivered the two whole frames and dropped the tail would hand its
application a batch that is shorter than the one the producer wrote, with nothing to say so.
The same rule covers the non-uniform shape: when `dt_ns == 0`, an offset array that does not
hold exactly `n` entries is malformed too.

**Malformed is the reader's answer, not the codec's.** These bytes are a well-formed TLV and every
conforming decoder decodes and round-trips them; the user range is a range the protocol does not
opine on ([05-protocol-tlvs.md](../../../../../../docs/reference/05-protocol-tlvs.md) §User
range). What fails is the packed convention, at the consumer that reads it.

Bound in C++ by `core/tests/batch_test.cpp` §`packed_block_malformed` — `read_packed_batch`
answers nothing for these bytes and for the offset-count case, and `encode_packed_batch` refuses
to write either shape.
