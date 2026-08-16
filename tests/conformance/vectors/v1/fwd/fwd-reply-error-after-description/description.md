# fwd/fwd-reply-error-after-description

`FWD{ op=REPLY, dst=/net/downlink/a/net/downlink/cli/reply-ep, src=/sensor/temp, kind=ERROR, STATUS{ DESCRIPTION "no such vertex", ERROR{ VALUE u16=0x0020 tr::path::not_found } } }` — the
[`fwd/fwd-reply-error`](../fwd-reply-error/description.md) frame with the STATUS's **optional
DESCRIPTION written before the ERROR**.

## The point this frame depicts

It is the same terminus, the same swapped routes and the same registered identity as its twin;
the only difference is the **order of the STATUS's own children**. It exists to pin one
acceptance rule across every core:

> A reply's ERROR is the **first `ERROR` (`0x08`) child of the STATUS**, found by scanning the
> STATUS's direct children. Its **position is not load-bearing**.

## Why that is the rule

Read the two positional statements in the doc set against each other:

- **RFC-0002 §C** (and reference/05 §`0x08`) pins position where position *is* load-bearing, in
  prose and in a table: "Its **first child is the identity**, selected by the child's type
  alone" — that is a rule about the children of an **ERROR**.
- **reference/05 §`0x09`** describes a STATUS as carrying "one or more ERROR TLVs **and optional
  DESCRIPTION text**", and §`0x08` says ERROR appears "inside STATUS TLVs (**zero or more**
  ERRORs per STATUS)". The layout block there is a sketch — every line past the first is marked
  `; optional`, and it ends in `...`. No sentence anywhere pins **which** child of a STATUS the
  ERROR is.

A document that states a positional rule exactly where it means one, and states none here, is
evidence that there is none here. DESCRIPTION is a child type §`0x09` names explicitly, so this
frame is not an exotic shape — it is the documented STATUS grammar with its two documented
children written in the other order.

The consequence of getting it wrong is a **diagnosability** loss, not a wire fault: a reader that
demands the ERROR at `children[0]` answers code `0` here, which is the same answer it gives for a
STATUS carrying no ERROR at all. It collapses "this peer sent no error identity" into "this peer
sent one and I declined to look", which is the worst of the two failure modes and is silent.

## Emitters are NOT licensed by this vector

The acceptance rule is one-directional. Every emitter in the tree writes the canonical order —
ERRORs first, detail after — and `fwd/fwd-reply-error` remains the byte-exact pin on what a
terminus **emits**. This vector pins only what a reader must **accept**. Being liberal on receipt
costs a `find` over a handful of direct children and cannot put a wrong byte on any wire.

## Byte breakdown

`0x0F FWD`, `opt.PL=1`, `length=0x0082` (130), 134 bytes total. Offsets **4–101** are
`fwd/fwd-reply-error` verbatim (see its table); only the STATUS differs. The 4-byte
header is not shared — this vector's outer FWD length is `0x0067` where that one's is
`0x0055`, so the two `input.bin` files first differ at offset 2. The `PATH`s carry packed `[u8 len][utf8]` segment records
([RFC-0018](../../../../../docs/spec/rfcs/0018-packed-path-segments.md)), so each is `opt=0x00` and each
segment costs `1 + len` rather than `4 + len`.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | `0F 40 67 00` | FWD, PL=1, body length 103 (18 more than the twin — the DESCRIPTION) |
| 4 | `01 00 01 00 03` | VALUE u8 `0x03` — `op = REPLY` |
| 9 | `06 00 29 00` … | PATH `dst` = `/net/downlink/a/net/downlink/cli/reply-ep`, PL=0, body 41 (unchanged) |
| 54 | `06 00 0C 00` … | PATH `src` = `/sensor/temp`, PL=0, body 12, the refused spelling (unchanged) |
| 70 | `01 00 01 00 01` | VALUE u8 `0x01` — `kind = ERROR` |
| 75 | `09 40 1C 00` | STATUS, PL=1, body length 28 |
| 79 | `03 00 0E 00 "no such vertex"` | **child 0** — DESCRIPTION (`0x03`), the optional human detail |
| 97 | `08 40 06 00` | **child 1** — ERROR, PL=1, body length 6 |
| 101 | `01 00 02 00 20 00` | VALUE u16 LE `0x0020` — `tr::path::not_found` |

## What this vector does and does not gate

Per [HARNESS.md](../../../../HARNESS.md), a vector gates the **codec** only — the contract is
`encode(decode(input.bin)) == input.bin`, which these bytes satisfy in every core and always did.
The acceptance rule above is a **behavioural** claim, and it is bound where it can fail: in each
binding's own suite, against this frame, asserting the typed identity comes back rather than `0`.
Those tests are named in HARNESS.md's behavioural table.

```
0f408200010001000306403e00020003006e657402000800646f776e6c696e6b0200010061020003006e657402000800646f776e6c696e6b02000300636c69020008007265706c792d6570064012000200060073656e736f720200040074656d70010001000109401c0003000e006e6f20737563682076657274657808400600010002002000
```
