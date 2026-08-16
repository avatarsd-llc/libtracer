# fwd/fwd-reverse-mint

The **forwarded** leg of [`fwd/fwd-mint-request`](../fwd-mint-request/description.md), after
one hop has contributed its reverse-direction element (RFC-0024 §7.1 amendment 1, spelled by
amendment 2):

```
0F 40 24 00                       FWD, opt=0x40 (PL=1), length=36
   01 00 01 00 80                 VALUE op = 0x80  ← READ (0x00) | mint request (bit 7)
   06 00 05 00 …                  PATH /temp        ← the residual: this hop consumed /sensor
   06 00 06 00 …                  PATH /net/a       ← src, grown canonically by the arrival mount
   15 00 08 00                    PATH_REF_REVERSE, length=8 (one element)
      07 00 00 00 03 00 00 00     the hop's own ref to the identity the request arrived on
```

Three things this frame is asserting at once:

- **The origin never emits the child.** `fwd/fwd-mint-request` is the origin's frame, and it
  is unchanged — bit-identical to `fwd/fwd-read`. The reverse list's bytes ride the
  *forwarded* legs only, which is what keeps "zero added **origin** bytes" true.
- **`src` stays canonical.** The reverse list is a *second* child, never a replacement for
  `src`: the return route must remain reachable in canonical form for a peer that does not
  implement the bound form at all (RFC-0024 §9.3).
- **The list is identified by its TYPE, not by its position.** `0x15` PATH_REF_REVERSE is the
  reverse list's own code (§7.1 amendment 2). Its body grammar is `0x14`'s exactly — `opt.PL`
  and `opt.LL` both 0, `length` a multiple of 8, at most 255 elements — so
  [`path-ref/reverse-len-not-multiple-of-8`](../../path-ref/reverse-len-not-multiple-of-8/description.md)
  rejects for the same reason `path-ref/ref-len-not-multiple-of-8` does.

## Why the type and not the position

A positional rule — "on a mint-flagged request the only trailing child is the reverse list" —
decodes this exact frame identically, which is why it has to be ruled on rather than measured.
It fails two other frames: a mint-flagged `WRITE` whose stored value is itself a raw `PATH_REF`
loses its payload, and any future RFC adding a second trailing child to a mint-flagged request
breaks it. A type byte costs nothing to read — a hop already compares each tail child's type —
so the role is spelled where every other element of this grammar spells it.

## What this vector gates

The codec, and only the codec (HARNESS.md §"What a vector gates"). Round-tripping these bytes
proves that a core carries a `0x15` child and its element array intact; it proves nothing about
whether that core *contributes* an element or strips the list when it cannot. The behaviour is
bound by the C++ host suite — `core/tests/bound_path_test.cpp`
(`test_reverse_list_is_typed_not_positional`) and `core/tests/edge_eviction_test.cpp`
(`test_reverse_mint_closes_the_disclosure`, which asserts this child's type on a real
two-node forward).
