# The terminus reply egress is its own injected backend

Status: **ACCEPTED (2026-08-03).** Implements [#795](https://github.com/avatarsd-llc/libtracer/issues/795). No wire change (the reply bytes are byte-for-byte identical); this is a *source-of-bytes* decision, recorded here plus a `core/CHANGELOG.md` note for the constructor signature change. Extends the bounded-seam programme of [ADR-0067](0067-bounded-recycling-source-and-per-owner-topology.md); reuses the by-value degrade of [ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md).

## Context

A terminus reply is built by `op_resolve_walk.hpp`'s `assemble`: one exactly-sized **head** segment (the swapped route bytes — `dst = request src`, `src = request dst` — plus `op`, `kind`, and an inline tail) prepended to refcount-clones of the stored payload. On a mint ([RFC-0024](../spec/rfcs/0024-bound-paths-node-scoped-vertex-ref-source-routing.md) §7) a second, fixed 12-byte `PATH_REF` segment is appended.

Both segments came from `view::heap_alloc` — hard-wired to `mem::heap_backend()`, the **global heap**. After the router's four flatten sites (#730), the terminus flattens (#766), and both tiers' ownership copies (#793, #801) moved onto injected backends, the reply head was the last *reply-egress* byte source a bounded node could not bound. (Two terminus byte sources sat outside this ADR's scope, both the same class: the composed-root folded READ frames one POINT header per subtree node, and the `":children"` folded READ frames one per registered child plus the outer listing header. Both are payload framing and belong on the value seam — [#831](https://github.com/avatarsd-llc/libtracer/issues/831), since closed by pointing *both* sites' headers at `graph_t`'s `value_backend` under ADR-0060.) It is reached on *every* reply, its size is **peer-driven** (a peer chooses the route depth that sizes the head), and it is reachable **pre-authorization** (the denied path builds a head too — ACL is evaluated inside `graph.read`/`write` after the reply route bytes are fixed). For an ADR-0067-class node that has injected `mr`, `rx`, `flat`, `ctl`, `value_backend` and its transport-receive backend all at one slab, this one allocation still escaped to `malloc`.

The failure *half* was already closed: a null segment yields an empty rope, which `resolve_node`'s `or_backpressure` turns into an addressed `kind=ERROR STATUS{BACKPRESSURE}` reply rather than a silent drop. Exhaustion is answered by value; there is no abort here. The **bound** half was open.

> **Erratum (2026-08-03) — "there is no abort here" is true of the *segment*, and over-broad as written.**
> It is exact for what the sentence is about: `view::segment_alloc` refuses by returning a null
> handle, and the head/mint allocation this ADR moved cannot throw. It is NOT true of the reply
> path as a whole. `assemble` first sizes the reply chain through
> `rope_t::try_reserve` (`core/src/op_resolve_walk.hpp:575`), and once the chain exceeds the
> rope's inline capacity that delegates to `tr::detail::try_reserve`
> (`core/include/libtracer/rope.hpp:107`), whose second step is the **throwing**
> `std::vector::reserve` run after a probe-and-free
> (`core/include/libtracer/mem_heap.hpp:116-121`). The helper is declared `noexcept`, so a lost
> race on the just-freed probe block terminates rather than degrading by value — its own comment
> concedes the trick is sound only single-threaded. Tracked as
> [#850](https://github.com/avatarsd-llc/libtracer/issues/850); the same residual makes
> [ADR-0053](0053-lazy-rope-backed-decode-view-partial-path-routing.md)'s "nothrow-reserves"
> sentence over-broad, annotated there. Nothing in this ADR's decision changes — the reply-egress
> **backend** swap is unaffected — but a reader must not take this sentence as "the terminus
> reply path cannot abort".

## Decision

**Option B — a dedicated injected `mem::mem_backend_t*` for terminus reply egress.**

`op_resolver_t` and `fwd_router_t` each gain an `egress` member, injected exactly like `flat`/`rx`/`mr` — a caller-owned pointer, no owned allocation, no synthetic capacity. It is threaded down `resolve_node` → `apply_op` → the `assemble*` reply builders, and both `heap_alloc` sites (the head and the mint) draw from it via the injectable `view::segment_alloc(backend, size)` — the nothrow form (`alloc` returns `nullptr` on refusal), so OOM is a null handle the existing degrade handles, never a throw that the `-fno-exceptions` profile would abort on.

The segments are already `mem_backend_t`-shaped (`view_t::over(segment_ptr_t)`), so this is a **backend swap, not a shape change**: no rope or segment change, and the reply bytes are identical. A backend indirection on the reply head is the same one allocation, just from a different backend — the reply-path (reply-spread) shape is unchanged.

**Default — fall back to the heap backend.** `egress` defaults to `&mem::heap_backend()`, so host and unbounded callers are byte-unchanged and only a bounded node that injects its slab gets the bound. This is the honest default: there is **no capacity constant** — the bound comes entirely from the injected resource (the node's one slab), which above the injection is bounded by the rx-admitted frame (the route bytes were decoded into the injected `rx` arena).

## Alternatives rejected

- **A, without a rename — point the head at `flat`.** Rejected: a silent re-scope. `flat` is documented in a public `@param` as the backend *every rope flatten* draws from, and a deployment sizes its slab against that sentence. A reply head is not a flatten — it is egress construction, sized against *route* bytes, not *payload* bytes, and on the span tier `flat` is provably inert. Folding the head in would add one head segment per in-flight reply to a budget deployments already set for flattens; a node that budgeted a small pool for flattens could begin refusing replies it used to send, with **no source change on its side**. `fwd_router.hpp` already lists the head as explicitly *out* of `flat`'s scope; this option contradicts a shipped contract.

- **A, with a rename — widen and rename `flat`** to a general terminus-byte seam (`bytes`/`seg`), documenting that its sizing now includes one head segment per in-flight reply. Honest, but a **public rename plus a re-sizing note for every existing injector** — cost paid by every deployment to fix a residual on one path. Option B keeps `flat`'s contract byte-for-byte and adds one clearly-scoped injection instead.

- **C — leave the head on the heap, and document it.** Rejected as terminal: it violates the ADR-0067 bounded-seam commitment for the MCU class — the one node type this fix exists to serve. Acceptable only as the interim state, which is what the docs recorded before this ADR.

## Consequences

- A bounded MCU node bounds its last terminus allocation by pointing `egress` at its slab. Exhaustion of that slab degrades through the **same** empty-rope → `or_backpressure` → addressed `STATUS{BACKPRESSURE}` path OOM already took: a refused RESULT head yields an empty rope, and if the smaller error head can still be built the answer is an addressed BACKPRESSURE on the same link; if it cannot, the reply is dropped. Both are by value; neither is an abort.
- Public constructor signature change on `op_resolver_t` (adds `egress` after `flat`) and `fwd_router_t` (adds `egress` after `max_label_bindings_per_link`), both defaulted — source-compatible, noted in `core/CHANGELOG.md`.
- `egress` MUST be thread-safe on the same terms `flat` is (the terminus resolves on a transport child's receive thread; several children receive concurrently, and a reply `segment` self-routes its reclaim on whichever thread drops the last reference).
- No spec instrument: the wire bytes are unchanged, so this is an ADR for the seam decision, not an RFC.

## References

- [#795](https://github.com/avatarsd-llc/libtracer/issues/795) — the residual and the option-B brief.
- [ADR-0067](0067-bounded-recycling-source-and-per-owner-topology.md) — the bounded-seam / per-owner-topology commitment this completes for the terminus.
- [ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md) — the nothrow, by-value failure model the degrade rides.
- #730 / #766 / #793 / #801 — the prior terminus/router allocation sites moved onto injected backends; this is the last of that set on the reply-egress path. [#831](https://github.com/avatarsd-llc/libtracer/issues/831) closed the remaining value-seam residual — the folded-READ POINT headers on **both** folded reads, `read_subtree_folded` and `read_children_folded` — by drawing them from `graph_t`'s `value_backend` (ADR-0060) — an application of that seam, not a new injection.
