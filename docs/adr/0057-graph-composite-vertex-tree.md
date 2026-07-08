# ADR-0057 — Graph-as-Composite: a parent/children vertex tree replaces the flat full-key map

Status: accepted (maintainer ratified 2026-07-08 grill)

## Context

`graph_t` kept its vertices in one flat `unordered_map<path_key_t, unique_ptr<vertex_t>>` keyed on
the full canonical PATH-payload bytes, and every hierarchical relation was recomputed from those
bytes. That shape carried four accumulating costs:

1. **Per-vertex full-key bytes, twice.** Each `vertex_t` stored its whole canonical key, and the
   map node stored a second copy as the map key — every ancestor prefix duplicated at every depth
   (`/a`, `/a/b`, `/a/b/c` store `a` three times, `b` twice), plus one hash-map node allocation per
   vertex.
2. **Per-ancestor map+lock hops in bubbling.** RFC-0005 vertical bubbling (`bubble_up`) derived
   each ancestor key with `key_view_t::parent()` and did a `find_ptr` — a shared-lock acquisition
   and hash lookup — per level, on the delivery path.
3. **The 4× open-coded ancestor walk.** The same derive-parent-key-then-look-it-up pattern was
   spelled out in the write-creates CREATE gate (`ensure_vertex_ptr`), the ACL inheritance walk
   (`acl_allows`), the creation-time subtree listener sum (`register_vertex_key`), and `bubble_up`.
4. **No child links.** Direct-member enumeration (`read_children`) and the subscribe-time
   descendant bookkeeping (`note_subscriber_added/removed`) scanned the ENTIRE map testing byte
   prefixes, and the planned subtree-precise effective-ACL invalidation (item 4 on ADR-0050's
   cached merge) has no structure to hang a subtree walk on.

CONTEXT.md already names the graph the **third composition axis** ("Graph (address) composition")
— a Composite of addressable vertices — but the implementation did not reify it.

## Decision

The vertex tree becomes a real Composite:

- **Each `vertex_t` holds a parent pointer and a children container**, and stores **one canonical
  NAME record** (its own name — `name()`), not the full key. The children container is
  small-vector-style: an inline array (`kInlineChildren = 2`) first, spilling to a heap vector kept
  **sorted by name record** so a wide composite (thousands of children under one parent) resolves a
  child in O(log n) instead of a linear scan. Tree links (parent/children/registered flag) are
  guarded by the graph's `map_mutex_` (unique for mutation at registration, shared for walks);
  parent pointers and name bytes are immutable after construction, so parent-chain walks
  (bubbling, ACL) need **no lock at all**.
