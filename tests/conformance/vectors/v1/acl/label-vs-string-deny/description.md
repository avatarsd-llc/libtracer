# acl/label-vs-string-deny

What [`acl/label-vs-string-allow`](../label-vs-string-allow/description.md)'s frame answers when the caller is **not** allowed — the deny half of the pair [RFC-0027](../../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md) §8.2 mandates.

```
0F 40 37 00                       FWD, opt=0x40 (PL=1), length=55
   01 00 01 00 03                 VALUE op = REPLY
   06 00 09 00 …                  PATH dst = /reply-ep      (the request's src)
   06 00 0E 00                     PATH src, length=14       ← the refused spelling, echoed
      00 16 04 00 00 01 00        element 0: PATH LABEL, index=0, generation=1
      06 "sensor"                 element 1: literal segment
   01 00 01 00 01                 VALUE kind = ERROR
   09 40 0A 00                    STATUS, PL=1, length=10
      08 40 06 00                 ERROR, PL=1, length=6
         01 00 02 00 20 00        VALUE u16 = 0x0020 — tr::path::not_found
```

## Why `not_found` and not `access::denied`

Deliberately, and it is the load-bearing choice in this vector.

§8.1 gives a labelled route the **anti-enumeration** property: probing it must yield what probing the string form yields — *exists + denied*, **never** *exists + here is a handle to it*. A label cannot be used to discover a namespace its holder cannot already walk.

Answering `tr::access::denied` here would break that. `denied` confirms the slot is live and that the caller reached a real vertex — it turns a refusal into an oracle. Answering `tr::path::not_found` makes a **denied** label and a **stale** one indistinguishable to the sender, which is exactly the property §8.1 asks for, and it costs the sender nothing: its recovery from both is the same one, and it already works.

## The echo, and what it is for

The reply's `src` carries the **refused spelling** back verbatim. That is what lets the sender correlate the refusal to the operation it sent and drop the cached label — falling back to the full-string path it still holds, and re-minting from the next reply (§7.2). Nothing else is repaired, withdrawn or renegotiated.

## What this vector gates

The codec, and only the codec. That a denied labelled operation actually produces this frame, that it forwards nothing, and that the label spelling is **never more permissive** than the string one is bound by `core/tests/path_label_forward_test.cpp` §8 against the production `fwd_router_t` wiring.

**One asymmetry is stated rather than hidden.** The label arm evaluates `acl_allows` at the dereferenced vertex; the canonical *mount descent* evaluates no per-vertex gate at a forwarder at all, because a name-addressed operation is gated at the **terminus**, where its ancestor ACLs are. So the two spellings are not symmetric at a forwarder hop, and the asymmetry runs in the safe direction: the labelled spelling is strictly no more permissive than the string one. That is the property §8.1 needs, and asserting a bare equality here would only hold by weakening the label arm.
