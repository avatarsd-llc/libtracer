# stream/fold-carries-composed-batch

[RFC-0025](../../../../../../docs/spec/rfcs/0025-stream-class-values.md) §7 vector 14
(Amendment 4, §4.1.3 clause 2) — **a `propagate(v, FOLD)` sweep carries a composed batch on a
STORED vertex, with ZERO graph change**:

```
POINT (PL=1) {                                      ; the branch-write frame for the swept subtree
  NAME  { "s" }                                     ; the root's leading NAME (RFC-0005 §B)
  POINT (PL=1) {
    NAME  { "adc0" }
    VALUE (PL=1) {                                  ; the node's ONE value — the composed batch
      TIME  { u64 LE = 1700000000000000000 }        ;   base sample time
      VALUE { i32 LE [0, 1500, -250] }              ;   packed offsets — non-uniform stream
      VALUE { u16 LE = 0x0064 }                     ;   sample 0
      VALUE { u16 LE = 0x0065 }                     ;   sample 1
      VALUE { u16 LE = 0x0066 }                     ;   sample 2
    }
  }
}
```

The 50 bytes from offset 17 to the end are `stream/batch-composed-folded` **verbatim**.

**This is the vector that proves the role-choice ruling.** The graph derives no time, holds no
flush counter and no window, and never learns that the value it moved was a batch. The
application composed it, swapped it in through the ordinary atomic LKV publish, and called
`propagate(/s, FOLD)`. Nothing in the fold, the sweep or the terminus was added, widened or
special-cased to make this work: a composed batch is an ordinary single structured `VALUE`, and
RFC-0005 §B already admits exactly one of those per node.

**The fold's STREAM refusal is not in tension with this — it is what makes it the answer.** A
`role_t::STREAM` vertex selected under `FOLD` still refuses with `tr::schema::type_mismatch`,
permanently (§4.1.2 clause 5, re-documented by Amendment 4). What it refuses is a **per-sample
ring**: N separately stored entries, no single foldable value, and no §B seat for a list.
Batching for a fold means **composing onto the vertex the sweep visits**, which is what this
vector does. A vertex that must keep a *history of batches* stays a STREAM — one ring entry per
batch — and delivers per-vertex. Both are the application's choice; neither is graph policy.

**One frame per subtree, never a container.** Two disjoint subtrees are two calls and two frames
(the retired-`LIST` ban, RFC-0005 §E). The terminus is untouched: §B's decomposition already
hands every covered subscription point the smallest subview covering the values at-or-below it,
so a covered leaf sees byte-identical bytes under `FOLD` and under the default emission.

```
07404300020001007307403a00020004006164633001402e000c00080000002a36fe9c971701000c0000000000dc05000006ffffff010002006400010002006500010002006600
```
