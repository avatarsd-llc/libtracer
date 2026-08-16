# acl/bound-vs-canonical-allow

The **allow** half of the pair RFC-0024 §6.3 mandates: the bound spelling of the operation
[`fwd/fwd-read`](../../fwd/fwd-read/description.md) spells canonically.

```
0F 40 1E 00                       FWD, opt=0x40 (PL=1), length=30
   01 00 01 00 00                 VALUE op = READ
   14 00 08 00                    PATH_REF, length=8        ← dst, the bound form
      02 00 00 00 00 00 00 00     element 0: index=2, generation=0
   06 00 09 00 …                  PATH src = /reply-ep      (PL=0, packed records)
```

**37 bytes against the canonical twin's 47**, on a two-segment address at `H = 1`. That
margin is the entire case for the form, and it widens with every hop the canonical spelling
has to name.

## The claim

Against a node where slot 2 is `/sensor/temp` at generation 0, and a caller the vertex's
`:acl` allows, this frame and `fwd/fwd-read`'s frame serve the **same value**. Not similar
— the same stored bytes, from the same vertex, through the same read.

That is true *by construction* rather than by policy: both spellings resolve to one
`vertex_t` and call one `graph_t::read` with the same right and the same caller context.
There is no second policy to keep in sync, which is what makes the RFC's §6.3 short.

**A by-construction argument is not a test**, which is why the pair is required anyway. The
lesson is on the record: RFC-0014 shipped two silent misroutes because no test used the
production wiring.

## What is NOT claimed

That element `index = 2` names anything on *your* node. An element is **node-scoped** — it
means nothing anywhere but on the host that minted it — so this vector's bytes address
`/sensor/temp` only on a node whose vertex index happens to match, and address nothing at
all anywhere else. That is not a defect of the vector; it is the property of the form. A
receiver that cannot validate an element **drops**, and never guesses (RFC-0024 §5.3).

## What this vector gates

The codec, and only the codec (HARNESS.md §"What a vector gates") — that a core carries a
`PATH_REF` in `dst` position across its decoder and encoder without losing a bit. The
equivalence claim above is bound by `core/tests/bound_path_test.cpp`,
`test_bound_read_matches_canonical`.
