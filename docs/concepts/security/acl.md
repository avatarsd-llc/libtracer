# Access control lists

## In one paragraph

An ACL is a list of entries stored in a vertex's `:acl` field. Each entry, an ACE, names one
subject and the rights it grants: `READ` (`0x01`) and `WRITE` (`0x02`) are the only two. An entry
flagged `INHERIT` covers the vertex's whole subtree, so a vertex's effective ACL is its own
entries plus those it inherits. Reads, `await` and subscribing need `READ`; writes, deliveries
into a target and creating a child need `WRITE`. Only the node's own application writes `:acl`.
A vertex with no effective entries is open, and so is every vertex until the application installs
a subject lookup; once an entry is present, a caller it does not name is refused with
`ERROR{tr::access::denied}`.

## The picture

```{mermaid}
:caption: "Legend: purple = application, green = vertex, sky blue = record."
flowchart LR
    accTitle: An inherited grant and a local one
    accDescr: The owner application writes an ACL on /sensor with an inheriting entry granting peer-a READ and WRITE. The child /sensor/temp has its own entry granting everyone READ. The effective ACL at /sensor/temp is both entries, so peer-a may read and write it and everyone else may read it.
    O([owner]):::app -->|"write :acl"| S("/sensor"):::vertex
    S --- A1["ACE: peer-a, READ and WRITE, INHERIT"]:::record
    S --> T("/sensor/temp"):::vertex
    T --- A2["ACE: EVERYONE@, READ"]:::record
    A1 -->|"inherited"| E{"effective ACL at /sensor/temp"}
    A2 --> E
```

## When you need it

When a remote subject should reach part of the graph and not the rest: a dashboard that reads
but never writes, a controller that writes only its own setpoints, a peer that may open no
connections of its own.

## How it works

**An entry's fields.** Each ACE is a run of `NAME` and value pairs: `type` (ALLOW `0`, DENY `1`),
optional `flags`, `subject`, `access_mask` (a `u32`) and an optional `expires_ns`. A field
repeated, a key unknown, a bit set outside `0x03` or a value wider than its field refuses the
whole write with `TYPE_MISMATCH`, and nothing is stored: an ACL is a security document, and a
field dropped in silence would widen access.

**Subjects.** A subject is compared byte for byte with the token the subject lookup returns. The
one special subject is `EVERYONE@`, and a lookup that returns those bytes is refused at every
gate, so no caller can pass itself off as everyone.

**Evaluation.** The default policy, `allow_only_policy_t`, stores ALLOW entries with the
`INHERIT` flag alone and refuses a write carrying DENY or any other flag. An operation is
allowed when some non-expired entry for its subject grants its bit. A host may bind
`full_acl_policy_t`, which also evaluates DENY, in order, first match per bit.

**Two gates for a subscription.** Appending to `:subscribers[]` needs `READ` on the producer.
Each delivery needs `WRITE` on the target. Rewriting someone else's slot `:subscribers[N]`
needs `READ` and must come from that slot's holder.

**Leases.** An entry with a non-zero `expires_ns` is a lease, compared with the `now` the
application's subject lookup supplies on its own clock. A lookup that supplies no `now` makes
every lease fail closed: a leased ALLOW grants nothing.

**Reading it back.** A read of `:acl` re-encodes the stored entries, so what an auditor reads is
what the gates evaluate. An empty structured ACL clears; a primitive one is refused. The graph
root has no `:acl`.

## Worked example

Two entries: `peer-a` may read and write the subtree, and everyone may read this vertex until a
lease runs out.

```{hex-walk} acl/acl-aces
:caption: An ACL of two ACEs.
str: key
u8: type, 0 = ALLOW
str: key
u8: flags, 0x1 = INHERIT
str: key
str: the subject
str: key
u32: access_mask, 0x3 = READ and WRITE
str: key
u8: type, 0 = ALLOW
str: key
u8: flags, none
str: key
str: the subject, the one special token
str: key
u32: access_mask, 0x1 = READ
str: key
u64: expires_ns, on the application's clock
```

## Cost

### Wire bytes

The example is 183 bytes: an ACE costs its five or so `NAME` and value pairs, about 70 to 100
bytes. An ACL is written by the owner and read on demand; it never rides on a data frame.

### Allocations

The first `:acl` write on a vertex draws its ACL state from the vertex's extension source, and
the entries, subjects included, are one table block. The effective ACL, own entries merged with
inherited ones, is cached beside them and rebuilt only after an `:acl` on the path changes.

### Latency

The data path pays one null check while no subject lookup is installed. With one, a gate whose
cached merge is current pays one atomic load to know it, then evaluates the merged entries; a
lease is judged at check time, never cached. `acl_cache_race_test` pins the cache; no bench on
this page times a gate.

### Flash and RAM per target

| Target | Policy | RAM |
| --- | --- | --- |
| NARROW | ALLOW-only | the entries, in the vertex's extension block |
| MID | ALLOW-only | same |
| WIDE | ALLOW-only, or the full policy with DENY | same |

## What the library does not do

- It has no `admin` right. Remote administration goes through a vertex the owner's code serves.
- It does not attach owner meaning to any subject; `OWNER@` is an ordinary token.
- It does not read a clock to expire a lease.
- It does not let a child entry take back an inherited grant under the default policy.

## Pitfalls

- **Symptom:** a vertex locks everyone out after a narrow grant. **Cause:** any entry closes an
  otherwise open vertex. **Fix:** grant every subject that still needs access, or `EVERYONE@`.
- **Symptom:** a remote subscribe through a connection answers `NOT_FOUND`. **Cause:** the
  connection vertex grants `READ` only, and the append is a write there. **Fix:** grant `READ`
  and `WRITE` on each connection vertex, and keep `WRITE` off the creator endpoint.
- **Symptom:** a leased grant never admits anyone. **Cause:** the subject lookup supplies no
  `now`. **Fix:** write the application's `now` beside the token.

## Where the rules live

**Normative source:** [v1 §3](../../spec/v1.md#3-wire-format), which incorporates
[reference 05 §0x0A ACL](../../reference/05-protocol-tlvs.md#0x0a--acl).

**Exercised by:** `acl-aces`, `ace-duplicate-key`, `ace-retired-bit`, `acl_test`,
`effective_acl_test`, `security_acl_test`, `mount_hop_acl_test`.

## History

:::{dropdown} How the ACL got here
- [ADR-0020](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md)
  chose NFSv4-style entries with inheritance.
- [RFC-0026](../../spec/rfcs/0026-ace-access-mask-canonical-u32.md) fixed `access_mask` at `u32`.
- [RFC-0036](../../spec/rfcs/0036-two-rights-fields-are-paths-under-their-vertex.md) cut the
  rights to `READ` and `WRITE` and made `:acl` the owner's alone.
- [RFC-0037](../../spec/rfcs/0037-ace-expiry-takes-the-applications-now.md) made a lease take the
  application's `now`.
:::

Next concept: [Subjects and identity](identity.md). Go deeper:
[reference 05 §0x0A ACL](../../reference/05-protocol-tlvs.md#0x0a--acl), then the
[subject resolver example](../../examples/acl-subject-resolver.md), then the
[graph module](../../modules/graph.md).
