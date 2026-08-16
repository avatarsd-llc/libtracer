# path/path-deep-204

204 single-byte segments — the depth that was the **byte-derived ceiling under the retired
`NAME`-child body**, kept as a vector after [RFC-0018](../../../../../docs/spec/rfcs/0018-packed-path-segments.md)
packed the body, and now a plain deep-path round-trip rather than a ceiling.

RFC-0023 repriced the segment cap 32 → 255. Under the `NAME`-TLV body grammar each segment cost
`4 + len`, so the normative 1024-byte PATH budget — measured as the PATH TLV's own `length` field
— admitted at most `⌊1020 / 5⌋ = 204` single-byte segments: the **byte** cap bound first and the
255 count could never fire. RFC-0018 packed each segment to `1 + len`, which moves the crossover:
1024 bytes now admit 512 one-byte records, so the **count** cap binds first for short segments.
That new ceiling is pinned by `path/path-deep-255-packed`, which RFC-0018 §5 owns; this vector
keeps the 204 depth so the two sit side by side and the crossover is visible in the tree.

**What this vector does and does not prove — measured, not assumed.** It pins the *bytes* of a
204-segment packed PATH: that the body is `204 × 2 = 408` bytes, that `length` is `98 01`
little-endian, and that a 412-byte frame round-trips byte-identically. It does **not** exercise
the segment cap. All three harnesses (`cpp`, `ts`, `rust`) are **codec**-tier, and
`docs/reference/05-protocol-tlvs.md` §"Enforcement of the PATH constraints" says the codec does
not enforce the addressing bounds and is not expected to — so this vector was verified to pass
unchanged with the cap pinned back to 32 in all three cores. The cap itself is an **encode-time**
MUST and is covered by the per-implementation unit suites named below.

Outer: `type=0x06` PATH, `opt=0x00` (`PL` MUST be 0 under the packed body — not `0x40`, which was
the pre-RFC-0018 spelling, and not `0x10`, which would add CR), `length=408` (little-endian
`98 01`). 204 packed segment records, each `01 61` (`len=1`, payload `"a"`, no NUL terminator —
a record has no type byte, no option byte and no trailer). **412 bytes total** as graph data —
the outer 4-byte header plus the 408-byte body.

```
06 00 98 01  01 61  01 61  …  01 61
└── header ┘ └── ×204 segment records (408 B) ──┘
```

Meaning: `/a/a/a/…/a` — 204 segments.

A 256-segment path is an **encode-time reject** and is not expressible here: the caps are
encode-time MUSTs, and `docs/spec/v1.md` §4's round-trip procedure has no encode-reject shape.
Those cases live in the per-implementation unit suites (`core/tests/path_test.cpp`,
`bindings/rust/tests/conformance_vectors.rs`,
`bindings/typescript/packages/client/test/vectors.test.mjs`).
