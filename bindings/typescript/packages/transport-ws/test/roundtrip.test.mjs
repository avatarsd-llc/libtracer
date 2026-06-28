// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Node round-trip test: stand up a local `ws` echo server, dial it with the
// TransportWs client, send a libtracer TLV (built with the core package
// @avatarsd-llc/libtracer) as one BINARY frame, and assert the echoed bytes are
// byte-identical and decode back to the same TLV. No fixed sleeps — everything is
// event-driven behind a deadline.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { WebSocketServer, WebSocket } from 'ws';
import { TransportWs } from '../dist/index.js';
import { encode, decode, equal, TYPE } from '@avatarsd-llc/libtracer';

/** A simple opaque NAME TLV built via the cross-validated core codec. */
function buildTlv(text) {
  return {
    type: TYPE.NAME,
    opt: { pl: false, ts: false, cr: false, ll: false, cw: false, tf: false },
    payload: new TextEncoder().encode(text),
    children: [],
    trailer: null,
  };
}

/** Resolve when an EventEmitter emits `event`, behind a deadline. */
function once(emitter, event, ms) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`timed out waiting for ${event}`)), ms);
    emitter.once(event, (...args) => {
      clearTimeout(timer);
      resolve(args);
    });
  });
}

test('TLV round-trips byte-identical over TransportWs against a ws echo server', async () => {
  const wss = new WebSocketServer({ host: '127.0.0.1', port: 0 });
  await once(wss, 'listening', 5000);
  const port = wss.address().port;

  // Echo every inbound BINARY frame straight back as a BINARY frame.
  wss.on('connection', (sock) => {
    sock.on('message', (data, isBinary) => {
      sock.send(data, { binary: isBinary });
    });
  });

  const tlv = buildTlv('sensor-7');
  const frame = encode(tlv);

  const transport = new TransportWs(`ws://127.0.0.1:${port}`, { WebSocket });

  const received = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('no echo within deadline')), 5000);
    transport.onFrame((bytes) => {
      clearTimeout(timer);
      resolve(bytes);
    });
  });

  await transport.connect();
  transport.send(frame);
  const got = await received;

  // Byte-identical echo (the WS layer masked the client frame and unmasked the
  // server frame transparently; the carried TLV bytes are untouched).
  assert.deepEqual([...got], [...frame], 'echoed bytes are byte-identical to the sent TLV');

  // ...and the received bytes decode back to the same TLV via the core codec.
  assert.ok(equal(decode(got), tlv), 'received bytes decode to an equal TLV');

  await transport.close();
  await new Promise((resolve) => wss.close(resolve));
});

test('TransportWs.send before connect throws', () => {
  const transport = new TransportWs('ws://127.0.0.1:1', { WebSocket });
  assert.throws(() => transport.send(new Uint8Array([1, 2, 3])), /before connect/);
});
