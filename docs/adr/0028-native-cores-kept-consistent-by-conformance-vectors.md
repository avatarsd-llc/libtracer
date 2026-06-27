# Each language gets a native core; consistency is enforced by shared conformance vectors, not by a shared C core via FFI

libtracer will have **C++, TypeScript, and (later) Rust** cores. Two ways to do that: **(B)** one C core that TS/Rust reach through **FFI** (Rust `-sys` + safe wrapper, TS = WASM-of-C) — zero drift by construction, but FFI friction, a C dependency everywhere, and a WASM bundle in the browser; or **(A)** **native reimplementations** per language — idiomatic, native perf, pure-TS in the browser, pure-Rust — at the cost of N× code and **drift risk**. This decision picks A and names the mechanism that makes A safe.

## Decision

**Each language ships its own native core. The language-agnostic conformance vectors under `tests/conformance/vectors/v1/` are the single source of truth, the C++ reference is *golden*, and every implementation is CI-gated against the same vectors — so drift is impossible to merge.**

- **No FFI binds the cores.** TypeScript is a **pure-TS** codec (no WASM-of-C); Rust is a **native** core (no `libtracer-sys` FFI). This **supersedes issue #6**, which specified the Rust binding as `libtracer-sys` (FFI) + a safe wrapper.
- **The vectors are the contract.** Each vector is `input.bin` + `expected.json` + `description.md`. Every core provides a small **conformance harness** — a CLI `<harness> <vectors-dir>` that decodes `input.bin`, round-trips (`encode(decode(input)) == input`), and emits **TAP** (one `ok`/`not ok` per vector). The contract is `tests/conformance/HARNESS.md`.
- **A polyglot driver gates CI.** `tests/conformance/run-all.py` runs every registered harness over the shared vectors, prints an *impl × vector* matrix, and **exits non-zero on any divergence** (a fail, a missing vector, or two cores disagreeing). The reference README already declared "a TypeScript / Rust core is admissible if it conforms" — this turns that into an enforced gate.
- **The C++ reference is golden.** When the wire changes, the C++ reference blesses new/updated vectors; every other core must then match them.

## Considered options

- **(B) One C core + FFI/WASM bindings.** Rejected: zero-drift-by-construction is real, but it forces a C dependency and FFI into every consumer, ships a heavy WASM bundle to the browser (where the client only needs a small codec + WebSocket), and is non-idiomatic. The conformance vectors already provide a drift guard without the FFI cost.
- **Codegen one codec from a schema.** Rejected: not idiomatic in any target, and a heavy generator to maintain; the hand-written native cores are the point.
- **Trust + manual review (no tooling).** Rejected: that *is* the drift everyone fears; the vectors must be a CI gate, not a convention.

## Consequences

- **Idiomatic native cores *and* provable lock-step.** The vectors double as the spec's executable conformance suite; "all green" means every core agrees byte-for-byte.
- **Issue #6's Rust-FFI strategy is superseded** — Rust becomes a native core with a conformance harness, like C++ and TS.
- **Phase 2 (when ≥2 cores exist):** add **differential fuzzing** (random valid frame → encode in core X → decode in core Y → assert equality) to catch drift the curated vectors miss, and a **coverage audit** (vectors exist for every type code × `opt`-bit combination) so green means the whole surface is exercised.
- **Cost owned deliberately:** N cores are N× the implementation work; the gate is what keeps that cost from becoming correctness debt.
