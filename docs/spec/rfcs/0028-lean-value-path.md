<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0028 — The lean value path: one block per publish, copy-or-share by size, retention per vertex, sync as a trait

| Field | Value |
| ---- | ---- |
| **RFC** | 0028 |
| **Title** | The lean value path: one block per publish, copy-or-share by size, retention per vertex, sync as a trait |
| **Status** | **draft** (2026-09-28), **reviewed 2026-09-29** on [PR #1627](https://github.com/avatarsd-llc/libtracer/pull/1627) against `main` 641eff22: direction accepted, the five §11 questions ruled (recorded in §11), and the review's seven text/scope corrections applied in this revision; **acceptance is pending merge**. Every number in §2 and §7 is measured on the host build at the commit this RFC was drafted against and is reproducible with `bench_lean_value_path` (§7.3). |
| **Author(s)** | AvatarSD (maintainer), with AI drafting |
| **Created** | 2026-09-28 |
| **Comment window** | waived by default while solo-maintained ([GOVERNANCE.md](../../../.github/GOVERNANCE.md) §"Errata, amendments, and the comment window"); invoke explicitly if outside input is wanted. |
| **Instrument** | **Implementation RFC.** No wire surface moves (§8.1): every frame that is valid today is valid after every slice, byte for byte. The public C++ API of `core/` (implementation-defined per [ADR-0013](../../adr/0013-v1-scope-boundaries.md)) changes in the ways §8.2 lists; each old name is **deleted outright in the same bump** — no shims, deprecation aliases or transition releases (§11 ruling 4 as amended). |
| **Tracking issue** | [#1624](https://github.com/avatarsd-llc/libtracer/issues/1624) (value-path design). Absorbs [#1618](https://github.com/avatarsd-llc/libtracer/issues/1618), [#1619](https://github.com/avatarsd-llc/libtracer/issues/1619), [#1620](https://github.com/avatarsd-llc/libtracer/issues/1620), [#1621](https://github.com/avatarsd-llc/libtracer/issues/1621), [#1622](https://github.com/avatarsd-llc/libtracer/issues/1622), [#1623](https://github.com/avatarsd-llc/libtracer/issues/1623), [#1625](https://github.com/avatarsd-llc/libtracer/issues/1625), [#1626](https://github.com/avatarsd-llc/libtracer/issues/1626) — each is closed by a named slice in §6, except #1623, which slice 6 answers only in its retention half: its persistence ask is tooling ([#58](https://github.com/avatarsd-llc/libtracer/issues/58), CONTEXT.md §network formation) and the `enabled` bit with its `bound` read-back is [#1533](https://github.com/avatarsd-llc/libtracer/issues/1533)'s own car. |
| **Target spec version** | none. `docs/spec/v1.md` is untouched; this RFC is about what the reference implementation allocates, copies and locks, not about what it puts on the wire. |
| **Scope** | **v-NEXT.** Gates no release. Slices 1 (§6.1) and 2 (§6.2) landed ahead of acceptance as ordinary fixes per §11 ruling 5 ([PR #1628](https://github.com/avatarsd-llc/libtracer/pull/1628), [PR #1629](https://github.com/avatarsd-llc/libtracer/pull/1629)). |
| **Amends / extends** | [RFC-0022](0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.D (the pin RATIO becomes an absolute SIZE threshold, §4.3 — the amendment RFC-0022 Amendment 2 itself measured the case for); [RFC-0025](0025-stream-class-values.md) §4.6.1 (the receiver ring becomes the `N` arm of one retention policy, §4.4; the byte charge is unchanged); [ADR-0060](../../adr/0060-lkv-copy-store-injected-value-backend.md) §1 (its stated, unimplemented end-state — "one allocation per write packing wrapper + value into one segment, retiring `mr_`" — is §4.1 of this RFC); [ADR-0064](../../adr/0064-lkv-publish-is-waiterless-and-the-slot-becomes-lock-free.md), [ADR-0069](../../adr/0069-lkv-slot-is-a-compile-time-policy-hazard-reclamation.md), [ADR-0080](../../adr/0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md) (the slot and reclamation policies stay traits; §4.5 adds the single-writer one and deletes the default that hangs); [ADR-0079](../../adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md) Decision 3 (the throwing `std::pmr` channel is removed from the LKV path, as it ruled). |

> **Numbering note.** 0012 was closed unmerged and 0015 was withdrawn; neither gap is reusable
> (see [RFC-0016](0016-composed-branch-read.md) §ghost history). 0028 is the next unused number.

---

## 1. Summary

A value that enters a node today — over the wire or from a local producer — is **allocated three
to five times and copied once or twice before it leaves**, and a publish to `K` target
subscribers allocates `K + 1` times. That is the measured shape of the path (§2.1), and it is
not what the library promises: `README.md` says "fan-out to N subscribers is N refcount bumps",
which is true of the callback leg and false of the target leg, where each delivery re-wraps the
same rope in a fresh `std::shared_ptr` control block.

The mechanisms behind that shape were each justified when added, and this RFC walks each one
back to the requirement it serves (§3), deletes what no requirement holds up (§4), and states
the minimal model that remains (§5): **one intrusive refcounted block per publish**, adopted
by every stage — ingress, LKV, fan-out, egress, local publish — with one **copy-or-share
policy** per vertex that keys on the value's **absolute size** (small values are copied, large
values are shared), one **retention policy** per vertex (`none` / `last` / `N`, the same enum
for app fields, so a write-only field retains no bytes), and the **lock and reader-protection
primitives as config traits**, so a single-writer single-core build compiles the LKV publish
to an atomic pointer exchange and the read to an interrupt-masked window — no OS mutex, no
spin, no `sched_yield`, which is the hang class of #1618 removed by construction rather than
by a guard.

The core slice is prototyped and measured (§7): a publish of a 4 KB value to 32 targets goes
from **33 allocations / 3,432 B of heap** to **1 allocation / 4,104 B**, and the large-value
ingress from a copy to a share. The plan is ten slices (§6), each shippable alone, ordered by
payoff over risk, with the #1618 hang first.

## 2. Motivation

### 2.1 What one publish costs today, measured

`bench_lean_value_path` (§7.3) counts every `operator new` and every byte it hands out over a
warm window of 1,000 publishes on the host build (`g++` 13.3, Release, x86-64). The per-publish
averages, for a value of 16 B, 4 KB and 64 KB:

| stage | what is measured | allocs | heap bytes | payload copies |
| --- | --- | ---: | ---: | ---: |
| `local-cb` | `graph_t::write`, STORED_VALUE, K callback subscribers, value pre-built | 1 | 104 | 0 |
| `local-target` K=1 | the same write, one TARGET subscriber | 2 | 208 | 0 |
| `local-target` K=8 | eight targets | 9 | 936 | 0 |
| `local-target` K=32 | thirty-two targets | 33 | 3,432 | 0 |
| `producer-own` | what a producer pays to hand over bytes it owns (`view::over_bytes`) | 2 | 40 + size | 1 |
| `egress-gather` | `transport_t::send(iov)` via the base gather every non-SG link inherits | 1 | size | 1 |
| `ingress-copy` | FWD{WRITE} through the terminus, default `kPinPayloadRatio` (never pin) | 5 | 229 + size | 1 |
| `ingress-pin` | the same frame with the per-vertex pin ratio engaged | 3 | 185 | 0 |

The numbers are size-independent except where the payload itself is being copied. Read
across the columns:

- **The LKV publish is one 104 B block, always** — `std::make_shared<const rope_t>` packing a
  16 B control block with an 80 B `rope_t` (ADR-0064 measured the same block at the same
  size). That block is the *only* cost of the callback leg at any K, which is the README's
  promise kept.
- **The target leg breaks the promise.** `dispatch_edge_target` clones the rope's links
  (`try_clone_rope`, a refcount per link) and then calls `store_value` on the target, which
  calls `vertex_t::store` → `try_make_lkv` → a *fresh* `make_shared` per target
  (as of `3fc129f2`, before slice 4 deleted the pair: `core/src/graph.cpp` `dispatch_edge_target`
  and `vertex_t::store` / `try_make_lkv` in `core/include/libtracer/vertex.hpp`). K targets
  ⇒ K + 1 blocks holding K + 1 copies of the same 80 B rope header around one shared payload.
- **A locally produced value costs two blocks and one copy before the write even starts**:
  `heap_alloc` allocates the 40 B `segment_t` header and the payload block separately, then
  `over_bytes` copies into it. With the LKV block that is three allocations per publish of a
  fresh value.
- **Ingress copies the payload by default** — the ADR-0041 §2 ownership copy — and the pin
  arm that avoids it saves two blocks and the copy. RFC-0022 Amendment 2 measured that the
  benefit tracks the payload's **absolute size** (indistinguishable at ≤ 1 KB, 1.2–2× faster
  at ≥ 4 KB on the host), yet the knob that selects it is a **ratio**
  (`payload * k >= segment`, `core/src/op_resolve_walk.hpp:512-527`) whose per-vertex override
  exists, by its own comment, "ONLY so RFC-0022 §6's arms rotate inside ONE process" (#774).
- **Egress copies the frame once per link** that does not override scatter-gather (the base
  `send(iov)` default, `transport.hpp:492-525`), and the ESP-IDF server link — which does
  override it — still gathers **once per peer** into a queued tx slot
  (`integrations/esp-idf/libtracer/httpd_ws_link.cpp:3736-3760`, the memcpy loop at `:3050`),
  so a 4 KB value fanned to 8 browser sessions is copied 8 times on the way out.

### 2.2 The nine findings this RFC answers

Each is an upstream issue; each is closed by a slice named in §6.

| # | finding | issue |
| --- | --- | --- |
| L1 | The default LKV slot is `std::atomic<std::shared_ptr<T>>`, which libstdc++ implements with a spin lock that `sched_yield`s. On a priority-preemptive single core a low-priority reader holding the spin word is never scheduled again while the writer yields to itself: a hang. `kSpinWaitSafe` exists (`config.hpp`) but is asserted only by `mem_pool.hpp:199`; the slot is unguarded. | [#1618](https://github.com/avatarsd-llc/libtracer/issues/1618) |
| L2 | A link holds its write mutex across a blocking transport write (`esp_ws_client_link.cpp:665`, bounded by `kWriteBudgetMs` = watchdog/4). Delivery is in-call, so the publisher stalls on the slowest peer. | [#1619](https://github.com/avatarsd-llc/libtracer/issues/1619) |
| L3 | K target subscribers ⇒ K + 1 allocations per publish (§2.1). | [#1620](https://github.com/avatarsd-llc/libtracer/issues/1620) |
| L4 | Egress gathers a copy per peer (§2.1, last bullet). | [#1620](https://github.com/avatarsd-llc/libtracer/issues/1620) |
| L5 | `vertex_t::write_seq_` is a `std::atomic<uint64_t>` bumped `seq_cst` on every publish; on a 32-bit target without 64-bit AMO that is a libatomic lock per publish. | [#1621](https://github.com/avatarsd-llc/libtracer/issues/1621) |
| L6 | `subscriber_remote_t` (120 B) holds the link and caller identities as two `std::string`s per remote subscriber, though the graph already interns links (`graph_t::intern_link`). | [#1622](https://github.com/avatarsd-llc/libtracer/issues/1622) |
| L7 | A subscription's RFC-0022 §3.A latch and its enabled state are two bits consumed out of a 16-bit `delivery_policy_t`, latched through a separate admission path. | [#1623](https://github.com/avatarsd-llc/libtracer/issues/1623) (retention half only — slice 6; the `enabled` bit + `bound` read-back is [#1533](https://github.com/avatarsd-llc/libtracer/issues/1533); persistence is tooling, [#58](https://github.com/avatarsd-llc/libtracer/issues/58)) |
| L8 | Reply assembly emits through `std::vector<std::byte>&` emitters (`tlv_emit.hpp`), so a reply is built by copy and then gathered again at the link. | [#1620](https://github.com/avatarsd-llc/libtracer/issues/1620) |
| L9 | `lkv_slot.hpp:66-71` documents a `map_mutex_` protection that the read path no longer takes — the prose and the code disagree about which lock protects the slot (`graph.cpp:1998` reads the slot with no lock). Corrected in slice 1, which rewrites that header. | [#1624](https://github.com/avatarsd-llc/libtracer/issues/1624) |
| — | A forwarded read has no deadline: a stale peer holds the forwarder's reply slot indefinitely. | [#1625](https://github.com/avatarsd-llc/libtracer/issues/1625) |
| — | Ingress cannot loan: the transport allocates its receive block without room for the record the graph will need, so sharing a large frame still allocates a record. | [#1626](https://github.com/avatarsd-llc/libtracer/issues/1626) |

### 2.3 Why now, and why as one RFC

Every one of these has a local fix. Taken one at a time they would add a fourth allocation
seam, a third callback idiom and a second retention knob, because each local fix is easiest to
express in the vocabulary that caused it. This RFC exists so the fixes share one vocabulary
(§5) and so each slice deletes something (§4) rather than adding a guard beside it.

## 3. Step 1 — make each requirement less dumb

The method is the one every mechanism in this path has to survive: name the requirement, name
**who owns it** (which deployment, which document, which promise), and ask whether the
requirement itself is right before asking whether the mechanism serving it is. A requirement
with no owner is deleted with its mechanism.

| # | requirement as currently served | owner | verdict |
| --- | --- | --- | --- |
| R1 | A reader on any thread may load the current value while a writer publishes, without a lock. | Host many-core deployments (ADR-0069 measured 4.2× at T=24). | **Keep as a trait.** On a single-writer single-core build it is not a requirement at all — there is no second core to race — and serving it there is what costs the spin lock (L1). |
| R2 | A publish must never abort the node on heap exhaustion; refusal is by value. | The embedded target (#477, ADR-0065). | **Keep.** But one failable seam serves it, not three (§4.9). |
| R3 | Fan-out to N subscribers is N refcount bumps. | The library's own README. | **Keep, and make it true** (L3). |
| R4 | A STREAM receiver keeps a byte-bounded ring of the last N values, charged to its own source. | The consumer (RFC-0025 §4.6.1). | **Keep the mechanism, rename the requirement**: it is the `N` arm of *retention*, not a role. |
| R5 | `set_history_depth(v, keep)` declares how many the ring keeps. | The application. | **Fold into R4.** Same knob as R4 under a different name. |
| R6 | A per-vertex ratio `k` decides whether ingress pins the frame or copies the payload. | The RFC-0022 §6 measurement rig, by its own comment (#774). | **Delete the ratio.** The measured variable is absolute size (RFC-0022 Amendment 2). Replace with one size threshold per vertex that governs every stage (§4.3). |
| R7 | The LKV wrapper may be allocated from an injected `std::pmr::memory_resource`. | Historical (ADR-0039); ADR-0079 Decision 3 already ruled the throwing channel off hot paths. | **Delete** (§4.1). |
| R8 | Hazard-pointer / QSBR / deferred-release reclamation for the read path. | Host many-core (ADR-0069, ADR-0080). | **Keep as traits;** never selected on the single-writer build. |
| R9 | A `wo` app field is write-only. | The application (RFC-0010 §A.3). | **Make it true:** today `app_field_store` stores the bytes and only `app_field_get` refuses (`vertex.hpp:2351-2383`). Retention `none` (§4.4). |
| R10 | A publisher must never wait on a peer. | Every deployment; it is what "delivery is in-call" costs otherwise. | **Keep; not served today** (L2, #1625). No lock across a blocking write; a deadline on every forwarded op (§4.7). |
| R11 | `await` needs a monotonically increasing per-vertex sequence to detect a change. | `await`'s predicate. | **Keep the requirement, shrink the mechanism:** a 32-bit sequence with wrap-safe compare serves it; 64 bits served nothing but the fear of wrap, at a libatomic lock per publish on rv32 (§4.6). |
| R12 | A remote edge must know which link to deliver on and which subject subscribed. | The router. | **Keep the requirement;** serve it with the interned link index the graph already keeps, not two strings per edge (§4.8). |
| R13 | Callbacks are `std::function` in `handlers_t` and `{fn, ctx}` in `subscriber_t`. | Nobody: ADR-0047's amendment kept `std::function` "pending measurement". | **One idiom** (§4.10). |
| R14 | `read` returns `value_ref_t`; field `read` returns `rope_t`; `history` returns `std::vector<rope_t>`. | Nobody. | **One owning read type** (§4.11). |
| R15 | Every wiring knob is a `graph_t` verb (a fresh count of `graph_t`'s public members on `main` gives ~103; the final figure is counted at slice 10's gate, §6.10). | Accretion. | **Fold** the per-vertex ones into a policy set at registration and the graph-wide hooks into a construction-time struct (§4.12). |
| R16 | An unsubscribe may run re-entrantly inside a delivery. | Every deployment (ADR-0080). | **Keep.** Unchanged by this RFC. |
| R17 | Delivery to a target terminates there (never re-dispatches). | ADR-0051 / RFC-0007. | **Keep.** Unchanged. |

## 4. Step 2 — the deletion list

The rule applied: if nothing had to be added back, not enough was deleted. §4.13 counts what
was added back.

Each row: what goes, why it can, and what a caller does instead.

### 4.1 D1 — the `std::shared_ptr<const rope_t>` LKV wrapper, `try_make_lkv`, the `mr_` pmr channel

**What.** `vertex_t::try_make_lkv` (`vertex.hpp:2639`), the `std::pmr::memory_resource* mr`
parameter of `vertex_t::store`, `graph_t::mr_`, the `allocate_shared` arm, and the
`kCtrlSlack` probe.

**Why.** ADR-0060 §1 already names the end-state: one block packing the refcount and the value.
The wrapper costs 104 B per publish (16 B control block + 80 B `rope_t`, of which 48 B are two
inline `view_t`s that a single-link value leaves half empty). The pmr arm is a throwing seam
ADR-0079 Decision 3 ruled off the hot path, and `try_make_lkv`'s `probe_bytes` exists only to
guess whether that seam will throw.

**Instead.** `value_t` (§5.1): one block from the vertex's `block_source_t`, refcount inline,
sized to the value. `value_ref_t` becomes the one owning handle over it. Direct callers of
`vertex_t::store(rope_t, mr)` pass no resource.

**`mr_`'s live users, and their replacement.** `mr_` is not only the value path's channel;
six pmr-typed control-plane containers on `graph_t` draw through it today and go with it:
`link_index_` (`std::pmr::vector<link_slot_t>`), `link_free_` (`std::pmr::vector<uint32_t>`),
`link_long_names_` (`std::pmr::vector<link_long_name_t>`), the per-slot candidate list
`link_slot_t::vs` (`std::pmr::vector<vertex_t*>`), the long-name spelling
`link_long_name_t::text` (`std::pmr::string`), and the `link_candidates` return type
(`std::pmr::vector<vertex_t*>`) — `core/include/libtracer/graph.hpp:2629`, `:3123`, `:3177`,
`:3200-3205`. Each becomes a `mem::block_array_t<T>` drawn from the injected `block_source_t`
directly (the nothrow, sized-release array `iov_table.hpp` already uses for its overflow block):
a refused growth is a `nullptr` the caller reports by value, never a `std::bad_alloc`, and
`link_candidates` fills a caller-owned span. The same carrier takes #1608's structural mints
(D9, §6.3a–e), so `src_mr_`, `source_resource_t`'s translation of refusal into a throw, and
`std::pmr::get_default_resource()` as a fallback all leave the graph together. `retired_seams_`
and the `pending_`/`unconditional_` key sets (`std::vector` / `std::set` on the global heap
today, #1608 tier 1) move onto the same carrier in §6.3d.

### 4.2 D2 — the per-target re-wrap (L3)

**What.** The `try_clone_rope` + `store_value(target, …)` pair in `dispatch_edge_target`
(as of `3fc129f2`; removed by slice 4, [PR #1642](https://github.com/avatarsd-llc/libtracer/pull/1642)), as a *value-producing* step.

**Why.** The target's LKV must hold the same bytes the source published; it does not need its
own wrapper around them. With D1 the source's block already carries the refcount.

**Instead.** The target slot **adopts** the source's block: one `retain`, one exchange. The
admission filter, the ACL gate and the ring admission of the target still run — on the shared
block, not on a copy. K targets ⇒ K refcount bumps (R3, kept true).

### 4.3 D3 — the pin ratio: `set_pin_payload_ratio`, `kPinPayloadRatio`, `vertex_ext_t::pin_payload_ratio`, `own_or_ref_tlv`'s `payload * k >= segment` test

**What.** All four, and `kPinNever`.

**Why.** R6. The measured variable is absolute payload size; the ratio was the measurement
rig's rotation knob (#774).

**Instead.** One per-vertex **`share_threshold_bytes`** (§5.3): a value **at or above** it is
*shared* — ingress retains the receive block, a local producer's loan is published in place,
egress emits it as an iov entry, a target adopts it; a value **below** it is *copied* into the
vertex's own block, at every one of those stages alike. The default is
`config_t::kShareThresholdBytes`; RFC-0022 Amendment 2's host knee (≈ 4 KB) is the host
default, and a pool-backed embedded target sets it per pool (the retention hazard RFC-0022
Amendment 2 records — a shared receive segment holds its pool slot until the LKV lets go — is
exactly what the threshold is for, and it is the consumer's number to set).

### 4.4 D4 — `set_history_depth`, `history_keep_last`, and the role-encoded retention

**What.** The verb and the field; the implicit rule "STORED keeps last, STREAM keeps a ring,
HANDLER keeps nothing"; the separate `app_field_table_t::values` lazily-allocated
vector-of-vectors that stores `wo` fields.

**Why.** R4/R5/R9. Three spellings of one property.

**Instead.** `retention_t { NONE, LAST, N }` per vertex **and per app field** (§5.4). A
HANDLER is `NONE`; a STORED_VALUE is `LAST`; a STREAM is `N` with the RFC-0025 byte charge
unchanged; a `wo` field is `NONE` and retains no bytes — `app_field_store` on a `wo` field
delivers to `on_app_field_write` and stores nothing. `set_history_depth(v, n)` is deleted
outright in the same bump; a caller writes `set_policy(v, {.retention = retention_t::N,
.depth = n})` (§4.12). No forwarding shim is kept (§11 ruling 4 as amended).

### 4.5 D5 — `sp_atomic_slot_t` as a shipped policy (L1)

**What.** The type (`lkv_slot.hpp`), and its selection as `default_config_t::lkv_slot_t`.

**Why.** It is the only slot policy that can spin, and the one build where spinning is a hang
is the one it defaults to. `hazard_slot_t` already serves a host whose reads of one shared
vertex contend across many cores. What is missing is the policy for a single-writer build,
not a guard on this one.

**Instead.** `single_writer_slot_t` (§5.5): a `value_t*` swapped **inside** `config_t::reader_guard_t`
on publish, with the displaced value's `release` run after the guard closes; the read is
`load` + `retain` inside the same guard — a trait that is an
interrupt-masked critical section on the single-core RTOS build (`tr::esp::critical_guard_t`),
an address-striped one-word lock whose contender sleeps on the host (`mutex_guard_t`), and
nothing at all on a build that selects the hazard slot. **As landed in slice 1 (PR #1628)**
`default_config_t::lkv_slot_t` is `single_writer_slot_t` over `mutex_guard_t` on *every*
target, the host included — the blocking perf gate refused `hazard_slot_t` as the host default
(§5.5) — and `hazard_slot_t` is the host opt-in (`LIBTRACER_LKV_SLOT=hazard_slot_t`).
`sp_atomic_slot_t` was deleted outright, not kept for a release. A `static_assert` refuses any spinning
policy when `!kSpinWaitSafe`, mirroring `mem_pool.hpp:199` — the guard that should have
covered the slot from the start covers every slot policy, because there is nothing left for it
to guard.

### 4.6 D6 — the 64-bit write sequence (L5)

**What.** `vertex_t::write_seq_` as `std::atomic<std::uint64_t>`; `current_seq()` returning
64 bits.

**Why.** R11. `await` compares for inequality against the sequence it captured; a 32-bit
counter that wraps after 4 × 10⁹ publishes is a change `await` misses only if exactly 2³²
publishes land between its capture and its check, which on the target that cares (a 1 kHz
publisher) is 49 days of silence followed by a spurious timeout, not a lost value.

**Instead.** `std::atomic<std::uint32_t>`, `seq_cst` as today (the lost-wakeup argument in
`vertex_t::store` is unchanged). Saves 4 B per vertex on rv32 and the libatomic lock per
publish.

### 4.7 D7 — the lock across the blocking write, and the unbounded forwarded op (L2, #1625)

**What.** `lock_guard(write_m_)` around `esp_transport_write` in the ESP-IDF client link;
`write_m_` around `::write` in `posix_endpoint.hpp:578`; the forwarder's reply slot held
without a deadline.

**Why.** R10.

**Instead.** A link's `send` **enqueues** under its lock and **writes** outside it, or
`try_lock`s and drops-and-counts when a write is in flight (the option #1619 costs at RAM 0).
Every forwarded op carries `config_t::kForwardDeadline`; a reply slot past its deadline is
released and the op answers `tr::flow::timeout`. Neither changes a byte on the wire.

### 4.8 D8 — the two strings per remote subscriber (L6)

**What.** `subscriber_remote_t::link` and `::caller` as `std::string`.

**Why.** R12. The graph interns links (`intern_link`, `link_index_name_lookups`); a subject is
a link plus an optional per-writer suffix the terminus derives once at subscribe.

**Instead.** A `link_id_t` (the interned index) and a `subject_id_t` interned in the same
table. `remote_delivery_t` carries the ids; the router resolves them on its side, where the
strings already live. 120 B → 56 B per remote subscriber on the host (two 32 B strings become
two 4 B ids); the two `view_t` routes stay (they are refcounts, not copies).

### 4.9 D9 — three allocation seams → one

**What.** `std::pmr` on the value path (gone with D1); `mem_backend_t` as a *separate* vtable
from `block_source_t`.

**Why.** ADR-0079 §Context counted four channels for one concern. After D1 there are two:
`block_source_t` (raw failable block) and `mem_backend_t` (refcounted segment with space and
I/O hooks). The second is the first plus three virtuals.

**Instead.** `mem_backend_t` **derives from** `block_source_t`; `segment_alloc` draws its
bytes through `try_alloc` and places the `segment_t` header **in the same block** (one
allocation where `heap_alloc` makes two today — the `producer-own` row of §2.1). `space()`,
`before_io`/`after_io` stay on the derived type for device memory (ADR-0024). Every injection
point takes a `block_source_t&`; a deployer that injects one slab has one slab.

**The structural mints draw from the same source.** #1608 measured that every
registration- and subscribe-time byte escapes the injected source today (+0 B on the seam
whatever is injected): `vertex_t`, `vertex_ext_t`, `value_handlers_t`, `edge_block_t`, the
path-key spill and the payload-right nodes all land on the global heap. With `mem_backend_t`
deriving from `block_source_t`, those mints draw from the vertex's source directly — placement
into `try_alloc`, sized `release` (`len` is on the block, so no pool needs a header) — in five
ratchet-gated sub-slices after slice 3 (§6.3a–e). A registration the source refuses is
reported **by value** on `try_register_vertex` (`result_t<vertex_handle_t>`, already the
failable spelling; `register_vertex` keeps its contract-violation meaning). The two constructor
roots — the root vertex and the graph's own bookkeeping allocated before any source is bound —
stay on the global heap, documented at the constructor. This is what makes "an injected source
bounds the node" a true sentence rather than a hot-path one.

### 4.10 D10 — the second callback idiom (R13)

**What.** `std::function` in `handlers_t` (6), `value_handlers_t` (3), `app_field_group_t`
(1), the admission node, and the `graph_t::configure_*` hooks — 29 `std::function` occurrences
across `graph.hpp` and `vertex.hpp`, at 32 B each on the host.

**Why.** ADR-0047 chose `{fn, ctx}` for the hot edge and left the rest "pending
measurement". `handlers_t` measures 216 B; a HANDLER vertex carries a `value_handlers_t` of
96 B. The `{fn, ctx}` slot the edge uses is 16 B.

**Instead.** Every hook is a `{fn, ctx}` slot (`receiver_slot_t`'s shape). A caller that has a
lambda with captures passes `&lambda` as `ctx` and a thunk as `fn`; a small header helper
(`tr::graph::thunk<F>`) writes the thunk. `handlers_t` → 96 B; `value_handlers_t` → 48 B.

### 4.11 D11 — three read-result types (R14)

**What.** `result_t<rope_t>` from field `read`; `result_t<std::vector<rope_t>>` from
`history` (which copies every retained rope's links into a fresh vector per call).

**Instead.** `value_ref_t` everywhere a value is read; `history(v, std::span<value_ref_t> out)`
fills a caller-owned span and returns the count. No allocation on any read.

### 4.12 D12 — the `graph_t` wiring verbs (R15)

**What.** Per-vertex: `set_history_depth`, `set_pin_payload_ratio`, `set_ring_source`,
`set_delivery_mode`, `set_vertex_ceiling`, `set_app_fields`, `set_app_fields_static`.
Graph-wide: `configure_remote_delivery_sink`, `configure_stats_sampler`,
`configure_subject_resolver`, `configure_subscription_observer`,
`configure_wire_target_resolver`.

**Instead.** `vertex_policy_t { retention, share_threshold_bytes, ring_source, ring_reliable,
delivery_mode, app_fields }` passed to `register_vertex` (and changeable through one
`set_policy(v, vertex_policy_t)`); `graph_hooks_t` with the five `{fn, ctx}` slots passed to
`graph_t`'s constructor. The public-name count is **re-counted at slice 10's gate** on the
final surface (a fresh count on `main` gives ~103 public members; the drafted "70 → 58" was
unverified and is withdrawn). The value verbs (`read`, `write`, `assign`, `propagate`, `await`,
`subscribe`, `unsubscribe`, `history`) do not move.

### 4.13 What was added back

Four things, against twelve deletions: `share_threshold_bytes` (one knob replacing a ratio and
a rig), `retention_t` (one enum replacing a verb, a field and a role rule), `reader_guard_t` +
`kSingleWriter` (one trait pair replacing a spin lock — and `reader_guard_t` is the pool's
`Sync` policy under one name, §5.5, so it adds a use, not a vocabulary), and `kForwardDeadline` (one constant
replacing an unbounded wait). Each is a *policy* the application states, in place of a
*mechanism* the library guessed.

## 5. Step 3 — the minimal model

### 5.1 `value_t` — one block

```text
value_t                      (from the vertex's block_source_t; one try_alloc per publish)
┌──────────────┬────────┬────────┬───────────┬────────────────────────────────────┐
│ refs u32     │ len u32│ flags  │ source*   │ bytes[len]   — or —  link {seg*,off,len}│
└──────────────┴────────┴────────┴───────────┴────────────────────────────────────┘
  flags: SHARED (payload is the link, not inline) | DEVICE (ADR-0024 space) | PINNED_RX
```

- `refs` is a `std::atomic<uint32_t>`: a 32-bit AMO is native on every target the library
  builds for, including rv32. `release` frees through `source` (sized reclaim: `len` is on the
  block, so no pool needs a header).
- **Below the threshold** the bytes follow the header — the small-value copy is one `memcpy`
  into a block that had to exist anyway.
- **At or above it** the block holds one link to the block the bytes already live in (the
  receive block, the producer's loan, or another vertex's `value_t`) and the payload is never
  moved. With the ingress loan (#1626, §6.9) the receive block *is* a `value_t` and even the
  link disappears.
- A multi-link rope is not a `value_t` shape. A value assembled from several segments is
  either small (copied contiguous) or large (shared as one link to a `rope_t` the transport
  built and owns). `rope_t` stays the **egress** type: `value_t::iov()` yields one span for
  an inline value, and the link's span for a shared one.

**Cost per publish** (host, measured on the prototype, §7.2): **1 allocation of 24 + size
bytes** (16 B header on rv32), independent of K and of stage.

**As landed in slice 3** ([PR #1639](https://github.com/avatarsd-llc/libtracer/pull/1639)) the
block is the **link-chain form only**: a header `{refs u32, n u32, source*}` — **16 B on the
host, 12 B on rv32**, no `flags` word yet — followed by `view_t links[n]` (24 B / 12 B each), so
a one-link publish is **40 B host / 24 B rv32** and a multi-link rope IS a `value_t` shape (its
links, verbatim). This corrects the layout above for the shipped slice because `view_t` has no
owner-less byte form, so there is nothing for an inline arm to be a view *over* until slice 5
(D3) adds the copy-or-share decision; the "24 + size" figure and the `bytes[len]` arm are that
slice's prototype, not slice 3's.

**As landed in slice 5** ([PR #1643](https://github.com/avatarsd-llc/libtracer/pull/1643)) the
inline arm costs **80 + size on the host and 44 + size on rv32**, not 24 + size / 16 + size.
Every stored byte range needs an owning segment, so the inline block embeds one next to the
header. The prototype had no owner and could not be read through a `view_t`, so its figure
was a lower bound the shipped shape cannot reach.

### 5.2 The path

```text
ingress  transport receives into a block from ITS source ─► terminus resolves ─► WRITE
           size < threshold : value_t{inline}  ← memcpy payload      (1 alloc, 1 copy)
           size ≥ threshold : value_t{link → rx block}               (1 alloc, 0 copies)
                              with ingress loan: rx block IS value_t (0 alloc, 0 copies)
local    loan(v, n) ─► producer fills span in place ─► publish(v)   (1 alloc, 0 copies)
LKV      slot.exchange(new) ; release(old)                           (0 alloc)
fan-out  callback : fn(ctx, const value_t&)                          (0 alloc, 0 copies)
         target   : retain ; target.slot.exchange                    (0 alloc, 0 copies)
         remote   : header → stack buffer ; iov{header, value.iov()} (0 alloc, 0 copies)
egress   link.send(iov)  — native SG writes it; a queued link retains the value_t
                           and releases it after the write            (0 copies)
retain   LAST : the slot holds one ref ; N : the ring holds N refs ; NONE : released after fan-out
```

The route header of a remote delivery (`FWD{WRITE, dst, src}`) is bounded by
`kMaxPathRefElements` and is emitted into a stack buffer through the **span form** of the
emitters (`tlv_emit.hpp`'s `store_header` shape, generalised to `emit_tlv_into`); the vector
form survives for tests and cold paths.

### 5.3 The copy-or-share policy

One number per vertex, `share_threshold_bytes`, default `config_t::kShareThresholdBytes`:

| | below | at or above |
| --- | --- | --- |
| ingress | copy payload into `value_t` | link the receive block |
| local publish | `publish(v, span)` copies | `loan` / `publish` in place |
| fan-out to a target | adopt (the block is already the value) | adopt |
| egress | iov over the inline bytes | iov over the linked bytes |
| retention hazard | none — the block is the vertex's own | the linked block lives as long as the LKV / ring does: the consumer sizes its pool with this in mind |

`0` means "share always", `SIZE_MAX` means "copy always" — the two arms RFC-0022 §3.D calls
`kPinNever` and unconditional pin, now as edge cases of one line.

### 5.4 The retention policy

`retention_t { NONE, LAST, N }` with a depth for `N`, on `vertex_policy_t` and on each
`app_field_t`:

| role today | retention | what changes |
| --- | --- | --- |
| HANDLER | `NONE` | none — already stores nothing |
| STORED_VALUE | `LAST` | none |
| STREAM | `N` + RFC-0025 byte charge | `set_history_depth` → `set_retention` (slice 6), `set_policy` at slice 10 |
| app field `ro`/`rw` | `LAST` | none |
| app field `wo` | `NONE` | **stores nothing** (today it stores and refuses reads) |

`NONE` on a value vertex is legal and is the pure-relay shape: the value is delivered and
released, and `read` answers `NOT_FOUND`. It is a permitted policy, not a named role (§11
ruling 3).

Retention is the whole of this policy. A subscription's RFC-0022 §3.A **latch** (the bit the
draft called "durability") and its `enabled` bit are edge state, not vertex retention, and are
out of this RFC's scope: the `enabled` bit and the `bound` read-back land as #1533's own car,
and persistence across restarts is tooling (#58), not a bit in a policy word — a bit neither
persists nor re-arms anything.

### 5.5 Sync as traits

```cpp
struct default_config_t {
    static constexpr bool kSingleWriter = false;  // one publisher thread per vertex, by contract (per-build trait, §11 ruling 1)
    static constexpr bool kSpinWaitSafe = true;   // unchanged; now asserted by every slot policy (landed, PR #1628)
    using lkv_slot_t     = single_writer_slot_t;  // the default on EVERY target (landed); hazard_slot_t is the host opt-in
    using reader_guard_t = mutex_guard_t;         // host: address-striped one-word locks whose contender sleeps (landed);
                                                  // tr::esp::critical_guard_t on the ESP-IDF chip targets; no_guard_t single-threaded
    using stripe_lock_t  = std::mutex;            // the await/edge-mutation lock; a scheduler-suspend
                                                  // guard, or no_lock_t, on a single-writer build
    static constexpr std::chrono::milliseconds kForwardDeadline{250};   // landed, PR #1629
    static constexpr std::size_t kShareThresholdBytes = 4096;          // host default (§11 ruling 2); deployments set per vertex
};
```

The draft named `hazard_slot_t` as the host default. Slice 1 as landed (PR #1628) refuted
that: the blocking perf gate measured `hazard_slot_t` at 112 B `mem:vertex` but 127 B per held
value (a 24 B node per published `shared_ptr`, whichever reclamation scheme — hazard or QSBR —
retires it) and 0.70–0.76× on `mixed/0/6/128`; `single_writer_slot_t` over the striped
`mutex_guard_t` measured 1.03× with no memory allowance. So `single_writer_slot_t` over
`mutex_guard_t` is the shipped host default, `hazard_slot_t` is opt-in for a host whose reads
of one shared vertex contend across many cores, and the per-value node goes away only with
slice 3's intrusive `value_t`, at which point the hazard default is re-measured, not assumed.

`single_writer_slot_t::store` takes the guard, swaps the one-word `value_t*` inside it, and runs
the displaced value's `release` after the guard closes; `::load` is
`reader_guard_t g; p = slot.load(acquire); retain(p);`. **The writer takes the guard too, on
every build, `kSingleWriter` included** — as landed in slice 3, correcting the draft's "one
exchange on publish": a reader's *load-then-retain* is two steps, and a writer that swaps and
releases between them frees a block the reader is about to retain, however few words the slot
is, so the guard exists to keep the writer's `release` out of that window, not to serialize
writers. Under `kSingleWriter` a debug build asserts the publishing thread's identity on the
vertex (not landed; deferred past slice 3). No slot policy may spin when `!kSpinWaitSafe`; the assertion is on the policy
(`S::may_spin`, landed), so a future policy cannot forget it.

**One Sync vocabulary, not two.** `reader_guard_t` and the pool's `Sync` policy
(`mem_pool.hpp`'s `pool_sync_policy` concept: `lock()`/`unlock()`, `is_isr_safe`,
`is_nonblocking`, `name`) are the same thing — a critical-section type the build binds once
per target: `spin_sync_t` / `mutex_guard_t` on the host, `portmux_sync_t` /
`critical_guard_t` on ESP-IDF. Slice 10 folds them into **one** trait: `reader_guard_t`
satisfies `pool_sync_policy`, the pool takes `config_t::reader_guard_t` as its `Sync` default,
and the two ESP-IDF spellings become one type (Q11 of the deployment-profile design).

The stripe mutex and condvar (`vertex_stripe.hpp`) stay for `await` and edge mutation —
control-plane frequency — but are taken through `stripe_lock_t`, so the single-writer build
substitutes the RTOS's own primitive and pays no pthread mutex.

### 5.6 The contracts as concepts

```cpp
template <class S> concept lkv_slot = requires(S s, value_t* v) {
    { s.store(v) } noexcept -> std::same_as<bool>;
    { s.load() }  noexcept -> std::same_as<value_ref_t>;
    { S::may_spin } -> std::convertible_to<bool>;
};
template <class G> concept reader_guard = std::is_trivially_destructible_v<G> || requires { G{}; };
template <class B> concept block_source = std::derived_from<B, mem::block_source_t>;
```

The static assertion in §5.5 reads `S::may_spin` — a policy declares it, and the config
refuses it when `!kSpinWaitSafe`.

### 5.7 Per-vertex bytes, before → after

| | host today | host after | rv32 today | rv32 after (estimate — measured at slice 3's gate) |
| --- | ---: | ---: | ---: | ---: |
| `vertex_t` | 96 | 88 (measured, slice 3) | 72 | **72** (measured, slice 3 — the draft's 64 was wrong) |
| held value (wrapper + payload header) | 104 + 40 | 40 for one link at slice 3 (16 B header + one 24 B link); 24 is slice 5's inline arm | 52 + 20 | 24 for one link at slice 3 (12 + 12); 16 is slice 5's inline arm |
| `subscriber_remote_t` | 120 | 56 | ~72 | ~32 |
| `handlers_t` | 216 | 96 | ~120 | ~56 |
| `value_handlers_t` | 96 | 48 | ~52 | ~28 |

The `vertex_t` saving is the slot (the 16 B `shared_ptr` that `single_writer_slot_t` holds
since slice 1 — the same width as the `atomic<shared_ptr>` it replaced, so slice 1 left
`vertex_t` at 96 / 72 B and `kMaxVertexBytes64/32` where they were — becomes an 8 B
`value_t*` at slice 3) and the sequence (8 → 4 B, slice 8); the ratchets in `config.hpp`
(`kMaxVertexBytes64/32`) move down with it, never up. Slice 3 moved `kMaxVertexBytes64` 96 → 88
and left `kMaxVertexBytes32` at 72: on rv32 the `shared_ptr` was two 4 B words and the
`value_t*` is one, but the 4 B it frees is absorbed by the struct's 8 B alignment padding, so
the measured size does not move until another 4 B member goes (slice 8's sequence). The
"after" column is reached at slice 8, not slice 1.

## 6. Step 4 — slices

Each slice ships alone, keeps every host test and bench green, and carries its own gate row
in `bench_lean_value_path`. Order is payoff over risk, with the liveness fixes first. Every
slice deletes the old API it replaces in the same bump — no shim, alias or transition release
(§11 ruling 4 as amended).

**Sequencing against the open queue.** The refusal car — [#1612](https://github.com/avatarsd-llc/libtracer/issues/1612)
(static BACKPRESSURE reply), [#1582](https://github.com/avatarsd-llc/libtracer/issues/1582)
(router `flat` sites) and [#1602](https://github.com/avatarsd-llc/libtracer/issues/1602) — lands
**before slice 3**: it is doctrine repair (exhaustion is a value) with no ratchet risk, and
slice 3 rewrites the paths it touches. The link-config aggregate
([#1593](https://github.com/avatarsd-llc/libtracer/issues/1593), absorbing #1606 asks 1–2)
lands **in the same bump as slice 10's** surface fold, so consumers see one breaking release,
not two.

### 6.1 Slice 1 — the slot that cannot spin (L1; closes #1618)

**Landed** as [PR #1628](https://github.com/avatarsd-llc/libtracer/pull/1628) (2026-09-29).
Added `single_writer_slot_t` (`basic_single_writer_slot_t<guard_t>`), `reader_guard_t` with
`mutex_guard_t` (host default) / `no_guard_t` / `tr::esp::critical_guard_t`, `kSingleWriter`,
and the policy-level `may_spin` assertion; **deleted `sp_atomic_slot_t` outright** — no
one-release keep. The default is `single_writer_slot_t` on every target; `hazard_slot_t` is
the host opt-in (see §5.5 for the gate numbers that decided it). **Gate, as run:** the slot
tests under both remaining policies; `lkv_slot_inversion` (`SCHED_FIFO` writer and
higher-priority reader on one CPU — fails on the old default, passes on both remaining
policies); `spin_slot_guard` compile check. **Risk: low**, retired.

### 6.2 Slice 2 — no lock across a blocking write; a deadline on forwarded ops (L2; closes #1619, #1625)

**Landed** as [PR #1629](https://github.com/avatarsd-llc/libtracer/pull/1629). Enqueue-then-write
in the two links that held a mutex across the write; `kForwardDeadline` on the forwarder's
reply slots. **Gate:** a stalled-transport test shows the publishing thread returns in a bound
independent of the link's write budget. **Risk: medium** — the router's reply-slot lifetime;
wire-neutral. Retired.

### 6.3 Slice 3 — `value_t` replaces the wrapper (D1, D9's first half; closes #1624's P1)

**Landed** as [PR #1639](https://github.com/avatarsd-llc/libtracer/pull/1639). One block per
publish (`value.hpp`); `value_ref_t` over it; both slot policies speak `value_t*` — `store`
**adopts** the reference it is handed, `load()` returns a `value_ref_t`; the pmr channel,
`src_mr_` and `try_make_lkv` deleted **on the value path**. Two corrections to the draft, as
built: (1) **`hazard_slot_t` keeps its node indirection** — the slot still holds
`atomic<node_t*>` and the 16 B node owns one reference to the value (`node_t{const value_t* v;
next}`), released when the node is recycled or destroyed — because that is what keeps a hazard
scan that misses a reader, or a `release` racing the retire list, from freeing a block a reader
holds: the value's last owner is always a node or a handle, never "whoever released last", so
the use-after-free class the draft named stays unreachable by construction and the ASan gate is
moot; (2) **the six pmr control-plane containers (`link_index_` and family) did not move** —
they are not on the value path, and their `block_array_t` port is a separate car under §6.3a–e.
**Gate, as run:** `local-cb` 1 allocation, 104 B → **40 B**; `kMaxVertexBytes64` 96 → 88;
`bench_forward_heap` zero-alloc PASS; symbol ratchet PASS (`dispatch_edge` +0, `edge_view_t`
48 B); the slot tests (`lkv_slot_test`, `lkv_slot_inversion`) rewritten for the adopt contract.

#### 6.3a–e — the structural mints (D9's registration half; closes #1608)

Five ratchet-gated sub-slices, each after slice 3 and each alone, moving one registration- or
subscribe-time mint from the global heap to the vertex's `block_source_t` by placement into
`try_alloc` with sized `release`. **Gate, shared:** a counting-source test registers `V`
vertices with `E` edges and asserts the injected source's `blocks` and `bytes` rose by the
mint's exact footprint and the global-heap count by 0; `sizeof` ratchets (`kMaxVertexBytes*`,
`edge_view_t`, `dispatch_edge`'s inline body) unchanged; refusal is returned by value on
`try_register_vertex` / `subscribe`. **Risk: low each** — the types do not change shape, only
where they live.

| sub-slice | mint | refusal surfaces as |
| --- | --- | --- |
| 3a | `vertex_t` | `try_register_vertex` → `BACKPRESSURE` by value; `register_vertex` keeps its contract meaning |
| 3b | `vertex_ext_t`, `value_handlers_t` | the registering call that needed the ext (`set_policy`, handler install) refuses by value |
| 3c | `edge_block_t` (published + parked arrays) | `subscribe` refuses by value, as an exhausted ring does today |
| 3d | path-key spill (`register_vertex_key`'s `std::vector<std::byte>` → `block_array_t<std::byte>`), `retired_seams_`, the `pending_`/`unconditional_` key sets | the key-bearing call refuses by value |
| 3e | payload-right nodes (`payload_right_store_`, each node's `rows`) and the admission nodes | the grant / admission install refuses by value |

The two constructor roots (the root vertex; the graph's own bookkeeping before a source is
bound) stay on the global heap and are documented at the constructor as the residual — the
only bytes an injected source does not bound.

### 6.4 Slice 4 — target adopt (D2, L3; closes #1620's fan-out half)

**Gate:** `local-target` at K=32: 33 → 1 allocation. **Risk: low** once slice 3 is in — the
change is inside `dispatch_edge_target`. Must re-measure the `always_inline` body against the
#1223 cliff.

**As landed** ([PR #1642](https://github.com/avatarsd-llc/libtracer/pull/1642)): 33 → 1 holds for
**stored** targets (1,320 B → 40 B at K=32). A HANDLER target and a value held in
`value_storage_t` still took one copy each; the HANDLER copy is removed by slice 7, which
rewrites the handler callback to receive `const value_t&`. `dispatch_edge_target` shrank
555 → 384 B; the `always_inline` body is unchanged.

### 6.5 Slice 5 — the size threshold (D3; closes #1624's P2)

`share_threshold_bytes` on the vertex; ingress selects by it; `set_pin_payload_ratio` and the
ratio deleted. **Gate:** `ingress-pin` becomes the ≥-threshold row and `ingress-copy` the
<-threshold row of one arm. **Risk: low** — the pin path exists; only the predicate changes.

**As landed** ([PR #1643](https://github.com/avatarsd-llc/libtracer/pull/1643)): the knob ships as
`graph_t::set_share_threshold_bytes(v, bytes)`, a stand-in verb until slice 10 introduces
`vertex_policy_t`. It is stored as a saturating 32-bit word in `vertex_ext_t`, so
`vertex_t` does not grow. The ESP-IDF build default is copy-always (`SIZE_MAX`) until an
on-silicon threshold sweep exists. The stored copy of a remote write now draws from the
graph's `control_source()`, not the router's `flat` backend, because the value's lifetime is
the vertex's.

### 6.6 Slice 6 — retention (D4; closes #1624's P3; answers #1623's retention half only)

`retention_t { NONE, LAST, N }` on `vertex_policy_t` and `app_field_t`; `wo` stores nothing.
**Retention only**: the RFC-0022 §3.A latch and the `enabled` bit stay where they are — the
`enabled` bit and the `bound` read-back land as [#1533](https://github.com/avatarsd-llc/libtracer/issues/1533)'s
own car (it is `ready-for-agent`), and persistence is tooling ([#58](https://github.com/avatarsd-llc/libtracer/issues/58));
this slice does **not** close #1623. `set_history_depth` is deleted in the same bump.
**Gate:** an app-field test that a `wo` write reaches `on_app_field_write` and leaves
`values` unallocated; a `NONE` value vertex answers `read` with `NOT_FOUND` after a delivered
write. **Risk: low.**

**As landed** ([PR #1652](https://github.com/avatarsd-llc/libtracer/pull/1652)):
- Retention ships as `graph_t::set_retention(v, r, depth)`, a stand-in verb until slice 10's
  `vertex_policy_t` (§4.4, §5.4 and the §8.2 table name `set_policy`, which does not exist yet).
- Allowed pairings: STORED_VALUE `NONE`|`LAST`, STREAM `NONE`|`N`, HANDLER `NONE`. An app field
  holds one value, so `N` on a field is treated as `LAST`.
- The ring moved onto the injected block source here (ruling Q12b), which §6.6 and the §6.3a–e
  table omitted: each ring entry lives inside the reservation it was admitted under, the
  `std::deque` is gone, and `kRingEntryOverhead` fell 48 → 40 B. `ring_state_t` stays heap.
- A `NONE` vertex whose only subscribers are synchronous handlers draws **0 blocks per publish**.
  A `NONE` source has no shared block, so each target subscriber of it makes its own copy.

### 6.7 Slice 7 — one callback idiom, one read type (D10, D11)

**Gate:** `sizeof(handlers_t)` ≤ 96 on the host; `history` allocates 0. **Risk: medium** —
API-wide; mechanical.

### 6.8 Slice 8 — interned identities and the 32-bit sequence (D6, D8; closes #1621, #1622)

**Gate:** `sizeof(subscriber_remote_t)` ≤ 56; `vertex_t` ratchets lowered. **Risk: low.**

### 6.9 Slice 9 — scatter-gather egress and the ingress loan (L4, L8; closes #1620's egress half, #1626)

Span-form emitters for the delivery header; queued links retain the `value_t` instead of
gathering; transports allocate receive blocks with a `value_t` header reserve. **Gate:**
`egress-gather` copies 0 through a queued link; `ingress-pin` at 0 allocations; **and, before
this slice lands, a host test with a two-frame race on one socket** — two publishes to the
same WS peer from two threads, the queued send of the first interleaving with the second's
frame — asserting every byte of both frames arrives whole and in order. No bench or test
catches that interleave today (§9 item 6). **Risk: medium-high** — the transports' tx queues
change shape, and a retained-`value_t` send is a partial write away from a corrupted frame.
Adds a retained-send virtual on `transport_t` (§8.2).

### 6.10 Slice 10 — one seam, one surface (D9's second half, D12, §5.6)

`mem_backend_t` derives from `block_source_t` (every downstream backend re-based);
`vertex_policy_t` and `graph_hooks_t`; `reader_guard_t` unified with the pool `Sync` trait
(§5.5); concepts. In the same bump: the `fwd_router_t` constructor takes `router_planes_t` and
the link constructors take their config aggregates (#1593, absorbing #1606 asks 1–2), so the
breaking release is one. **Gate:** `producer-own` 2 → 1 allocation; `graph_t`'s public members
**counted on the final surface at this gate** and recorded in the CHANGELOG (the draft's "≤ 58"
was unverified; `main` holds ~103) — the count is of what ships, since nothing old is kept.
**Risk: medium** — the widest API change, the least behaviour change.

## 7. Measurements

### 7.1 Before (today's tree)

The §2.1 table, in full, from `bench_lean_value_path` at the drafting commit (host: `g++`
13.3, `-O3 -DNDEBUG`, x86-64, 1,000 warm publishes per row; `ns` is indicative, the counting override
sits in this binary):

```text
stage          size   K  allocs   bytes  copies   ns
local-cb         16   1   1.000   104.0    0      80
local-cb         16   8   1.000   104.0    0     126
local-cb         16  32   1.000   104.0    0     326
local-target     16   1   2.000   208.0    0     153
local-target     16   8   9.000   936.0    0     650
local-target     16  32  33.000  3432.0    0    2458
producer-own     16   -   2.000    56.0    1      36
egress-gather    16   1   1.000    16.0    1      17
ingress-copy     16   1   5.000   245.0    1     539
ingress-pin      16   1   3.000   185.0    0     524
local-target   4096  32  33.000  3432.0    0    2450
producer-own   4096   -   2.000  4136.0    1      73
egress-gather  4096   1   1.000  4096.0    1      48
ingress-copy   4096   1   5.000  4325.0    1     589
ingress-pin    4096   1   3.000   185.0    0     513
local-target  65536  32  33.000  3432.0    0    2735
producer-own  65536   -   2.000 65576.0    1     755
egress-gather 65536   1   1.000 65536.0    1     732
ingress-copy  65536   1   5.000 65767.0    1    1637
ingress-pin   65536   1   3.000   185.0    0     942
```

### 7.2 After (the prototype of slices 3 + 4 + 5's local arm)

The `proto-fanout` stage of the same binary: one `{refs, len, bytes}` block, published to a
slot and shared to K target slots by refcount, egress as a one-entry iov:

```text
stage          size   K  allocs   bytes  copies   ns
proto-fanout     16   1   1.000    24.0    0      39
proto-fanout     16   8   1.000    24.0    0     141
proto-fanout     16  32   1.000    24.0    0     493
proto-fanout   4096  32   1.000  4104.0    0     532
proto-fanout  65536  32   1.000 65544.0    0    1223
```

Read against §7.1: at K=32 the publish goes from **33 allocations to 1** and from 3,432 B of
per-publish heap to the value's own block (40 B for a one-link value as landed in slice 3;
the prototype's 24 + size is the lower bound §5.1 corrects); the indicative time from 2,458 ns to 493 ns at
16 B. The prototype's `ns` includes the producer's `memset` of the payload (its own fill, not a
library copy), which is why the 64 KB row is fill-bound.

What the prototype does **not** measure, and the slices must: the ACL gate and admission
filter on the target leg (unchanged code, run on a shared block), and the rv32 sizes of §5.7
(slice 3 measured `vertex_t` at 72 B on rv32, not the 64 B estimated).

### 7.3 Reproducing

```bash
cmake -S bench -B build/bench -DCMAKE_BUILD_TYPE=Release && cmake --build build/bench --target bench_lean_value_path
./build/bench/bench_lean_value_path
```

Each row is `LEAN_PATH stage size K allocs_per_op bytes_per_op payload_copies ns_per_op`. The
existing host suite (165 tests) and every bench target build and pass unchanged at the
drafting commit with this binary added.

## 8. Compatibility

### 8.1 Wire

None. No frame changes shape, no field is added, no conformance vector moves. Every slice is
verifiable against the existing vectors.

### 8.2 C++ API (implementation-defined, ADR-0013)

| removed | replaced by | slice |
| --- | --- | --- |
| `vertex_t::store(rope_t, std::pmr::memory_resource*)` | `vertex_t::store(value_t*)` | 3 |
| `read` → `rope_t`, `history` → `std::vector<rope_t>` | `value_ref_t`, `history(v, span)` | 7 |
| `set_pin_payload_ratio`, `kPinPayloadRatio`, `kPinNever` | `set_share_threshold_bytes` (landed), folding into `vertex_policy_t::share_threshold_bytes` at 10; `kShareThresholdBytes` | 5 |
| `set_history_depth` | `set_retention` (landed), folding into `vertex_policy_t::retention` at 10 | 6 |
| `std::function` hooks | `{fn, ctx}` slots + `tr::graph::thunk<F>` | 7 |
| `subscriber_remote_t::link/caller` strings | `link_id_t`, `subject_id_t` | 8 |
| `sp_atomic_slot_t` | `single_writer_slot_t` / `hazard_slot_t` | 1 (landed) |
| `set_*` / `configure_*` verbs (12) | `vertex_policy_t`, `graph_hooks_t` | 10 |
| `subscriber_fn_t` signature (`const rope_t&`-shaped) | `fn(ctx, const value_t&)` | 3 |
| `store_value` returning a wrapper / bool | returns `result_t<value_ref_t>`, refusal by value | 3 |
| `value_ref_t` over `std::shared_ptr<const rope_t>` | `value_ref_t` over an intrusive `value_t*` (same verbs, new shape and size) | 3 |
| `graph_t(std::pmr::memory_resource*, mem_backend_t*, …)` and the `mr_` channel | `graph_t(block_source_t&, graph_hooks_t)` — one source, one hooks struct | 3 (source), 10 (hooks) |
| `current_seq()` → `uint64_t`, `write_seq_` 64-bit | `uint32_t` with wrap-safe compare | 8 |
| `transport_t::send(iov)` as the only egress verb | plus a retained-send virtual (`send(value_ref_t, header_span)`) that a queued link overrides | 9 |
| `mem_backend_t` as a free-standing vtable | `mem_backend_t : block_source_t` — every downstream backend (`heap`, pools, ESP, device) re-based | 10 |
| `fwd_router_t(graph_t&, <positional planes>)` | `fwd_router_t(graph_t&, router_planes_t)` | 10 |
| `httpd_ws_link_t` / link constructors with positional knobs | per-link config aggregates (#1593) | 10 |
| the six pmr control-plane containers on `graph_t` (D1), `link_candidates` → `std::pmr::vector` | `block_array_t<T>`; `link_candidates(name, std::span<vertex_t*> out)` | 3 |

**No removal keeps a shim.** Per §11 ruling 4 as amended there is no backward compatibility:
every old name is deleted in the bump that introduces its replacement, with no deprecation
alias or transition release, and consumers adapt in the same bump. Each row's CHANGELOG entry
carries the one-line rewrite. The bindings (`bindings/`) consume the wire, not this API, and
are unaffected.

## 9. Risks

1. **The retention hazard moves from a knob to a policy.** A shared receive block lives as long
   as the LKV or ring that links it; on a pool-backed transport that is a pool slot held per
   retained large value. The threshold is the consumer's guard, and a deployment that sets it
   to 0 on a STREAM with `N = 64` has asked for 64 held receive blocks. Slice 5 documents this
   at the knob and adds the held-block count to `source_stats_t`.
2. **The single-writer contract is a contract.** A build that sets `kSingleWriter` and then
   publishes one vertex from two tasks has a data race the library cannot detect in release.
   Debug builds assert the owner; the config comment says what the flag promises.
3. **The always-inline fan-out body** (`dispatch_edge`) is one field away from the #1223
   cliff. Slice 4 re-runs the fan-out gate before and after; a regression there blocks the
   slice, not the RFC.
4. **The hazard domain over raw pointers** (slice 3) would have re-opened ADR-0069's retire
   path; slice 3 as landed kept the node indirection with the node owning one reference
   (§6.3), so the retire path is unchanged and this risk is retired.
5. **Ten slices is a long tail.** Slices 1–2 stand alone and have landed; slices 3–5 are
   the RFC's core and are worth landing as a unit; 6–10 are cleanups whose value is bytes and
   surface, and each can be dropped without invalidating the others.
6. **Ungated today: slice 9's queued WS send can interleave with another frame on the same
   socket.** A link that retains a `value_t` and writes it from its queue shares the socket
   with every other frame that link emits; nothing in the bench set or the host suite exercises
   two frames racing on one socket, so a torn frame would ship green. **Gate added:** the
   two-frame race host test of §6.9 must exist and pass *before* slice 9 lands, not with it.
7. **Retired with slice 3 as landed.** The draft's hazard retire over a raw `value_t*` was
   use-after-free class because the last owner became "whoever calls `release` last"; the
   shipped slot keeps a node that owns one reference (§6.3), so the last owner is always a node
   or a `value_ref_t` and no ASan arm is needed. `hazard_slot_t` remains the host opt-in.

## 10. Alternatives considered

- **Keep `std::shared_ptr`, add a hazard slot to the embedded default.** Fixes L1 and
  nothing else; the target leg still allocates per target; `hazard_slot_t`'s registry is
  `(kHazardReaderSlots + 1) × 128` B = 8,320 B at the host default, which is the wrong price
  on a target that has one writer.
- **Per-edge copy at the target** (the RFC-0022 §3.A "copy" delivery class as the default).
  It is what the code does today in effect (a wrapper per target); it is the thing being
  deleted.
- **Keep the ratio and add the threshold beside it.** Two knobs for one measured variable; the
  ratio has no owner (§3 R6).
- **A separate retention verb for app fields.** The third spelling of one property (§4.4).
- **`std::function` everywhere, for ergonomics.** 32 B per hook and a type-erased call on the
  handler path; ADR-0047 already measured the edge case the other way and left the rest
  pending. `thunk<F>` keeps the lambda ergonomics at 16 B.
- **A per-publish arena instead of a per-value block.** Bounds the publish but not the
  retention; a retained value has to outlive the publish, which is exactly what a block with
  a refcount is.

## 11. Discussion

**Rulings (2026-09-29, PR #1627).** 1. `kSingleWriter` is a per-build trait. 2. The host
default for `kShareThresholdBytes` is 4,096 B; deployments set their own value per vertex.
3. `NONE` retention on a value vertex is a permitted policy, not a named role. 4. (as
amended) There is **no backward compatibility**: the surface fold and every slice delete the
old API outright, with no shims, deprecation aliases or transition releases; consumers adapt
in the same bump, and §6.10's gate counts the final surface only. 5. Slices 1 and 2 land
ahead of acceptance as ordinary fixes (done: PR #1628, PR #1629).

The questions as posed, kept for the record:

1. §5.5 — is `kSingleWriter` a per-build trait (proposed) or a per-vertex bit? Per-build is
   the only form that removes the primitive from the binary.
2. §5.3 — the host default for `kShareThresholdBytes`: 4,096 (RFC-0022 Amendment 2's knee) or
   `SIZE_MAX` (copy always, today's behaviour) until a deployment measures.
3. §4.4 — whether a `NONE`-retention *value* vertex (pure relay) is a role the library wants
   to name, or only a policy it permits.
4. §4.12 — how far the surface fold goes in one release. (Ruled: all of it, in one bump,
   with nothing kept.)
5. §6 — whether slices 1 and 2 land ahead of this RFC's acceptance as ordinary fixes, since
   neither depends on §5.
