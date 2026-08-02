# fwd/fwd-mint-reply

The answer to [`fwd/fwd-mint-request`](../fwd-mint-request/description.md): an ordinary
`FWD{REPLY}` with the minted binding appended as its **last** child (RFC-0024 §7.5).

```
0F 40 44 00                       FWD, opt=0x40 (PL=1), length=68
   01 00 01 00 03                 VALUE op = REPLY
   06 40 0C 00 …                  PATH dst = /reply-ep      (the request's src)
   06 40 12 00 …                  PATH src = /sensor/temp   (the request's dst)
   01 00 01 00 00                 VALUE kind = RESULT
   01 00 04 00 D2 04 00 00        VALUE u32 = 1234          (the answer itself)
   14 00 08 00                    PATH_REF, length=8        ← the mint
      02 00 00 00 00 00 00 00     element 0: index=2, generation=0
```

The graph this was taken from registers `/sensor/temp` into an empty node, which allocates
two vertices — the `/sensor` placeholder at slot 1 and the leaf at slot 2 — under the
structural root at slot 0. Hence `index = 2`. `generation = 0` because nothing has been
retired.

## Why the mint is LAST

The reply's child order is `op, dst, src, kind, payload…` (RFC-0004 §D). Putting the
`PATH_REF` after the payload rather than beside `kind` means a positional reader of an
ordinary reply reads exactly what it read before and stops; only an origin that **asked**
for a mint reads past the payload. Placing it earlier would have moved the payload's index
for every reader of every reply, mint or not.

## Why nothing is minted on the error side

A mint rides **success alone**. The operation that carries it has already passed every gate
on its way to the terminus, so a vref is never produced for a destination the caller could
not have reached canonically — probing the bound form yields exactly what probing the
canonical form yields, *exists + denied*, and never *exists + here is a handle to it*
(RFC-0024 §6.1). See [`acl/bound-vs-canonical-deny`](../../acl/bound-vs-canonical-deny/description.md),
which carries no `PATH_REF`.

## What this vector gates

The codec, and only the codec. That a mint-flagged operation actually produces this
`PATH_REF`, that an unflagged one produces none, and that a denied one produces none either
is bound by `core/tests/bound_path_test.cpp` — `test_mint_round_trip` and
`test_mint_denied_by_acl`.
