# Governance

libtracer is an open protocol with an open reference implementation. This document describes how decisions are made.

## Stewardship

The libtracer project is stewarded by **Avatar LLC**. The company:

- Holds the **"libtracer" trademark** and enforces the [trademark policy](TRADEMARKS.md).
- Holds **copyright** in contributions made by Avatar LLC employees and contractors. Outside contributions remain copyright of their authors, licensed under Apache 2.0 (code) or CC BY 4.0 (spec) per the Developer Certificate of Origin.
- **Funds and prioritizes** development of the reference implementation.
- **May build proprietary products and services on top of libtracer** (Layer 3 in the [README](README.md)) and reserves the right to do so without obligation to open-source those products.

The company explicitly does **not**:

- Control accepted RFCs once a steering committee is formed (currently the maintainer is the BDFL — see "Roles" below).
- Hold a veto over code contributions to the reference implementation beyond normal maintainer review.
- Restrict or revoke the open licenses on already-published code or spec versions. Once published, both are irrevocable under their respective licenses.
- Charge fees for trademark use by compatible implementations (see [TRADEMARKS.md](TRADEMARKS.md)).

If the project moves to a foundation or independent steering body in the future, Avatar LLC will document the transfer publicly. Until then, this section is the honest description of who holds what.

## Scope

There are three distinct decision domains, each with different rules:

1. **Protocol (the spec)** — wire format, framing, identifiers, conformance rules. Lives in `docs/spec/`. Changes here affect every implementation and every deployed device. **High bar.**
2. **Reference implementation** — code in `core/`, `bindings/`, `integrations/`. Changes here affect users of the reference impl but cannot break compatibility with implementations that follow the spec. **Normal bar.**
3. **Tooling, docs, examples** — everything else. **Low bar — PRs welcome.**

## Roles

- **Maintainers** — have commit access. Listed in [MAINTAINERS.md](MAINTAINERS.md). Currently a single maintainer (BDFL model). The project will move to a steering committee once there are at least three active independent contributors.
- **Contributors** — anyone who opens a PR or RFC.
- **Implementers** — maintainers of third-party implementations registered in [implementations/](implementations/). They have a standing seat in protocol-change discussions because spec changes affect them directly.

## Spec changes (Layer 1)

Anything that changes the wire format, conformance rules, or normative MUST/SHOULD clauses requires an **RFC**:

1. Open an issue tagged `rfc` describing the problem and proposed change.
2. If there is interest, open a PR adding a document under `docs/spec/rfcs/NNNN-short-title.md` using the template at `docs/spec/rfcs/0000-template.md`.
3. The RFC stays open for **at least 14 days** to give implementers time to respond.
4. The RFC is accepted if maintainers reach lazy consensus and no registered implementer raises a sustained objection.
5. Accepted RFCs are merged into a numbered spec version (e.g., v1 → v2). Spec versions are immutable once released.

**Backwards-incompatible changes require a major spec version bump.** Implementations declare which spec versions they support.

## Reference-implementation changes (Layer 2)

Normal PR flow. A maintainer reviews and merges. No RFC needed unless the change implies a spec change.

Bindings (`bindings/rust/`, `bindings/typescript/`) and integrations (`integrations/*`) may have their own sub-maintainers listed in their respective READMEs.

## Conflicts of interest

Maintainers who also work on proprietary products built on libtracer (Layer 3) MUST recuse themselves from RFC decisions where their employer has a direct competitive stake. The recusal is a comment on the RFC; no formal process beyond that.

## Trademark and the "libtracer" name

The "libtracer" name is governed separately — see [TRADEMARKS.md](TRADEMARKS.md). The maintainers hold the trademark. Trademark decisions are not subject to the RFC process.

## Changing this document

Same as a spec change: RFC, 14-day window, maintainer approval.
