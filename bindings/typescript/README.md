# libtracer — TypeScript packages (npm workspace)

This directory is an [npm workspace](https://docs.npmjs.com/cli/v10/using-npm/workspaces)
monorepo holding the TypeScript side of libtracer: one cross-validated **core**
package plus **per-transport** packages. The packaging architecture is decided in
[ADR-0033](../../docs/adr/0033-npm-subpackage-monorepo.md).

## Packages

| Package                     | Path                              | Publishes as                            | On npm  | Maturity     |
| --------------------------- | --------------------------------- | --------------------------------------- | ------- | ------------ |
| core (in-process codec)     | `packages/core`                   | `@avatarsd-llc/libtracer`               | `0.7.0` | cross-validated |
| client SDK                  | `packages/client`                 | `@avatarsd-llc/libtracer-client`        | `0.7.0` | experimental |
| WebSocket transport         | `packages/transport-ws`           | `@avatarsd-llc/libtracer-ws`            | `0.7.0` | experimental |
| WebTransport transport      | `packages/transport-webtransport` | `@avatarsd-llc/libtracer-webtransport`  | `0.7.0` | experimental |

**All four are published**, each at `0.7.0` (verified against registry.npmjs.org on
2026-08-04 — `dist-tags.latest`, and no package is `private`). The three non-core rows
read "scaffold" until now, which was wrong on both axes: they are on the registry, and
they are not stubs — `client` is ~1.9 kLoC with session / roundtrip / live-interop tests,
and both transports carry codec and interop tests that `ts.yml`, `ws-interop.yml` and
`wt-interop.yml` run. **Maturity** is the separate axis and it is not uniform: the
[capability matrix](../../docs/capability-matrix.md) marks the TypeScript **wire codec**
✅ — byte-exact against the shared vectors, via the harness the polyglot driver invokes —
while **client / node** and **transports** are 🟡, functional but experimental, so those
APIs may still move.

The **core** carries no transports, so a consumer that only needs the codec
never pulls a transport dependency. Per-layer slicing (L0/L1/L2/L4) is done with
**subpath `exports`** inside the core (`@avatarsd-llc/libtracer/wire`), not by
exploding into many packages — see the ADR for the trade-off.

## Develop

```sh
# from this directory (bindings/typescript)
npm install            # installs + links all workspace packages
npm run build          # builds every package (tsc)
npm test               # runs the core conformance harness
npm run conformance    # same, explicit
npm run bench          # core perf bench over the shared vectors
```

The single lockfile lives here at the workspace root. The conformance harness
and perf bench run under plain `node` with **no build step**, which is what the
polyglot conformance driver (`tests/conformance/run-all.py`) invokes.
