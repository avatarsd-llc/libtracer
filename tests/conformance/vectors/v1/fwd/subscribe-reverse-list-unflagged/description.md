# fwd/subscribe-reverse-list-unflagged

FWD{ op=WRITE, dst=/sensor/temp, src=/net/downlink/cli/reply-ep, FIELD{subscribers, 0x01}, SUBSCRIBER{/reply-ep}, PATH_REF_REVERSE{<pair 7:1>, reply-ep} } — a relayed subscribe as the hop forwards it.

The origin sends the subscribe with **no flag and no `0x15` child**. Every relaying hop builds the reverse list ([RFC-0029](../../../../../docs/spec/rfcs/0029-one-path-primitive.md) §6.3). Where the request carries no `0x15` child, the hop **creates** one, seeded with its own element followed by the request's `src` as it arrived ([RFC-0029](../../../../../docs/spec/rfcs/0029-one-path-primitive.md) Amendment 1):

- its own element is the PAIR of the connection vertex the request arrived on (`7:1`);
- the `src` as it arrived is `/reply-ep`.

So the subscription's holder learns a delivery route that starts with a PAIR, without the origin asking for one.

```
0F 40 74 00                          FWD, PL = 1, length 116
   01 00 01 00 01                       VALUE op = WRITE
   06 00 0C 00 06 "sensor" 04 "temp"    PATH dst = /sensor/temp (the hop's residual)
   10 40 14 00 …                        FIELD{ NAME subscribers, VALUE 0x01 }
   06 00 1A 00                          PATH src = /net/downlink/cli/reply-ep (grown canonically, §6.1)
   04 40 0D 00 06 00 09 00 08 "reply-ep"   SUBSCRIBER{ PATH /reply-ep }
   15 00 14 00                          PATH_REF_REVERSE, length 20 — created by this hop
      00 16 08 07 00 00 00 01 00 00 00     this hop's element: PAIR 7:1
      08 "reply-ep"                        the request's src as it arrived
```

`fwd/subscribe-reverse-path-name-fallback` is the same hop with no PAIR to give.

## What this vector gates

Per [HARNESS.md](../../../../HARNESS.md), a vector gates the **codec** only (`encode(decode(input.bin)) == input.bin`). The routing claim is bound by `core/tests/path_pair_test.cpp`'s `learning_vectors()`, which drives the production `fwd_router_t` and compares the frame it emits with these bytes, after re-spelling the node's own slots as the vector's `3:1` / `7:1`.
