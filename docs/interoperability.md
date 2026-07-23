# Interoperability

**You get interop enforcement without a certification cartel.** libtracer reaches
interoperability the opposite way to a consumer-appliance standard like Matter: not
by pre-agreeing a normative catalog of device semantics and gating shipment behind a
certification body, but by keeping the protocol a lean, byte-agnostic router and
making every node **describe itself well enough that a competent reader can build the
integration on the spot**. The "reader" is a developer — increasingly a developer's
AI agent. This document is the direction for that reader, and for the vendor writing
the device it will read.

> **Audience**: a vendor (or a vendor's coding agent) building a libtracer device, or
> an integration that must talk to devices it did not design. It tells you what you
> must do to stay interoperable, what is merely recommended, and what you may drop
> entirely when RAM and flash are expensive.

---

## The model: interop by legibility, not by pre-agreement

Three interoperability models exist, and libtracer deliberately picks the third:

| Model | Coupling | Cost | Guarantee |
| ---- | ---- | ---- | ---- |
| **Certified semantics** (Matter clusters) | agree on exact bytes up front, enforced | a governance cartel + a certification + attestation regime | guaranteed cross-vendor interop |
| **Recommended vocabulary** (a well-known-names registry) | agree on names, nagged | a maintained registry; a linter | best-effort; drifts |
| **Legible self-description** (libtracer) | describe yourself understandably | none on the wire; none required on the node | opportunistic; the reader absorbs drift |

libtracer's core never learns what a byte *means* — that is the [type-agnosticism
commitment](reference/00-overview.md) (load-bearing claim 5: *the graph imposes no
shape on user data*). Meaning lives entirely at L5, carried as ordinary,
self-describing data. Interop is therefore not a protocol feature you conform to; it
is a **legibility discipline** you follow so the next reader can understand you.

The action space a reader must master is tiny and uniform: **`read` / `write` /
`await`** against path handles, the same three verbs for every vertex, every
transport, every device. The discovery surface is uniform too: walk the vertex tree,
read each vertex's [`:schema`](reference/00-overview.md), read a device's controller
catalog from its creator endpoint (`read /net/export:schema`). A general-purpose
reader does not need a per-vendor SDK — it needs to walk one graph and read its
self-descriptions.

---

## What a vendor MUST do to stay interoperable

There is no conformance stamp. There are three obligations, and they are cheap.

1. **Keep the byte-agnostic seam.** Do not smuggle application semantics into the
   protocol — no private wire type that a generic forwarder must understand, no
   meaning encoded in framing. App meaning rides as data (a `VALUE`, an app field),
   never as a protocol extension. A frame from your device must be routable and
   forwardable by a node that has never heard of you.

2. **Name things the way you would want them read.** A vertex's NAME is surfaced in
   its `:schema` for free, on every node, with no opt-in. Two devices that both call
   an ambient-temperature reading `temperature` are already partially interoperable —
   a reader can match on it. This is the lightest possible convention: *agree on
   nothing centrally; just be nameable.*

3. **Annotate yourself legibly where you can afford to.** For anything a name alone
   cannot disambiguate — units, direction, valid range, purpose — install an
   [owner field descriptor table](spec/rfcs/0010-owner-app-fields-and-schema.md)
   (RFC-0010). Its descriptor bytes are stored and served **verbatim; the runtime
   never interprets them** — which makes them the ideal carrier for a
   natural-language brief and a semantic tag (e.g. `org.example.temperature.celsius.f32`)
   that a reader matches while the core stays blind. A bare structural descriptor
   with no human-legible content is technically valid and practically useless: the
   reader guesses, exactly as a human would. **Legibility is the whole product** —
   spend your descriptor bytes on meaning, not just shape.

That is the entire vendor contract: stay out of the protocol's byte-plane, be
nameable, and be legible. Everything below is optional.

---

## Where "enforcement" lives — and why it is never the runtime

The enforcement that keeps a fleet interoperable is real, but it is entirely
**design-time tooling**, matching libtracer's standing doctrine that *analyzers police
designs, the runtime does not* (RFC-0007 / ADR-0051 — delivery terminates at the
target; no runtime limits, no runtime semantics):

- **The orchestrator** (typically the network-formation web UI,
  [reference 13](reference/13-network-formation.md)) reads `:schema` across a subtree,
  matches names and semantic tags, and proposes wiring compatible producers to
  consumers. Interop *happens* here, as a tooling capability, at wiring time.
- **A schema linter** warns, at design time, when a vertex claims a well-known role
  but its descriptor does not match the recommended shape. Advisory teeth — no
  signing authority, no runtime cost, no shipment gate.

