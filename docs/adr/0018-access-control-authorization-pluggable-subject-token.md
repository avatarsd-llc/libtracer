# Access control is authorization over a pluggable subject-token; ACL-lists and capabilities are one model, not rivals

Status: accepted

The protocol reserves an `ACL` type code (`0x0A`) and a `PermissionDenied` status, but L4 implements neither — access control is greenfield. Two pressures force a decision now: in-band vertex creation ([ADR-0017](0017-in-band-vertex-creation-controller-orchestration.md)) is defined as *"ACL-gated,"* and the orchestration use case needs **third-party binding** (an orchestrator that is neither producer nor consumer grants a consumer the right to subscribe a producer). This ADR pins the access model and, crucially, the relationship between the ACL-list and capability approaches that initially looked like rivals.

## Decision

**Access control is authorization, not authentication.** A device holds, per vertex or field, a list of `subject → rights` where rights ∈ `{read, write, subscribe, create, admin}`, and enforces it locally on each operation (`PermissionDenied` on failure). libtracer's core does **not** authenticate; the *authenticity* of a subject's identity is the transport / security module's job (TLS, the P3 profile). On an unauthenticated bus (bare UART/CAN) ACL is therefore **advisory**.

**The subject is a pluggable token.** This is the load-bearing move: it separates *authorization* (the ACL, stable) from *identity-provenance* (the token, evolvable). v1 uses the transport-authenticated **`origin_peer_id`** as the token. A later security module may supply **PKI / asymmetric credentials** as a stronger token **without changing the ACL model at all**.

**ACL-lists and capabilities are the same model over a weaker vs. stronger token — not rival designs.** An "ACL entry" is `subject→rights` the device stores; a "capability" is the same `subject→rights` carried by the requester as a signed token the device verifies. The axis that actually differs is the *token's* self-describing-ness and cryptographic strength — which is exactly the pluggable part. So we do not "choose ACL over capabilities"; we ship the **authorization model now** and defer the **token exchange / key management (PKI, asymmetric)** as a separate module.

**An owner peer is the root of trust.** A device is provisioned with an **owner peer** that bootstraps its ACL and can **delegate `admin`** to orchestrators. Third-party binding is then: an orchestrator holding delegated `admin` on producer P writes a grant into P's `:acl` (e.g. "consumer C may subscribe") and wires the subscription — all through ordinary field-writes.

## Considered options

- **Capabilities-first (signed tokens as the v1 model).** Rejected for v1: requires signing/verification crypto, too heavy for a Cortex-M0, and unnecessary for the trusted-fleet use case of the originating production firmware (an ESP32-C6 smart-agriculture node). Re-framed as a *stronger token* under the same ACL model, deferred to a security module.
- **Hardcode `origin_peer_id` as the subject.** Rejected: it forecloses the evolution to PKI; the subject must be a *token abstraction* so the identity layer can strengthen without touching authorization.
- **Put authentication in the protocol core.** Rejected: authenticity is a transport/security-module concern (the protocol already takes no adversarial-integrity stance — CRC is a bit-flip check, not a MAC). Conflating authn into L4 would bloat the required modules and the MCU footprint.
- **No access control (rely on network isolation).** Rejected: in-band creation ([ADR-0017](0017-in-band-vertex-creation-controller-orchestration.md)) makes "who may create a controller / subscribe a sensor" a real authorization question; an open creation field is a foot-gun.

## Consequences

- v1 ships a **minimal, MCU-fittable** access model (peer_id subject + device-held ACL-list) that **evolves to PKI without a model change** — only the token strengthens.
- **Authorization is decoupled from authentication**: ACL is meaningful exactly to the degree the transport authenticates the token; this is explicit, not hidden.
- **Third-party binding** works via delegated `admin` from the owner peer — no special primitive.
- ACL enforcement is **per-operation at L4** (read/write/subscribe/create), keyed on the resolved subject-token; the `ACL 0x0A` TLV carries grant entries, written via `:acl` field-writes.
- This is an **implementation/L4** decision, not a wire-format change beyond using the reserved `ACL` code; two conforming nodes still interoperate over the wire ([ADR-0013](0013-v1-scope-boundaries.md)).
