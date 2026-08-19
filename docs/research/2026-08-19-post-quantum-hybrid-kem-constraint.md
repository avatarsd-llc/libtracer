# Post-quantum: a hybrid-KEM constraint on the in-graph handshake, recorded before it is specced

> **Status**: constraint note (descriptive), 2026-08-19. Filed against
> [#1387](https://github.com/avatarsd-llc/libtracer/issues/1387). This is **not an RFC** and
> decides nothing — it records a requirement the future handshake RFC has to satisfy, and
> prices it in bytes so the requirement can be argued rather than asserted. The auth
> *decisions* on record live in [ADR-0045](../adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md)
> and [RFC-0011](../spec/rfcs/0011-node-identity-facet.md); where this note and those
> disagree, they win. Sibling to the horizon note tracked in
> [#1385](https://github.com/avatarsd-llc/libtracer/issues/1385), where post-quantum is gap 2.

## 1. Why the timing matters

libtracer freezes its wire on release and treats protocol v1 as immutable thereafter
([CONTEXT.md](../../CONTEXT.md) §Protocol version). A handshake specified without a
post-quantum escape hatch is therefore not a thing that gets patched later — it is a
protocol v2. The cost asymmetry is the whole argument: recording the constraint now costs
this page, and discovering it after the handshake ships costs a wire break.

The threat that bites first is **harvest-now-decrypt-later**: an on-path observer records
today's session traffic and decrypts it once a cryptographically relevant quantum computer
exists. That threat lands on the *session key agreement*, not on the identity signature —
a signature only has to resist forgery while it is being verified, which is why the two
halves of this note carry different urgency.

## 2. Prior art in this tree — cited, not restated

The auth direction is already on record and this note does not re-derive it:

- **Per-hop raw-ed25519 TOFU identity** — [ADR-0045](../adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md)
  §Decision 3: the public key *is* the identity, pairing is trust-on-first-use, and
  pubkey-derived subject ids slot into the existing ACL model unchanged. X.509 / CA PKI is
  rejected there (§Decision 5), so there is no certificate layer in which an algorithm could
  hide.
- **Noise-class channel for plaintext MCU links** — ADR-0045 §Decision 4: QUIC/WebTransport
  already carry TLS 1.3, so the gap is WS/TCP on-device, and the named filler is the module
  catalog's `security_noise` slot — status **future**
  (`docs/reference/10-module-catalog.md:142`). Nothing has been specced against that slot yet:
  the highest RFC in the tree is [RFC-0027](../spec/rfcs/0027-label-switched-path-compression.md),
  and none of 0001–0027 is a handshake.
- **The carrier already exists.** [reference/16](../reference/16-websocket-session-auth.md)
  §"The payload is opaque" commits the session-auth frame to being a *carrier*, with a
  three-valued verdict — accept / **continue** / reject — where `continue` explicitly admits
  "a multi-round-trip handshake", and any verdict may carry a reply payload. That page already
  names an ed25519/Noise handshake as the intended future payload. So the transport-side
  plumbing a hybrid handshake needs is in place; what is missing is the handshake itself.
- **The identity record is already algorithm-tagged** — [RFC-0011](../spec/rfcs/0011-node-identity-facet.md)
  (accepted 2026-07-19). See §4.

## 3. The constraint

> **The RFC that specifies the in-graph session handshake MUST specify a hybrid key
> agreement: a classical ECDH (X25519) share and a post-quantum KEM (ML-KEM, FIPS 203)
> encapsulation, combined so that the session secret survives the failure of either one
> alone.**

This is phrased as a requirement *on that RFC*, not as a decision made here. What it forbids
is a handshake whose session secret depends on X25519 alone with "PQ later" left as an
unspecified upgrade path — because "later" is protocol v2.

Three sub-requirements the RFC has to answer explicitly rather than inherit:

1. **Combiner.** Both secrets must be inputs to the key schedule such that an attacker needs
   to break *both* — the established pattern used by the hybrid TLS and hybrid-Noise work
   (descriptively: an X25519 + ML-KEM-768 hybrid group, commonly written `X25519MLKEM768`).
   This note deliberately does **not** cite a draft number for those; the RFC should pin the
   exact published document it follows at the time it is written.
2. **Parameter set.** Not chosen here. §5 prices ML-KEM-512 and ML-KEM-768 so the choice can
   be made on numbers.
3. **Transcript binding.** Whatever the combiner, the identity signature must bind the *whole*
   transcript including the PQ share, or the hybrid is a decoration.

Not in scope for the constraint: PQ **signatures** for identity. A classical ed25519 identity
key is acceptable at ship time — see §4 for why that is safe *given* the tag, and §5 for what
changing it would cost.

## 4. Algorithm-tagged identity — already satisfied, with two consequences

Issue #1387 item 2 asks that the identity format carry an algorithm tag from day one. **The
tree already does this**, and the note records that rather than requesting it.

RFC-0011 shipped `:identity` as
`SETTINGS{ NAME "kind" VALUE u8, NAME "key" VALUE <key bytes> }` in that fixed order, with an
**RFC-gated identity-kind registry** in which `0x01` = ed25519 raw public key, exactly 32
bytes (`docs/reference/05-protocol-tlvs.md:822`, registry at `:830-835`). A `kind` that contradicts the
`key` length is rejected as `TYPE_MISMATCH` and never reaches the wire. The complete ed25519
record is **60 bytes**.

So the constraint here is **non-regression** — the handshake RFC must not introduce a second,
untagged place where a key algorithm is implied by position or by context — plus two
consequences that follow from the format and are worth stating before someone rediscovers
them mid-RFC:

**(a) A PQ identity kind is a registry addition, not a format break.** The default TLV length
field is a fixed-width u16 (`docs/reference/01-data-format.md:17`), so a `VALUE` may carry up
to 65,535 bytes without even setting `opt.LL`. Every ML-DSA public key in FIPS 204 fits with
four orders of magnitude to spare. Adding, say, `kind 0x02 = ML-DSA-65 raw public key, exactly
1952 bytes` is one row in the registry and one RFC — no wire-format change, no v2. That is the
tag doing exactly the job #1387 wants it to do.

**(b) `:identity` is pre-auth readable, so a PQ kind is an amplification question.** The
record resolves **above** the READ gate — deliberately, because TOFU pinning is impossible
otherwise (`core/src/graph.cpp:3574-3577`; CONTEXT.md §Node identity). Today an
unauthenticated peer can pull 60 bytes. With an ML-DSA-65 key that same unauthenticated read
returns **1,980 bytes** (60 − 32 + 1952), a **33x** amplification of a pre-auth,
pre-rate-limit surface. That is not an objection to a PQ kind; it is a question the RFC that
adds one must answer (rate limiting? a truncated fingerprint kind for the pre-auth read, with
the full key served post-accept?), and it is much cheaper to answer in the RFC than in a
CVE.

## 5. The NARROW cost, in bytes

### 5.1 The primitives

Public sizes, from the published standards — cited so the numbers can be checked rather than
believed. Key/ciphertext sizes: **FIPS 203** (ML-KEM) Table 3; signature/key sizes: **FIPS 204**
(ML-DSA) Table 2. Classical baselines: X25519 (RFC 7748) and ed25519 (RFC 8032).

| Primitive | Public key / ek | Ciphertext / signature | Private key / dk | Shared secret |
| ---- | ---- | ---- | ---- | ---- |
| X25519 | 32 B | — | 32 B | 32 B |
| ed25519 | 32 B | 64 B (sig) | 32 B | — |
| ML-KEM-512 | 800 B | 768 B | 1,632 B | 32 B |
| ML-KEM-768 | 1,184 B | 1,088 B | 2,400 B | 32 B |
| ML-DSA-44 | 1,312 B | 2,420 B (sig) | 2,560 B | — |
| ML-DSA-65 | 1,952 B | 3,309 B (sig) | 4,032 B | — |

### 5.2 What a hybrid handshake therefore costs

For an X25519 + ML-KEM-768 hybrid, per handshake:

| Quantity | Classical | Hybrid | Delta |
| ---- | ---- | ---- | ---- |
| Initiator share on the wire | 32 B | 32 + 1,184 = **1,216 B** | +1,184 B |
| Responder share on the wire | 32 B | 32 + 1,088 = **1,120 B** | +1,088 B |
| Handshake bytes, both directions | 64 B | **2,336 B** | **+2,272 B (~+2.2 KiB)** |
| Transient key material held in flight (ek + dk + ct) | 64 B | **4,672 B (~4.6 KiB)** | +4,608 B |

Dropping to ML-KEM-512 saves 384 B on the initiator share, 320 B on the responder share, and
1,152 B of transient state (3,232 B in flight). That is the trade the RFC gets to make.

### 5.3 Against the real budget — and what is actually gated

Being precise about this, because the honest answer is uncomfortable: **no RAM ceiling is
gated anywhere in this repo.**

- `.github/workflows/footprint-cortexm0.yml:8` gates **flash**, not RAM — 16 KiB for the
  minimum-feature P0 node — and it runs in `--mode warn` with a standing, un-attributed
  overage (`:14`), so it is a sentinel, not a wall.
- `.github/workflows/esp-idf.yml:48` states the ESP footprint contract in as many words:
  **"TRACKED, never GATED"**, by maintainer ruling, because what has to fit is the *product*
  image CI cannot know.
- libtracer's own static RAM on ESP32 is **16 B** (`.github/workflows/esp-idf.yml:62`) — it
  holds no internal buffers at all (ADR-0041 §5). So "libtracer's RAM budget" is not the
  quantity a PQ handshake spends. **The host-injected slab is.**

The budget that actually bites is therefore the one a deployment writes down. The reference
one in this tree is the ESP-IDF `full_node` example
(`integrations/esp-idf/examples/full_node/main/app_main.cpp:247-249`): a **24 KiB** static
slab, partitioned once at bring-up into a **12 KiB** RX region of **1,536 B** datagram slots
(eight at most, fewer once per-slot bookkeeping is taken), 2 KiB of label tables, and ~10 KiB
of pmr containers.

Priced against that:

- **One in-flight hybrid handshake (~4.6 KiB of transient key material) is ~19% of the entire
  24 KiB slab.** Two concurrent joining peers is ~38%. That is the headline NARROW number, and
  it is a *concurrency* number, not a steady-state one — the state is transient, but it is
  transient at exactly the moment several peers reconnect after an AP flap.
- **The wire shares fit, barely.** A 1,216 B initiator share lands inside one 1,536 B RX slot
  with ~320 B of headroom. ML-KEM-512 would leave ~700 B. Neither needs reassembly on WS/TCP;
  both would on a smaller MTU.
- **Egress is where it spills.** `httpd_ws_link_t::kDefaultTxInlineBytes` is **1,600 B**
  (`integrations/esp-idf/libtracer/include/libtracer_esp/httpd_ws_link.hpp:240`) — the inline
  capacity of one TX work slot, and the dominant half of the link's per-link RAM. A 1,120 B
  responder share fits inline. An **ML-DSA-65 signature (3,309 B)** or a **1,980 B PQ identity
  record** does not: it keeps the pooled shell and takes a nothrow heap payload. Fine
  functionally, not fine as a design assumption on a device sized for no-allocation
  steady-state sends.
- **CAN is the hard stop.** At 8 payload bytes per classic frame, a 1,216 B initiator share is
  **152 frames** of reassembly. ADR-0045 §Decision 5 already puts CAN out of auth scope
  (physically-trusted enclosure harness), and these numbers are the arithmetic reason to keep
  it there rather than a reason to revisit it.
- **The thin rungs are out of the question, and that is fine.** The P1 rung carries a **≤ 25 KB
  Cortex-M0 footprint design target** (`docs/reference/12-deployment-profiles.md:32`), and
  ADR-0045 justifies the whole Noise-over-TLS choice by "16 KB-class devices". A hybrid
  handshake's *transient state alone* is a fifth of the P1 target. The conclusion is not "PQ is
  too expensive" — it is that **the hybrid handshake is a MID/WIDE-rung feature**, and the RFC
  must say so, because the module catalog's `security_noise` slot is currently written as
  pairing with "any transport" (`docs/reference/10-module-catalog.md:142`).
- **The ESP32-C6 has room.** 512 KB of RAM (`docs/interop/esp32-production-node.md:340`); ~4.6
  KiB of transient handshake state is under 1%. The pressure is entirely from the
  *self-imposed* slab discipline, not from the silicon — which means it is a sizing decision a
  deployment can make, and the RFC should say what to raise.

### 5.4 What is NOT measured, and must be before the RFC

**Code size.** No number for an ML-KEM implementation's flash cost on any gated profile exists
in this tree, and none is invented here. Before the handshake RFC is written, someone must
cross-compile a candidate ML-KEM-768 implementation for the Cortex-M0 profile and read the
flash delta off the `footprint-cortexm0` artifact series, and read the ESP delta off the
`footprint-<target>` artifact. Until then the flash half of the NARROW cost is unknown, and
"it's only a few KB" is a guess. **Throughput and latency are likewise unmeasured** — the
handshake is per-session, not per-message, so it should not touch steady-state numbers at all,
but that is a prediction, not a result.

## 6. Negotiation and downgrade — the questions, not the answers

Issue #1387 item 3 asks for negotiation rules. They cannot be written here; what can be
written is the shape of the problem the RFC inherits.

- **How does a PQ-capable node reach a classical-only MCU peer?** libtracer has **no capability
  negotiation** anywhere — CONTEXT.md §Capability negotiation says so flatly, and protocol v1
  has no negotiable wire features by design ([ADR-0013](../adr/0013-v1-scope-boundaries.md)).
  So a handshake-level negotiation would be the first of its kind in the protocol and must be
  argued as such, not slipped in.
- **Is downgrade policy or protocol?** If a node may fall back to classical-only, an on-path
  attacker who can force that fallback has erased the PQ property, which is the classic
  downgrade attack. The alternatives — refuse the peer, or accept classical under an explicit
  local policy flag — are a *deployment* choice with a *protocol* dependency, and the RFC has to
  place the boundary.
- **The only pre-handshake capability signal that exists today is the `:identity` kind.** It is
  pre-auth readable (§4b) and RFC-gated, which makes it the natural place to look — and
  immediately raises whether an *identity* tag should be load-bearing for *session* algorithm
  selection at all. It probably should not; naming it here is to note that nothing else is
  available, not to recommend it.

## 7. What this note does not decide

Explicitly, so nothing here is mistaken for a ruling:

- **It is not an RFC and creates no normative surface.** Nothing under [docs/spec/](../spec/)
  changes. No wire bytes are specified, no TLV type or `kind` value is claimed, no registry row
  is added.
- **It mandates no parameter set and no mandatory-to-implement algorithm.** ML-KEM-768 is priced
  because it is the common hybrid pairing, not because it is chosen; ML-KEM-512 is priced
  precisely so the choice stays open.
- **It does not schedule anything.** It does not say the handshake must ship, or when.
- **It does not amend ADR-0045.** The ed25519-TOFU + Noise direction stands exactly as written.
- **The handshake needs its own RFC.** Per [GOVERNANCE.md](../../.github/GOVERNANCE.md) a change
  to the normative surface is an amendment: RFC + maintainer approval. **That RFC MUST cite
  [#1387](https://github.com/avatarsd-llc/libtracer/issues/1387)** and either satisfy §3 or
  record, on the numbers in §5, why it does not.
