# conn/bad-payload-type

RFC-0014 §2's `bad-payload-type`: a write to `/net/<module>/conn` whose payload is a TLV type
that is **neither `SPEC` nor `NAME`**. It is refused `ERROR{tr::schema::type_mismatch}`
(`0x0030`), and — the load-bearing half — it is **not assigned**.

```
SETTINGS{ NAME "addr" NAME "127.0.0.1",
          NAME "port" VALUE u16=8080 (LE) }
```

```
0b4023000200040061646472020009003132372e302e302e3102000400706f72
7401000200901f
```
(one byte string, wrapped here for width; the canonical bytes are `input.bin`.)

## The bytes are the create's own config, minus the envelope

These 39 bytes are **byte-identical** to the `config` child of `conn/create-via-spec`. That is
chosen, not incidental. A bare `VALUE` would make the refusal look like a type check on
something obviously wrong; a `SETTINGS` carrying exactly the config the endpoint wants makes
the actual rule visible:

> **The TYPE is the verb.** The endpoint reads the written TLV's type to select the operation
> and never infers one from the payload's contents.

So a peer that sends the config it means, in the shape the endpoint parses, still gets nothing
— because it named no operation. A lenient implementation that "helpfully" treated a
`SETTINGS` as a create would be creating a connection with **no name**, which ADR-0073 §5 and
`conn/create-via-spec` both forbid: the creator chooses the name, or the retry story collapses.

## Refused is not the same as ignored

The creator endpoint is **write-only and valueless** (RFC-0014 §2): the write is *executed*,
never *assigned*. So the refusal has to be total — nothing stored, nothing propagated to
subscribers under RFC-0008, no fall-through to an ordinary vertex assign. A node that stored
these bytes as the endpoint's value would answer them back on the next read of a vertex the
RFC says has no readable value at all except `:schema`.

The RFC also names the **empty** payload in the same clause. It takes the same answer and is
covered as an ablation in the binding test rather than as its own vector — there is no
interesting byte string to pin, and the two share one code path.

## What this vector gates, and where the behaviour is bound

Codec only, per [HARNESS.md](../../../../HARNESS.md) — and note this vector is the clearest case
of why: these bytes round-trip green in every core precisely because they are a perfectly
well-formed `SETTINGS`. Bound in `core/tests/transport_vertex_test.cpp`,
`test_conformance_vectors`: the write answers `TYPE_MISMATCH`, no connection appears, the
router's demux table stays empty — and then `conn/create-via-spec`, whose `config` child is
byte-identical to these bytes, *does* create over the same endpoint. That positive control is
what keeps the refusal from being a refusal of everything.
