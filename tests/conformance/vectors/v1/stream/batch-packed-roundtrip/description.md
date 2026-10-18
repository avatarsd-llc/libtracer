# stream/batch-packed-roundtrip

[RFC-0025](../../../../../../docs/spec/rfcs/0025-stream-class-values.md) §7 vector 16
(Amendment 5, §4.1.4) — **a packed sample block: n fixed-width sample frames in ONE opaque
`VALUE`**:

```
BATCH (PL=1, type=0x80) {
  TIME  (PL=0) { u64 LE = 1700000000000000000 }   ; the batch BASE — sample time of frame 0
  VALUE (PL=0) { FFFF0000 FF00FF00 FF0000FF 80FFFFFF }
                                                  ; the BLOCK — n = 16 / 4 = 4 ARGB samples
}
```

**The descriptor decides the shape.** The stream's §4.3 descriptor declares `width = 4` (bytes
per sample frame) and `dt_ns = 1000`. With `width != 0` the sample frames are not one child
each but one unstructured `VALUE` of `n × width` bytes, `n = len / width`, and frame `i` is bytes
`[i × width, (i + 1) × width)` of it. Time is unchanged from the framed shape: the `TIME` base,
and on this uniform stream `t(i) = base + i × dt_ns` at 0 bytes per sample.

**What it saves.** The framed spelling of the same batch spends a 4-byte `VALUE` header on every
sample and is 48 bytes; this is 36. At one sample the two spellings are the same bytes, which is
why a producer can adopt the block without a transition.

**What the bytes do not say.** Nothing in the record carries `width`; a consumer reads it from
the descriptor, exactly as it reads `dt_ns`. The graph never interprets the record (claim 5),
and the codec round-trips it as the ordinary structured TLV it is.

Bound in C++ by `core/tests/batch_test.cpp` §`packed_block_roundtrip` — byte-exact against
`tr::wire::encode_packed_batch`, and read back by `tr::wire::read_packed_batch` as four samples
with the derived times.
