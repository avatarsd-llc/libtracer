// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @file
 * @brief One concept: write a remote vertex, then read it back. Each call is one FWD
 * frame out and one FWD{REPLY} back, matched by the path it answers for.
 *
 * Run: `node examples/write-read.mjs` (from `bindings/typescript/`, after `npm run build`).
 */

import assert from 'node:assert/strict';
import { LibtracerClient, encodeValue } from '@avatarsd-llc/libtracer-client';
import { standInNode } from './stand-in-node.mjs';

// The client takes any transport with send() + onFrame(); here, an in-memory link.
const client = new LibtracerClient(standInNode().loopback());

// A write carries a complete VALUE TLV; it resolves when the node acks.
const reading = Uint8Array.of(0xfc, 0x53, 0x00, 0x00); // 21500, u32 little-endian
await client.write('/sensor/temp', encodeValue(reading));
console.log('wrote /sensor/temp');

// A read resolves to the decoded VALUE: the last value written, nothing queued.
const value = await client.read('/sensor/temp');
assert.deepEqual(value.payload, reading);
console.log(`read /sensor/temp -> ${Buffer.from(value.payload).readUInt32LE(0)}`);

// Last writer wins: a second write replaces the first.
await client.write('/sensor/temp', encodeValue(Uint8Array.of(0, 0, 0, 0)));
assert.deepEqual((await client.read('/sensor/temp')).payload, Uint8Array.of(0, 0, 0, 0));
console.log('second write replaced the first');
