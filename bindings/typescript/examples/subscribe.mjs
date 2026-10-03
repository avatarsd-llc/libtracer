// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @file
 * @brief One concept: subscribe to a remote producer. Subscribing is itself a write —
 * a SUBSCRIBER into `/sensor/temp:subscribers[]` — and each later write to the producer
 * arrives as a delivery to the handler.
 *
 * Run: `node examples/subscribe.mjs` (from `bindings/typescript/`, after `npm run build`).
 */

import assert from 'node:assert/strict';
import { LibtracerClient, encodeValue } from '@avatarsd-llc/libtracer-client';
import { standInNode } from './stand-in-node.mjs';

const node = standInNode();
const consumer = new LibtracerClient(node.loopback());
const producer = new LibtracerClient(node.loopback());

// The handler fires inline, once per delivered VALUE.
const seen = [];
const unsubscribe = await consumer.subscribe('/sensor/temp', (value) => seen.push(value[0]));
console.log('subscribed to /sensor/temp');

// Two writes by another client -> two deliveries, in order.
await producer.write('/sensor/temp', encodeValue(Uint8Array.of(20)));
await producer.write('/sensor/temp', encodeValue(Uint8Array.of(21)));
assert.deepEqual(seen, [20, 21]);
console.log(`delivered: ${seen.join(', ')}`);

// unsubscribe() is a local detach: the handler stops firing.
unsubscribe();
await producer.write('/sensor/temp', encodeValue(Uint8Array.of(22)));
assert.deepEqual(seen, [20, 21]);
console.log('after unsubscribe: nothing more delivered');
