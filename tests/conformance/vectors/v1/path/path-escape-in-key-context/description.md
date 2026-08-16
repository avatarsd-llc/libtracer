# path/path-escape-in-key-context

A packed `PATH` (RFC-0018) whose second record is the **§5.4 escape**:

```
06 00 0D 00                       PATH, opt = 0x00 (PL MUST be 0), body 13 bytes
   06 73 65 6E 73 6F 72              record: len 6, "sensor"
   00 16 04 01 00 02 00              ESCAPE: len 0, kind 0x16, len 4, u32 LE 0x00020001
```

The escape is `00 <u8 kind> <u8 len> <len bytes>` — self-delimiting on purpose, so the node
least likely to implement `kind` is still able to **skip** it by length rather than dropping a
frame it is only relaying (RFC-0018 §8). `kind = 0x16` is reserved for
[RFC-0027](../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md)'s label
element; nothing in the reference core mints one yet, and this vector does not require that it
can.

## The rule this pins

RFC-0018 §5.4 Amendment 1 makes the escape **conditionally** admissible:

> The escape is admissible in a **frame path** only. It MUST be rejected wherever a path is used
> as a **canonical key** — `path_lookup_key` and every `key_view_t` comparison — because a label
> is not canonical bytes.

So there are two required behaviours, and a conforming core needs both:

| context | required behaviour |
| --- | --- |
| a `dst`/`src` being FORWARDED | step over the record by `3 + len` and keep walking; the literal segments around it keep their positions |
| a canonical key — a vertex-map lookup, an ADVERTISE route, a `SUBSCRIBER` target, any `key_view_t` ancestor/descendant test | refuse; a resolver answers `ERROR{tr::path::invalid}` (`0x0021`) |

## Why key context has to refuse

`key_view_t`'s load-bearing invariant is that a strict **byte prefix** of a valid key is exactly
a strict **ancestor** of it (RFC-0018 §5.1). That holds because every prefix boundary of a
well-framed key lands on a record boundary and every record is literal text. An escape record is
neither text nor an address component: admitting one into a key would put a non-string record
inside the very bytes the invariant is stated over, and two different labels for one address
would be two spellings of one key — the injectivity `docs/reference/02` depends on, lost.

Keeping the refusal at the KEY door rather than at the frame door is what lets both rules hold
at once: a forwarder that only relays this frame never builds a key from it and never refuses it.

## Why this is an `input.bin` case and not a `reject.bin` one

`reject.bin` means **decode** must fail (HARNESS.md §negative cases). A packed `PATH` body is
`opt.PL = 0` — opaque bytes to the codec — so there is no framing here for a codec to refuse,
and the TypeScript and Rust cores are codecs with no resolver. Every core decodes and
round-trips these bytes correctly. The requirement is a **resolver** requirement, exactly as
the retired `path-value-children-illegal` recorded one.

## What this replaces

`path/path-value-children-illegal` pinned the pre-RFC-0018 spelling of the same class of bug
(#436): a `PATH{VALUE "sensor"}` that a resolver silently rewrote into `/sensor`'s key. Under a
packed body a record has no type byte, so a child cannot be mistyped and that vector became
**unrepresentable** — it is retired with this RFC. What survives is the shape of the rule: an
illegally-spelled address answers `ERROR{tr::path::invalid}` rather than resolving to something.
This vector and `path/path-record-overruns-body` are its two successors.
