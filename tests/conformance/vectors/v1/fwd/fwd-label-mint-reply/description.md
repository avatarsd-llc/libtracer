# fwd/fwd-label-mint-reply

A reply as a **minting forwarder relays it** ([RFC-0027](../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md) §6.1): the hop's own local part is spelled as a **path label** instead of the mount run it stands for.

```
0F 40 36 00                       FWD, opt=0x40 (PL=1), length=54
   01 00 01 00 03                 VALUE op = REPLY
   06 00 09 00 …                  PATH dst = /reply-ep      (PL=0, packed records)
   06 00 13 00                     PATH src, length=19       ← the rewritten part
      00 16 04 00 00 01 00        element 0: PATH LABEL, index=0, generation=1
      06 "sensor"                 element 1: literal segment
      04 "temp"                   element 2: literal segment
   01 00 01 00 00                 VALUE kind = RESULT       (REQUIRED on a REPLY)
   01 00 04 00 D2 04 00 00        VALUE u32 = 1234          (the answer itself)
```

## What the hop did

The reply arrived over the forwarder's `net/uplink/b` child and is leaving over `net/downlink/cli`. The hop's **local part** — the whole three-segment mount run `net/uplink/b`, 13 packed bytes — is contributed to the reply's `src` as **one 7-byte label element**.

One label covers a hop's *entire* mount run, never one segment (§5.3.3, amendment 6). That is why the element is worth its 7 bytes: on a one-segment mount name a label would be *larger* than the string, which is precisely the reading amendment 6 rejected.

The label value is the RFC-0018 escape record `00 <kind=0x16> <len=4> <u32 LE>` (§5.3.2, amendment 5), split 16/16 — index in the low half, generation in the high half (§4.1). Generation `0` is the reserved "no label" and never reaches the wire.

## Everything else is relayed byte-for-byte — including `kind`

The `kind = RESULT` child is **required** on every `FWD{REPLY}` ([RFC-0004](../../../../../docs/spec/rfcs/0004-remote-operation-addressing.md) §B: `kind ∈ {RESULT, ERROR}`), and it sits between `src` and the payload. A minting hop relays it untouched, because the mint rewrites **`src` and nothing else** — the sibling vectors [`fwd/fwd-mint-reply`](../fwd-mint-reply/description.md) and [`fwd/fwd-label-terminus-reply`](../fwd-label-terminus-reply/description.md) carry the same child in the same slot.

These bytes lacked it until 2026-08-24. The defect was in the **test builder**, not the router: `core/tests/fwd_frame_builder.hpp` — the one place the test tree states the FWD child order — had no `kind` slot at all, so the reply the fixture fed the hop was already an illegal frame and the hop relayed the illegality faithfully into the banked bytes. The builder now emits replies only through `b_fwd_reply`, which cannot omit the discriminant; the router needed no change, and this vector grew by the five bytes the child costs.

## Host scope — what these bytes do NOT mean

`index=0, generation=1` names a slot **on the host that minted it and nowhere else** (§4.1). A different host reading this element learns nothing from it and MUST NOT interpret it. It is an **address**, never a capability: §8.1's post-auth rule and §8.2's per-operation re-check are what keep that true.

## A hop that does not mint is fully conformant

A forwarder with no injected mint table relays this reply with its `src` **untouched** and nothing on the route notices (§6.3). That is the shipped default, not a degraded mode — and it is why no existing vector's bytes move.

## What this vector gates

The codec, and only the codec — the harness routes nothing ([HARNESS.md](../../../../HARNESS.md) §"The execution model has ONE forwarder"). That a minting hop actually *emits* this rewrite, that an un-injected one emits the unlabelled reply instead, and that the mint happens once per local part rather than per frame are **behavioural** claims, and they are bound by `core/tests/path_label_forward_test.cpp`, which drives the production `fwd_router_t` wiring and asserts these bytes byte-exactly against what the router puts on the wire.
