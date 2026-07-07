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
