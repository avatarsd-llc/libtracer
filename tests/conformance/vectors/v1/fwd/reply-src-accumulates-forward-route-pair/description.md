# fwd/reply-src-accumulates-forward-route-pair

FWD{ op=REPLY, dst=/reply-ep, src=<pair 7:1><pair 3:1>, kind=RESULT, payload=VALUE u32=1234 } — a success reply as a **relaying hop** forwards it ([RFC-0029](../../../../../docs/spec/rfcs/0029-one-path-primitive.md) §6.2).

The reply arrives on the hop's link `net/uplink/b` as `dst=/net/downlink/cli/reply-ep`, `src=<pair 3:1>` (the terminus's seed, as in `fwd/fwd-reply-result`). The hop:

1. consumes its mount run from `dst`, leaving `/reply-ep`, and egresses on `net/downlink/cli`;
2. prepends **its own element** to `src`: the PAIR of the connection vertex the reply arrived on (`7:1`). The elements behind it are relayed as they arrived.

The `src` the origin receives is a complete PAIR chain back to the responder, learned with no flag asking for it.

```
0F 40 39 00                          FWD, PL = 1, length 57
   01 00 01 00 03                       VALUE op = REPLY
   06 00 09 00 08 "reply-ep"            PATH dst = /reply-ep
   06 00 16 00                          PATH src, length 22
      00 16 08 07 00 00 00 01 00 00 00     element 0: PAIR 7:1, this hop's
      00 16 08 03 00 00 00 01 00 00 00     element 1: PAIR 3:1, the terminus's seed
   01 00 01 00 00                       VALUE kind = RESULT
   01 00 04 00 D2 04 00 00              VALUE u32 1234
```

`fwd/reply-src-accumulates-forward-route-name` is the same relay when the hop has no PAIR to give.

## What this vector gates

Per [HARNESS.md](../../../../HARNESS.md), a vector gates the **codec** only (`encode(decode(input.bin)) == input.bin`). The routing claim is bound by `core/tests/path_pair_test.cpp`'s `learning_vectors()`, which drives the production `fwd_router_t` and compares the frame it emits with these bytes, after re-spelling the node's own slots as the vector's `3:1` / `7:1`.
