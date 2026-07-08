#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Conformance harness for the shared vectors under tests/conformance/vectors/v1/.
// Mirrors core/tests/conformance_runner.cpp --tap: for every vector directory
// containing an input.bin, it checks the round-trip
//     encode(decode(input.bin)) == input.bin   (byte-for-byte)
// and for every directory containing a reject.bin (negative case), that
//     decode(reject.bin) FAILS with the error named by expected.json's "reject"
// then emits TAP version 13 to stdout (see tests/conformance/HARNESS.md). Exit 0
// iff every vector is `ok`. Node/stdlib only — no build step, no dependencies.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { decode, encode } from '../src/codec.mjs';

/** @param {string} hex @returns {Uint8Array} */
function fromHex(hex) {
  if (hex.length % 2 !== 0) throw new Error('BAD_HEX');
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) {
    const byte = Number.parseInt(hex.substr(i * 2, 2), 16);
    if (Number.isNaN(byte)) throw new Error('BAD_HEX');
    out[i] = byte;
  }
  return out;
}

/** @param {Uint8Array} bytes @returns {string} */
function toHex(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += bytes[i].toString(16).padStart(2, '0');
  return s;
}

/**
 * `--roundtrip`: differential-fuzz batch mode. Read one hex frame per stdin line;
 * for each, print decode->encode re-encoded as hex, or `ERR:<reason>` on a decode
 * failure. One output line per input line. The driver (tests/conformance/diff_fuzz.py)
 * compares these against the C++ core + the canonical generator, byte-for-byte.
 */
function runRoundtrip() {
  const text = readFileSync(0, 'utf8');
  const lines = text.split('\n');
  // A trailing newline yields a final empty element — drop only that one.
  if (lines.length && lines[lines.length - 1] === '') lines.pop();
  const out = [];
  for (const raw of lines) {
    const line = raw.replace(/[\r\n]+$/, '');
    if (line === '') {
      out.push('ERR:EMPTY_LINE');
      continue;
    }
    try {
      out.push(toHex(encode(decode(fromHex(line)))));
    } catch (err) {
      out.push(`ERR:${err && err.code ? err.code : err && err.message ? err.message : err}`);
    }
  }
  // Do NOT process.exit() here: a large write to a pipe is async and exit() would
  // truncate it. Returning lets the event loop drain stdout, then exit 0 naturally.
  process.stdout.write(out.join('\n') + '\n');
}

/** @param {string} dir @returns {string[]} absolute paths to every input.bin / reject.bin under dir */
function findInputs(dir) {
  /** @type {string[]} */
  const out = [];
  for (const entry of readdirSync(dir)) {
    const p = join(dir, entry);
    if (statSync(p).isDirectory()) out.push(...findInputs(p));
    else if (entry === 'input.bin' || entry === 'reject.bin') out.push(p);
  }
  return out;
}

/**
 * One negative case: decode(reject.bin) MUST fail with exactly the error named
 * by the sibling expected.json's top-level "reject" field.
 *
 * @param {string} rejectBin absolute path to the case's reject.bin
 * @returns {{ok: boolean, diag: string}}
 */
function checkReject(rejectBin) {
  const expected = JSON.parse(readFileSync(join(rejectBin, '..', 'expected.json'), 'utf8'));
  const want = expected.reject;
  if (typeof want !== 'string' || want === '') {
    return { ok: false, diag: 'reject.bin without a "reject" expectation in expected.json' };
  }
  try {
    decode(new Uint8Array(readFileSync(rejectBin)));
  } catch (err) {
    const got = err && err.code ? err.code : String(err && err.message ? err.message : err);
    return got === want
      ? { ok: true, diag: '' }
      : { ok: false, diag: `decode failed with ${got}, expected ${want}` };
  }
  return { ok: false, diag: `decode succeeded, expected ${want}` };
}

/** @param {Uint8Array} a @param {Uint8Array} b */
function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

function main() {
  if (process.argv[2] === '--roundtrip') {
    runRoundtrip();
    return;
  }

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
      if (abs.endsWith('reject.bin')) {
        ({ ok, diag } = checkReject(abs));
      } else {
        const input = new Uint8Array(readFileSync(abs));
        ok = bytesEqual(encode(decode(input)), input);
        if (!ok) diag = 'round-trip differs from input.bin';
      }
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
