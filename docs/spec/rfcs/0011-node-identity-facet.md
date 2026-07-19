<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0011 — Node identity facet: a wire-readable, pre-auth `:identity` field serving the ADR-0045 ed25519 TOFU public key at every vertex

| Field | Value |
| ---- | ---- |
| **RFC** | 0011 |
| **Title** | Node identity facet: a wire-readable, pre-auth `:identity` field serving the ADR-0045 ed25519 TOFU public key at every vertex |
| **Status** | **accepted** (2026-07-19 — maintainer ruling, window waived) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-07-17 |
| **Comment window closes** | 2026-07-31 (≥ 14 days after opening) |
| **Tracking issue** | [#410](https://github.com/avatarsd-llc/libtracer/issues/410) |
| **Target spec version** | v1 (additive — occupies previously-erroring space only) |

## Summary

The ratified identity direction — **raw ed25519 public keys with trust-on-first-use
pairing** ([ADR-0045](../../adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md)
decision 3: "the public key *is* the identity") — gets a **read surface**: a
protocol-defined, read-only field **`:identity`** that **every vertex of a node
answers with the node-scoped identity record** — a `SETTINGS`-shaped TLV carrying a
`kind` member (algorithm agility, `0x01` = ed25519) and a `key` member (the raw
32-byte public key). The read is **pre-auth by design** (a public key is exactly
what an unauthenticated peer must obtain to authenticate and pin) and is therefore
exempt from the READ gate. A node without a keypair keeps today's behavior
byte-for-byte: `ERROR{tr::schema::not_found}` — the `ENOTTY` of an unsupported
field. The facet serves three consumers with one 60-byte record: **(a)** the
cross-path dedup key for **client-side topology stitching**
([ADR-0044](../../adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)
point 3 — separate paths stay separate; core never dedups; this RFC hands clients
the key it anticipated), **(b)** the **cycle-termination key** for recursive
client-side walks through transport vertices, and **(c)** the **auth subject**
alignment — the same 32 bytes an ACL ACE names once the ADR-0045 ed25519 login
lands (the caller-identity gap of
[#375](https://github.com/avatarsd-llc/libtracer/issues/375)). No new wire verbs,
no new type codes, no new error identities.

## Motivation

1. **ADR-0044 promised the key and deliberately did not provide it.**
   [ADR-0044](../../adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)
   point 3 rules that the deduplicated "real graph" — the picture in which
   `ws→boardB` and `ws→boardA/can→boardB` collapse into one device node — is
   **client/app logic**, computed by "matching whatever identity its deployment
   trusts (announced device ids, auth pubkeys per ADR-0045)", and its Consequences
   section states the gap precisely: client-side identity matching "gains its
   natural key once ed25519 lands: the pubkey a client authenticated against is
   the strongest cross-path identity available." ed25519 TOFU is ratified
   ([ADR-0045](../../adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md)
   decision 3), but **no standard field surface serves the pubkey**: today a
   client can only learn a node's key as a side effect of authenticating to it,
   and only over the app-defined auth subtree (`/device/auth/*` is an ADR-0045
   *example* shape, not a protocol surface a generic walker can rely on).
   [docs/reference/03-addressing.md](../../reference/03-addressing.md) §Address
   scopes defines the global scope as "a logical view assembled by composing
   routes through transport-vertices" — this RFC gives the assembler its
   dedup key; and the same file's §Collision rules sentence ("Conflicting peer
   identities on the network are a discovery-layer problem") predates ADR-0044/45
   and is reconciled by this RFC's edits (identity matching is client-side,
   keyed by this facet — discovery is only IP bootstrap, ADR-0044 point 4).
2. **Recursive topology walks have no termination key.** Per ADR-0044 point 1, a
   transport vertex's `:children[]` read is synthesized from live announce
   traffic, and per point 2 every longer route is a **distinct address by
   design**. A client walking entry points recursively (the SPA's topology view;
   the single-IP-entry case of ADR-0044 §Context) therefore constructs
   `/net/ws0/A/can/n2/…`, then enumerates n2's transport vertices and finds a
   route back toward A — each extension is a *new, valid* address, so the walk
   never self-terminates by addressing alone. (The FWD plane's own loop-freedom
   — `dst` shrinks monotonically,
   [docs/reference/02-graph-model.md](../../reference/02-graph-model.md) §the
   shedding rule — protects frames, not the walker's recursion.) The walker
   needs a per-node key to mark visited nodes; reading `:identity` at each
   enumerated node is that key, applied **client-side** — core still never
   dedups (CONTEXT.md §Cycle termination is untouched).
3. **The auth subject needs one canonical byte-form**
   ([#375](https://github.com/avatarsd-llc/libtracer/issues/375)). The HANDLER
   caller-identity gap is being closed on the implementation side (paired impl
   #406), and the ADR-0045 target makes the ed25519 pubkey the subject the
   `subject_resolver_t` seam yields
   ([ADR-0018](../../adr/0018-access-control-authorization-pluggable-subject-token.md):
   the subject is a pluggable token). For ACL ACEs, TOFU pins, and stitching keys
   to name the **same** identity without translation tables, the readable
   identity record and the subject token must agree on bytes. This RFC pins that
   alignment at SHOULD strength (§C.6).
4. **The surface precedent already exists.** `read :schema` is a **synthesized**
   protocol read (`core/src/graph.cpp` `read_schema`: the runtime emits a POINT
   from state it holds natively — zero stored per-vertex bytes), and
   [docs/reference/02-graph-model.md](../../reference/02-graph-model.md) §Schema
   and field discipline already carries read-only protocol rows (`:schema`,
   `:liveness.last_seen_ns`). `:identity` is one more synthesized read-only row:
   node-scoped state (the keypair) projected through the per-vertex `:` field
   surface, exactly as `read_schema` projects the vertex name and QoS knobs.

## Proposed change

### A. Where the facet lives: a protocol-defined `:identity` field, answered by every vertex

**`identity` becomes a protocol-minted field NAME** on the `:` control plane —
one more standard field beside `:schema` / `:acl` / `:settings` (the protocol
owns bare field names; applications mint only below `settings.app.`,
[RFC-0010](0010-owner-app-fields-and-schema.md) §A.1, so no collision space is
spent that was not already the protocol's).

- **Node-scoped, vertex-served.** The identity is a property of the **node**
  (one path tree, one graph instance — CONTEXT.md §Transport vertex: "a node is
  therefore one path tree"), not of any vertex. Every vertex of a node MUST
  answer `read <vertex>:identity` with the **same, byte-identical** record
  (§B). The ioctl analogy of CONTEXT.md §Field-write carries exactly: a
  device-scoped ioctl answers identically on every fd of the device.
- **Why every vertex, not only the root.** A walker that has just crossed a
  transport vertex stands at *some* vertex of the peer — it does not know, and
  MUST NOT need to discover, where the peer's root is. `read
  /net/can0/n5:identity` (the vertex address resolves through `can0`, the
  remaining suffix terminates at the peer, the FIELD read applies at the
  terminus per [RFC-0004](0004-remote-operation-addressing.md)) identifies node
  n5 in **one round-trip from wherever the walk stands**. Root-only service
  would add a root-discovery round-trip per node per path — on exactly the
  multi-hop routes where round-trips are most expensive.
- **Cost.** Zero per-vertex bytes: the record is synthesized on read from one
  node-scoped keypair, the `read_schema` pattern. A node's field-resolution
  gains one branch beside the existing `schema` branch.
- **Schema visibility: none, deliberately.** `identity` is **not** listed in
  `read :schema`, and this RFC does not ask for it. The synthesized protocol
  part of the `:schema` POINT is a `SETTINGS` container of **QoS knobs** —
  members of `:settings`. `identity` is a **top-level `:` field**, not a
  settings member; listing it there would assert that `:settings.identity`
  exists, which is false. There is no region of the POINT that enumerates the
  `:` field plane at all: `:acl`, `:subscribers`, `:children` and `:schema`
  itself are all unlisted. The `:` plane is not self-describing in v1 —
  discovery is by attempting the read and treating `tr::schema::not_found` as
  "absent" (the ENOTTY doctrine, §C.3) — and `identity` is not the field that
  should force that to change. A client learns whether a node is identifiable
  by reading `:identity`, which costs the same round-trip as reading `:schema`
  would, and is pre-auth besides (§C.2) where `:schema` is READ-gated.

### B. The reply: a SETTINGS-shaped record with algorithm agility — byte-precise

`read <vertex>:identity` returns one **`SETTINGS` (`0x0B`, `opt.PL=1`)** TLV —
the established NAME-keyed record shape of the `:` plane (schema descriptors,
`:settings` container reads) — with two required members in fixed order:

```
SETTINGS (PL=1) {
  NAME "kind"  VALUE <u8>          ; required, first — identity-kind registry below
  NAME "key"   VALUE <key bytes>   ; required, second — the raw public key
  ...                              ; optional future members — readers MUST ignore
                                   ; unknown NAMEs (the RFC-0010 unknown-key rule)
}
```

**Identity-kind registry** (additions RFC-gated, like the error registry):

| `kind` | Meaning | `key` length |
| ---- | ---- | ---- |
| `0x00` | reserved — invalid | — |
| `0x01` | ed25519 raw public key (RFC 8032 encoding) | exactly 32 bytes |

Worked bytes — the complete ed25519 record, 60 bytes:

```
0B 40 38 00                                SETTINGS: type=0x0B opt=0x40(PL=1) len=56
  02 00 04 00 6B 69 6E 64                  NAME "kind"                    (8 bytes)
  01 00 01 00 01                           VALUE u8 = 0x01 (ed25519)      (5 bytes)
  02 00 03 00 6B 65 79                     NAME "key"                     (7 bytes)
  01 00 20 00 <32 raw pubkey bytes>        VALUE = ed25519 public key    (36 bytes)
```

`4 (header) + 56 (payload) = 60 bytes`, trailer per the serving link's egress
policy as for any reply. Validation on decode: a `kind` outside the registry, a
missing/misordered required member, or a `key` length that does not match the
`kind` (kind `0x01` ⇒ exactly 32 bytes) MUST be rejected with
`ERROR{tr::schema::type_mismatch}`.

Why a record and not bare `VALUE` bytes: ADR-0045 ratified ed25519 **for the
deployments in scope**, not forever; the `kind` member is 24 bytes of overhead
per read (a control-plane, once-per-node read — never the data hot path) that
buys a post-quantum or second-kind future without a surface break. The bare
32-byte form is the recorded alternative (§Alternatives).

### C. Semantics — normative rules

1. **Serve everywhere, identically.** A node holding an identity keypair MUST
   serve `read <vertex>:identity` at every vertex of its graph; all vertices of
   one node MUST return byte-identical records. A multi-homed node MUST present
   the same record on every transport — this identity-per-node invariant is
   what makes the record a valid cross-path key.
2. **Pre-auth readable — exempt from the READ gate.** The `:identity` read MUST
   be served regardless of the caller's subject and the vertex's effective ACL,
   including to `anonymous`. Rationale: the public key is precisely what an
   unauthenticated peer must obtain to TOFU-pin and to verify the ADR-0045
   challenge signature — gating it behind READ would deadlock first contact
   (the default ACL ships closed except the auth subtree, ADR-0045 decision 2);
   and it discloses nothing the target Noise handshake (`security_noise`,
   ADR-0045 decision 4) does not present as its static key anyway. This is a
   narrow, named exemption to the enforcement rule "data/field read need READ"
   ([docs/reference/05-protocol-tlvs.md](../../reference/05-protocol-tlvs.md)
   §`0x0A` enforcement); it applies to `:identity` only. (Privacy tension
   flagged in §Discussion 1.)
3. **No keypair ⇒ `ERROR{tr::schema::not_found}`.** A node without an installed
   identity keypair MUST return `tr::schema::not_found` (`0x0031`) on any
   `:identity` read — byte-for-byte today's behavior. Rationale: `:` fields are
   **optional by doctrine** (CONTEXT.md §Field-write: an unsupported field is
   the `ENOTTY` of an unsupported ioctl), and a keyless node genuinely has no
   identity facet — the surface is absent, not empty. An empty-record reply was
   rejected: it fabricates an "identity exists but is vacant" state no consumer
   can act on, forces walkers to distinguish empty-record from malformed-record,
   and breaks the clean invariant *record ⇒ pinnable key*. `SCHEMA_NOT_FOUND` is
   already exactly what a walker must tolerate from pre-RFC nodes, so the
   keyless case and the old-implementation case collapse into one client
   behavior: "unidentifiable node — do not stitch, do not recurse through it on
   identity's authority." This also matches the RFC-0010 precedent
   (`read_settings_app` with no table ⇒ `SCHEMA_NOT_FOUND`).
4. **No write surface, no member addressing.** A write to `:identity` (or any
   field chain below it), from any caller including the owner acting remotely,
   MUST return `ERROR{tr::schema::not_found}` — no write surface exists,
   caller-independent (the read-only posture of
   [RFC-0010](0010-owner-app-fields-and-schema.md) §A.3 / Discussion 2).
   Installing or rotating the keypair is a **local, owner-facing host API** —
   the owner-initiated doctrine: the graph is a projection of device state, and
   the node's identity is device state. A member read (`:identity.key`) or an
   indexed read (`:identity[0]`) is not defined in v1 and returns
   `ERROR{tr::schema::not_found}` — **for every caller, including one the
   vertex's ACL denies**; the record is served whole (60 bytes —
   sub-addressing buys nothing and costs a resolver branch on MCU-class nodes).
   Because §C.2 places `:identity` above the READ gate, the *whole* `identity`
   field namespace must resolve there: a shape left to fall through would meet
   the gate and answer a denied caller `PERMISSION_DENIED`, contradicting this
   clause. Nothing is disclosed by the narrower answer — the record itself is
   world-readable by design. **Code-pinned** by
   `test_record_has_no_sub_addressing`
   ([#433](https://github.com/avatarsd-llc/libtracer/pull/433)), which found
   both halves false as first shipped: `:identity.key` was `PERMISSION_DENIED`
   for a denied caller, and `:identity[0]` **served the whole record to
   anyone**.
5. **The record is a claim; trust stays per-hop.** This RFC adds **no**
   cryptographic binding: a `:identity` record read over a multi-hop route is
   asserted by the terminus and relayed by every hop, and per
   [ADR-0045](../../adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md)
   decision 5 (end-to-end identity rejected/deferred; trust is per-hop and
   transitive) an on-path node can lie. A client MUST NOT treat a read record
   as authenticated; authenticity attaches only where the client verified that
   node against that key (the ADR-0045 signature login, or a `security_noise`
   handshake presenting it as the static key) or trusts the intervening hops.
   Cross-path stitching over unauthenticated routes is therefore best-effort
   client policy — which is exactly ADR-0044 point 3's ruling, unchanged.
   Clients SHOULD pin on first authenticated contact (TOFU) and SHOULD alarm on
   a pinned node presenting a different key (the SSH model).
6. **Subject alignment.** Where the ADR-0045 ed25519 login is in use, the
   subject token the `subject_resolver_t` seam yields for an authenticated peer
   SHOULD be **byte-identical to the record's `key` member** (the raw 32 bytes),
   so an ACL ACE's `subject`, a client's TOFU pin, and a walker's stitching key
   all name the same bytes with no derivation step. (SHOULD, not MUST: the
   subject is a pluggable token per ADR-0018; the ed25519-login RFC may tighten
   this.)
7. **Core never dedups — restated.** Nothing in this RFC licenses any node to
   collapse, prefer, or fail-over between routes that read the same identity.
   Separate paths stay separate paths (ADR-0044 point 2); the identity facet is
   an input to **client-side** projection only.
8. **Private material.** The record carries public key material only. An
   implementation MUST NOT serve private-key bytes on this or any wire surface.

### Files this RFC edits (on acceptance)

- [docs/reference/02-graph-model.md](../../reference/02-graph-model.md) —
  §Schema and field discipline: the core field table gains the `:identity` row
  (read-only, node-scoped, pre-auth); a new §Node identity facet subsection
  carries rules §C.1–C.8 (serve-everywhere, claim-not-proof, subject alignment).
- [docs/reference/05-protocol-tlvs.md](../../reference/05-protocol-tlvs.md) —
  §`0x0B` SETTINGS: the identity-record layout, member order, and the
  identity-kind registry (§B); §`0x0A` ACL enforcement paragraph: the named
  `:identity` READ-gate exemption (§C.2); §`0x0B` "Where it appears" gains the
  `:identity` read.
- [docs/reference/03-addressing.md](../../reference/03-addressing.md) —
  §Address scopes / Global scope: the logical view's dedup key is the identity
  facet, applied client-side; §Collision rules: the stale "conflicting peer
  identities are a discovery-layer problem" sentence reconciled to the
  ADR-0044/45 model.
- [CONTEXT.md](../../../CONTEXT.md) — glossary entry **Node identity facet**
  (node-scoped, every-vertex, pre-auth, claim-not-proof, keyless ⇒
  `SCHEMA_NOT_FOUND`; _Avoid_: "core dedups paths by identity", "the identity
  read authenticates the node", "identity is a vertex property"); §Access
  control gains the subject-alignment sentence.
- `core/` reference implementation + `core/CHANGELOG.md` (follows this RFC, not
  normative): the `identity` branch in field resolution beside the existing
  `read_schema` dispatch (`core/src/graph.cpp`), the pre-ACL-check exemption,
  the local keypair-install host API; ties into the #406 caller-identity work.
- `tests/conformance/vectors/v1/identity/` — new vector category (below).

### Conformance vectors this RFC adds

New category `tests/conformance/vectors/v1/identity/` (additive — no existing
vector changes):

1. **`identity/record-roundtrip`** — the canonical 60-byte ed25519 record of §B:
   `encode(decode(x)) == x` byte-exactly; `expected.json` names `kind` and `key`.
2. **`identity/read-any-vertex`** — one node, keypair installed, three vertices
   at different depths: `read <v>:identity` returns byte-identical records at
   all three.
3. **`identity/fwd-read-terminus`** — a `FWD`-carried field read of `:identity`
   arriving at a terminus serves the same bytes as the local read; the reply
   retraces the accumulated `src` route unchanged.
4. **`identity/preauth-acl-exempt`** — subject resolver installed, effective
   ACL grants `anonymous` nothing: a data read is `tr::access::denied`, the
   `:identity` read on the same vertex still serves the record.
5. **`identity/no-keypair-enotty`** — keyless node: `:identity` read returns
   `ERROR{tr::schema::not_found}` (`0x0031`); the node is otherwise
   byte-for-byte a pre-RFC node.
6. **`identity/write-no-surface`** — a write to `:identity` (and to
   `:identity.key`) returns `ERROR{tr::schema::not_found}`, for an anonymous
   caller and for a caller holding WRITE alike.
7. **`identity/kind-length-mismatch`** — decode of a record with `kind=0x01`
   and a 31-byte `key` ⇒ `ERROR{tr::schema::type_mismatch}`; unknown trailing
   member after `key` ⇒ accepted and ignored.

## Compatibility

- **No protocol-v1 break.** Every behavior defined here occupies
  previously-erroring space: `:identity` is `tr::schema::not_found` on read and
  write today, on every vertex, everywhere. A node without a keypair — and
  every existing implementation — is byte-for-byte unchanged. No new wire
  verbs, no new type codes, no new error identities; the identity-kind registry
  is new but rides existing `VALUE` bytes inside an existing `SETTINGS` shape.
- **New reservations:** `identity` as a protocol field NAME (constrains future
  protocol minting only — applications were never able to mint bare field
  names); the identity-kind registry with `0x01` = ed25519 (additions
  RFC-gated).
- **New conformance vectors:** the seven `tests/conformance/vectors/v1/identity/`
  cases above; no existing vector's bytes change.
- **Migration:** none required. Deployments adopt by installing a keypair
  through the host API when the ed25519 identity module lands; the strawberry-fw
  SEC-001 HMAC bridge (ADR-0045 decision 3, "bridge now") is unaffected — a
  node MAY serve `:identity` while its login dance is still HMAC, and clients
  MAY stitch on it before any login exists at all (unauthenticated stitching is
  best-effort per §C.5). The SPA topology walker replaces its interim
  announced-device-id matching with `:identity` reads where available, falling
  back to "unidentifiable node" on `SCHEMA_NOT_FOUND`.

## Alternatives considered

- **A `/self` (or `/device/identity`) data vertex.** Rejected. It spends `/`
  namespace on what is a control facet of the node (the `:`-not-`/` rule,
  CONTEXT.md §Field-write: control facet ⇒ `:`, distinct lifecycle ⇒ `/` — a
  static identity has no lifecycle); it forces every walker to discover the
  peer's root before identifying it (an extra round-trip per node per path,
  §A); and a real vertex drags subscriber-list/ACL/LKV machinery for 32 static
  bytes — the exact per-vertex cost RFC-0010 §D measured at ≈ 900 B on the
  target MCU and exists to avoid.
- **Serving it from the ADR-0045 auth subtree** (`read /device/auth/pubkey`).
  Rejected as *the* standard surface: ADR-0045 decision 1 deliberately makes
  the auth dance **app-defined** vertices — there is no protocol-fixed auth
  path a generic walker can rely on, and standardizing one now would harden an
  example into protocol shape prematurely. Nothing stops a deployment also
  exposing its key there; `:identity` is the uniform surface tooling can count
  on.
- **A member of `:schema`.** Rejected. The two-part `:schema` shape is frozen
  by [RFC-0010](0010-owner-app-fields-and-schema.md) §B.2 (synthesized protocol
  part + verbatim owner part), and schema describes a **vertex's field
  surface**, not node state; stuffing a node property into it would bloat every
  schema read by 60 bytes and make walkers parse a POINT to extract an identity.
- **A `:settings.identity` knob.** Rejected. The settings container is the
  writable QoS/config plane ([docs/reference/05-protocol-tlvs.md](../../reference/05-protocol-tlvs.md)
  §`0x0B`); identity is read-only and node-scoped, and parking a read-only
  facet among writable knobs muddies the RFC-0010 container-read contract.
- **A bare 32-byte `VALUE` reply (no record).** Rejected — 24 bytes cheaper on
  a once-per-node control read, at the price of freezing ed25519 into the
  surface forever. ADR-0045 chose raw keys over X.509 for footprint, not as an
  eternal algorithm commitment; the `kind` member is the minimum agility that
  avoids a surface break when a second kind arrives. Recorded for the
  maintainer in §Discussion 2.
- **A dedicated IDENTITY type code.** Rejected — spends a scarce core code
  (`0x0F`–`0x1F` reserved range) on one read-only record when the
  `SETTINGS`-shaped NAME-keyed record is the established `:` plane reply shape;
  "`PL=1` plus a type byte that says what the children mean" is satisfied by
  `SETTINGS` (retired-LIST lesson, CONTEXT.md §Flagged ambiguities).
- **Carrying identity at the discovery layer** (mDNS TXT record, static
  config). Rejected as the mechanism: discovery is **only** IP-peer bootstrap
  ([ADR-0044](../../adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)
  point 4 — "what can I dial" and nothing else); CAN peers and nodes behind a
  single IP entry never appear there, and the stitching/termination consumers
  need the key **through any path**, which only an in-graph read provides.
- **Protocol-level cross-path dedup keyed on the identity** (core collapses or
  fails over between routes reading one key). Rejected —
  [ADR-0044](../../adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)
  §Considered options rejected exactly this multipath machinery, with
  [ADR-0040](../../adr/0040-net-plane-is-explicit-source-routed-only.md)'s
  failure mode (it destroys the failover signal explicit redundant routes
  exist to provide). This RFC changes nothing there: it supplies the key,
  clients apply it.
- **Cryptographically bound identity reads** (challenge-in-the-read, signed
  records, end-to-end origin proof). Rejected/deferred —
  [ADR-0045](../../adr/0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md)
  decision 5 rejects end-to-end multi-hop identity and defers origin-signed
  payloads to the application layer; the facet is deliberately a claim whose
  binding comes from the per-hop login/Noise mechanisms ADR-0045 already
  ordered. X.509/CA identity records are rejected there for the same reasons.

## Discussion

Genuinely contentious points, flagged for the maintainer:

1. **The pre-auth ACL exemption** (§C.2). ✅ **RESOLVED 2026-07-17 — the
   maintainer rules the hard MUST-serve posture, ratifying §C.2 as drafted and
   as shipped; no code change.** A stable, unauthenticated-readable 32-byte
   identity is a fingerprinting/tracking primitive: any peer that can reach a
   node can enumerate and recognize it forever. The RFC accepts this because
   TOFU pairing structurally requires pre-auth key disclosure and the target
   Noise handshake discloses the static key anyway.

   **The accepted cost, stated plainly: a node's identity is world-readable
   pre-auth. A scanner can fingerprint devices, and there is no off switch.**

   The rejected alternative — a default-shipped `EVERYONE@ READ` ACE on the
   facet that a deployment MAY delete — would have preserved walker behavior in
   the common case and given privacy-critical deployments that off switch. It
   was rejected on two grounds: it forces every walker to handle
   `tr::access::denied` as a **fourth outcome** beside record / not_found /
   unreachable, and a node whose operator deleted the ACE becomes
   **un-pinnable** — it cannot be TOFU'd, so it cannot participate in the
   ADR-0045 challenge at all. That is not a privacy switch; it is an
   unauthenticatable node. A deployment that genuinely must not be fingerprinted
   is asking not to be reachable, which is a transport-layer or network-layer
   posture, not a field-level ACE.

   Recorded here per this section's own instruction ("Implementer input wanted
   before the window closes") — the window stays open to 2026-07-31 and this
   ruling is the maintainer's, not an implementer's, since
   [`docs/implementations.md`](../../implementations.md) registers none.
2. **Record vs bare VALUE** (§B): 60 vs 36 bytes per read. If the maintainer
   judges algorithm agility not worth 24 bytes on a control-plane read, the
   bare form needs a different agility story (a v2 surface, or kind-by-length
   — rejected in drafting as undecodable for equal-length kinds).
3. **`kind` as u8 vs string NAME.** The u8 + RFC-gated registry mirrors the
   error-code registry's compact half; a string kind (`"ed25519"`) would mirror
   its extensible half and permit vendor kinds without an RFC. Drafted as u8
   because identity kinds — unlike vendor errors — are precisely the thing that
   SHOULD be RFC-gated: two implementations disagreeing on a kind's key format
   cannot interoperate on pins or ACEs.
4. **Rotation and multi-key overlap.** v1 serves exactly one record; rotation
   is replacement through the local host API, surfaced to clients as a TOFU
   alarm (§C.5) — the SSH model, adequate for the deployments in scope. A
   graceful-rotation overlap (old + new key served together) would need the
   record to become a sequence; deliberately deferred until a consumer needs it.
5. **Tightening §C.6 to MUST.** When the ed25519-login RFC lands (the wire form
   of the ADR-0045 target, feeding the #375/#406 caller-identity seam), the
   subject-token = raw-key alignment should likely harden from SHOULD to MUST
   in that RFC, which owns the login's subject derivation.

Per [GOVERNANCE.md](../../../.github/GOVERNANCE.md), the tracking issue
([#410](https://github.com/avatarsd-llc/libtracer/issues/410)) stays open at
least 14 days for implementer feedback before this document is merged (unless
the standing solo-maintainer waiver is applied, as on RFC-0002/0005/0008).
Sustained objections and their resolution to be recorded here.