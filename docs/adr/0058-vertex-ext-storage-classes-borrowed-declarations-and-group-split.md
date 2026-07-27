# ADR-0058 — Vertex-extension storage classes: borrowed app-field declarations and a co-occurrence group-split of `vertex_ext_t`

Status: accepted

## Context

RFC-0010 app fields install a per-vertex field descriptor table `{name, access, descriptor, initial value?}` through the local host API. The reference impl stored it as `std::vector<app_field_t>`, where each `app_field_t` owns all four members (`std::string name; app_access_t; std::vector<std::byte> descriptor; std::vector<std::byte> value;` ≈ 88 B) inside the lazily-allocated cold `vertex_ext_t`.

Two costs surfaced on the C6 (#388), where the whole table is board firmware pinned in flash:

1. **Owning duplication of immutable declarations.** An MCU owner's table is `constexpr` in `.rodata` already. `set_app_fields` *moved a heap copy* of it into RAM — one `std::string`, two `std::vector`s, plus a separate descriptor allocation *per field*. A 5-field table cost ~872 B of which ~100 B is real content and ~772 B is container plumbing + allocator overhead. RFC-0010 app tables cost 1.5× the `/meta` workaround they replaced — a regression that boot-looped the C6 on `bad_alloc`.

2. **All-or-nothing extension block.** `ensure_ext()` allocates the *entire* 304 B `vertex_ext_t` on first need of *any* cold feature. A vertex that only declares app fields still pays for the 128 B of value-seam `std::function` handlers (`on_read`/`on_write`/`on_children`) and ~100 B of ACL vectors it never touches. `ensure_ext` honours pay-for-what-you-use *between* vertices (a plain leaf allocates nothing) but violates it *within* the block (one feature drags in all of them).

The bytes a vertex holds fall into three residence/mutability classes, and the old layout conflated them:

- **① live value** — the vertex payload (hot core, atomic `shared_ptr<rope>`, data plane). Not involved here.
- **② owner declaration** (`name`/`access`/`descriptor`) — immutable after install, identical across every vertex of a device type, naturally flash-resident.
- **③ app-field value** — per-vertex mutable, silent control-plane state (§announce write: no `await`, no propagate).

## Decision

**Two changes, landed as two PRs.**

### Step 1 — Borrowed declarations + view-slot storage

1. **An additive non-owning install overload** beside the owning one, chosen explicitly by name (never by argument-type overload resolution, so the storage decision is legible at the call site):
   ```cpp
   struct app_field_static_t { std::string_view name; app_access_t access;
                               std::span<const std::byte> descriptor; };   // owns nothing
   void graph_t::set_app_fields_static(vertex_handle_t, std::span<const app_field_static_t>);
   void graph_t::set_app_fields(vertex_handle_t, std::vector<app_field_t>);  // kept — runtime-formed
   ```
   Contract of the borrowed overload: *the `name` and `descriptor` bytes must outlive the vertex — pass pointers into static storage (flash/`.rodata`), never into stack or a soon-freed heap.* The same lifetime discipline the net plane accepted for `receiver_slot_t` (fn-ptr + `void* ctx`, ADR-0038/#337). The owning overload stays the safe default for host/dynamic callers who genuinely form tables at runtime.

2. **One internal representation, view-shaped.** Both install paths converge on a slot that stores views, not owned bytes:
   ```cpp
   struct app_field_slot_t { std::string_view name; app_access_t access;
                             std::span<const std::byte> descriptor; };
   ```
   - Borrowed install: slots point straight at the caller's flash; **zero declaration RAM**.
   - Owning install: the runtime table's `name`+`descriptor` bytes are copied into **one owned backing buffer** and the slots point into it — **one allocation for the whole table**, not N per-field vectors. The owning path gets lighter and faster too.

   The `:schema`/settings read path (`app_fields_snapshot()`) keeps returning an owning `std::vector<app_field_t>`, materialised under the lock, so every emit call site and its use-outside-the-lock safety is unchanged. The materialisation is a cold control-plane copy, freed immediately — the RAM win is in *resident* storage, not the transient read.

3. **A separate, lazily-allocated value store (class ③)** — index-keyed, `unique_ptr`-null until the first field write on the vertex (the #389 lazy-history pattern). A table whose fields are declared-but-never-written costs zero value RAM.

### Step 2 — Co-occurrence group-split of `vertex_ext_t`

Move the two independently-sheddable feature groups behind their own lazy pointers, so first-use of one allocates only that group:

- **value-seam handlers** `{on_read, on_write, on_children}` → `std::unique_ptr<value_handlers_t>` — HANDLER-role vertices only (transports/connections/synthesized listings). Set once at registration, read lock-free (the pointer never changes after adopt).
- **app-field** `{view slots, backing, lazy value store, on_app_field_write}` → `std::unique_ptr<app_field_group_t>` — RFC-0010 vertices only.

`on_app_field_write` **moves out of the public `handlers_t` input into the app-field group's storage** — it is the *apply seam of app fields* (co-occurs with the table), not part of the value seam of a HANDLER-role vertex. `handlers_t` stays the 4-member install input; `adopt_identity` splits it into the two groups. An app-field-only leaf then allocates the app-field group and sheds the ~96 B value seam entirely.

**Amendment (2026-07-10, at implementation):** `acl` and `stream` (`settings`/`history`/`last_flushed_seq`) **stay inline**, not split as the original sketch above proposed:

- **ACL is not sheddable per-vertex.** The ADR-0050 effective-ACE cache (`eff_aces`/`eff_aces_inherit`) is stored on **every gated vertex, including bare descendants** — a leaf with no own ACEs still caches its inherited merge, and `mark_acl_cache_dirty` is a *lock-free* release-store on the hot subtree-invalidation walk. Behind a lazy pointer, any gated leaf would allocate the group anyway (so no RAM is saved for the app-field-leaf case that motivates this ADR), and the lock-free dirty-mark would have to allocate-or-skip on a null group — real concurrency risk for zero benefit. The inline ACL cost is load-bearing, not dead weight.
- **`settings` stays inline** because `settings()` is a lock-free `const settings_t&` reader; keeping it inline avoids a second pointer-hop on that path, and `settings_t` is small and common. `history` is already lazy (#389); `last_flushed_seq` is 8 B.

So Step 2 as shipped is the two *unconditionally-dead-for-a-leaf* groups (value handlers, app-field), which is where the entire measured #388 win lives.

## Consequences

- **RAM.** Step 1 removes the declaration duplication (~520 B on the C6 for a 5-field table); Step 2 removes the ~230 B of unrelated handler + ACL dead weight an app-field-only vertex paid via `ensure_ext`. Together they take the RFC-0010 table below the `/meta` workaround it replaced, so the C6 fits.
- **API.** Wire-invariant — `:schema` serves the same verbatim bytes regardless of install path — so **no RFC**; a `core/CHANGELOG.md` note under the public-API discipline. The exact `set_app_fields_static` signature is posted to #388 to claim the shape before either session writes it (avoids the #382-style host-API collision with the parallel session for the originating production firmware — an ESP32-C6 smart-agriculture node — which co-owns RFC-0010).
- **Cost.** The group-split adds a second pointer hop (hot core → slim ext → group) on *cold control-plane* paths only; the LKV read/write hot path never touches the ext. The owning read snapshot pays a cold transient copy.
- **fn-ptr handlers stay deferred** (ADR-0047 §3, #215): `std::function` is heap-free for libtracer's `[this]`-shaped captures (measured), so converting saves no allocation; the only win is struct size (32 B → 16 B/slot), which the group-split already neutralises for the app-field case by removing handlers from that vertex's cost entirely.


## Erratum 1 — "zero declaration RAM" was not true as implemented

Step 1.2 promised that a borrowed install's "slots point straight at the caller's flash — zero
declaration RAM". The bytes were indeed never copied, but the runtime still **copied the caller's
`app_field_static_t` array into an owned `std::vector<app_field_slot_t>`** — and those two types
were field-for-field identical (`std::string_view name; app_access_t access;
std::span<const std::byte> descriptor;`), so the copy converted between a type and itself.

Measured on host with the `vertex_app5` / `vertex_app5_static` gate rows, live usable-size bytes
per vertex for a 5-field table:

| | bytes | allocs |
| --- | ---: | ---: |
| bare leaf | 136 | 7 |
| + owning install | 695 | 17 |
| + borrowed install, as promised | 592 | 11 |
| + borrowed install, after this fix | **392** | **10** |

The slots vector was **the largest single resident block** on the borrowed path (200 B of the
456 B the table added), ahead of `vertex_ext_t` (168 B) and `app_field_group_t` (88 B). The
remaining two are irreducible: the ext block is needed by any gated vertex, and the group *is*
the table plus its apply seam.

**The fix:** the two types are unified (`app_field_static_t` is now an alias of
`app_field_slot_t`), and `app_field_table_t::slots` becomes a `std::span` that a borrowed install
points at the caller's array directly. The owning install keeps its copy in a
`std::unique_ptr<app_field_slot_t[]>`, which — with the span — is the same size the vector was on
both host and rv32, so the struct does not grow and the win is not partly given back.

**The contract is correspondingly stronger, and this is a real API change:** the caller must now
keep the **array** alive for the vertex's lifetime, not merely the `name`/`descriptor` bytes it
points at. Wire-invariant (`:schema` serves the same verbatim bytes), so no RFC — but noted in
`core/CHANGELOG.md`. On rv32 (`sizeof(app_field_slot_t) == 20`, measured with
`riscv32-esp-elf-g++`) the saving is ~100-112 B per vertex including multi_heap block overhead;
the 392/592 figures are host and do not transfer.

Nothing ever mutated a slot after install — every write to `.slots` is in the two build paths —
so viewing immutable caller storage is safe by construction, and `slots` is now
`span<const app_field_slot_t>` so that is compiler-enforced rather than doc-enforced.

## Erratum 2 — the erratum-1 tightening reached callers silently

Erratum 1 strengthened the borrowed install's contract: the caller must now keep the **array**
alive, not merely the bytes it points at. That was documented, put in `core/CHANGELOG.md`, and
audited against every in-tree caller. It still broke a downstream one, because the parameter was
a `std::span<const app_field_static_t>` and **a `std::span` binds implicitly to a
`std::vector`** — so a caller that built its table into a function-local vector kept compiling,
unchanged, under a contract it no longer satisfied.

That is what happened to the reference firmware's C shim
([strawberry-fw #80](https://github.com/avatarsd-llc/strawberry-fw/issues/80)):
`trc_app_fields_set_static` converted a C POD array into a local
`std::vector<app_field_static_t>`, installed it, and returned. Every `:settings.app.*` write on
that node then read freed heap; it surfaced as `PERMISSION_DENIED` — garbage access bits, not an
ACL decision — across seven host suites, and was found by bisecting 188 commits rather than by a
compiler diagnostic. The in-tree audit could not have caught it: the offending caller was in
another repository.

**The fix:** `set_app_fields_static` takes a `borrowed_fields_t` instead of a span. It converts
implicitly from `const app_field_static_t (&)[N]` and from `std::array` — the spellings a
`static`/`constexpr` table takes, and the shape of every existing caller, so the change is source
compatible — and from nothing else. `borrowed_fields_t::unchecked(span)` is the explicit opt-out
for a table whose extent is only known at run time, which is what a language binding mapping a
foreign POD array needs. The lifetime promise still rests on the caller; what changed is that
making it is now deliberate rather than implicit.

**What this does and does not buy.** It rejects the container-and-temporary class of mistake,
which is the one that actually occurred and the one a dynamic table naturally falls into. It does
**not** prove static storage duration: a block-scope `app_field_static_t[N]` binds exactly like a
namespace-scope one, and C++ cannot express that constraint on a parameter. A caller determined to
dangle can still dangle — via a local array, or via `unchecked`. The guard is a diagnostic for the
common error, not a proof of the contract.

The generalisable lesson, and the reason this is recorded rather than just fixed: **a lifetime
contract tightened underneath an unchanged signature is invisible to every caller that is not
recompiled against a changed type.** Documentation, a CHANGELOG entry, and an in-tree caller audit
were all present here and all insufficient. When a borrow contract changes, change the type.

## Alternatives rejected

- **Replace the owning overload with the borrowed one** (all callers pass views) — rejected: runtime-formed tables (host, dynamic config) have no static storage to point at; forcing them to fabricate it is the footgun the two-overload split exists to avoid.
- **Revert erratum 1 — have the borrowed install copy the caller's array into `owned_slots`** (erratum 2's alternative) — rejected: it hands back the 200 B/leaf erratum 1 measured, and pays `n * sizeof(app_field_slot_t)` **per vertex** for an array that is byte-identical across every vertex sharing a table. The reference firmware installs two shared tables across 77 endpoints, where per-vertex copies cost ~3.5 KB against a measured 4.3 KB heap floor. The caller-side answer — compile the table once into static storage, install the same slots everywhere — costs ~120 B for the same node, so the borrow contract is worth keeping and worth guarding.
- **Rename the span-taking form** so `set_app_fields_static` could become array-reference-only — rejected: same protection as `borrowed_fields_t` with no new type, but it churns a spelling that is already in the ADR, the interop guide, the CHANGELOG and three in-tree callers, for a name the header itself notes is "already in the wild".
- **Discriminated per-table owning-vs-borrowed storage** (a flag, two representations) — rejected: branches every read site and keeps N per-field allocations on the owning path. The single view-slot representation with an optional backing buffer subsumes both.
- **Value inline in the slot** — rejected: re-fuses classes ② and ③, re-bloats the slot array, and makes every RO/never-written field carry an empty-vector header.
- **Maximal per-member ext-split** — rejected: pointer-chase and allocation count grow past the measured need. The four co-occurrence groups are driven by the #388 problem, not speculative.
