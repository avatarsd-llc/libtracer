# stream/tf1-reserved

RFC-0025 §4.2.1 (Amendment 1) clause 4, ruling Q10: **`TF=1` is RESERVED grammar, not removed.**

A frame whose **root** trailer timestamp is relative (`opt.TS=1, TF=1`) is a shape the reference
writer never mints and the three-clock model has no use for — and it is still legal bytes every
core MUST decode, record and carry. These are those bytes.

```
FWD{ op=READ, dst=/sensor/temp, src=/reply-ep }   opt = PL|TS|TF, trailer_ts = i32 -250000 ns
```

```
0F622200010001000006000C000673656E736F720474656D7006000900087265706C792D6570702FFCFF
```

| Bytes | Meaning |
| --- | --- |
| `0F` | FWD |
| `62` | `opt` = `PL` (0x40) + `TS` (0x20) + `TF` (0x02) — structured, trailer-stamped, **relative** |
| `22 00` | body length 34 — the trailer is **excluded**, as it always is |
| `01 00 01 00 00` | `op` = READ (0x00) |
| `06 00 0C 00 …` | `dst` = `/sensor/temp` (packed PATH) |
| `06 00 09 00 …` | `src` = `/reply-ep` (packed PATH) |
| `70 2F FC FF` | trailer TS, **relative i32 LE = −250000 ns** (`TF=1` narrows the field 8 → 4) |

42 bytes. The body is **byte-identical** to `fwd/fwd-read`'s 34-byte body; the whole diff against
that vector is the opt byte (`0x40` → `0x62`) and the four trailer bytes. That is deliberate — it
makes the vector a one-variable experiment. Anything a core does differently with these bytes is
about `TF=1` and nothing else.

## Why a root, and why negative

- **Root, not child.** `crc/value-rel-ts-crc16-nested` and `framing/relative-ts-nested` already
  carry `TF=1` on a *child*, anchored by a parent's absolute stamp — the well-formed case. This
  vector is the **anchorless** one: a relative offset at the outermost frame, where there is no
  ancestor to be relative to. That is the case the rules in clause 4 are actually about, and it
  is a case no existing vector carries.
- **Negative.** −250000 ns exercises the sign. A core that read the four bytes as `u32` would
  round-trip this file unchanged (the bytes are re-emitted verbatim either way) and still record
  a delta of 4 294 717 296 instead of −250 000, which is why the bound tests below assert the
  **decoded value**, not the re-encoded bytes.

## What `TF=1` obliges, and what it does not

Clause 4 is four rules, and their whole point is that "reserved" here means *carried*, not
*rejected* and not *acted on*:

1. **A decoder MUST record the relative flag and its delta and succeed.** Not reject — a
   `TF=1` root is well-formed grammar. A core that hard-refused it would break every future
   use of the reserved surface.
2. **A relay MUST carry a `TF=1` frame verbatim.** The forwarding rebuild re-heads the frame
   (`dst` shrinks, `src` grows) but the trailer window — bits *and* bytes, in whichever width
   `TF` names — crosses the hop untouched. A relay does not get to normalize a stamp it does
   not understand.
3. **The reply-echo path MUST decline a `TF=1` root.** The #1109 wire-time echo answers a
   stamped request by copying its stamp onto the reply, so an origin can compute
   `RTT = origin_now − echoed_stamp` on its own clock with no request id and no clock sync.
   That arithmetic needs an absolute value. A relative offset at the root has no anchor, so
   echoing it would propagate a number that means nothing; the reply carries **no trailer at
   all**, which is the same thing an unstamped request gets. Declining is not an error: the
   frame still resolves and still answers.
4. **The reference writer stays gated.** It does not *mint* `TF=1` — not on the trailer, not
   ever, under the three-clock model. Which is the substance of this vector's name: `TF=1` is
   additive future surface RFC-0025 does not use, not surface it deletes.

## The trailer is not where sample time lives

The clause this vector belongs to exists because §4.2 originally spent the trailer on it. Under
Amendment 1 the three clocks each have exactly one carrier and they are not interchangeable:

| clock | carrier |
| --- | --- |
| **WIRE / TX** — when this frame left an interface | the optional trailer (`opt.TS=1`), **outermost frame only**, **always `TF=0`** |
| **SAMPLE** — when the datum was acquired | a payload **`TIME` (`0x0C`)** TLV *inside the value* |
| **PLAYOUT** — when the consumer should present it | **nowhere on the wire**; receiver-derived |

So the reason the reference writer never mints the bytes in this file is not that they are
malformed — it is that under the three-clock model the trailer has exactly one job, and a
relative offset is not it. A batch's per-sample time rides a `TIME{u64 base}` child (plus, for a
non-uniform stream, a packed `i32` offset array), at **0 bytes per sample** for a uniform one;
that is `stream/batch-time-roundtrip`'s subject, not this vector's.

## What this vector gates, and where the behaviour is bound

Codec only, per [HARNESS.md](../../../../HARNESS.md) — and note that the round-trip is the
*weakest* of the four rules above: re-emitting the bytes verbatim is exactly what a core that
never looked at the `TF` bit would also do. Rules 1 and 3 are bound in
`core/tests/op_resolve_test.cpp` (`test_tf1_reserved_root`), rule 2 in
`core/tests/fwd_multihop_test.cpp` over live links. Rule 4 is a property of the writer and is
gated by the absence this document describes, not by an assertion.
