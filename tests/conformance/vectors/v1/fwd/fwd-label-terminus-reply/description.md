# fwd/fwd-label-terminus-reply

The reply a **minting terminus emits** ([RFC-0027](../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md) §6.1 **point 3**): *"the terminus does the same for the residual it resolved."*

```
0F 40 2A 00                       FWD, opt=0x40 (PL=1), length=42
   01 00 01 00 03                 VALUE op = REPLY
   06 00 09 00 …                  PATH dst = /reply-ep      (PL=0, packed records)
   06 00 07 00                     PATH src, length=7        ← the rewritten residual
      00 16 04 00 00 01 00        element 0: PATH LABEL, index=0, generation=1
   01 00 01 00 00                 VALUE kind = RESULT
   01 00 04 00 D2 04 00 00        VALUE u32 = 1234          (the answer itself)
```

## What the terminus did

A `FWD{READ}` addressed `/sensor/temp` arrived over this node's `net/downlink/cli` child and resolved **locally** — no mount matched, so this node is the terminus and `/sensor/temp` is the **residual** every forwarding hop in front of it left. The reply's `src` **is** the request's `dst` (RFC-0004 §D's swap), so at a terminus the region the rewrite lands in and the part being rewritten are the same bytes: the label stands where the whole residual stood.

That makes §6.1's *"replaces, never appends"* **literal** here rather than accounted for. On the forwarding half the comparison is against the string spelling of the same accumulation (7 bytes against a 13-byte mount run, `fwd/fwd-label-mint-reply`); here the frame simply gets **shorter** — 7 bytes where 12 were, on this two-segment residual.

## Point 3 is what makes point 4 reachable

§6.1 point 4 — *"the first reply therefore returns to the original sender with fully-minted `src` and `dst`"* — is unsatisfiable while the last part of that `src` is still a string. The forwarding hops contribute their local parts (§6.1 point 2, erratum 2); this vector is the **base** of that accumulation, and the two compose left to right: hop labels first, then the terminus's.

## Host scope — what these bytes do NOT mean

`index=0, generation=1` names a slot **on the host that minted it and nowhere else** (§4.1). What it aliases is that host's own reference to the vertex the residual resolved to — the identical `(index, generation)` pair RFC-0024's bind mint hands back — and a different host reading this element learns nothing from it. It is an **address**, never a capability: §8.1's post-auth rule and §8.2's per-operation re-check are what keep that true, and a **denied** operation mints nothing at all.

## A terminus that does not mint is fully conformant

A node with no injected mint table echoes the request's `dst` as the reply's `src` **untouched** (§6.3). That is the shipped default, not a degraded mode, and it is why `fwd/fwd-reply-result` and every other reply vector are byte-unchanged.

## What this vector gates

The codec, and only the codec — the harness routes nothing ([HARNESS.md](../../../../HARNESS.md) §"The execution model has ONE forwarder"). That a minting terminus actually *emits* this rewrite, that an un-injected one emits the unlabelled reply instead, that a **denied** operation mints nothing, and that the mint happens once per child rather than per frame are **behavioural** claims, and they are bound by `core/tests/path_label_terminus_test.cpp`, which drives the production `fwd_router_t` + `op_resolver_t` wiring and asserts these bytes byte-exactly against what the node puts on the wire.
