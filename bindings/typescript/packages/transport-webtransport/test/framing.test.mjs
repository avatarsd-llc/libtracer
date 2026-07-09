// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief The pure length-prefix record codec (ADR-0043 Phase B).
 *
 * The wire vectors here pin the format the C++ side speaks (tcp_transport_t /
 * quic_transport_t / webtransport_transport_t: `u32-LE length ++ frame`),
 * including the split/coalesced chunk behavior of a real QUIC stream.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  PREFIX_BYTES,
  MAX_FRAME,
  MalformedPrefixError,
  encodeRecord,
  FrameReassembler,
} from '../dist/framing.js';

test('encodeRecord pins the u32-LE prefix wire shape', () => {
  const frame = Uint8Array.from([0xaa, 0xbb, 0xcc, 0xdd, 0xee]);
  const record = encodeRecord(frame);
  // 5 as u32 LITTLE-endian, then the frame bytes — the exact C++ layout.
  assert.deepEqual([...record], [0x05, 0x00, 0x00, 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee]);
  assert.equal(PREFIX_BYTES, 4);
});

test('encodeRecord: a 300-byte length lands in the second LE byte', () => {
  const record = encodeRecord(new Uint8Array(300));
  assert.deepEqual([...record.slice(0, 4)], [0x2c, 0x01, 0x00, 0x00]); // 300 = 0x012c
});

test('encodeRecord rejects a frame beyond MAX_FRAME', () => {
  // A fake oversized view without allocating 16 MiB.
  const fake = { byteLength: MAX_FRAME + 1 };
  assert.throws(() => encodeRecord(fake), RangeError);
});

test('reassembler: one exact record delivers one frame', () => {
  const r = new FrameReassembler();
  const frame = Uint8Array.from([1, 2, 3]);
  const frames = r.push(encodeRecord(frame));
  assert.equal(frames.length, 1);
  assert.deepEqual([...frames[0]], [1, 2, 3]);
  assert.equal(r.buffered, 0);
});

test('reassembler: a record split byte-by-byte reassembles into ONE frame', () => {
  const r = new FrameReassembler();
  const frame = Uint8Array.from([9, 8, 7, 6, 5, 4]);
  const record = encodeRecord(frame);
  const got = [];
  for (let i = 0; i < record.byteLength; i += 1) {
    got.push(...r.push(record.slice(i, i + 1)));
  }
  assert.equal(got.length, 1);
  assert.deepEqual([...got[0]], [...frame]);
});

test('reassembler: two records coalesced into one chunk deliver TWO frames', () => {
  const r = new FrameReassembler();
  const f1 = Uint8Array.from([0x11]);
  const f2 = Uint8Array.from([0x22, 0x33]);
  const both = new Uint8Array([...encodeRecord(f1), ...encodeRecord(f2)]);
  const frames = r.push(both);
  assert.equal(frames.length, 2);
  assert.deepEqual([...frames[0]], [...f1]);
  assert.deepEqual([...frames[1]], [...f2]);
});

test('reassembler: a zero-length record is a no-op (skipped, framing intact)', () => {
  const r = new FrameReassembler();
  const f = Uint8Array.from([0x42]);
  const chunk = new Uint8Array([...encodeRecord(new Uint8Array(0)), ...encodeRecord(f)]);
  const frames = r.push(chunk);
  assert.equal(frames.length, 1);
  assert.deepEqual([...frames[0]], [0x42]);
});

test('reassembler: an oversize prefix throws MalformedPrefixError', () => {
  const r = new FrameReassembler();
  const bad = new Uint8Array(4);
  new DataView(bad.buffer).setUint32(0, MAX_FRAME + 1, true);
  assert.throws(() => r.push(bad), MalformedPrefixError);
});

test('reassembler: frames are independent copies of the stream buffer', () => {
  const r = new FrameReassembler();
  const chunk = encodeRecord(Uint8Array.from([7, 7, 7]));
  const [frame] = r.push(chunk);
  chunk.fill(0);
  assert.deepEqual([...frame], [7, 7, 7]);
});
