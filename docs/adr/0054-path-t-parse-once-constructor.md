# A parse-once `path_t` constructor: retire the `*path_t::parse(...)` deref idiom

Status: **accepted** (2026-07-06 — maintainer-directed). Reverses the earlier working
assumption (tracked as issue #31, "keep the unchecked `*`-deref of `result_t`, no
repo-wide idiom change"): the idiom **is** changed here.

## Context

`path_t::parse(std::string_view)` returns `result_t<path_t>` (`std::expected<path_t,
status_t>`) — the right shape for a **runtime** string, whose validity is a genuine
runtime condition. But almost every call site passes a **literal**:

```cpp
node.write(*path_t::parse("/net:children[]"), spec("stored_value", "temp"));
```

and immediately `*`-derefs the `std::expected` **unchecked**. Across the repo that was
**173 occurrences in 168 lines** (159 in `core/tests`, 6 examples, 6 esp-idf, 1 in
`core/src`). For a literal that pattern is pure ceremony: the `result_t` models a failure
mode that cannot occur for a correct source constant, and the bare `*` is an
unchecked-precondition footgun (a typo'd literal is a silent `std::bad_expected_access` /
UB, not a clear diagnostic). Only **two** sites parse a real runtime string.

The load-bearing constraint (maintainer): a caller **must be able to parse a path once
and hold the value, reusing it across many operations without re-parsing the string on
the hot path**. `docs/reference/02` keys dispatch on the *parsed* PATH-TLV payload bytes,
never the string form — so the parse is a one-time cost that a hot path must not repeat.

## Decision

Add an **infallible, `explicit` constructor** `path_t(std::string_view)` for known-good /
literal paths, and rewrite the 168 literal sites from `*path_t::parse("…")` to
`path_t("…")`. Keep `path_t::parse` (fallible) for the two runtime-string sites.

```cpp
path_t p("/sensor/temp");   // parse ONCE
write(p, a);                // reuse — no re-parse
write(p, b);                // reuse — no re-parse
```

- **Infallible by hard-abort.** A malformed *literal* is a source bug, so the constructor
  `std::abort()`s rather than yielding a `result_t` the caller would only `*`-deref
  unchecked anyway. No exceptions are thrown, so it is usable under `-fno-exceptions` (the
  MCU builds). A **runtime** string keeps `parse` + a real check.
- **`explicit`.** Construction is always a visible, deliberate parse — never an implicit
  per-call conversion hiding inside an API taking `std::string_view`. This is what
  protects the hold-and-reuse property: the graph API stays `const path_t&`, so passing a
  held `path_t` never re-parses, and there is no `string_view` overload tempting a hot
  loop into parsing every iteration.
- **`path_t` unchanged otherwise** — still the canonical PATH-TLV payload bytes (a
  sequence of NAME children) plus the optional field tail; the vertex-map key is those
  bytes. The constructor just parses-and-holds what `*parse` produced.

## Considered alternatives

- **Keep `*path_t::parse(...)` and document why it's fine (the original #31 plan).**
  Rejected by the maintainer: the unchecked `*` on a `result_t` is a footgun worth
  removing at the source, not defending in prose.
- **A `"/foo"_path` user-defined literal.** Prototyped, then rejected: the maintainer did
  not want a new UDL surface. A UDL also reads as "compile-time constant" while still
  parsing at runtime on evaluation, which is the same hot-loop hazard a bare
  `string_view` overload has.
- **`std::string_view` overloads on `read`/`write`/`register_vertex`/…** Rejected: they
  parse **per call**, directly defeating the parse-once-and-hold requirement, and grow the
  API surface on every path-taking method.
- **A static `path_t::of("/foo")`.** Functionally identical to the constructor; the
  constructor is the more natural spelling for "make a `path_t` from a string" and is what
  the maintainer asked for (`path_t("/path/to/something")`).

## Consequences

- New public API: `path_t(std::string_view)` and a restored `path_t() = default`. Noted
  in `core/CHANGELOG.md`. Not a wire/spec surface (`path_t` is reference-impl per
  ADR-0013) — no RFC, no conformance-vector change.
- 168 literal call sites shrink from `*path_t::parse("…")` to `path_t("…")`; the two
  runtime-string sites keep the fallible `parse` + check.
- The unchecked `*`-deref of `result_t` is **not** eliminated project-wide — this ADR
  scopes only the `path_t` parse idiom. The remaining `*`-deref of genuinely-fallible
  results (e.g. `*register_vertex(...)`, `*wire::decode(...)`) is a separate question,
  left open.