Neither sits in the data path. The runtime stays a byte-router that never numerically
interprets a payload. A coding agent that synthesizes an integration is likewise a
**design-time** actor: it reads schemas, emits plain `read`/`write`/`await` client
code, a human reviews and tests it, and that code then runs deterministically forever
with the agent nowhere near the hot path. The intelligence is amortized into
generated code; the node stays a 16 KB reactor.

---

## Enforcement is optional — drop it when RAM/flash is expensive

None of the above is load-bearing on the device. The self-description and the tooling
that consumes it are **optional modules**, not required ones ([everything is a
module](reference/00-overview.md); the required set is the frame codec, path resolver,
view/refcount machinery, and forwarder — nothing else). Under RAM or flash pressure a
node sheds legibility the way it sheds any optional module, and interop degrades
**gracefully** rather than breaking:

| Budget | What the node ships | Interop it offers |
| ---- | ---- | ---- |
| Comfortable | full `:schema` + RFC-0010 descriptors with briefs + on-device linting hooks | a reader integrates it unassisted |
| Tight | vertex NAMEs only (free in `:schema`) | a reader matches on names; briefs come from out-of-band docs |
| Bare (16 KB reactor / ISR publisher) | opaque vertices, no descriptor tables | fully functional data plane; meaning supplied entirely out-of-band by the integrator |

A node that describes nothing is still a **conforming node** — it reads, writes,
awaits, and forwards exactly like any other. It has simply pushed 100% of the
legibility burden off-device, onto documentation and the integrator's reader. This is
the point of putting meaning in *data* and never in the *protocol*: the interop story
scales down to zero on-device cost, so a Cortex-M0 sensor pays nothing for it and a
Linux gateway pays for as much as it wants.

---

## The consequence: a node is a discovery surface for an agent

Because the graph is uniform and self-describing, a libtracer node is a natural
target for agent-driven integration — its `:schema` tree maps almost directly onto a
tool/resource discovery surface (MCP-shaped). Point an agent at a node, let it walk
the vertices, and the schemas *become* the tools it can drive. Three properties make
this work in practice:

- **Drift is absorbed, not fought.** A competent reader understands that
  `temperature`, `temp`, and `ambient_c` are the same concept from context — the only
  interop model here that tolerates vocabulary divergence instead of legislating it
  away. (It tolerates *legible* divergence, not *cryptic absence*: see obligation 3.)
- **Feedback is legible.** The uniform `tr::<concept>::<error>` model
  ([RFC-0002](spec/rfcs/0002-protocol-error-model.md)) plus the closed error boundary
  (applications emit *data*, never protocol errors) gives a reader clean, structured
  signals to self-correct against — eight concepts, not a per-vendor error soup.
- **The integration is compiled away.** The agent's output is ordinary code on the
  three-verb API; the runtime never depends on the agent being present or correct.

---

## The honest trade

This buys **opportunistic, best-effort, gracefully-degrading** interoperability, not
the *guaranteed* cross-vendor interop a certification stamp confers, and it carries no
consumer trust anchor (no device attestation, no "certified genuine" guarantee). That
trade is deliberate and correctly scoped: libtracer targets the professional /
single-integrator / fleet domain, where a human or an agent is in the loop at
integration time and reviews what gets wired — not the autonomous consumer-appliance
domain, where Matter's rigidity and attestation earn their cartel.

So the direction for a vendor is simple: **do not wait for a standards body, and do
not build one.** Stay byte-agnostic, be nameable, be legible, and let the reader do
the rest. The community only has to converge on *legibility*, never on a vocabulary —
which is the one kind of agreement that both holds up and stays free.

---

## Worked guides

The model above, made executable:

- **[Building a custom interoperable device](interop/custom-device.md)** — a
  generic vendor device end to end, plus the detailed matrix of what a custom
  device *can*, *must*, and *must not* implement.
- **[A production ESP32 node](interop/esp32-production-node.md)** — the hardened
  embedded profile: memory slabs, transport composition, task-stack discipline,
  backpressure behavior, and validation practice from a shipped ESP32-C6
  deployment.

```{toctree}
:hidden:
:maxdepth: 1

Custom interoperable device <interop/custom-device>
Production ESP32 node <interop/esp32-production-node>
```

---

> **See also**: [reference/00-overview.md](reference/00-overview.md) (the six-layer
> model and load-bearing claims), [RFC-0010](spec/rfcs/0010-owner-app-fields-and-schema.md)
> (owner app fields and `:schema`), [reference 13](reference/13-network-formation.md)
> (how an orchestrator forms a graph across nodes), and the design rationale in the
> repository's [ADRs](https://github.com/avatarsd-llc/libtracer/tree/main/docs/adr/).
