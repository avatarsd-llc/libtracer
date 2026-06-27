#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// TypeScript codec perf bench — the `lang` axis (TS core) of the ranged
// cross-core perf matrix (ADR-0032, issue #96). It measures decode+encode
// roundtrip latency and throughput over the SHARED conformance vectors under
// tests/conformance/vectors/v1/ (the "vector data" workload — real structured
// TLVs, not synthetic scalars), so a C++-vs-TS surface can later be rendered
// over the SAME inputs.
//
// Output: one machine-parseable RESULT line per vector, in the SAME
// tab-separated column contract as the C++ bench (bench/bench_common.hpp,
// consumed by bench/collate.py):
//
//   RESULT \t system \t mode \t size \t fanout \t endpoints \t pub_s \t
//          deliv_s \t mb_s \t p50ns \t p99ns \t meanns
//
// with system="ts-core", mode="codec", fanout=1, endpoints=1. pub_s and deliv_s
// are roundtrips/sec (one roundtrip == one decode + one encode); mb_s is
// bytes/sec/1e6; p50/p99/mean are per-roundtrip nanoseconds. A human-readable
// per-vector ns/op footer follows for readability. Node/stdlib only — no deps.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, relative, sep, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { decode, encode } from '../src/codec.mjs';

/** @param {string} dir @returns {string[]} absolute paths to every input.bin under dir */
function findInputs(dir) {
  /** @type {string[]} */
  const out = [];
  for (const entry of readdirSync(dir)) {
    const p = join(dir, entry);
    if (statSync(p).isDirectory()) out.push(...findInputs(p));
    else if (entry === 'input.bin') out.push(p);
  }
  return out;
}

/** Target per-vector throughput-phase wall time, so each ns/op is stable. */
const TARGET_NS = 100_000_000n; // 100 ms throughput phase
const LATENCY_SAMPLES = 20_000; // individual timed roundtrips for percentiles
const WARMUP = 5_000;

/**
 * One decode+encode roundtrip on the given bytes (the unit of work).
 *
 * @param {Uint8Array} input
 */
function roundtrip(input) {
  return encode(decode(input));
}

/**
 * Measure a single vector: a tight throughput loop (>= TARGET_NS) for the rate,
 * then a batch of individually-timed roundtrips for the latency percentiles.
 *
 * @param {Uint8Array} input
 * @returns {{iters: number, pubS: number, mbS: number, p50: number, p99: number, mean: number}}
 */
function measure(input) {
  // Warm up the JIT before any timing.
  for (let i = 0; i < WARMUP; i++) roundtrip(input);

  // Calibrate iteration count from a short probe so the timed loop ~= TARGET_NS.
  const probeN = 2_000;
  let t0 = process.hrtime.bigint();
  for (let i = 0; i < probeN; i++) roundtrip(input);
  let probe = process.hrtime.bigint() - t0;
  if (probe <= 0n) probe = 1n;
  const perOp = probe / BigInt(probeN) || 1n;
  let iters = Number(TARGET_NS / perOp);
  if (iters < probeN) iters = probeN;
  if (iters > 50_000_000) iters = 50_000_000;

  // Throughput phase: one tight loop, measure total wall time.
  t0 = process.hrtime.bigint();
  for (let i = 0; i < iters; i++) roundtrip(input);
  const elapsed = process.hrtime.bigint() - t0;
  const secs = Number(elapsed) / 1e9;
  const pubS = iters / secs;
  const mbS = (iters * input.length) / secs / 1e6;

  // Latency phase: time individual roundtrips for the percentiles.
  const samples = new Float64Array(LATENCY_SAMPLES);
  for (let i = 0; i < LATENCY_SAMPLES; i++) {
    const s = process.hrtime.bigint();
    roundtrip(input);
    samples[i] = Number(process.hrtime.bigint() - s);
  }
  samples.sort();
  const at = (p) => samples[Math.min(samples.length - 1, Math.floor(p * samples.length))];
  let sum = 0;
  for (let i = 0; i < samples.length; i++) sum += samples[i];
  return {
    iters,
    pubS,
    mbS,
    p50: Math.round(at(0.5)),
    p99: Math.round(at(0.99)),
    mean: Math.round(sum / samples.length),
  };
}

function main() {
  const self = fileURLToPath(import.meta.url);
  const defaultDir = join(dirname(self), '..', '..', '..', 'tests', 'conformance', 'vectors', 'v1');
  const vectorsDir = process.argv[2] || defaultDir;

  // Sort by POSIX relative path so the row order matches the C++ harness.
  const cases = findInputs(vectorsDir)
    .map((abs) => ({
      rel: relative(vectorsDir, join(abs, '..')).split(sep).join('/'),
      abs,
    }))
    .sort((a, b) => (a.rel < b.rel ? -1 : a.rel > b.rel ? 1 : 0));

  if (cases.length === 0) {
    process.stderr.write(`no input.bin vectors found under ${vectorsDir}\n`);
    process.exit(2);
  }

  /** @type {{rel: string, size: number, m: ReturnType<typeof measure>}[]} */
  const rows = [];
  for (const { rel, abs } of cases) {
    const input = new Uint8Array(readFileSync(abs));
    const m = measure(input);
    rows.push({ rel, size: input.length, m });
    // RESULT line — identical column contract to bench/bench_common.hpp emit().
    process.stdout.write(
      `RESULT\tts-core\tcodec\t${input.length}\t1\t1\t` +
        `${m.pubS.toFixed(0)}\t${m.pubS.toFixed(0)}\t${m.mbS.toFixed(1)}\t` +
        `${m.p50}\t${m.p99}\t${m.mean}\n`,
    );
  }

  // Human-readable footer (per-vector ns/op), to stderr so RESULT stdout stays
  // clean for collate.py-style tooling.
  process.stderr.write(`\n# ts-core codec bench — ${rows.length} vectors (${vectorsDir})\n`);
  process.stderr.write(
    `# ${'vector'.padEnd(34)}${'size'.padStart(7)}${'ns/op'.padStart(12)}` +
      `${'p50'.padStart(9)}${'p99'.padStart(9)}${'M op/s'.padStart(10)}\n`,
  );
  for (const r of rows) {
    process.stderr.write(
      `# ${r.rel.padEnd(34)}${String(r.size + 'B').padStart(7)}${String(r.m.mean).padStart(12)}` +
        `${String(r.m.p50).padStart(9)}${String(r.m.p99).padStart(9)}` +
        `${(r.m.pubS / 1e6).toFixed(2).padStart(10)}\n`,
    );
  }
}

main();
