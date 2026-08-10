# Access control uses NFSv4-style ACEs with inheritance; `admin` is precisely `WRITE_ACL`

Status: accepted

[ADR-0018](0018-access-control-authorization-pluggable-subject-token.md) established that access control is *authorization over a pluggable subject-token*, but left the **rights model** open and used a flat `READ/WRITE/SUBSCRIBE` bitfield ([reference 05](../reference/05-protocol-tlvs.md) §`0x0A` ACL). Designing the byte layout for in-band creation ([ADR-0017](0017-in-band-vertex-creation-controller-orchestration.md)) exposed two gaps: there is no `create` or `admin` right, and there is no way to apply an ACL to a **composite** (a subtree) without writing it on every leaf. This ADR adopts the **NFSv4 ACL model** to fill both.

## Decision

**Each grant is an ACE (access control entry), NFSv4-style:** `{type: ALLOW|DENY, flags, subject-token, access_mask, expires_ns?}`. The `05` ACL TLV (`0x0A`, recursive: outer collection of inner ACEs) keeps its shape; each inner ACE gains `type` and `flags` and its `permissions` becomes the richer `access_mask`.

**The `access_mask`:**

```
READ=0x01  WRITE=0x02  SUBSCRIBE=0x04  CREATE=0x08(add child)  DELETE=0x10
READ_ACL=0x20  WRITE_ACL=0x40  WRITE_OWNER=0x80     (0x100+ reserved)
```

So **`admin` is precisely `WRITE_ACL`** — the right to modify the ACL / delegate — distinct from acting on the vertex and from `WRITE_OWNER` (transfer ownership). `CREATE` gates the in-band creation field-write of [ADR-0017](0017-in-band-vertex-creation-controller-orchestration.md).

**ACEs ALLOW or DENY**, evaluated in order, first-match-per-bit (NFSv4 semantics) — so "EVERYONE read, DENY peer-X" is expressible. **Special subjects** `OWNER@` and `EVERYONE@` avoid enumerating principals.

**Composite ACLs inherit.** An ACE carries `flags` (`INHERIT=0x1`, `INHERIT_ONLY=0x2`, `NO_PROPAGATE=0x4`, `GROUP=0x8`). An ACE with `INHERIT` on a composite vertex applies to its whole subtree — riding the **graph/address composition** axis — so `:acl` is **not** written per leaf (the same economy as a composite subscription). A vertex's **effective ACL** = its own ACEs + inherited ancestor ACEs.

**Wire-full, MCU-subset enforcement.** The wire layout is the full NFSv4 model. The **required-modules MCU profile enforces a subset** — ALLOW-only, a single `INHERIT` flag, no DENY ordering — so a Cortex-M0 stays small; the full DENY/ordered/audit model is the `security_acl` host module (post-MVP, [reference 10](../reference/10-module-catalog.md)).

## Considered options

- **Flat `subject → permission-bitfield` (the original `05` shape).** Rejected: no `create`/`admin` (ADR-0017/0018 unenforceable), no DENY, and **no inheritance** — a composite ACL would have to be re-written on every leaf, which does not scale with the graph/address composition.
- **A bespoke libtracer rights model.** Rejected: NFSv4 ACLs are a mature, well-understood standard with exactly the primitives we need (a rich mask, ALLOW/DENY, directory inheritance that maps to subtree inheritance). Reinventing it would be worse-understood and no smaller.
- **POSIX mode bits (owner/group/other rwx).** Rejected: too coarse — no per-subject grants, no `subscribe`/`create`/`admin` distinction, no inheritance control.

## Consequences

- ADR-0017's `create` and ADR-0018's `admin`/delegation are now **enforceable** (`CREATE`, `WRITE_ACL` bits).
- **One `:acl` write on a composite covers its subtree** via `INHERIT`, matching composite subscription's economy.
- The `05` `0x0A` ACL byte layout is revised to the ACE shape (it self-declared revisable for `security_acl`); enforcement remains deferred to the `security_acl` module, with the MCU subset in the required modules.
- This **extends ADR-0018** (the rights model it left open); the pluggable-subject-token and authz≠authn decisions are unchanged — the `subject` field of an ACE *is* the pluggable token.
- It is an **L4/module** decision, not a new wire primitive beyond the already-reserved `ACL 0x0A` code.

## Erratum (2026-08-10): `OWNER@` is withdrawn — it was published as special and never evaluated as one

The Decision section reads:

> **Special subjects** `OWNER@` and `EVERYONE@` avoid enumerating principals.

**Only `EVERYONE@` was ever built.** `detail_acl::ace_applies`
(`core/include/libtracer/security_acl.hpp`) branches on exactly one string; every other
subject — `OWNER@` included — is compared byte-equal against the resolver's token, and a
repo-wide grep finds `OWNER@` only in prose, never in a branch. So the name was published as a
special subject in this ADR, in [reference 05](../reference/05-protocol-tlvs.md) §`0x0A` and in
[`CONTEXT.md`](../../CONTEXT.md), while behaving as an ordinary opaque token everywhere.

That gap is harmful in two opposite directions, which is why it is corrected rather than left
as a known gap:

1. **An `OWNER@` ACE is silently inert, and inertness here reads as a lock.** An operator
   following the old text writes `{subject: "OWNER@", access_mask: WRITE_ACL}` believing the
   vertex owner keeps admin. No caller's token is those bytes, so the ACE matches nobody — and
   because *any* present ACE closes an otherwise-open vertex, the write **locks** the vertex it
   was meant to delegate. A grant that reads as a grant and evaluates as nothing is the failure
   class [#906](https://github.com/avatarsd-llc/libtracer/issues/906) fixed on the parse side.
2. **The name is impersonable.** Being an ordinary token, a deployment whose resolver passes a
   caller-supplied identity through can mint a principal literally named `OWNER@`, matching such
   an ACE exactly. [#908](https://github.com/avatarsd-llc/libtracer/issues/908) reserved
   `EVERYONE@` against the resolver's output and deliberately left `OWNER@` alone, on the ground
   that reserving a name whose semantics do not exist would be inventing those semantics in the
   wrong place.

**The correction is to withdraw the name, not to reserve it.** `EVERYONE@` is the one special
subject. `OWNER@` is an ordinary opaque token with no meaning to this core; a deployment may
still use those bytes as a principal name and must not expect owner semantics from them.
Withdrawing removes *both* consequences at once — with no document telling an operator to write
such an ACE, there is nothing for an impersonated `OWNER@` to match.

**This is an erratum, not an amendment.** No wire surface moves: the `subject` field is opaque
bytes before and after, `encode_acl`/`decode_acl` are untouched, and no conformance vector
changes. The ACE shape, the `access_mask` (including the still-declared `WRITE_OWNER` bit), the
inheritance model and the MCU subset are all unchanged.

**Real owner semantics remain open, and are an amendment.** They need a per-vertex owner
identity the graph does not hold, and they change how a *stored* ACE evaluates — an existing
`OWNER@` ACE that matches nobody today would start matching somebody. That needs an RFC, and it
would reserve the token the way #908 reserved `EVERYONE@`. `WRITE_OWNER` stays declared in the
mask against that day. ([#1033](https://github.com/avatarsd-llc/libtracer/issues/1033).)
