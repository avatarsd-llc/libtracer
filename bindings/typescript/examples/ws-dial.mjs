// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @file
 * @brief One concept: dial a link. `TransportWs` dials a WebSocket listener and carries one
 * libtracer frame per BINARY message; the client on top is the same one the in-memory
 * examples use.
 *
 * Run: `node examples/ws-dial.mjs` (from `bindings/typescript/`, after `npm run build`).
 */

import assert from 'node:assert/strict';
import { WebSocketServer, WebSocket } from 'ws';
import { TransportWs } from '@avatarsd-llc/libtracer-ws';
import { LibtracerClient, encodeValue } from '@avatarsd-llc/libtracer-client';
import { standInNode } from './stand-in-node.mjs';

// LISTEN: a loopback listener on port 0 (the kernel picks), fronting the stand-in node.
const node = standInNode();
const wss = new WebSocketServer({ host: '127.0.0.1', port: 0 });
await new Promise((resolve) => wss.once('listening', resolve));
wss.on('connection', (sock) =>
  sock.on('message', (data) => node.handle(new Uint8Array(data), (b) => sock.send(b, { binary: true }))),
);
const url = `ws://127.0.0.1:${wss.address().port}`;

// DIAL: connect() resolves once the WebSocket is open; only then can frames cross.
const transport = new TransportWs(url, { WebSocket });
await transport.connect();
console.log(`dialled ${url}`);

const client = new LibtracerClient(transport);
await client.write('/sensor/temp', encodeValue(Uint8Array.of(42)));
assert.equal((await client.read('/sensor/temp')).payload[0], 42);
console.log('write + read crossed the socket');

await transport.close();
await new Promise((resolve) => wss.close(resolve));
