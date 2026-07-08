# errors/error-invalid-frame

Bare ERROR carrying the registered-code identity `tr::frame::invalid` (code `0x0002`),
code-only. This is the error a receiver MUST emit when it encounters a frame with a
reserved `opt` bit set (bit 7 or bit 0 non-zero), `type=0x00`, or an oversize length
field — per `docs/reference/01-data-format.md` §options bitfield (reserved bits
MUST be zero; non-zero ⇒ reject as `tr::frame::invalid`) and RFC-0002 §D registry.

```
08400600010002000200
```

Layout:
- `08 40 06 00` — ERROR (type=0x08), opt=0x40 (PL=1), length=6
- `01 00 02 00 02 00` — VALUE child: type=0x01, opt=0x00, length=2, payload=0x0002 (u16 LE = `tr::frame::invalid`)
