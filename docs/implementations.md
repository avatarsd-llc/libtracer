# implementations — Registry of libtracer implementations

This directory is a public registry of implementations of the libtracer protocol. The reference implementation lives in [`core/`](../core/); everything listed here is **independent**, including projects in other languages, on other platforms, or with different design tradeoffs.

Independent implementations are encouraged — they validate the spec and demonstrate that libtracer is a real protocol, not just one library.

## Registered implementations

| Name | Language | Spec versions | Conformance | Repo / Maintainer |
|------|----------|---------------|-------------|-------------------|
| _(none yet)_ | — | — | — | — |

## How to register

1. Implement the libtracer spec (see [docs/spec/](spec/)).
2. Run the conformance vectors at [tests/conformance/](../tests/conformance/) and pass them.
3. Open a PR adding a row to the table above and a stub file `docs/implementations/<name>.md` describing your implementation, the spec versions it targets, and a link to your repo.
4. Maintainers will review and merge. Listing is editorial — we may decline implementations that are abandoned, do not pass conformance, or misuse the trademark (see [TRADEMARKS.md](../.github/TRADEMARKS.md)).

## Why this lives here

Listing implementations in this repo (rather than scattered across the web) gives users a single place to discover them, and gives implementers a standing seat in spec discussions per [GOVERNANCE.md](../.github/GOVERNANCE.md). It also protects users by making "compatibility" verifiable rather than asserted.
