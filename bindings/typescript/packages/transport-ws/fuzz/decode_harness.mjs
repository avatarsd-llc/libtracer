// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief ws differential-fuzz harness (#60 / hardening) — the TypeScript side
 * of the RFC 6455 WebSocket frame-decoder differential fuzzer.
 *
 * The twin of the C++ core/tests/ws_fuzz_harness.cpp: it reads one hex-encoded
 * frame per stdin line and prints the BYTE-IDENTICAL canonical decode result,
 * so tests/conformance/ws_diff_fuzz.py can compare the two cores line-for-line.
 *
 * Contract (one deterministic line of stdout per stdin line):
 *   - OK\t<opcode>\t<fin>\t<consumed>\t<payload-hex>  on a full decode, where
 *     <opcode> is the raw 4-bit opcode (decimal), <fin> is 0/1, <consumed> is the
 *     bytes consumed from the front of the buffer, and <payload-hex> is the
 *     unmasked payload as lowercase hex (empty for a zero-length payload);
 *   - NEED_MORE                                        on decodeFrame() -> null
 *     (truncated header/length/mask/payload, or a 64-bit over-long length on a
 *     short buffer);
 *   - ERR:<reason>                                     on a non-hex / empty line.
 *
 * Imports the compiled codec from dist/ (run `npm run build` first), matching the
 * other transport-ws tests which import from ../dist/ws.js.
 */

import { decodeFrame } from '../dist/ws.js';

function fromHex(s) {
  if (s.length % 2 !== 0) return null;
  const out = new Uint8Array(s.length / 2);
  for (let i = 0; i < s.length; i += 2) {
    const byte = parseInt(s.slice(i, i + 2), 16);
    if (Number.isNaN(byte) || !/^[0-9a-fA-F]{2}$/.test(s.slice(i, i + 2))) return null;
    out[i / 2] = byte;
  }
  return out;
}

function toHex(bytes) {
  let out = '';
  for (const b of bytes) out += b.toString(16).padStart(2, '0');
  return out;
}

function decodeLine(line) {
  // Strip a trailing CR (and any stray newline) the way the C++ harness does.
  const trimmed = line.replace(/[\r\n]+$/, '');
  if (trimmed.length === 0) return 'ERR:EMPTY_LINE';
  const bytes = fromHex(trimmed);
  if (bytes === null) return 'ERR:BAD_HEX';
  const dec = decodeFrame(bytes);
  if (dec === null) return 'NEED_MORE';
  const { frame, consumed } = dec;
  return `OK\t${frame.op}\t${frame.fin ? 1 : 0}\t${consumed}\t${toHex(frame.payload)}`;
}

async function main() {
  const chunks = [];
  for await (const chunk of process.stdin) chunks.push(chunk);
  const input = Buffer.concat(chunks).toString('utf8');
  // Split on \n; a trailing newline yields a final empty element we drop, so the
  // output line count matches the input frame count exactly (the driver asserts).
  const lines = input.split('\n');
  if (lines.length > 0 && lines[lines.length - 1] === '') lines.pop();
  const out = lines.map(decodeLine);
  process.stdout.write(out.length ? out.join('\n') + '\n' : '');
}

main();
