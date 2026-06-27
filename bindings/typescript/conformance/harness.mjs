#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Conformance harness for the shared vectors under tests/conformance/vectors/v1/.
// Mirrors core/tests/conformance_runner.cpp --tap: for every vector directory
// containing an input.bin, it checks the round-trip
//     encode(decode(input.bin)) == input.bin   (byte-for-byte)
// and emits TAP version 13 to stdout (see tests/conformance/HARNESS.md). Exit 0
// iff every vector is `ok`. Node/stdlib only — no build step, no dependencies.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, relative, sep } from 'node:path';
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

/** @param {Uint8Array} a @param {Uint8Array} b */
function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

function main() {
  const vectorsDir = process.argv[2];
  if (!vectorsDir) {
    const self = fileURLToPath(import.meta.url);
    process.stderr.write(`usage: node ${self} <vectors-dir>\n`);
    process.exit(2);
  }

  // Sort by POSIX relative path so keys match the C++ harness across platforms.
  const cases = findInputs(vectorsDir)
    .map((abs) => ({
      rel: relative(vectorsDir, join(abs, '..')).split(sep).join('/'),
      abs,
    }))
    .sort((a, b) => (a.rel < b.rel ? -1 : a.rel > b.rel ? 1 : 0));

  const lines = ['TAP version 13', `1..${cases.length}`];
  let n = 0;
  let fails = 0;
  for (const { rel, abs } of cases) {
    let ok = false;
    let diag = '';
    try {
      const input = new Uint8Array(readFileSync(abs));
      ok = bytesEqual(encode(decode(input)), input);
      if (!ok) diag = 'round-trip differs from input.bin';
    } catch (err) {
      diag = err && err.code ? err.code : String(err && err.message ? err.message : err);
    }
    n += 1;
    lines.push(`${ok ? 'ok' : 'not ok'} ${n} - ${rel}`);
    if (!ok) {
      lines.push(`# ${diag}`);
      fails += 1;
    }
  }

  process.stdout.write(lines.join('\n') + '\n');
  process.exit(fails === 0 ? 0 : 1);
}

main();
