# @avatarsd-llc/libtracer

The TypeScript core for [libtracer](https://github.com/avatarsd-llc/libtracer) —
a native, pure-TypeScript implementation of the in-process protocol core (the
L2/L3 wire codec today). It carries **no transports**; those live in separate
per-transport packages (e.g. `@avatarsd-llc/libtracer-ws`). The codec is gated
byte-for-byte against the shared conformance vectors and cross-matched with the
C++ and Rust cores (ADR-0028).

## Install

```sh
npm install @avatarsd-llc/libtracer
```

ESM only; Node ≥ 18 (or any modern bundler/browser).

## Use

```ts
// Barrel entry — everything the core exports.
import { decode, encode, TYPE } from '@avatarsd-llc/libtracer';

// Or import just the wire layer for tree-shaking (subpath entry).
import { decode, encode } from '@avatarsd-llc/libtracer/wire';

const tlv = decode(bytes);
const reencoded = encode(tlv); // byte-for-byte round-trip
```

### Subpath / layer entries

The package ships a tree-shakeable `exports` map. Today only the wire layer is
implemented in TS, so the live entries are:

| Subpath  | Layer       | Status      |
| -------- | ----------- | ----------- |
| `.`      | barrel      | implemented |
| `./wire` | L2/L3 codec | implemented |

`./mem` (L0), `./view` (L1) and `./graph` (L4) are **reserved** for when those
layers are ported to TS — see
[ADR-0033](../../../../docs/adr/0033-npm-subpackage-monorepo.md). The subpath
mechanism (not one-package-per-layer) is the chosen way to slice modules.

## Development

This package is one member of the `bindings/typescript` npm workspace.

```sh
# from bindings/typescript (the workspace root)
npm install
npm run build         # tsc -> dist/  (all workspaces)
npm test              # conformance harness over the shared vectors
```

The conformance harness (`conformance/harness.mjs`) and perf bench
(`bench/perf.mjs`) import the raw `src/codec.mjs` directly and need **no build
step** — that keeps the polyglot conformance driver
(`tests/conformance/run-all.py`) dependency-free.

## Releasing

Tag the repo with `ts-vX.Y.Z` to trigger publish (see
`.github/workflows/publish-npm.yml` once added).
