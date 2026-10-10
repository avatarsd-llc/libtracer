<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0036 — Two rights, and `:` fields are paths under their vertex

<!-- status: proposed -->

| Field | Value |
| ---- | ---- |
| **RFC** | 0036 |
| **Title** | Two rights, and `:` fields are paths under their vertex |
| **Status** | **proposed** (2026-10-10). The maintainer set the direction on 2026-10-10; this document is awaiting rulings on §12. |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-10-10 |
| **Comment window** | Waived by default while the project is solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"). Invoke it explicitly if outside input is wanted. `docs/implementations.md` lists no registered implementation, so the waiver's revert trigger has not fired. |
| **Instrument** | **Amendment.** It changes the `access_mask` registry of [reference/05](../../reference/05-protocol-tlvs.md) §`0x0A`, a normative annex. An `:acl` write that is accepted today is refused, and several field writes change which caller they admit. A conforming peer can observe each of these changes. |
| **Tracking issue** | [#2088](https://github.com/avatarsd-llc/libtracer/issues/2088). It blocks [#2019](https://github.com/avatarsd-llc/libtracer/issues/2019) (wire spelling for a suspended subscriber slot), whose RFC-0035 has an open question that this RFC answers (§8). |
| **Target spec version** | v1 itself. `docs/spec/v1.md` still reads "(DRAFT)". |
| **Amends** | reference/05 §`0x0A` (the rights registry and the enforcement paragraph); [ADR-0020](../../adr/0020-acl-nfsv4-style-aces-with-inheritance.md) (its `access_mask`, its "`admin` is `WRITE_ACL`" decision, and the owner-semantics item its #1033 erratum left open); [RFC-0014](0014-creator-endpoint-connection-lifecycle-and-link-liveness.md) §5 and Amendment 2 (the `CREATE` row and the payload-right table); [RFC-0005](0005-subtree-subscriptions.md) §D as amended (write-creates under `CREATE`); [RFC-0009](0009-vertex-removal-and-subscriber-eviction.md) §D.1 (the `[N]` write gate); [ADR-0026](../../adr/0026-consumer-initiated-subscription-client-write.md) (the `SUBSCRIBE` fan-out gate). |
| **Relates to** | RFC-0034, the subscription selector's fields ([PR #2068](https://github.com/avatarsd-llc/libtracer/pull/2068)); RFC-0035, `:subscribers[N]` as one surface ([PR #2086](https://github.com/avatarsd-llc/libtracer/pull/2086)). |

> **Numbering note.** RFC-0034 and RFC-0035 are in open pull requests and are not yet on
> `main`, so this record leaves a two-number gap on `main` until they merge. The gap is not a
> skipped number. Numbering gaps and why they are not reused are recorded in the
> [ADR and RFC index](../../adr-rfc-index.md#numbering-gaps).

---

## 1. Summary

The `access_mask` keeps two rights, **READ** (`0x01`) and **WRITE** (`0x02`). The other six
rights are retired: SUBSCRIBE, CREATE, DELETE, READ_ACL, WRITE_ACL and WRITE_OWNER. Each `:` field
is a path under its vertex and is governed by the vertex's own ACL:

- Reading any `:` field needs READ on the vertex. `:identity` keeps its exemption (RFC-0011).
- Writing a `:` field needs WRITE on the vertex. Two fields have their own rule:
  - **`v:acl` is written by the owner only.** The owner is the empty local caller. No ACE can
    grant a remote peer write on `:acl`.
  - **`v:subscribers[]`:** a caller may create, toggle or clear an entry that it holds itself if it
    has READ on `v`. Only the owner can change an entry held by someone else.
- `v:children[]` creation, write-creates and a creator endpoint's `SPEC` are all WRITE.

An ACE that carries any bit other than READ or WRITE is refused when it is installed, with
`tr::schema::type_mismatch`. It is not silently ignored.

Remote administration goes through a vertex the owner's application registers. Its handler
applies ACEs locally, under the owner's channel. The protocol keeps no second kind of authority.

## 2. Motivation

### 2.1 Eight rights, six gates, and a `:` plane that is not uniform

ADR-0020 adopted the NFSv4 mask so that creation and administration could be enforced. In the
reference core today:

| Right | Bit | What gates on it today |
| ---- | ---- | ---- |
| READ | `0x01` | data reads, `await`, and every `:` field read except a whole `:acl` read |
| WRITE | `0x02` | data writes, fan-in at a delivery target, `:subscribers[N]`, `:settings.app.<name>`, a creator endpoint's `NAME` (remove) |
| SUBSCRIBE | `0x04` | the `:subscribers[]` append, and the replacement edge of an `[N]` replace |
| CREATE | `0x08` | `:children[]`, write-creates through a creation hook, a creator endpoint's `SPEC` (create) |
| DELETE | `0x10` | nothing. RFC-0009 §A.2 records it as reserved and unused |
| READ_ACL | `0x20` | a whole `:acl` read |
| WRITE_ACL | `0x40` | an `:acl` write |
| WRITE_OWNER | `0x80` | nothing. There is no owner identity to transfer |

So the `:` plane, the vertex's `ioctl` (CONTEXT.md §Field-write), has a separate right for nearly
every field. A deployer who wants "peer P may use this vertex" has to know that this means READ,
WRITE and SUBSCRIBE, that `:acl` reads need a fourth right, and that creation needs a fifth. Two
bits gate nothing, and a mask that carries them is accepted and stored anyway.

### 2.2 Administration held remotely is a root right

A peer that holds WRITE_ACL can rewrite the ACL, so it can grant itself every other right. Under
the default ALLOW-only profile, an inherited WRITE_ACL cannot be taken back at a descendant
either. WRITE_ACL is therefore not one right among eight. It is the whole authority, held by a
remote subject that the node can identify only by the opaque token its resolver returns.
[ADR-0086](../../adr/0086-identity-is-app-level-key-plus-opaque-credential-anchored-names-are-a-policy.md)
places identity at the application level and keeps permissions in the receiver's ACL. It leaves
delegation chains out of scope. A remote admin bit is a one-step delegation chain, and the
protocol is the wrong place to hold it.

### 2.3 Owner semantics have been open since #1033

ADR-0020's 2026-08-10 erratum withdrew `OWNER@` and recorded that "real owner semantics remain
open, and are an amendment". The core has had an owner since #905 all the same: the **empty
local caller**, which `graph_t::acl_allows` (`core/src/graph.cpp:graph_t::acl_allows`) admits
before any resolver runs, and which no remote operation can spell. `write_ctx_t::is_local_owner`
(`core/include/libtracer/vertex.hpp:write_ctx_t`) is how a door asks for it, and that header
explains why there is no `OWNER@` sentinel. This RFC makes that owner the normative one.

### 2.4 RFC-0035 needs a rule for other parties' subscriptions

RFC-0035 §12 Q2 asks who may suspend or clear a subscription the caller did not make. It weighs
WRITE_ACL against a new `MANAGE_SUBSCRIBERS` bit. Both answers add authority to the ACL. This RFC
answers by taking authority out of it (§8).

## 3. The rule

**Normative.** "MUST" and "MUST NOT" are used as in RFC 2119.

### 3.1 Two rights

The `access_mask` registry of reference/05 §`0x0A` becomes:

```
READ=0x01  WRITE=0x02     (0x04 and above reserved)
```

The canonical wire width stays u32 ([RFC-0026](0026-ace-access-mask-canonical-u32.md)), and so
does the acceptance rule for narrower spellings. The six retired bits, `0x04` to `0x80`, go back to
the reserved range together with `0x100` and above.

**Install-time refusal.** An `:acl` write MUST be refused with `tr::schema::type_mismatch`
(`0x0030`) when any ACE's `access_mask` has a bit set outside `0x03`. The whole write is refused and
nothing is stored. This is the same family of refusal as a DENY ACE under the ALLOW-only profile,
or a flag bit beyond `INHERIT`: an ACE whose meaning the evaluator would silently weaken is not
stored (#906). A mask of `0` stays legal. It grants nothing, as it does today.

### 3.2 A `:` field is a path under its vertex

A `:` field has no ACL of its own. It is evaluated against the **effective ACL of the vertex it is
under**: the vertex's own ACEs plus the `INHERIT` ACEs of its ancestors (ADR-0020, unchanged). In
this sense the fields form the vertex's ACL subtree.

| Operation | Right demanded at `v` |
| ---- | ---- |
| read of any `v:<field>`, every shape | READ |
| write of `v:settings.app.<name>` | WRITE (unchanged; RFC-0010 §A.3's declaration gate still follows it) |
| `v:children[]` append (creation) | WRITE |
| write of `v:acl` | the owner only (§3.3) |
| `v:subscribers[]` append | READ (§3.4) |
| write of `v:subscribers[N]` | the holder with READ, or the owner (§3.4) |

`v:schema`, `v:stats` and `v:identity` have no write surface, and that does not change.
`:identity` stays readable above the READ gate (RFC-0011; reference/05 §`0x0B`). A shape that names
nothing is still resolved before the gate wherever it is today, so the #869 divergences pinned by
`field_shape_matrix` are not changed by this RFC. One of them goes away, though: a `:acl` read no
longer has two rights to choose between, so its shape test no longer selects which right is
demanded (§4, row 3).

### 3.3 `v:acl` is written by the owner only

The **owner** is the node's local host: an operation whose caller context is empty. The empty
context is the trusted channel that ACL evaluation short-circuits (#905). A remote operation always
carries a non-empty context (the inbound link's name), so it cannot be the owner. Every network
write path must carry its caller for this to hold, which #974 enforced.

- A write of `v:acl` from any caller that is not the owner MUST be refused with
  `tr::access::denied` (`0x0050`). No ACE can grant it, so no ACE is evaluated.
- A read of `v:acl` needs READ on `v`, like every other field. A peer that can read the vertex can
  read who else may read it. A node that wants to hide its policy withholds READ.
- There is no owner subject token. `OWNER@` stays what ADR-0020's erratum made it: an ordinary
  opaque token with no meaning to the core.

Ownership is **node-scoped**. There is one owner per node and no per-vertex owner identity, so
there is nothing to transfer and WRITE_OWNER has nothing to gate.

**Remote administration.** A deployment that wants a remote peer to manage ACLs registers an
**owner-app vertex**. That is an ordinary vertex whose write handler, or whose `settings.app` field
admission (RFC-0010 §A.3), receives the request together with the writer's subject
(`write_ctx_t::subject`), decides by the application's own policy whether to honor it, and then
writes the target's `:acl` through the host API as the owner. The peer needs WRITE on the owner-app
vertex and nothing else. The policy for who may grant what to whom, how far delegation goes, and
what gets audited lives in the application, where ADR-0086 puts identity. The protocol defines no
admin vertex type (§12 Q7).

### 3.4 `v:subscribers[]`: your own entry needs READ, everyone else's is the owner's

An entry's **holder** is the subject that its admission was made under. That is the writer of the
append (RFC-0021 §E, "the gate is the writer's"), and not necessarily the target. A replace makes
the replacer the holder. An edge admitted by a local host call is held by the owner. The core
already stores the context per edge (`subscriber_remote_t::caller`,
`core/include/libtracer/subscriber.hpp:subscriber_remote_t`), so the holder costs no new state.
Two holders are compared as RFC-0035 §7.1 compares them: both contexts are resolved through the
node's subject lookup at check time (§12 Q1).

| Write | The caller holds the entry | The caller does not |
| ---- | ---- | ---- |
| `:subscribers[]` append (create) | READ. The new entry is the caller's own | — |
| `[N]` clear (empty `STATUS`) | READ | owner only |
| `[N]` toggle (RFC-0035, if accepted) | READ | owner only |
| `[N]` replace (a `SUBSCRIBER`) | READ. The new edge is the caller's own too | owner only |

- **Why READ is enough for the caller's own entry.** A subscription delivers what a read of `v`
  would return, when it changes. Requiring READ means a caller cannot subscribe to data it may not
  read. Clearing or toggling one's own entry gives no power beyond what the caller already had,
  because it could let the edge lapse by leaving.
- **Why everyone else's is the owner's.** Clearing, suspending or retargeting another party's
  subscription decides what that party receives. No right on `v` should carry that decision. The
  owner can still do it, and through an owner-app vertex (§3.3) it can let a remote peer do it
  under the application's policy. The RFC-0034 selector is exactly such a vertex (§9).
- **The target's fan-in gate does not change.** A delivery into a local target still needs WRITE
  on the target under the holder's context (ADR-0026's second gate).
- **Order.** The shape checks stay ahead of every gate. A non-owner without READ on `v` MUST be
  refused `tr::access::denied` before slot `N` is looked up. A caller with READ then learns whether
  slot `N` exists and whether it holds it. A READ of the list tells it the same, as RFC-0035 §7.2
  already argues.

### 3.5 In-tree access is the owner's

The application's own writes, and any in-tree module that acts for the application, call the graph
with the empty caller and are allowed natively. Examples are a mux vertex, a bridge, a selector and
the creation hooks. This is today's rule (#905). This RFC adds one duty, which the reference
already keeps: **a module acting for a remote caller MUST pass that caller's context**, never the
empty one. The router does this at every terminus (`fwd_router_t::deliver_local` takes its caller
as a required parameter). A module that wants the owner's authority on purpose is an owner-app
vertex, and its policy is the gate (§3.3).

### 3.6 Remotely created children

A child created by a remote peer, through a creator endpoint's `SPEC`, a `:children[]` append or a
creation hook, is **owned by the node** like every other vertex. Its creator gets nothing for
having created it.

It **starts with no ACEs of its own**. Its effective ACL is the `INHERIT` ACEs of its ancestors,
and if there are none it is open by default, which is today's rule for any vertex. The core stamps
no creator ACE (§12 Q5). A creation hook or a transport factory runs locally with the creator's
caller context in hand. If the application wants its creators to manage what they create, it can
install such an ACE as the owner. That is policy, and it costs the core nothing.

For a creator endpoint, the deployer therefore decides what a new connection admits through the
inheritable ACEs on `/net` and `/net/<module>`, as it does today.

## 4. Every core gate that tests a retired right, and what it becomes

Found with `git grep` on `origin/main` (`f3001401`) for `acl_right_t::SUBSCRIBE`, `CREATE`,
`DELETE`, `READ_ACL`, `WRITE_ACL` and `WRITE_OWNER`, and for the ALLOW-only profile
(`allow_only_policy_t`, `kAcceptsDeny`). Outside `core/src` the hits are tests and examples
(`core/tests/acl_test.cpp`, `mount_hop_acl_test.cpp`, `payload_right_table_test.cpp`,
`read_back_backend_test.cpp`, `security_acl_test.cpp`, `core/examples/acl_right_bits.cpp`), the
CHANGELOG, and one ESP-IDF README sentence.

| # | Site | Today | After |
| ---- | ---- | ---- | ---- |
| 1 | `graph_t::admit_subscriber` (`core/src/graph.cpp`), the single admission step every subscribe door reaches (ADR-0049) | SUBSCRIBE on the producer | READ on the producer |
| 2 | `field_surface_t::write_subscribers`, the `[N]` arm (`core/src/graph_fields.cpp`) | WRITE on `v`, before the payload is looked at | the owner passes; otherwise the caller must hold slot `N` **and** have READ on `v`. For a replace, row 1's READ is the same test, so it runs once |
| 3 | `field_surface_t::read_acl` | READ_ACL for a whole read, READ for other shapes | READ for every shape. The ternary goes |
| 4 | `field_surface_t::write_acl` | WRITE_ACL | `write_ctx_t::is_local_owner()`. No ACE walk and no resolver call |
| 5 | `field_surface_t::write_children` | CREATE on `v` | WRITE on `v` |
| 6 | `graph_t::find_or_create_ptr`, a missing level under a creation hook (RFC-0030 §7.2) | CREATE on the hooked parent | WRITE on the hooked parent |
| 7 | `graph_t::write_impl`, `graph_t::declared_write_right` and the creator endpoint's `kRights` table (`core/src/transport_vertex.cpp`) | a vertex may map the written TLV's type to a right: `SPEC` to CREATE, `NAME` to WRITE (RFC-0014 Amendment 2) | WRITE for every value write. `payload_right_t`, the `rights` parameters, `declared_write_right` and `leading_type` are deleted. The node keeps its `:schema` catalog (RFC-0014 Amendment 3), which rides the same declaration |
| 8 | `detail_acl::parse_acl_into` (`core/include/libtracer/security_acl.hpp`) | stores any u32 mask | refuses a mask with a bit outside `0x03` (§3.1). One more term in the condition that already refuses DENY and unknown flags |
| — | DELETE, WRITE_OWNER | declared, gate nothing | the enumerators are removed |
| — | the mount-hop gate, `fwd_router_t::fwd_op_right` | READ or WRITE by the frame's op | unchanged. A subscribe through a mount is a `FWD{WRITE}` and still needs WRITE at each connection vertex it crosses, as the 0.18.1 layout says |
| — | `field_surface_t::write_settings` | the owner, or WRITE | unchanged. It already has the shape this RFC gives every field |

**The ALLOW-only MCU profile** (`allow_only_policy_t`, the `default_config_t::acl_policy_t`
binding) and the full policy (`full_acl_policy_t`) evaluate one bit through the merged list, and
that does not change. What changes is that the two rules of §3.3 and §3.4 that no ACE can express
are **identity tests**, not ACE evaluations. Both profiles enforce them identically, and the
ALLOW-only limit (an inherited right cannot be taken back at a descendant) no longer reaches
administration, because there is no administration right left to inherit.

## 5. Perf, RAM, code size and branches across NARROW, MID and WIDE

| | Effect |
| ---- | ---- |
| RAM per vertex, per edge, per ACE | **none**. The holder is the context each edge already stores, and the mask stays u32 in memory. Narrowing `ace_t::access_mask` to a byte would save 8 B per ACE on LP64 and ILP32 alike, but it would break user code that brace-initializes a mask from a `std::uint32_t`, for a control-plane structure that is counted in tens. It is not proposed |
| RAM per creator endpoint | **one table-source draw fewer** (the two `payload_right_t` rows) and the array header inside the declaration node. That is tens of bytes per transport module. The node and its flag stay, for the catalog |
| value-write hot path (`write_impl`) | **one relaxed flag-bit test fewer on every value write**, on every class. That test is the `has_payload_rights()` check, which was taken only by creator endpoints |
| fan-out and delivery | unchanged |
| subscribe admission | unchanged: one `acl_allows` for a different bit |
| `:acl` write | cheaper: an empty-context test instead of an ACE walk and a resolver call |
| `[N]` write by a non-owner | one holder comparison on a cold control-plane path: up to two subject lookups with a resolver installed (§12 Q1), a byte compare without one |
| code size | an indicative figure from a RelWithDebInfo x86-64 build of a 2026-10-09 branch (`c75583e2`): `graph_t::declared_write_right` (81 B) goes, `declare_payload_rights` (506 B) loses its rows half, and the `write_acl` door (779 B) loses an `acl_allows` call. That is roughly **0.3–0.5 KB less `.text`** on x86-64. The implementation PR measures it on the ESP32 size gate |
| gate decision points | counted by reading `origin/main`, to be confirmed by the complexity ratchet (#1790): `graph.cpp` **−11** (`write_impl` −2, `declared_write_right` −4, `leading_type` −3, `declare_payload_rights` −2), `graph_fields.cpp` **+1** (`read_acl` −1, the `[N]` holder-or-owner test +2), `security_acl.hpp` **+1** (the mask term). **About −9 net**, all by deletion and merging; no function is split |

- **NARROW (MCU, ALLOW-only):** no RAM, a smaller image, and one test fewer on every write. The two
  owner rules need no resolver and no ACE storage.
- **MID:** the same, and a deployer has two rights to reason about instead of eight.
- **WIDE (host, thousands of edges):** a non-owner `[N]` write costs at most two subject lookups.
  It is O(1) in the edge count and runs off the delivery path. Nothing on the fan-out path changes.

## 6. Compatibility

This is a **breaking change before 1.0**. No backward compatibility is owed (the RFC-0028 ruling,
restated as RFC-0029 ruling 10). But every deployed ACL has to be looked at, so it gets the
upgrade note in §7.

- **An `:acl` write carrying a retired bit is refused** where it used to be stored. An application
  that installs its ACLs at boot with SUBSCRIBE, CREATE, READ_ACL or WRITE_ACL set gets
  `type_mismatch`, and nothing is installed, so the vertex stays as open or as closed as it was. The
  refusal is loud on purpose: an ACE that silently lost bits would change what it grants.
- **A remote `:acl` write is refused** even for a peer that held WRITE_ACL.
- **A peer with READ can now subscribe** where it used to need SUBSCRIBE too. The cost of the edges
  it can hold is bounded by the node's `retained` seam
  ([reference/09](../../reference/09-memory-substrate.md)), which is sized against the subscription
  population, and its exhaustion refuses the admission as `backpressure`. A READ-only "poll but
  never subscribe" grant is no longer expressible (§12 Q6).
- **A peer with WRITE can no longer clear or replace another party's slot.** A holder with READ
  can now clear its own slot without WRITE.
- **Creating through a creator endpoint needs WRITE** on the endpoint, and WRITE now also removes.
  Create-only and remove-only are no longer separately grantable through the ACL (§12 Q4).
- **Vectors** (`tests/conformance/vectors/v1/acl/`):
  - `acl/acl-aces` is **unchanged**. Its masks are `0x03` and `0x01`.
  - `acl/ace-duplicate-key` is **repaired in place**. The second `access_mask` value changes from
    `u16=0xFFFF` to `u16=0x0003` (READ|WRITE). The vector stays 84 bytes, and only its last two
    bytes change, `FF FF` to `03 00`. It still pins the same refusal: the repeated key is rejected,
    and last-wins would have widened READ to READ|WRITE. Without the repair, the vector would be
    refused for two reasons, and it would no longer isolate the one it is about. The new bytes are
    `0A4050000A404C0002000400747970650100010000020007007375626A65637401000600706565722D6102000B006163636573735F6D61736B01000200010002000B006163636573735F6D61736B010002000300`.
  - `acl/ace-retired-bit` is **new**. It is one ALLOW ACE for `peer-a` whose `access_mask` is
    `u32=0x00000004` (the old SUBSCRIBE), and every reader must reject it. It is 65 bytes:
    `0A403D000A40390002000400747970650100010000020007007375626A65637401000600706565722D6102000B006163636573735F6D61736B0100040004000000`.
    The codec round-trips it. The refusal belongs to the reader and is bound per core
    (`tests/conformance/HARNESS.md`), as for `ace-duplicate-key`.
  - `conn/remove-via-name` keeps its bytes. Its description stops saying "not `DELETE` (`0x10`)",
    because there is no DELETE right left to contrast with.
- **C++ API.** `acl_right_t` keeps `READ` and `WRITE` only. `payload_right_t` and every `rights`
  parameter of the registration calls are removed. This goes in `core/CHANGELOG.md`.
- **Rust binding.** `structured::acl`'s reader refuses a mask outside `0x03`, and the `Ace` doc
  comment lists two rights. This goes in `bindings/rust/CHANGELOG.md`.

## 7. Upgrading

*Informative. This text goes into reference/05 §`0x0A` and the core CHANGELOG on acceptance.*

Rewrite each ACE the application installs:

| Old right | New |
| ---- | ---- |
| SUBSCRIBE | READ. Subscribing needs READ on the producer |
| CREATE | WRITE on the parent, or on the creator endpoint |
| READ_ACL | READ |
| WRITE_ACL | drop it. Administer through an owner-app vertex (§3.3) |
| DELETE, WRITE_OWNER | drop them. Neither gated anything |

Then check three things:

1. **Who clears subscriptions.** A remote tool that cleared or retargeted other parties'
   subscriptions with WRITE is now refused. Move that function into an owner-app vertex, or into a
   selector (RFC-0034).
2. **The 0.18.1 connection-vertex layout** becomes: an inheritable READ on `/net`; READ and WRITE on
   each connection vertex, written once the connection exists; and no WRITE on
   `/net/<module>/conn`, which is what stops a remote peer from opening connections. As before,
   under the ALLOW-only profile WRITE stays off `/net` and `/net/<module>`. `mount_hop_acl_test`
   pins the layout.
3. **Remote formation.** An orchestrator (reference/13) used to be granted WRITE_ACL. It now needs
   WRITE where it creates and binds, READ where it subscribes, and an owner-app vertex for any ACL
   it has to set.

## 8. How RFC-0035 changes

RFC-0035 ([PR #2086](https://github.com/avatarsd-llc/libtracer/pull/2086)) is revised to cite
this RFC, and it lands after it (#2088 blocks #2019):

- **§7.1** renames the edge's "owner" to **holder**, the term of §3.4, so that "owner" means only
  the node's owner. "Only the local caller and `WRITE_ACL` holders can manage it" becomes "only the
  owner can manage it".
- **§7.2's table** becomes §3.4's table. The create and the caller's own `[N]` clear, toggle and
  replace need READ, where RFC-0035 proposed SUBSCRIBE. Another party's slot is owner-only, where
  RFC-0035 proposed WRITE_ACL. The sentence "a node built without ACL enforcement admits every
  caller to every operation, ownership included" follows the ruling on §12 Q2.
- **§7.2's order paragraph** reads "a non-owner without READ" in place of "a caller that holds
  neither SUBSCRIBE nor WRITE_ACL". The disclosure argument is unchanged.
- **§7.3**: the holder keeps READ, where RFC-0035 says SUBSCRIBE, so it can resume or re-append.
  A node that wants a party to stop receiving revokes its READ and clears the edge.
- **§12 Q2 is answered**: neither WRITE_ACL nor a `MANAGE_SUBSCRIBERS` bit. Other parties' slots
  are the owner's. Remote management goes through an owner-app vertex, and the selector is the
  first one.
- **§15**'s breaking gate reads "a peer that is not the owner" in place of "holding WRITE but not
  WRITE_ACL", and "a holder with READ gains the clear" in place of "an owner with SUBSCRIBE and no
  WRITE".
- **§8** (the selector) is unchanged, and this RFC relies on it.

## 9. How RFC-0034 fits

The RFC-0034 selector ([PR #2068](https://github.com/avatarsd-llc/libtracer/pull/2068)) **writes
as the owner**. A switch suspends and resumes refs on their producers through the host API under
the empty local caller, so it can toggle any entry it lists, whoever holds it. A remote controller
needs WRITE on the selector's own vertex, where `settings.app.active` is an RFC-0010 field, and no
right at all on the producers.

That makes the selector the reference instance of §3.3's owner-app vertex. **The ACL on the
selector's vertex is the delegation**: granting a peer WRITE there lets it switch exactly the refs
the owner listed, and nothing else. RFC-0034 §2.3's last-writer-wins rule and its "the selector
owns nothing on the wire" stand unchanged.

## 10. What this overrides, explicitly

- **ADR-0020**: "`admin` is precisely `WRITE_ACL`" and its eight-bit mask. The ADR's other
  decisions stand: NFSv4-style ACEs, ALLOW and DENY, `INHERIT`, the MCU subset and `EVERYONE@`. On
  acceptance its status key gains `superseded-in-part-by: RFC-0036`. The owner-semantics item its
  #1033 erratum left open is closed without a reserved token, by §3.3.
- **RFC-0014 §5 and Amendment 2**: the create-but-not-remove split and the payload-right table.
  "The create right is delegable on the endpoint's own ACL without any right on the parent
  transport" stays true, with WRITE in the place of CREATE.
- **RFC-0009 §D.1 and RFC-0005 §D**: WRITE for any `[N]` write, and CREATE for write-creates.
- **ADR-0026**: the SUBSCRIBE fan-out gate becomes READ. The two-ACL shape (the producer gates
  subscribe, the target gates fan-in) is unchanged.
- **CONTEXT.md**: the entries for Write-creates, Network formation, ACL entry and SUBSCRIBER
  direction.

## 11. Alternatives considered

- **An ADMIN right** (one bit for `:acl` and for other parties' subscriptions). Rejected. A remote
  holder of a bit that can rewrite the ACL holds every right, so the bit is the whole authority
  under a narrower name. It is also a delegation chain, which ADR-0086 keeps out of the protocol,
  and it is impersonable by whatever a pass-through resolver returns. Under the ALLOW-only profile
  an inherited ADMIN cannot be taken back at a descendant. And it is a third right, which the
  ruling excludes. An owner-app vertex expresses everything ADMIN would, plus the policy ADMIN
  cannot express.
- **Keeping WRITE_ACL.** Rejected for the same reasons. It would also leave `:acl` as the one `:`
  field not governed by its vertex's READ and WRITE, which is the non-uniformity this RFC exists to
  remove. Keeping READ_ACL beside it would keep a second read right for one field, to hide a policy
  from peers that may already read everything the policy protects.
- **An `OWNER@` sentinel.** Rejected. ADR-0020's erratum (#1033) withdrew it for two reasons. An
  `OWNER@` ACE matched nobody and locked the vertex it was meant to delegate. And the name is
  impersonable through a resolver that passes caller-supplied identity through. Bringing it back
  would mean reserving it as #908 reserved `EVERYONE@`, storing a per-vertex owner identity that the
  graph does not hold (RAM on every vertex), and changing how existing stored ACEs evaluate. The
  empty local caller is a stronger owner than any token: no ACE can spell it, and no remote
  operation can carry it.
- **Keeping SUBSCRIBE separate from READ.** Rejected by the ruling. A subscription discloses
  nothing a read does not. Its cost to the producer is bounded by the `retained` seam, not by a
  right. Keeping the bit would keep the poll-only grant at the price of a third right on every core.
- **Keeping CREATE separate from WRITE.** Rejected. Creating is a write to the parent or to the
  endpoint. The one thing CREATE bought, create-without-remove on a creator endpoint, can be had
  from the endpoint's admission filter, which sees the payload type and the writer's subject
  (§12 Q4).
- **Silently masking retired bits on install.** Rejected. An ACE that loses bits without a word
  grants something other than what its writer wrote, which is the failure #906 removed from the
  parser.

## 12. Open questions for the maintainer

Each has a recommendation, judged on throughput, latency, RAM and the NARROW to WIDE spectrum.

1. **How two holders are compared.** (a) Resolve both caller contexts through the subject lookup at
   check time, as RFC-0035 §7.1 does: this is stable when a peer comes back over a new link under
   the same key, and costs up to two lookups on a cold path. (b) Byte-compare the stored contexts:
   no lookup, but a peer that reconnects loses its own edge to the owner. **Recommended: (a)**,
   which falls back to (b) when no resolver is installed.
2. **Whether the two owner rules hold on a node without enforcement.** (a) Always: they are
   identity tests that need no resolver, they cost one branch, and they stop a peer from planting
   ACEs that wake up when a resolver is installed later. (b) Only once a resolver is installed,
   which keeps "open by default" literal, as RFC-0035 §7.2 assumed. **Recommended: (a)**. Read and
   write stay open by default.
3. **What install refuses.** (a) Every bit outside `0x03`, the six retired ones and `0x100` and
   above alike, with `type_mismatch`: one compare, and a future right is never inert on an old
   core. (b) The six retired bits only, so that reserved high bits stay storable. **Recommended:
   (a)**. A new error identity is not needed; `type_mismatch` already covers DENY under ALLOW-only.
4. **Create-versus-remove on a creator endpoint.** (a) Accept that WRITE does both. A deployment
   that needs create-only refuses `NAME` from non-owners in the endpoint's admission filter, which
   sees the type and the subject. (b) Keep the payload-right table with only READ and WRITE in it.
   That is useless. (c) Split create and remove into two vertices, which is a wire change.
   **Recommended: (a)**. It deletes the table and keeps the hot-path saving.
5. **The initial ACL of a remotely created child.** (a) Inherit only. The core stamps nothing, and
   a hook may install a creator ACE as the owner. This costs no RAM and stores no identity. (b) The
   core stamps `{creator, READ|WRITE}` on every created child, which costs an ACE, a subject copy
   and an ACL-cache invalidation per creation. **Recommended: (a)**.
6. **The lost poll-only grant.** (a) Accept it. Edge cost is bounded by the `retained` seam and
   refused as `backpressure`. (b) Add an owner-declared "no remote subscribers" vertex policy bit
   later, if a deployment asks for one. That is a compile-time or registration-time declaration, not
   an ACL right. **Recommended: (a) now, with (b) filed only on demand.**
7. **A standard admin vertex type.** (a) None for now. Reference/13 gains an informative recipe for
   an owner-app vertex that applies ACEs. (b) Specify a typed admin vertex now, as RFC-0034 does
   for the selector, so that two management tools interoperate. **Recommended: (a)**, and an RFC
   for (b) once a second tool exists.

## 13. Implementation (follows acceptance)

1. **Core.** Apply the gates of §4, the install refusal of §3.1, the deletion of the payload-right
   table, and the holder test. Rewrite `acl_right_bits` and the affected tests. Add a CHANGELOG
   entry with the upgrade table. Build the CI matrix, including `acl_full` ON and OFF, and record
   the ESP32 size delta.
2. **Vectors and Rust.** Repair `acl/ace-duplicate-key` and add `acl/ace-retired-bit`, both bound by
   `core/tests/acl_test.cpp` and `bindings/rust/tests/conformance_vectors.rs`. Change the Rust
   reader and its CHANGELOG.
3. **Docs.** Reference/05 §`0x0A` (the registry, the enforcement paragraph and the upgrade note),
   reference/02 (write-creates and the `[N]` table), reference/13 (formation and the admin recipe),
   reference/18 and reference/19 (the CREATE gate), CONTEXT.md, ADR-0020's status key, a forward
   note on RFC-0014 Amendment 2, and the ESP-IDF README sentence.
4. **RFC-0035** is revised per §8 before it is accepted.

## Discussion

The maintainer's direction of 2026-10-10 is "two rights, and `:` fields are paths under their
vertex". This document turns it into normative text, maps it onto every gate in the reference core,
and lists what is still open in §12.
