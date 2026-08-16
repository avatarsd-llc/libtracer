# path/path-deep-255-packed

A packed `PATH` (RFC-0018) at **the RFC-0023 segment cap**: 255 segments of one byte each.

```
06 00 FE 01                       PATH, opt = 0x00, body 510 bytes
   01 61  01 61  01 61  …            255 records: len 1, "a"
```

## Which cap binds, and why that moved

Two normative limits apply to an address (`docs/reference/03-addressing.md` §limits):

| limit | value | source |
| --- | ---: | --- |
| total path bytes | 1024 | reference/03 |
| segment count | 255 | [RFC-0023](../../../../../docs/spec/rfcs/0023-path-segment-cap-repriced-32-to-255.md) |

Under the pre-RFC-0018 `NAME`-child encoding a one-byte segment cost `4 + 1 = 5` bytes, so the
**byte** cap bound first: 204 segments filled 1020 bytes and the 255 count could not be reached
(`path/path-deep-204` is that case, and it keeps its own arithmetic on record).

Under the packed body a one-byte segment costs `1 + 1 = 2` bytes, so 1024 bytes admit **512**
records — and the **count** cap binds instead. RFC-0018 §5 states this crossover explicitly and
takes ownership of this vector: the 255 is now a real, reachable bound rather than an
unreachable one, and a core that only enforced the byte cap would accept 300 segments here.

## What a core must do

Accept these bytes: 255 segments and 510 bytes are both within the limits, so this is a legal
address and `/a/a/…/a` resolves like any other. A 256th record MUST be refused with
`ERROR{tr::path::invalid}` (`0x0021`) — by the count, not by the byte total, which is still less
than half spent.
