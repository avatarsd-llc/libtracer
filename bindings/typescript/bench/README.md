<!--
SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# TypeScript codec perf bench

The **TS `lang`-axis** of the ranged cross-core perf matrix
([issue #96](https://github.com/avatarsd-llc/libtracer/issues/96),
[ADR-0032](../../../docs/adr/0032-continuous-cross-core-perf-conformance-matrix.md)).

## What it measures

Per conformance vector, the **decode + encode roundtrip** latency and throughput
of the pure-TypeScript core codec (`../src/codec.mjs`):

- **pub_s / deliv_s** — roundtrips per second (one roundtrip = one `decode` + one
  `encode`).
- **mb_s** — bytes per second / 1e6.
- **p50 / p99 / mean** — per-roundtrip nanoseconds.

The workload is the **shared conformance vectors** under
`tests/conformance/vectors/v1/` — the same real, structured TLVs the correctness
harness uses (`../conformance/harness.mjs`). Sharing the inputs is the point: the
C++ bench and this bench measure the SAME vector data, so a C++-vs-TS surface can
later be rendered over one workload (ADR-0032).

## Output contract

One machine-parseable `RESULT` line per vector on **stdout**, in the SAME
tab-separated column contract as the C++ bench (`bench/bench_common.hpp`,
consumed by `bench/collate.py`):

```
RESULT \t system \t mode \t size \t fanout \t endpoints \t pub_s \t deliv_s \t mb_s \t p50ns \t p99ns \t meanns
```

with `system="ts-core"`, `mode="codec"`, `fanout=1`, `endpoints=1`, and
`size` = the vector's byte length. A human-readable per-vector ns/op summary is
written to **stderr** so the `RESULT` stdout stream stays clean for tooling.

## How to run

```sh
node bench/perf.mjs                                  # defaults to ../../tests/conformance/vectors/v1
node bench/perf.mjs /path/to/tests/conformance/vectors/v1
npm run bench
```

Pure ESM, Node stdlib only — no dependencies, no build step. Each vector runs a
warmed-up throughput loop (≥100 ms) plus a batch of individually-timed roundtrips
for the percentiles, using `process.hrtime.bigint()`.
