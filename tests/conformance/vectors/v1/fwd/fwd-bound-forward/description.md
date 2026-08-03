# fwd/fwd-bound-forward

A bound request as it **arrives at a forwarder**: the `dst` is a `PATH_REF` with **two**
elements left, so this host is a hop and not the terminus (RFC-0024 §4.1).

```
0F 40 31 00                       FWD, opt=0x40 (PL=1), length=49
   01 00 01 00 00                 VALUE op = READ
   14 00 10 00                    PATH_REF, opt=0x00, length=16   ← the bound dst
      01 00 00 00 00 00 00 00     element 0: index=1,      generation=0
      EF BE 00 00 07 00 00 00     element 1: index=0xBEEF,  generation=7
   06 40 0C 00 …                  PATH src = /reply-ep
   01 00 04 00 09 00 00 00        VALUE u32 = 9
```

Element 0 is **this host's own** reference to its next-hop connection vertex — the only
element it may read. Element 1 was minted by the next host and means nothing here; it is
carried through untouched, because an element is node-scoped and no receiver can validate
another host's.

## What the hop does with it

Read element 0, bounds-check the index against the vertex map, compare the generation, then
evaluate the ACL at the dereferenced vertex for the operation's own right (RFC-0024 §5.1).
No mount descent runs: there is no NAME to fold a digest over and no segment to compare.
On success the hop consumes element 0 and forwards the remainder —
[`fwd/fwd-bound-forwarded`](../fwd-bound-forwarded/description.md), byte for byte. On any
failure it **drops**: no re-resolution, no nearest match, no retry against a different
vertex, and no fall-through to a local terminus (§5.3).

The bytes here are the ones a node whose child `up` has its connection vertex at slot 1 in
a fresh graph really receives: the structural root takes slot 0 and `/up` takes slot 1, at
generation 0 because nothing has been retired.

## What this vector gates

The codec, and only the codec — this pair carries a *routed* shape and the harness routes
nothing (HARNESS.md §"The execution model has ONE forwarder"). That a hop actually consumes
element 0, grows `src`, and drops on every validation failure is bound by
`core/tests/bound_forward_test.cpp`, which asserts both vectors byte-exact against what the
router emits and pins each refusal by ablation.
