# libtracer — TypeScript packages (npm workspace)

This directory is an [npm workspace](https://docs.npmjs.com/cli/v10/using-npm/workspaces)
monorepo holding the TypeScript side of libtracer: one cross-validated **core**
package plus **per-transport** packages. The packaging architecture is decided in
[ADR-0033](../../docs/adr/0033-npm-subpackage-monorepo.md).

## Packages

| Package                     | Path                              | Publishes as                            | Status      |
| --------------------------- | --------------------------------- | --------------------------------------- | ----------- |
| core (in-process codec)     | `packages/core`                   | `@avatarsd-llc/libtracer`               | published   |
| client SDK (experimental)   | `packages/client`                 | `@avatarsd-llc/libtracer-client`        | scaffold    |
| WebSocket transport         | `packages/transport-ws`           | `@avatarsd-llc/libtracer-ws`            | scaffold    |
| WebTransport transport      | `packages/transport-webtransport` | `@avatarsd-llc/libtracer-webtransport`  | scaffold    |

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
