# Identity is app level: the node key is the only identity the protocol knows, an optional opaque credential rides beside it, and anchored names are a verifier policy in an integration module

<!-- status: accepted -->

Status: **accepted** (maintainer-ratified 2026-10-07, identity grill held during the IETF publication rollout). Amends [ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md): its Decisions 1, 2 and 5 stand; its Decision 3 (raw ed25519 keys, trust on first use) stands as the default and gains an optional credential and a verifier policy; its Decision 4 ("the path is a Noise-pattern channel") becomes a normative link binding. Builds on the subject seam of [ADR-0018](0018-access-control-authorization-pluggable-subject-token.md), the `:identity` facet of [RFC-0011](../spec/rfcs/0011-node-identity-facet.md), and the subject/addressing split of [ADR-0082](0082-auth-subject-and-peer-named-are-decoupled-claims-default-stays-false.md). The wire part lands later, through its own RFC (Decision 9).

## Context

[ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md) made the raw ed25519 public key the identity, paired by trust on first use (TOFU), and [RFC-0011](../spec/rfcs/0011-node-identity-facet.md) gave that key a read surface, `:identity`. TOFU answers "is this the same key I saw before". It does not answer "is this key the one my own deployment vouches for under this name", which is what an operator asks when ten boards come out of a box and one phone is meant to own them.

The talk "Why Naming Matters" at the IETF 125 DINRG session argued that a usable identity is a **name bound to a key**, and that the binding should be checked against **local trust anchors** rather than a global authority. That frame fits libtracer: names here are Tracer paths, and the trust anchor a deployment already has is the device or person that provisions it.

Before the v1.0 wire freeze, the question is how much of this the protocol must carry. Three facts constrain the answer:

- Noise authenticates **keys**, nothing else. Whether a key is acceptable is decided after the handshake. TOFU and an anchored name differ only in that check.
- The core takes no cryptographic stance ([ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md) Considered options: "crypto in core" rejected) and must still fit a 16 KB-class device.
- Permissions already have one home: the receiver's ACL behind the subject token ([ADR-0018](0018-access-control-authorization-pluggable-subject-token.md)).

## Decision

**1. Identity is app level.** The node key (`:identity`, [RFC-0011](../spec/rfcs/0011-node-identity-facet.md)) is the only cryptographic identity the protocol knows. There is no single device identity: a name, an owner, a role, a serial number are all things an application or a policy derives from the key, and none of them is a protocol concept.

**2. One optional, opaque credential rides beside the key.** The wire MAY carry one **credential**: an opaque byte string next to the key, in the `:identity` record and in the Noise handshake payload. It is optional; a node without one behaves exactly as today. The core never parses, verifies or interprets it; it only carries the bytes to the verifier policy.

**3. A verifier policy maps key + credential to a subject token.** The mapping is a compile-time policy behind the existing subject seam ([ADR-0018](0018-access-control-authorization-pluggable-subject-token.md); today `subject_resolver_fn_t` in `core/include/libtracer/graph.hpp`). The default policy is a no-op: the subject is derived from the key alone, which is TOFU. Any cryptographic check lives in an integration module, never in core, so a build with the default policy links no verifier code.

**4. Permissions stay in the receiver's ACL.** The credential says *who* (a name the anchor vouches for); the receiver's ACL says *what that subject may do*. A credential never carries rights. Delegation chains (an anchor vouching for an anchor) are out of scope.

**5. Anchored names ship as a default verifier policy in an integration module, not as normative text.** A local **anchor key** signs one statement:

