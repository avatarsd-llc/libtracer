# The libtracer specification

The **normative** specification of the libtracer wire protocol. This page is the
entry point: what is normative, how the layers relate, and in what order to read
them. The specification itself is [Protocol v1](v1.md).

## What is normative

Normative status is not a property of a directory. It is declared by
[v1.md](v1.md), and it reaches outside `docs/spec/`:

- **[Protocol v1](v1.md)** is the normative core — scope, terminology, the
  conformance procedure, and the static path-handle requirements.
- **Three reference documents are incorporated as normative annexes** by
  [v1.md §3](v1.md#3-wire-format). Their MUST/SHOULD/MAY clauses are clauses of
  the specification; their paragraphs marked *informative* are not:
  - [01-data-format.md](../reference/01-data-format.md) — frame layout, the
    `opt` bits, fixed-width lengths, the trailer.
  - [05-protocol-tlvs.md](../reference/05-protocol-tlvs.md) — the type-code
    registry and each core type's byte-precise payload layout.
  - [03-addressing.md](../reference/03-addressing.md) §path syntax — canonical
    PATH constraints.

  A reader of `v1.md` alone cannot see the frame layout at all; the annexes are
  where the bytes are.
- **Everything else is informative** — the rest of the [reference
  suite](../reference/README.md), the [design notes](../design/README.md), the
  [C++ API reference](../modules/index.md), and the rationale record (ADRs) and
  change proposals (RFCs) that live in the repository. When an informative
  document and the specification disagree, the specification wins.

## How the specification is layered

Three tiers, read in this order:

1. **[v1.md](v1.md) plus its incorporated annexes** — what a conforming
   implementation must do.
2. **RFCs** ([`docs/spec/rfcs/`](https://github.com/avatarsd-llc/libtracer/tree/main/docs/spec/rfcs/))
   — the change instruments. An accepted RFC is the record of *why* a normative
   clause reads the way it does; the clause itself lives in `v1.md` or an annex.
3. **ADRs** ([`docs/adr/`](https://github.com/avatarsd-llc/libtracer/tree/main/docs/adr/))
   — architecture decisions and their errata: the rationale behind the reference
   implementation and the protocol shape.

RFCs and ADRs are contributor instruments and are not published on this site.

## Versioning

Spec versions are integers (v1, v2, …). Once a version is published it is
**immutable** — a correction that changes conformance behavior requires a new
version. Editorial fixes (typos, clarifications that do not change behavior) are
applied in place with a changelog entry.

## Conformance

An implementation is "libtracer vN compatible" if and only if it:

1. Honors every MUST clause in `vN.md` and in the documents `vN.md` incorporates.
2. Passes every test vector under
   [`tests/conformance/`](https://github.com/avatarsd-llc/libtracer/tree/main/tests/conformance/)
   tagged for vN.

That is the entire compatibility contract. There is no certification authority.
The [capability matrix](../capability-matrix.md) records how far each known
implementation is verified, and the [implementation
registry](../implementations.md) is where an independent implementation is
listed.

## Proposing a change

The instrument depends on what moves. An **erratum** corrects text that
contradicts already-agreed behavior and never changes the wire surface; an
**amendment** changes the normative surface itself and needs an RFC. See
[GOVERNANCE.md](https://github.com/avatarsd-llc/libtracer/blob/main/.github/GOVERNANCE.md)
for the process.

```{toctree}
:hidden:
:maxdepth: 2

Protocol v1 — the wire format <v1>
```
