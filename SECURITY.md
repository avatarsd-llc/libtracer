# Security policy

## Reporting a vulnerability

Use GitHub's **private vulnerability reporting** on this repository — the *Security* tab →
*Report a vulnerability*. That opens a private advisory visible only to the maintainers; please
use it rather than a public issue for anything with a security impact.

Include what you would want to receive: the affected version or commit, the transport and build
configuration (which `LIBTRACER_*` options, which policy, which target), and the smallest
reproducer you have. A frame dump or a failing test is worth more than a description.

We aim to acknowledge a report within a week. libtracer is solo-maintained and pre-1.0, so
please assume best effort rather than a contractual window; if a report is time-critical, say so
in the first message.

## Supported versions

libtracer is **pre-1.0** and the wire protocol is still marked `DRAFT`
([`docs/spec/v1.md`](docs/spec/v1.md)). Only the **latest released version** receives fixes.
There are no maintained release branches, and no backports to earlier tags.

## Threat model — what this project does and does not defend against

This section exists because the biggest security risk in a library like this one is a correct
component used under an incorrect assumption. The design decisions below are ratified in ADRs
and linked; where the **shipped code** differs from the ratified **design**, that is called out
explicitly, because the gap is the part an integrator most needs to know.

### Authorization is not authentication

libtracer implements **authorization**: a device-held mapping from an opaque *subject token* to
rights on a vertex, enforced locally, as NFSv4-style ACEs with inheritance
([ADR-0018](docs/adr/0018-access-control-authorization-pluggable-subject-token.md),
[ADR-0020](docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md)).

It does **not** implement authentication. The core contains **zero crypto** and never verifies a
credential. Where the token comes from is the integrator's `subject_resolver_t` — a function from
caller context to opaque bytes. The FWD terminus passes the *inbound link* as the caller context.

The consequence, stated in ADR-0018 and repeated here because it is the load-bearing one:

> **An ACL is meaningful exactly to the degree the transport authenticates the token.**

On a link that does not authenticate its peer, the ACL is **advisory**. It will stop an honest
mistake. It will not stop an attacker, because an attacker chooses which link to connect on and
the resolver's answer is derived from that link.

### The graph is open by default, twice over

As shipped:

1. **ACL enforcement is off until a subject resolver is installed.** With no resolver,
   `graph_t::acl_allows` does not gate at all — local, in-process use is trusted by construction.
2. **A vertex whose *effective* ACL is empty is unrestricted.** `NO_MATCH` is deliberately not
   `DENY`; the open-by-default decision belongs to the caller, not the policy.

There is one sharp edge in the other direction: because *any* present ACE closes an
otherwise-open vertex, writing an ACE that matches nobody **locks** the vertex rather than
leaving it open. Verify that a stored ACE's subject is one your resolver can actually return.

> **Design/implementation gap.**
> [ADR-0045](docs/adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md) §2 specifies
> that the default ACL ships *closed except the auth subtree*. **That is the ratified target, not
> the current behaviour** — there is no `anonymous` subject and no `/device/auth/*` in `core/`
> today. Do not deploy on the assumption that a fresh graph is closed. It is open.

### There is no authentication mechanism in core yet

ADR-0045 ratifies *how* authentication will work — as ordinary graph reads and writes against
app-defined vertices (`read /device/auth/challenge` → nonce; `write /device/auth/login` → the
connection's subject rebinds through the existing resolver seam), with no wire verb, no protocol
phase, and crypto kept app-side. **None of that is implemented in `core/`.** The vertices are
app-defined by design, so an integrator can build the flow today; the core provides the seam and
nothing else.

### Trust is per-hop and transitive — there is no end-to-end identity

A `FWD` frame arriving over an authenticated link carries **that link's** subject. Board 2
authenticates board 1's link, not the browser behind board 1. Multi-hop, origin-attributed
identity **does not exist** and is explicitly out of scope
([ADR-0045](docs/adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md) §5). If your
deployment needs it, layer signed payloads at the application level.

### Link confidentiality is the link's job, and not every link has it

| Transport | Confidentiality |
| --- | --- |
| QUIC / WebTransport | TLS 1.3 by construction ([ADR-0043](docs/adr/0043-quic-webtransport-optional-module-msquic.md)) |
| WebSocket / TCP | **plaintext** |
| CAN | none — out of scope; a physically-trusted enclosure-internal harness |

The QUIC dialer exposes `quic_dial_tls_t::insecure_no_verify`, which **skips server-certificate
validation entirely**. It exists to reach a self-signed development certificate and is named to
be greppable. It must never be set in a deployed build.

For plaintext MCU links the intended answer is a Noise-pattern channel (the module catalog's
`security_noise` slot) rather than dragging X.509 and a CA store onto a 16 KB-class device.
**That module is not implemented.** Until it is, a WS/TCP link is in the clear, and an
authenticated session on such a link **can be hijacked mid-flight by an on-path attacker**.

### X.509 / CA PKI is rejected, by decision

Not missing — rejected
([ADR-0045](docs/adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md) §5). It
reintroduces a central authority into a deliberately decentralized system, and certificate-chain
parsing is precisely the footprint the target class cannot carry. The identity roadmap is raw
ed25519 keypairs with trust-on-first-use — the public key *is* the identity, the SSH/WireGuard
model rather than certificate enrollment — which slots into the ACL model unchanged. That
roadmap is **not implemented**.

### The CRC is not an integrity check

The optional trailer CRC detects bit flips on a lossy link. It is **not** a message
authentication code and takes no adversarial stance
([ADR-0004](docs/adr/0004-crc-in-optional-trailer.md)). An attacker who can modify a frame can
recompute it.

### Subject tokens

The token space is opaque bytes with exactly one carved-out spelling: **`EVERYONE@` is
reserved**, and the core enforces the reservation on the *resolver's output* — a caller that
resolves to it is refused at every gate, fail-closed
([#908](https://github.com/avatarsd-llc/libtracer/issues/908)). This matters for the deployments
most at risk: those whose resolver passes a caller-supplied identity through (a username, a
certificate CN, a peer name) could otherwise mint a principal indistinguishable from the
wildcard.

`OWNER@` is **not** a special subject. Earlier revisions of the reference docs said it was; no
evaluator ever special-cased it, and it has been withdrawn
([#1033](https://github.com/avatarsd-llc/libtracer/issues/1033)). Treat it as an ordinary token
with no owner semantics attached.

### Parsing an ACL is strict on purpose

A lenient read of an access-control document does not lose a field — it changes what the document
grants. A `type` sent as a big-endian `u16` `0x0001` (DENY) has `0x00` in its low byte, so a
width-tolerant load turns a refusal into a grant; a dropped `expires_ns` turns a time-limited
grant permanent. `parse_acl` therefore rejects over-wide numeric payloads, wrong-typed values,
unknown keys, repeated keys and broken key/value pairing
([#906](https://github.com/avatarsd-llc/libtracer/issues/906)). Unknown-key tolerance is granted
for `SETTINGS` and deliberately **not** for ACLs.

## Deploying with this in mind

A short checklist that follows from the above:

- Install a `subject_resolver_t`. Without one there is no enforcement at all.
- Do not let the resolver pass caller-supplied identity through unfiltered.
- Terminate untrusted links on TLS-carrying transports (QUIC / WebTransport), or accept that
  the ACL on a plaintext link is advisory.
- Never ship `insecure_no_verify`.
- Check that every stored ACE names a subject your resolver can return — an ACE that matches
  nobody locks the vertex.
- Do not attribute a multi-hop `FWD` to its origin. It carries the last hop's subject.
