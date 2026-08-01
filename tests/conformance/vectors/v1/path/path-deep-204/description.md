# path/path-deep-204

The **deepest PATH today's body encoding can express** — 204 single-byte NAME segments, pinning
the byte-derived ceiling that RFC-0023 §4.2 computes.

RFC-0023 repriced the segment cap 32 → 255. Under the current NAME-TLV body grammar
(`docs/reference/05-protocol-tlvs.md` §`0x06`) each segment costs `4 + len`, so the normative
1024-byte PATH budget — measured as the PATH TLV's own `length` field — admits at most
`⌊1020 / 5⌋ = 204` single-byte segments. The **byte** cap therefore binds before the count cap,
and the 255 is the encoding-independent ceiling that starts doing independent work only under
RFC-0018's packed body (which owns its own `path/path-deep-255-packed` vector).

**What this vector does and does not prove — measured, not assumed.** It pins the *bytes* of a
204-segment PATH: that 1020 is the largest body reachable at `4 + len` per segment, that `length`
is `FC 03` little-endian, and that a 1024-byte frame round-trips byte-identically. It does **not**
exercise the segment cap. All three harnesses (`cpp`, `ts`, `rust`) are **codec**-tier, and
`docs/reference/05-protocol-tlvs.md` §"Enforcement of the PATH constraints" says the codec does
not enforce the addressing bounds and is not expected to — so this vector was verified to pass
unchanged with the cap pinned back to 32 in all three cores. The cap itself is an **encode-time**
MUST and is covered by the per-implementation unit suites named below.

Outer: `type=0x06` PATH, `opt=0x40` (PL=1 only — not `0x50`, which would add CR), `length=1020`
(little-endian `FC 03`). 204 NAME children, each `02 00 01 00 61` (`type=0x02`, `opt=0x00`,
`length=1`, payload `"a"`, no NUL terminator, no inner trailer). **1024 bytes total** as graph
data — the outer 4-byte header plus the 1020-byte body.

```
06 40 FC 03  02 00 01 00 61  02 00 01 00 61  …  02 00 01 00 61
└── header ┘ └── ×204 NAME children (1020 B) ─────────────────┘
```

Meaning: `/a/a/a/…/a` — 204 segments.

A 205-segment path (1025 bytes) and a 256-segment path are both **encode-time rejects** and are
not expressible here: the caps are encode-time MUSTs, and `docs/spec/v1.md` §4's round-trip
procedure has no encode-reject shape. Those cases live in the per-implementation unit suites
(`core/tests/path_test.cpp`, `bindings/rust/tests/conformance_vectors.rs`,
`bindings/typescript/packages/client/test/vectors.test.mjs`).
