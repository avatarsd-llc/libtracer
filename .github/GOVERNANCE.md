# Governance

libtracer is an open protocol with an open reference implementation. This document describes how decisions are made.

## Stewardship

The libtracer project is stewarded by **avatarsd LLC**. The company:

- Holds the **"libtracer" trademark** and enforces the [trademark policy](TRADEMARKS.md).
- Holds **copyright** in contributions made by avatarsd LLC employees and contractors. Outside contributions remain copyright of their authors, licensed under Apache 2.0 (code) or CC BY 4.0 (spec) per the Developer Certificate of Origin.
- **Funds and prioritizes** development of the reference implementation.
- **May build proprietary products and services on top of libtracer** (Layer 3 in the [README](../README.md)) and reserves the right to do so without obligation to open-source those products.

The company explicitly does **not**:

- Control accepted RFCs once a steering committee is formed (currently the maintainer is the BDFL — see "Roles" below).
- Hold a veto over code contributions to the reference implementation beyond normal maintainer review.
- Restrict or revoke the open licenses on already-published code or spec versions. Once published, both are irrevocable under their respective licenses.
- Charge fees for trademark use by compatible implementations (see [TRADEMARKS.md](TRADEMARKS.md)).

If the project moves to a foundation or independent steering body in the future, avatarsd LLC will document the transfer publicly. Until then, this section is the honest description of who holds what.

## Scope

There are three distinct decision domains, each with different rules:

1. **Protocol (the spec)** — wire format, framing, identifiers, conformance rules. Lives in `docs/spec/`. Changes here affect every implementation and every deployed device. **High bar.**
2. **Reference implementation** — code in `core/`, `bindings/`, `integrations/`. Changes here affect users of the reference impl but cannot break compatibility with implementations that follow the spec. **Normal bar.**
3. **Tooling, docs, examples** — everything else. **Low bar — PRs welcome.**

## Roles

- **Maintainers** — have commit access. Currently a single maintainer (BDFL model): **@avatarsd** (founder — spec, core, bindings, integrations), stewarded by **avatarsd LLC** (trademark and copyright holder; code Apache 2.0, spec CC BY 4.0). Sub-maintainer areas (`bindings/rust/`, `bindings/typescript/`, `integrations/platformio/`, `integrations/esphome/`) are open — volunteer via an issue tagged `maintainer-volunteer`; sub-maintainers own their area without a vote on protocol-level decisions outside it. The project will move to a steering committee once there are at least three active independent contributors.
- **Contributors** — anyone who opens a PR or RFC.
- **Implementers** — maintainers of third-party implementations registered in [docs/implementations.md](../docs/implementations.md). They have a standing seat in protocol-change discussions because spec changes affect them directly.

## Spec changes (Layer 1)

Anything that changes the wire format, conformance rules, or normative MUST/SHOULD clauses requires an **RFC**:

1. Open an issue tagged `rfc` describing the problem and proposed change.
2. If there is interest, open a PR adding a document under `docs/spec/rfcs/NNNN-short-title.md` using the template at `docs/spec/rfcs/0000-template.md`.
3. The RFC stays open for a comment window of **at least 14 days** — **waived by default** while the project has a single maintainer; see [Errata, amendments, and the comment window](#errata-amendments-and-the-comment-window).
4. The RFC is accepted if maintainers reach lazy consensus and no registered implementer raises a sustained objection.
5. Accepted RFCs are merged into a numbered spec version (e.g., v1 → v2). Spec versions are immutable once released.

**Backwards-incompatible changes require a major spec version bump.** Implementations declare which spec versions they support.

### Errata, amendments, and the comment window

Not every change to a normative document is a proposal. Two instruments exist, and picking the wrong one is why corrections stall:

- **Erratum** — a normative document contradicts behaviour that is **already shipped and already agreed**. The decision was made; only the text is wrong. An erratum lands as an ordinary PR with **no comment window**, and must state what the text said, what the behaviour is, and which change made them diverge. It may not alter the wire surface: if applying it would change what a conforming implementation does, it is not an erratum.
- **Amendment** — the normative surface itself changes: a new or altered MUST/SHOULD, a new error code, a new frame shape, a behaviour a conforming peer could observe. An amendment needs an RFC and maintainer approval. Its comment window is **waived by default** and is *invoked explicitly* when outside input is actually wanted — which is the point of a window, and is worth doing for anything a registered implementer would have to change code for.

**Why the default is "waived".** This is a solo-maintained spec: a window with no commenters is ceremony, and pretending otherwise has a real cost — correcting a doc that contradicts shipped code gets deferred for two weeks, during which the wrong text keeps being read and cited. The waiver is not new; it is being written down. Of the twelve accepted RFCs, **eleven record it** — RFC-0001 calls the window "dead ceremony", RFC-0016 calls the waiver "the standing solo-maintainer ruling", and RFC-0014 waives it "per the RFC-0009 precedent". The single exception is RFC-0004, accepted 2026-06-28, before the practice settled.

**This reverts the moment the project is not solo-maintained.** The window returns to mandatory as soon as there is a second maintainer or one registered implementer in `docs/implementations.md` — at which point a window has someone to serve.

## Reference-implementation changes (Layer 2)

Normal PR flow. A maintainer reviews and merges. No RFC needed unless the change implies a spec change.

Bindings (`bindings/rust/`, `bindings/typescript/`) and integrations (`integrations/*`) may have their own sub-maintainers listed in their respective READMEs.

## Conflicts of interest

Maintainers who also work on proprietary products built on libtracer (Layer 3) MUST recuse themselves from RFC decisions where their employer has a direct competitive stake. The recusal is a comment on the RFC; no formal process beyond that.

## Trademark and the "libtracer" name

The "libtracer" name is governed separately — see [TRADEMARKS.md](TRADEMARKS.md). The maintainers hold the trademark. Trademark decisions are not subject to the RFC process.

## Changing this document

Same as a spec change: RFC and maintainer approval, with the comment window governed by [Errata, amendments, and the comment window](#errata-amendments-and-the-comment-window).
