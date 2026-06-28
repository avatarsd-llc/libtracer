// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// RFC 6455 vector-agreement test. Every assertion here mirrors a known vector in
// the C++ codec test (core/tests/ws_test.cpp). Matching these byte-for-byte is
// the cross-implementation agreement guarantee: the TS codec produces/accepts the
// SAME bytes as the C++ `tr::net::ws` codec, so a TS client interoperates with
// the C++ transport_ws server.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  Opcode,
  acceptKey,
  encodeFrame,
  encodeClientFrame,
  decodeFrame,
} from '../dist/ws.js';

const enc = (s) => new Uint8Array([...s].map((c) => c.charCodeAt(0)));
const str = (b) => String.fromCharCode(...b);

test('accept_key matches RFC 6455 §1.3 vector', () => {
  assert.equal(acceptKey('dGhlIHNhbXBsZSBub25jZQ=='), 's3pPLMBiTxaQ9kYGzzhZRbK+xOo=');
});

test('masked client "Hello" frame (RFC 6455 §5.7) decodes', () => {
  const maskedHello = new Uint8Array([
    0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58,
  ]);
  const dec = decodeFrame(maskedHello);
  assert.ok(dec, 'frame decodes');
  assert.equal(dec.frame.op, Opcode.TEXT, 'opcode == TEXT');
  assert.equal(dec.frame.fin, true, 'FIN set');
  assert.equal(str(dec.frame.payload), 'Hello', 'payload unmasks to "Hello"');
  assert.equal(dec.consumed, 11, 'consumed == 11 bytes');
});

test('encodeClientFrame reproduces the RFC 6455 §5.7 masked "Hello" bytes', () => {
  // mask key 0x37fa213d is the §5.7 example key; the output must be byte-identical
  // to the C++ ws::encode_client_frame for the same key.
  const out = encodeClientFrame(Opcode.TEXT, enc('Hello'), 0x37fa213d);
  assert.deepEqual(
    [...out],
    [0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58],
  );
});

test('encode_frame(BINARY, "Hi") == 82 02 H i and round-trips', () => {
  const out = encodeFrame(Opcode.BINARY, enc('Hi'));
  assert.deepEqual([...out], [0x82, 0x02, 0x48, 0x69]);

  const dec = decodeFrame(out);
  assert.ok(dec);
  assert.equal(dec.frame.op, Opcode.BINARY, 'round-trip opcode == BINARY');
  assert.equal(dec.frame.fin, true, 'round-trip FIN set');
  assert.equal(str(dec.frame.payload), 'Hi', 'round-trip payload == "Hi"');
  assert.equal(dec.consumed, out.length, 'round-trip consumes whole buffer');
});

test('1-byte buffer returns null (need-more)', () => {
  assert.equal(decodeFrame(new Uint8Array([0x81])), null);
});

test('200-byte payload uses the 126 marker + 2-byte big-endian length', () => {
  const payload = new Uint8Array(200).fill(0xab);
  const out = encodeFrame(Opcode.BINARY, payload);
  assert.equal(out.length, 2 + 2 + 200, 'frame is 2 + 2-byte-len + 200 payload');
  assert.equal(out[1], 126, 'uses the 126 extended-length marker');
  assert.equal(out[2], 0x00, 'length high byte');
  assert.equal(out[3], 0xc8, 'length low byte == 200 (0xC8)');

  const dec = decodeFrame(out);
  assert.ok(dec);
  assert.equal(dec.frame.payload.length, 200, 'decoded payload is 200 bytes');
  assert.deepEqual([...dec.frame.payload], [...payload], 'payload round-trips byte-exactly');
  assert.equal(dec.consumed, out.length, 'consumes whole buffer');
});

test('64-bit over-long length is rejected (null), no overflow/OOB read', () => {
  const evil = new Uint8Array([
    0x82, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xaa, 0xbb,
  ]);
  assert.equal(decodeFrame(evil), null);
});

test('decodeFrame leaves trailing bytes for the next frame', () => {
  // Two BINARY frames concatenated: decode the first, report what it consumed.
  const a = encodeFrame(Opcode.BINARY, enc('Hi'));
  const b = encodeFrame(Opcode.BINARY, enc('Yo'));
  const both = new Uint8Array([...a, ...b]);
  const dec = decodeFrame(both);
  assert.ok(dec);
  assert.equal(dec.consumed, a.length, 'consumes only the first frame');
  assert.equal(str(dec.frame.payload), 'Hi');
  const rest = decodeFrame(both.subarray(dec.consumed));
  assert.ok(rest);
  assert.equal(str(rest.frame.payload), 'Yo');
});