> (Tracer path in the anchor's namespace, subject key, generation)

The verifier holds a store of trusted anchor keys and accepts a credential whose signature verifies under one of them, whose key matches the handshake key, and whose generation is not lower than the highest it has seen for that name. The anchor itself MAY be trusted on first use, so a phone or laptop that meets the devices first becomes their anchor without any factory step. **Enrolment** is an ordinary write: the anchor writes the signed statement to an ACL-gated vertex on the device, as [ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md) Decision 1 does for login. A build-time anchor (a key compiled into the firmware) stays available as an option for fleets that want one.

**6. No record expires, because there are no clocks.** Nothing in the credential or the anchor store carries a time ([`CLAUDE.md`](../../CLAUDE.md) §Design rules: no timers or clock reads in libtracer). Trust in a name ends in one of two ways:

- the anchor **re-enrols** the name with a higher generation; verifiers keep the highest generation seen per name and refuse lower ones;
- the anchor is **removed** from the verifier's store, which drops every name it vouched for.

An application that wants expiry checks it in its own verifier policy against its own clock.

**7. Per-hop identity stays, and this is not the rejected PKI.** [ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md) Decision 5 stands: trust is per hop, and X.509 with a global CA stays rejected. One level of vouching by a local anchor is a different thing, and the difference is the cost ADR-0045 rejected:

- **no ASN.1**: the statement is three fixed fields, not a DER certificate;
- **no chains**: one signature by one anchor; there is no path to build or validate;
- **no CA store**: the verifier trusts the anchors its own deployment enrolled, not a list shipped by a third party;
- **one ed25519 verify**: the whole check costs what a TOFU login already costs.

**8. Noise becomes an optional normative link binding, "Tracer over Noise".** The spec gains a binding that a link MAY use and, if it uses Noise, MUST follow:

- handshake pattern **XX** or **IK**;
- the static key is the node's **ed25519** key (the `:identity` key, used in its X25519 form for the Diffie-Hellman);
- the credential, when present, rides in the **handshake payload**.

A Noise link that does not follow the binding is not a conforming Tracer link. The binding fixes interop; the module catalog's `security_noise` slot remains the implementation.

**9. Wire timing.** The wire part (the credential member of `:identity` and the Tracer over Noise binding) lands **before the v1.0 freeze**, through one RFC: an amendment of [RFC-0011](../spec/rfcs/0011-node-identity-facet.md) plus the binding. This ADR records the rulings; it does not change the spec.

## Terms

CONTEXT.md is at its line cap, so the two terms this ADR introduces are defined here until a glossary line frees up.

- **Credential (opaque)**: an optional byte string a node presents beside its key, in `:identity` and in the Noise handshake payload. The core carries it and never interprets it; a verifier policy may map it, with the key, to a subject token. _Avoid_: "certificate" (it implies X.509), "credential grants rights" (rights live in the receiver's ACL).
- **Anchored name**: a Tracer path bound to a key by a signature of a local anchor key, with a generation that only grows. Checked by an integration-module verifier policy, never by core; not normative. _Avoid_: "device identity" (the key is the identity, the name is a policy's view of it), "certificate chain" (there is one level).

## Considered options

- **TOFU only (no credential).** Rejected: it leaves every deployment that wants names to invent its own side channel, and a wire member added after the v1.0 freeze would be an incompatible change. An empty optional member costs nothing now.
- **A normative anchored-name format.** Rejected: it would make one naming policy part of the protocol and force every implementation to carry its verifier. The opaque credential lets the anchored-name policy, or any other, ride without the spec choosing.
- **Rights in the credential (a capability).** Rejected for now: [ADR-0018](0018-access-control-authorization-pluggable-subject-token.md) keeps rights in the receiver's ACL, and a capability would need delegation and revocation the no-clock rule cannot express.
- **Expiry fields in the statement.** Rejected: checking them needs a clock in the verifier. Generation plus anchor removal ends trust without one.
- **X.509 and a global CA.** Stays rejected, see Decision 7.
- **Noise left to the implementation.** Rejected: two implementations that pick different patterns or key handling cannot talk, so a published spec needs the binding.

## Consequences

- The protocol's identity surface stays one key per node, plus one optional opaque member. Nodes that never set a credential are unchanged on the wire.
- A deployment gets names by choosing a verifier policy, not by waiting for a protocol change. The anchored-name policy is the shipped default for that choice, on ESP-IDF and host.
- The core stays crypto-free. The verifier code and its anchor store live in the integration module and cost nothing in a build that does not select it.
- Trust ends by event, never by time: re-enrolment with a higher generation, or anchor removal.
- Writing the RFC (Decision 9) is the next step. It must land before the v1.0 wire freeze, so it blocks publishing the protocol draft.
