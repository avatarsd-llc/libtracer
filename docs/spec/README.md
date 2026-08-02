# libtracer protocol specification

This directory contains the **normative** specification of the libtracer wire protocol. Anything outside this directory (architecture notes, design rationale, implementation guides) is informative, **except the documents [v1.md](v1.md) §3 incorporates normatively by reference** — `docs/reference/01-data-format.md`, `docs/reference/05-protocol-tlvs.md`, and `docs/reference/03-addressing.md` §path syntax. Those files live under `docs/reference/`, but their MUST/SHOULD/MAY clauses are clauses of the specification; their *informative*-marked notes are not.

## Structure

```text
docs/spec/
├── v1.md                Spec version 1 (the current version)
├── rfcs/                Proposed changes — see GOVERNANCE.md
│   ├── 0000-template.md
│   └── NNNN-*.md
└── README.md            (this file)
```

## Versioning

Spec versions are integers (v1, v2, …). Once a version is published, it is **immutable** — corrections that change conformance behavior require a new version. Editorial fixes (typos, clarifications that do not change behavior) may be applied in place with a changelog entry.

## Conformance

An implementation is "libtracer vN compatible" if and only if it:

1. Honors every MUST clause in `vN.md`.
2. Passes every test vector under [`tests/conformance/`](https://github.com/avatarsd-llc/libtracer/tree/main/tests/conformance/) tagged for vN.

That is the entire compatibility contract. There is no certification authority.

## Proposing a change

See [GOVERNANCE.md](https://github.com/avatarsd-llc/libtracer/blob/main/.github/GOVERNANCE.md) for the RFC process.