- **Resolution is an O(segments) child walk from the root.** `find_ptr(key)` decomposes the key
  into NAME records (mirroring `key_view_t`'s framing, ragged-tail behavior included) and descends.
  Resolution is wiring-frequency — the hot path holds a `vertex_handle_t` (ADR-0056, load-bearing
  claim 6) — so trading the hash lookup for a tree walk is the ADR-0047-correct call. Handles and
  the read/write/await hot path are unchanged.
- **Missing intermediates are placeholder nodes.** `register_vertex("/a/b/c")` with no `/a`
  creates unregistered structural nodes for `/a` and `/a/b`: `find` does not return them,
  `read_children` does not enumerate them, and a later registration **fills** the placeholder in
  place (same allocation, so no pointer ever moves). This preserves the previous flat-map behavior
  exactly (intermediates "did not exist") while giving the tree full connectivity.
- **Bubbling walks parent pointers** (RFC-0005): `bubble_up` fans out along `parent()` links —
  zero map lookups, zero lock acquisitions per level. The 4× ancestor-walk pattern is absorbed:
  the ACL inheritance walk collects ancestor ACE lists via parent pointers (through the `with_aces`
  verb), the creation-time listener sum is the parent's `listeners_above() + own_subs()` (O(1)),
  and the write-creates CREATE gate finds the nearest existing ancestor on the same descent.
- **ACL:** `acl_allows` walks parent pointers to evaluate inherited ACEs. This is the structure the
  item-4 follow-up (`effective_acl_t` + subtree-precise cache invalidation) builds on; the cache
  itself is deliberately NOT built here.
- **Write-creates** (`mkdir -p`, RFC-0005) creates intermediate levels as real registered children,
  as before.
- **Sweep sets stay byte-keyed** (RFC-0008 `pending_` / `unconditional_` ordered sets — the minimal
  change). Since vertices no longer store their full key, the graph renders one on demand with
  `build_key(vertex)` (a parent-pointer walk concatenating name records), used only at
  sweep/observed-write/wiring frequency. **Judgment call — render on demand, not cache:** caching
  the full key per vertex would reinstate the prefix duplication this ADR removes and make the
  change a net per-vertex memory INCREASE; rendering costs one O(key-length) build exactly where
  the old code already paid an O(key-length) owned copy into the set (`mark_pending` /
  `clear_pending` / `set_delivery_mode` / the `propagate` range bound), and idle (unobserved)
  writes still build nothing (the RFC-0005 listeners gate runs first).
- **`key_view_t` stays** for parsing PATH payload keys at the wire boundary; `find` walks the same
  framing.

## Vertex lifetime and pointer stability

Vertices are **still never erased** (retire-LIST remains deferred, #223): the insert-only invariant
is what makes the raw `vertex_t*` inside `vertex_handle_t` — and every pointer held past a lock —
sound for the graph's lifetime. Under the Composite each node is owned by its **parent's children
container via non-moving allocation**: one `unique_ptr<vertex_t>` per child (the container may
reallocate its pointer array; the pointed-to `vertex_t` never moves — it is non-copyable and
pinned, as before). Filling a placeholder mutates the existing node in place; it never replaces the
allocation. The whole tree is released recursively when the graph is destroyed (depth-bounded by
path depth). Erasure remains future work gated on a real lifetime scheme (refcount / epoch
reclamation / tombstone) — a bare detach-from-parent would dangle every outstanding handle, the
same class of bug as the route_handle `clear_link` dangling ref fixed in #220.

## Considered options

- **Keep the flat map** — rejected: the four Context costs are structural; every follow-up
  (subtree-precise ACL cache, member enumeration at scale) would keep paying the whole-map scans.
- **Hash-trie / HAMT keyed on segments** — rejected: heavier nodes and code for a lookup that is
  wiring-frequency anyway; the sorted-children Composite already gives O(depth · log fanout).
- **Per-vertex full-key cache vs render-on-demand** — render-on-demand chosen (see Decision); the
  cache would negate the memory win and add a second copy of the truth to keep consistent.
- **Key-ordered sweep sets vs tree-marking** — sets kept (minimal change); tree-marking (a dirty
  flag per node + subtree walk on sweep) is a candidate follow-up once the rope-cursor sweep work
  lands, and switching later is invisible outside `graph.cpp`.

## Consequences

- The item-4 ACL work (`effective_acl_t`, cached effective-ACE merge, ADR-0050) becomes
  **subtree-precise**: an `:acl` write can invalidate exactly its subtree by child links instead of
  generation-bumping the world.
- `read_children` and `note_subscriber_added/removed` become O(children)/O(subtree) instead of
  O(all vertices).
- `vertex_t::key()` is gone (replaced by `name()`, the single NAME record) — a public-header API
  change noted in `core/CHANGELOG.md`; no caller outside `graph.cpp` used it (vertices are opaque
  behind `vertex_handle_t`, ADR-0056).
- Retire/erase remains future work (above).

## Perf notes

- **Hot path unchanged** — read/write/await on a `vertex_handle_t` never touch the tree. The tree
  links live at the cold tail of `vertex_t`, so the hot members' offsets are unchanged. Two
  incidental hot-path guards were added while landing this: a relaxed `pending_count_` mirror lets
  the per-eager-write `clear_pending` skip the key render + sweep lock while nothing is pending,
  and `dispatch_edge`'s target/remote legs are split into helpers so its per-edge body keeps
  inlining into the fan-out loop.
- **Bubbling cheaper** — per-ancestor cost drops from shared-lock + hash lookup to one pointer
  chase, and the walk takes no lock at all.
- **Resolution O(depth · log fanout) at wiring frequency** — `write(path)` / FWD resolve / edge
  target re-dispatch pay a tree descent instead of one FNV hash over the full key; small trees are
  a couple of inline compares. Measured (bench_libtracer, GCC 14 `-O3`, best-of-5 interleaved
  vs main): fan-out 8/128/1024/8192 within ±3% (noise floor ~4%), multi-thread rows +4–6%,
  `bench_fanout_clone_storm` flat; write-**by-path** at 128–1024 distinct endpoints is the one
  measurable trade, ~−9…−13% (per-level sorted-child binary search vs one hash) — acceptable
  because per-publish resolution is exactly what ADR-0056 handles exist to avoid.
- **Memory: segment-per-node vs full-key-per-node.** Removed per vertex: the map node allocation,
  the map's duplicate `path_key_t` (24 B + full key bytes), and the vertex's own full-key heap
  bytes beyond its last record. Added per vertex: parent pointer + inline child slots + spill
  vector + flag (~56 B RAM). Net: heap per-vertex cost goes down for any tree of depth ≥ 2, and the
  duplicated-prefix growth term is gone entirely. Code size: `graph.cpp` text +~5 KB on x86-64
  `-O3` (tree walk/insert paths); the MCU profile compiles `-Os`.
