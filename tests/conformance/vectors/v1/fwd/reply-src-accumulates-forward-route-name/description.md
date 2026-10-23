# fwd/reply-src-accumulates-forward-route-name

FWD{ op=REPLY, dst=/reply-ep, src=/net/uplink/b<pair 3:1>, kind=RESULT, payload=VALUE u32=1234 } — the NAME-fallback arm of `fwd/reply-src-accumulates-forward-route-pair` ([RFC-0029](../../../../../docs/spec/rfcs/0029-one-path-primitive.md) §6.2).

A link cannot exist without its connection vertex (#1940), so the hop that can issue no PAIR is one whose connection vertex has **retired** under the link: a PAIR for a retired vertex would name its successor's tenancy (§4.4). That hop grows `src` by its **NAME run** instead, so the chain stays a full route, mixed, and is never relayed one hop short.

```
0F 40 3B 00                          FWD, PL = 1, length 59
   01 00 01 00 03                       VALUE op = REPLY
   06 00 09 00 08 "reply-ep"            PATH dst = /reply-ep
   06 00 18 00                          PATH src, length 24
      03 "net" 06 "uplink" 01 "b"          this hop's NAME run
      00 16 08 03 00 00 00 01 00 00 00     PAIR 3:1, the terminus's seed
   01 00 01 00 00                       VALUE kind = RESULT
   01 00 04 00 D2 04 00 00              VALUE u32 1234
```

## What this vector gates

Per [HARNESS.md](../../../../HARNESS.md), a vector gates the **codec** only (`encode(decode(input.bin)) == input.bin`). The routing claim is bound by `core/tests/path_pair_test.cpp`'s `learning_vectors()`, which drives the production `fwd_router_t` and compares the frame it emits with these bytes, after re-spelling the node's own slots as the vector's `3:1` / `7:1`.
