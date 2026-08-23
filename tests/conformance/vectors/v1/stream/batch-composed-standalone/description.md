# stream/batch-composed-standalone

[RFC-0025](../../../../../../docs/spec/rfcs/0025-stream-class-values.md) §7 vector 12
(Amendment 4, §4.1.3) — **a batch the APPLICATION composed, in the standalone carriage**, with
the sample frames' bytes **referenced rather than copied**:

```
BATCH (PL=1, type=0x80) {
  TIME  (PL=0) { u64 LE = 1700000000000000000 }   ; the batch BASE — sample time of frame 0
  VALUE (PL=0) { i32 LE [0, 1500, -250] }         ; the packed offset run — NON-UNIFORM stream
  VALUE (PL=0) { u16 LE = 0x0064 }                ; sample 0
  VALUE (PL=0) { u16 LE = 0x0065 }                ; sample 1
  VALUE (PL=0) { u16 LE = 0x0066 }                ; sample 2
}
```

**Batching is user-orchestrated.** The graph composes nothing here: it has no clock, holds no
flush counter and no window, and never learns that this value is a batch (claim 5, trivially).
The base, the offsets and the samples are payload bytes the **application** chose, roped into
one value and swapped in through the ordinary atomic LKV publish, after which the app calls
`propagate`/push. `batch_count` and `batch_window_ns` are **retired** — when to swap and push is
the app's decision, on the app's side of the API.

**Composition is a rope append, not a serialization.** The reference helper
(`tr::wire::compose_batch`, `core/include/libtracer/batch.hpp`) allocates exactly **one** small
owned segment — the 30 head bytes above the first sample frame: the record header, the `TIME`
base child and the packed offset child — and ropes the app's existing sample segments on behind
it as refcounted links. The 18 bytes of sample payload are never read, moved or duplicated. That
is why these bytes are checkable at all: a re-encoding composer would produce the same *shape*
and would still round-trip, but it would have paid for a copy the layout exists to avoid. The
precedent is the composed branch read ([RFC-0016](../../../../../../docs/spec/rfcs/0016-composed-branch-read.md))
and the FWD plane's `src` accumulation — existing elements referenced, never rewritten.

**Non-uniform, because the descriptor says so.** The stream's §4.3 descriptor declares
`dt_ns == 0` here, so per-sample time is the packed `i32` LE offset run in one `VALUE` child
between the `TIME` base and the sample frames: `t(i) = base + offsets[i]`, and the offsets are
**signed** — sample 2 precedes the base by 250 ns. Which of the two shapes these bytes are is a
fact about the **descriptor**, never about the record; a uniform stream (`dt_ns != 0`) carries no
offset child at all and derives every sample time at 0 bytes per sample
(`stream/batch-time-roundtrip`).

**One layout, two carriages.** `stream/batch-composed-folded` is these bytes with one byte
changed — the header type, `0x80` → `0x01` `VALUE` — which is the whole of the carriage rule of
§4.1.2 clause 6 as scoped by the 2026-08-23 erratum. The record carries **no trailer**: wire/TX
time is `opt.TS` on the outermost frame only and always `TF=0` (§4.2.1 consequence 1).

```
80402e000c00080000002a36fe9c971701000c0000000000dc05000006ffffff010002006400010002006500010002006600
```
