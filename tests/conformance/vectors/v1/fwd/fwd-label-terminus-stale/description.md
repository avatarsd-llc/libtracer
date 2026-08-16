# fwd/fwd-label-terminus-stale

The answer a **terminus** gives a label it cannot validate ([RFC-0027](../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md) §7.2). The negative half of [`fwd/fwd-label-terminus-deref`](../fwd-label-terminus-deref/description.md), and the terminus-side twin of [`fwd/fwd-label-stale`](../fwd-label-stale/description.md), which is a *hop's* refusal.

```
0F 40 30 00                       FWD, opt=0x40 (PL=1), length=48
   01 00 01 00 03                 VALUE op = REPLY
   06 00 09 00 …                  PATH dst = /reply-ep
   06 00 07 00                     PATH src, length=7        ← the refused label, echoed
      00 16 04 0B 00 05 00        element 0: PATH LABEL, index=11, generation=5
   01 00 01 00 01                 VALUE kind = ERROR
   09 40 0A 00                     STATUS
      08 40 06 00                   ERROR
         01 00 02 00 20 00           VALUE u16 = 0x0020  (tr::path::not_found)
```

## One answer covers every way validation fails

Out of range, generation mismatch, unminted slot, a label this host never minted, a label minted for a **different peer** (§4.1's node-scope rule from the receiving side), a label whose peer identity was re-stamped when a child departed and re-added (§7.1), and a label whose vertex retired between the mint and this frame. To the sender they are one case — *"this label is no longer an address"* — and its recovery from all of them is identical: **fall back to the full-string path it still holds, and re-mint from the next reply**. One failed operation is the entire cost.

`tr::path::not_found` (`0x0020`) and not `tr::access::denied`: `denied` would confirm the slot is live and turn a refusal into an enumeration oracle. §8.1 requires a labelled probe to yield what a string probe yields, and a string probe of an address that does not resolve yields `not_found`. (A labelled operation that *does* resolve and is then refused by the ACL answers `denied` — the same rule, read against what the string probe yields *there*.)

## Nothing is forwarded, applied, repaired — or minted

§7.2 is four MUST NOTs and one MUST, and the reply's own `src` is where the last of them shows: the refused label is echoed back **unchanged**, because a reply's `src` is the request's `dst` and a refusing terminus rewrites nothing. It does not mint a fresh label to "correct" the sender, does not re-resolve against a nearest match, does not retry against another slot, and does not fall through to a canonical walk — the label **replaced** the string bytes (§6.1), so there is nothing left to walk.

The refusal also mutates **no table state**: no repair, no re-mint, no aging (§7.3). A second identical frame earns a second identical refusal. That is what makes §8.4's bound honest — a peer presenting labels it did not mint buys one `NOT_FOUND` per attempt and no state at all.

## What this vector gates

The codec, and only the codec — the harness routes nothing ([HARNESS.md](../../../../HARNESS.md) §"The execution model has ONE forwarder"). Staleness is **table state, not wire state**: nothing in a labelled request says whether the receiving host will validate it, which is precisely why the behavioural claim — that this is what a terminus emits, that the counter moves, and that the operation the frame carried landed nowhere — is bound by `core/tests/path_label_terminus_test.cpp` against the production wiring.
