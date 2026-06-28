// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Vector-level correctness (ADR-0034 "the gate"): the client's outbound payload
// builders MUST produce the shared conformance vectors byte-for-byte, and the
// inbound path MUST decode VALUE vectors into the right payload. This pins
// wire-compatibility WITHOUT a live peer.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { decode, TYPE } from '@avatarsd-llc/libtracer';
import { LibtracerClient, encodeValue, encodePath, encodeSubscriber } from '../dist/index.js';

const HERE = dirname(fileURLToPath(import.meta.url));
// test/ -> client -> packages -> typescript -> bindings -> repo root
const VECTORS = join(HERE, '..', '..', '..', '..', '..', 'tests', 'conformance', 'vectors', 'v1');

/** Read a vector's canonical `input.bin` bytes. */
function vector(rel) {
  return new Uint8Array(readFileSync(join(VECTORS, ...rel.split('/'), 'input.bin')));
}

/** @param {Uint8Array} a @param {Uint8Array} b */
function sameBytes(a, b) {
  return a.length === b.length && a.every((x, i) => x === b[i]);
}

test('encodeSubscriber matches the subscriber-path vector byte-for-byte', () => {
  const expected = vector('tlv-types/subscriber-path');
  const built = encodeSubscriber(['sensor', 'temp']);
  assert.ok(sameBytes(built, expected), `built ${Buffer.from(built).toString('hex')} != ${Buffer.from(expected).toString('hex')}`);
  // Same via the static method on the client class.
  assert.ok(sameBytes(LibtracerClient.encodeSubscriber(['sensor', 'temp']), expected));
});

test('encodePath matches the path-sensor-temp vector byte-for-byte', () => {
  assert.ok(sameBytes(encodePath(['sensor', 'temp']), vector('path/path-sensor-temp')));
});

test('encodeValue matches the value-bool-true vector byte-for-byte', () => {
  assert.ok(sameBytes(encodeValue(new Uint8Array([0x01])), vector('tlv-types/value-bool-true')));
});

test('encodeValue (LL=1) matches the value-ll-u32 vector byte-for-byte', () => {
  const built = encodeValue(new Uint8Array([0xaa, 0xbb, 0xcc]), { longLength: true });
  assert.ok(sameBytes(built, vector('tlv-types/value-ll-u32')));
});

test('encodeValue (absolute wire timestamp) matches the value-ts-abs vector byte-for-byte', () => {
  // bytes_le 08 07 06 05 04 03 02 01 => u64 LE 0x0102030405060708.
  const built = encodeValue(new Uint8Array([0xaa, 0xbb, 0xcc]), { timestampNs: 0x0102030405060708n });
  assert.ok(sameBytes(built, vector('tlv-types/value-ts-abs')));
});

test('inbound: a VALUE vector decodes to its payload via the core codec', () => {
  const tlv = decode(vector('tlv-types/value-ll-u32'));
  assert.equal(tlv.type, TYPE.VALUE);
  assert.ok(sameBytes(tlv.payload, new Uint8Array([0xaa, 0xbb, 0xcc])));
});

test('invalid path segments are rejected before any bytes are emitted', () => {
  assert.throws(() => encodePath([]), /at least one segment/);
  assert.throws(() => encodePath(['a/b']), /reserved character/);
  assert.throws(() => encodePath(['']), /1\.\.64/);
  assert.throws(() => encodePath(['x'.repeat(65)]), /1\.\.64/);
});
