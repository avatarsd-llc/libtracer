# acl/label-vs-string-allow

The **allow** half of the pair [RFC-0027](../../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md) §8.2 mandates: the **labelled** spelling of an operation whose string twin spells `dst=/net/uplink/b/sensor`.

```
0F 40 2C 00                       FWD, opt=0x40 (PL=1), length=44
   01 00 01 00 01                 VALUE op = WRITE
   06 00 0E 00                     PATH dst, length=14       ← the labelled form
      00 16 04 00 00 01 00        element 0: PATH LABEL, index=0, generation=1
      06 "sensor"                 element 1: literal segment
   06 00 09 00 …                  PATH src = /reply-ep      (PL=0, packed records)
   01 00 04 00 05 00 00 00        VALUE u32 = 5
```

**44 bytes against the string twin's 50**, on a three-segment mount run at one hop. The label element costs 7 bytes where `net/uplink/b` costs 13.

## The claim

Against a node where the label's slot is live and the dereferenced vertex's `:acl` grants the caller `WRITE`, this frame and its string twin forward the **same residual** — the same bytes, over the same link, to the same next hop.

That is true *by construction*: both spellings arrive at one `bound_egress` call, which bounds-checks the index, compares the generation, and then evaluates `acl_allows` at the dereferenced vertex for the operation's own right. **A generation match authorizes nothing** (§8.2) — it says the vertex is the same one, never that the caller may still act on it. A label holds no authorization state of any kind, so a revoked right takes effect on the very next operation over an already-minted label; there is no snapshot to go stale.

**A by-construction argument is not a test**, which is why the pair is required anyway. The lesson is on the record: RFC-0014 shipped two silent misroutes because no test used the production wiring.

## What this vector gates

The codec, and only the codec. The equality claim above is bound by `core/tests/path_label_forward_test.cpp` §8, which drives both spellings through the production `fwd_router_t` and compares what each puts on the wire. Its deny half is [`acl/label-vs-string-deny`](../label-vs-string-deny/description.md).
