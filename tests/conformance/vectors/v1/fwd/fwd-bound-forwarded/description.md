# fwd/fwd-bound-forwarded

[`fwd/fwd-bound-forward`](../fwd-bound-forward/description.md) after **one bound hop**.

```
0F 40 30 00                       FWD, opt=0x40 (PL=1), length=48
   01 00 01 00 00                 VALUE op = READ
   14 00 08 00                    PATH_REF, opt=0x00, length=8    ← one element left
      EF BE 00 00 07 00 00 00     element: index=0xBEEF, generation=7
   06 40 13 00 …                  PATH src = /cli/reply-ep
   01 00 04 00 09 00 00 00        VALUE u32 = 9
```

Two changes, and there are no others:

- the `dst` **shrank by exactly 8 bytes** — one element, the hop's own, consumed. The same
  monotone shrink the canonical `dst` performs, which is why a bound path is loop-free by
  construction and needs no visited set (RFC-0024 §4.1);
- the `src` **grew by the inbound mount run** (`cli`), exactly as a canonical hop grows it.
  A bound path changes how the *forward* address is spelled and nothing about the return
  route: `src` accumulates canonically on a bound frame, so the reply routes home through
  the same strip-K descent it always did, and a peer that never speaks the bound form still
  answers.

The `PATH_REF` re-heads with `opt = 0x00`. `PL` stays clear on the way out for the same
reason it was clear on the way in: the body is a fixed-stride record array, and a generic
`PL = 1` walker would read the first four body bytes as a TLV header and mis-frame it.

## What this vector gates

The codec, and only the codec. The behavioural half — that this is what the hop emits for
`fwd/fwd-bound-forward`, byte for byte — is bound by `core/tests/bound_forward_test.cpp`,
which routes the paired input through a real `fwd_router_t` and compares the egress against
these bytes.
