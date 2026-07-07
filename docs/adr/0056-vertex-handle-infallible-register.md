# ADR-0056 — Opaque `vertex_handle_t` and an infallible `register_vertex`: retire the raw-pointer graph API

Status: accepted

## Context

The pervasive graph-API idiom is `tr::graph::vertex_t* t = *g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);`. It carries two coupled footguns: (1) the `*` unchecked-derefs a fallible `result_t<vertex_t*>` (`std::expected`), and the tree builds `-fno-exceptions` so `.value()` throws and `operator*` (UB on error) is the only ergonomic extractor — the `*` is partly *forced* by the build; (2) a raw `vertex_t*` is the caller-held handle, passed back into `read/write/await/assign/propagate/subscribe/history/field-write`.

ADR-0054 (parse-once `path_t`) explicitly left this open. Three facts shape the fix: ownership is already correct and zero-alloc-per-hold (`graph_t` holds `unordered_map<path_key_t, unique_ptr<vertex_t>>`, insert-only, so the `vertex_t*` is a stable non-owning borrow); callers never dereference `vertex_t` (it is opaque, only handed back to `graph_t`); and a failed `register_vertex` on a literal path is a source bug, not a runtime condition.

## Decision

1. **`tr::graph::vertex_handle_t`** — a zero-cost, non-null, opaque, non-owning wrapper over `vertex_t*` (name mirrors `route_handle_t` / `path_handle`). Graph-only construction (`friend graph_t`); no `operator*` / raw-pointer accessor exposed to callers. Pointer-sized, trivially copyable → identical pointer-load codegen. Swap it into every *public* `graph_t` signature; internal implementation methods keep `vertex_t*`. `find` returns `std::optional<vertex_handle_t>`.
2. **Infallible init-time `register_vertex`** — abort-on-error (like ADR-0054's `path_t(std::string_view)`), returning `vertex_handle_t` directly (no `result_t`, no `*`): `vertex_handle_t t = g.register_vertex(path_t("/sensor/temp"), STORED_VALUE);`. Keep a fallible `try_register_vertex(...) -> result_t<vertex_handle_t>` for genuine runtime-string sites.

## Consequences

Both footguns die at zero runtime cost; the `*` disappears from the ~77 register sites. Breaking C++ API change (implementation-defined per ADR-0013; wire protocol untouched) → `core/CHANGELOG.md` note; targets 0.4.0. ~a dozen `graph_t` signatures + internals + ~70 caller-facing sites (tests/examples/docs), mostly compiler-guided.

## Alternatives rejected

`shared_ptr` (control block + atomic per hold — violates the ~16 KB Cortex-M0 zero-overhead ethos, ADR-0042; no shared ownership needed); `unique_ptr` (owning — graph already holds it); `observer_ptr` (non-standard; a typed handle we control is stronger); pmr (orthogonal — governs where vertex bytes live, not handle safety; a pmr-backed `vertices_` is a worthwhile separate change).
