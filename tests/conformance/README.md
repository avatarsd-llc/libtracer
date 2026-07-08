# Conformance test vectors

Cross-implementation test data. **Every implementation of libtracer — the reference impl, the Rust binding, third-party reimplementations — runs the same vectors.** Passing them is the operational definition of compatibility (`docs/spec/v1.md` §4).

## Layout

Vectors are organized **by protocol surface (category)**, one directory per test case, each with three files:

```text
tests/conformance/
└── vectors/
    └── v1/                         Protocol version 1 (see ADR-0002 — protocol vs release version)
        ├── framing/                header, opt bits, length widths, minimum frame
        ├── path/                   PATH TLVs, path handles, segment limits
        ├── tlv-types/              per-type-code payloads (VALUE, NAME, SUBSCRIBER, …)
        ├── errors/                 ERROR / STATUS, the error-code registry
        ├── crc/                    CRC-32C / CRC-16 trailer verification
        ├── address-shift/          ep[0..N] slicing and reassembly
        └── router-dedup/           ROUTER envelope, (origin_peer_id, ts) dedup
            └── <case-name>/
                ├── input.bin       raw bytes the parser receives
                ├── expected.json   the decoded structure (machine-readable)
                └── description.md  human description + byte breakdown
```

Negative cases (bytes every core MUST refuse — e.g. a reserved opt bit set) carry
`reject.bin` **instead of** `input.bin`, and their `expected.json` names the
required decode error in a top-level `"reject"` field. See [HARNESS.md](HARNESS.md).

> This category layout (per r1 Q5.1) is canonical. It supersedes the earlier `encode/decode/roundtrip/` sketch and the `path_canonical/` mention in `docs/reference/02-graph-model.md`; those are reconciled by the v0.1 consolidation RFC. A driver in each implementation walks `input.bin` → decode → compare against `expected.json`, and re-encodes `expected.json` → compare against `input.bin` (round-trip).

## Seed vectors

Four golden frames, taken byte-for-byte from the reference worked examples:

| Category | Case | Bytes | Frame |
| ---- | ---- | ---- | ---- |
| `framing` | `empty-status-ok` | 4 | `09 00 00 00` (empty STATUS=OK / unsubscribe sentinel) |
| `tlv-types` | `value-bool-true` | 5 | `01 00 01 00 01` (VALUE boolean true) |
| `path` | `path-sensor-temp` | 22 | `06 40 12 00 …` (static PATH `/sensor/temp`, `opt=0x40` PL-only) |
| `crc` | `value-crc32c` | 13 | `01 10 05 00 AA BB CC DD EE B6 C9 12 23` (VALUE + CRC-32C `0x2312C9B6`) |

## Adding vectors

A new vector is appropriate when:

- A spec change (new RFC) requires verifying behavior across implementations.
- A bug surfaced ambiguity that the spec did not pin down — fix the spec first, then add a vector future implementations would have caught.

Vectors for a published spec version are **append-only**. Removing or changing a vector for a frozen version is a spec change.
