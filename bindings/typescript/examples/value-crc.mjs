// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @file
 * @brief One concept: a VALUE frame with a CRC trailer, and what `decode` says when a
 * byte flips in transit. Core package only — no client, no transport.
 *
 * Run: `node examples/value-crc.mjs` (from `bindings/typescript/`, after `npm run build`).
 */

import assert from 'node:assert/strict';
import { encode, decode, TYPE, ERROR } from '@avatarsd-llc/libtracer';

const opt = { pl: false, ts: false, cr: true, ll: false, cw: false, tf: false };
const reading = Uint8Array.of(0xfc, 0x53, 0x00, 0x00); // 21500, u32 little-endian

// opt.CR = 1 asks the encoder for a CRC-32C trailer over the payload.
const frame = encode({ type: TYPE.VALUE, opt, payload: reading, children: [], trailer: null });
assert.equal(frame.length, 4 + 4 + 4); // header + payload + CRC
console.log(`frame (${frame.length} bytes): ${Buffer.from(frame).toString('hex')}`);

// Decoding checks the trailer and hands back the payload unchanged.
const tlv = decode(frame);
assert.deepEqual(tlv.payload, reading);
assert.equal(tlv.trailer.crc.width, 'CRC32C');
console.log(`decoded payload ok, crc32c 0x${tlv.trailer.crc.value.toString(16)}`);

// Flip one payload bit: the frame still parses, but the CRC no longer matches.
const damaged = Uint8Array.from(frame);
damaged[4] ^= 0x01;
assert.throws(() => decode(damaged), (err) => err.code === ERROR.FRAME_CRC_FAIL);
console.log(`one flipped bit -> ${ERROR.FRAME_CRC_FAIL}`);

// Without the trailer the same damage is invisible to the codec.
const bare = encode({ type: TYPE.VALUE, opt: { ...opt, cr: false }, payload: reading, children: [], trailer: null });
bare[4] ^= 0x01;
decode(bare);
console.log('same flip without a CRC -> decodes silently');
