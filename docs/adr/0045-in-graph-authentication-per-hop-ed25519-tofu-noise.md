# Authentication is in-graph vertex operations over the existing subject seam; the identity roadmap is per-hop ed25519 raw-key TOFU plus Noise link encryption; X.509 PKI and end-to-end multi-hop identity are rejected

Status: accepted (maintainer-ratified 2026-07-03, migration design grilling for the originating production firmware — an ESP32-C6 smart-agriculture node). Fills the token-provenance slot [ADR-0018](0018-access-control-authorization-pluggable-subject-token.md) deliberately reserved, over the ACE model of [ADR-0020](0020-acl-nfsv4-style-aces-with-inheritance.md); link confidentiality builds on [ADR-0043](0043-quic-webtransport-optional-module-msquic.md) (TLS 1.3 on QUIC/WebTransport) and the catalog's `security_noise` slot. Companion to [ADR-0044](0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md) and [ADR-0046](0046-bulk-transfer-is-ordinary-auth-gated-writes.md).

## Context

[ADR-0018](0018-access-control-authorization-pluggable-subject-token.md) pinned authorization (device-held `subject → rights`, enforced locally) and made the subject a **pluggable token**, explicitly deferring identity-provenance. The ACL core subset has since landed (`subject_resolver_t` / `subject_token_t` in `core/include/libtracer/graph.hpp`). The origin-firmware migration now forces the deferred half: the SPA logs into boards today via an HMAC challenge-response (the origin firmware's SEC-001), and that flow must survive the move to libtracer — without inventing a login *protocol*, and without dragging crypto into a core that runs on 16 KB-class devices.

The question has three parts: *how* a connection authenticates (the mechanism's shape), *what* identity is (the credential model), and *where* confidentiality comes from (the link). Each got a ruling.

## Decision

**1. Authentication is graph operations on ordinary vertices — no wire verb, no protocol phase.** A login is a read/write dance against app-defined vertices, e.g.:

- `read /device/auth/challenge` → a nonce;
- `write /device/auth/login` (credential proof over the nonce) → on success, the **connection's subject rebinds** from `anonymous` to the authenticated subject, through the **existing `subject_resolver_t` / `subject_token_t` seam**.

Crypto and credential storage stay **app/host-owned**; core gains **zero crypto** — it only ever sees an opaque token change. Because the dance is plain reads/writes, it is **transport-uniform**: the same vertices work over WS, TCP, and QUIC without any transport knowing auth exists.

**2. `anonymous` is a real, ACL-configurable subject.** The default ACL ships **closed except the auth subtree** — an unauthenticated connection can reach `/device/auth/*` and nothing else. A deployment MAY deliberately open paths to `anonymous` (the kiosk case: a public read-only dashboard) with ordinary [ADR-0020](0020-acl-nfsv4-style-aces-with-inheritance.md) ACEs; openness is a per-deployment ACL choice, never a default.

**3. Bridge now, target next.** *Now:* the existing origin-firmware SEC-001 HMAC challenge-response, expressed as exactly these vertices — no security regression, no new crypto, immediate migration path. *Target:* **per-hop identity via raw ed25519 keypairs** — the public key **is** the identity; pairing is **trust-on-first-use** (the SSH/WireGuard model, not certificate enrollment); the login write carries a **signature** over the nonce instead of an HMAC; pubkey-derived subject ids slot into the ACL model **unchanged** — this is precisely the "stronger token" [ADR-0018](0018-access-control-authorization-pluggable-subject-token.md) reserved the seam for.

**4. Link confidentiality is the link's job.** QUIC/WebTransport already carry TLS 1.3 ([ADR-0043](0043-quic-webtransport-optional-module-msquic.md)) — the hosted links are covered. For plaintext MCU links (WS/TCP on-device) the path is a **Noise-pattern channel** — the module catalog's `security_noise` slot — NOT dragging full TLS (X.509 parsing, cert chains, a CA store) onto 16 KB-class devices. Noise composes naturally with raw ed25519 identities; TLS does not.

**5. Rejections and non-goals, stated plainly.**

- **X.509 / CA PKI — rejected.** It reintroduces a center (a CA) into a deliberately decentralized system, and cert-chain parsing/validation is exactly the footprint MCU-class devices cannot carry. Raw keys + TOFU deliver the identity property the deployments in scope need.
- **End-to-end identity across FWD hops — deferred, out of scope.** Trust is **per-hop and transitive**: board2 authenticates *board1's link*, not the browser behind it. A FWD arriving over an authenticated link carries that link's subject, full stop. Nobody should assume end-to-end identity exists — it does not, and this ADR says so precisely so the assumption cannot creep in silently.
- **CAN — out of scope.** CAN is a physically-trusted bus (an enclosure-internal harness); authenticating it solves no threat the deployments have.

**Honest caveat:** on a plaintext link, signature login leaks no long-term secret (unlike token replay under HMAC-with-shared-secret misuse), but an authenticated **session can still be hijacked mid-flight** by an on-path attacker until the Noise channel lands. This is **no regression** versus today's SEC-001 over plaintext WS — it is the same exposure, now stated instead of implied.

## Considered options

- **A wire-level AUTH frame / protocol login phase.** Rejected: it would be the first operation that is not read/write/await ([ADR-0006](0006-read-write-await-api-no-connect.md)), would need per-transport plumbing, and buys nothing the vertex dance lacks — the graph already has request/response, ACL gating, and observability.
- **Crypto in core** (core validates signatures/HMACs itself). Rejected: core takes no adversarial-integrity stance (CRC is a bit-flip check, [ADR-0004](0004-crc-in-optional-trailer.md)); credential verification is host policy behind the resolver seam, keeping the 16 KB profile crypto-free and letting hosts pick their primitive.
- **X.509 PKI as the identity model.** Rejected — see Decision 5.
- **End-to-end (origin-signed) identity across hops now.** Deferred — see Decision 5. If a deployment ever needs it, it layers as signed payloads at the application level, not as protocol machinery.
- **Full TLS on MCU plaintext links.** Rejected in favor of Noise — see Decision 4.

## Consequences

- The origin firmware's login migrates as-is: SEC-001's HMAC dance becomes two vertex operations; the SPA's auth code becomes ordinary SDK reads/writes.
- The ACL model of [ADR-0018](0018-access-control-authorization-pluggable-subject-token.md)/[ADR-0020](0020-acl-nfsv4-style-aces-with-inheritance.md) is untouched — subjects strengthen (`anonymous` → HMAC-authed → ed25519-pubkey-derived) with zero model change, which is the seam doing exactly what it was built for.
- Devices ship **closed by default**: `anonymous` sees only the auth subtree unless a deployment writes ACEs saying otherwise.
- The security roadmap is now ordered and bounded: vertices-as-auth (now) → ed25519 TOFU login (target) → Noise on plaintext links (`security_noise`); each step is independently shippable and none touches core.
- Client-side identity matching across paths ([ADR-0044](0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)) gains its natural key once ed25519 lands: the pubkey a client authenticated against is the strongest cross-path identity available — still applied client-side, never by the protocol.
