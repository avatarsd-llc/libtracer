# path/path-value-children-illegal

An **illegal** PATH: its children are VALUE TLVs (`type=0x01`) carrying the segment text,
where `docs/reference/05-protocol-tlvs.md` §PATH requires NAME (`type=0x02`) —

> Each child MUST be a NAME TLV (`type=0x02`); other types are invalid in PATH context

(normative by incorporation, ADR-0007). Compare `path/path-sensor-temp`, the legal
spelling of the same address: **same 22 bytes total, different bytes** — the two differ
only in each child's type byte.

```
06 40 12 00 01 00 06 00 73 65 6E 73 6F 72 01 00 04 00 74 65 6D 70
             ^^                            ^^
             VALUE, must be NAME (0x02)
```

## Why this is an `input.bin` case and not a `reject.bin` one

`reject.bin` means **decode** must fail (HARNESS.md §negative cases). Decoding is not
where this is caught, and should not be: the bytes are well-formed TLV, a PATH's children
are read as a generic child sequence, and the codec cores (TypeScript, Rust) are codecs
with no resolver. So every core decodes and round-trips these bytes — correctly.

The requirement this vector records is a **resolver** requirement:

> A resolver MUST NOT address a vertex through a PATH containing a non-NAME child. It
> answers `ERROR{tr::path::invalid}` (`0x0021`).

## What went wrong without it (#436)

The reference resolver had a fallback that re-materialized a non-canonical PATH's lookup
key by emitting **every** child body as a NAME, regardless of the child's actual type.
That fallback exists for a real and legal case — a foreign encoder's LL-widened or
trailer-carrying NAMEs — but applied blindly it silently **rewrote** the bytes above into
the legal spelling's key, so this PATH resolved `/sensor/temp` and a `FWD{READ}` returned
its stored value.

That breaks the injectivity `docs/reference/02` depends on: two byte-different PATHs
addressed one vertex, so any peer, cache or router keyed on PATH bytes had two spellings
for one address. The vector exists so no core can re-acquire that behaviour quietly.
