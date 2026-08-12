# errors/error-kindless-spec-type-mismatch

Bare ERROR carrying the registered-code identity `tr::schema::type_mismatch` (code `0x0030`),
code-only. This is the pinned answer (RFC-0014 §2 clause-kind rule, decided in #1062) to a
creator-endpoint connection `SPEC` whose config names no `kind` and matches no staged link: an
incomplete config — a missing required field, the same convention as a DIAL missing `addr` or
either role missing `port` — not an address to a missing thing. `tr::path::not_found` (`0x0020`)
must never answer this case: RFC-0014 reserves it for an **absent** creator endpoint (the
creatability probe). First child VALUE holds the u16 LE registered code; no detail children.

```
08400600010002003000
```
