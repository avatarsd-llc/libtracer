# fwd/fwd-label-stale

The labelled request a hop **refuses** once the vertex its label stood for has departed ([RFC-0027](../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md) §7.1, §7.2).

```
0F 40 31 00                       FWD, opt=0x40 (PL=1), length=49
   01 00 01 00 00                 VALUE op = READ
   06 00 13 00                     PATH dst, length=19       ← the labelled address
      00 16 04 00 00 01 00        element 0: PATH LABEL, index=0, generation=1
      06 "sensor"                 element 1: literal segment
      04 "temp"                   element 2: literal segment
   06 00 09 00 …                  PATH src = /reply-ep      (PL=0, packed records)
   01 00 04 00 09 00 00 00        VALUE u32 = 9
```

These are the bytes an origin sends after caching the spelling [`fwd/fwd-label-mint-reply`](../fwd-label-mint-reply/description.md) came back with: the hop's mount run is gone, replaced by the label that hop minted for it.

They were hand-built by a test until the origin car (RFC-0027 §6.1 amendment 8): `core/tests/path_label_origin_test.cpp` now composes this frame from `fwd_router_t::adopt_path_label` + `fwd_router_t::label_dispatch` — the origin adopting the minted `src` and spending it as its next `dst` — and asserts it byte-exact against these bytes. Note what the origin contributed to the spelling and what it did not: its own first-hop local part goes on as **literal segments** (it stands up no label table of its own), so the run stripped out of the `dst` here is the origin's, and the label is the hop's.

## The same bytes serve both outcomes, and that is the point

Against a **live** slot this frame forwards over the link the label names — the label is turned straight into the resolution the hop already made, with no digest fold, no segment compare and no name hash.

Against a **bumped** generation the identical frame answers `tr::path::not_found` (`0x0020`) and delivers nothing.

Nothing in the frame distinguishes the two. **Staleness is table state, not wire state**, which is why there is no "stale" bit to encode here and why this vector's behavioural half has to be bound by a host test rather than by the harness.

## What the hop does on a refusal

Per §7.2, a host that receives a label it cannot validate — out of range, generation mismatch, unminted slot, or a label it did not mint — **MUST NOT** forward it, **MUST NOT** apply the operation, and **MUST NOT** attempt any repair of its own: no re-resolution against a nearest match, no retry against a different slot, no guessing. It **MUST** answer a `NOT_FOUND`-class error.

There is **no fall-through to the canonical walk**, and that is where this differs from the local bound-slot precedent (`graph_t::dispatch_edge_target`, which *does* fall through). The precedent can fall through because the canonical spelling is still sitting beside the bound one; here the label **replaced** the string bytes, so there is nothing left to walk and "falling through" would mean reading a slot index as UTF-8.

## The sender's recovery, and why it is complete

Fall back to the **full-string path it still holds**, and re-mint from the next reply (§6.1). One failed operation is the entire cost. Canonical bytes are the mint key and the fallback (§5.1), so every label has a string original by construction and no address is ever reachable in label form alone.

There is **no withdraw frame, no unbind, no lease and no TTL** (§7.3). Nothing is told; the next frame discovers it.

## What this vector gates

The codec, and only the codec. That a stale label actually refuses, that the refusal is `tr::path::not_found` rather than `tr::path::invalid`, that nothing is forwarded and nothing is applied, that the refusal is counted, and that refusing mutates no table state are bound by `core/tests/path_label_forward_test.cpp` against the production `fwd_router_t` wiring.
