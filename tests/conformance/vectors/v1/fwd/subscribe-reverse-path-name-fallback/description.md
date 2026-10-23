# fwd/subscribe-reverse-path-name-fallback

FWD{ op=WRITE, dst=/sensor/temp, src=/net/downlink/cli/reply-ep, FIELD{subscribers, 0x01}, SUBSCRIBER{/reply-ep}, PATH_REF_REVERSE{/net/downlink/cli/reply-ep} } — the NAME-fallback arm of `fwd/subscribe-reverse-list-unflagged` ([RFC-0029](../../../../../docs/spec/rfcs/0029-one-path-primitive.md) Amendment 1).

The arrival link's connection vertex has retired under it, so the hop can issue no PAIR (§4.4). The `0x15` child it creates starts with the hop's **NAME run** (`net/downlink/cli`) instead, followed by the request's `src` as it arrived (`reply-ep`). The two vectors differ only in that element.

```
0F 40 7A 00                          FWD, PL = 1, length 122
   …                                    op, dst, FIELD, src, SUBSCRIBER as in -unflagged
   15 00 1A 00                          PATH_REF_REVERSE, length 26 — created by this hop
      03 "net" 08 "downlink" 03 "cli"      this hop's NAME run
      08 "reply-ep"                        the request's src as it arrived
```

## What this vector gates

Per [HARNESS.md](../../../../HARNESS.md), a vector gates the **codec** only (`encode(decode(input.bin)) == input.bin`). The routing claim is bound by `core/tests/path_pair_test.cpp`'s `learning_vectors()`, which drives the production `fwd_router_t` and compares the frame it emits with these bytes, after re-spelling the node's own slots as the vector's `3:1` / `7:1`.
