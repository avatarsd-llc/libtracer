// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Mock-transport round-trip (ADR-0034 "the gate"): drive LibtracerClient over an
// in-memory fake ClientTransport — no socket. Assert the bytes `write`/`subscribe`
// emit, and that feeding an inbound VALUE (and a ROUTER-wrapped VALUE) frame fires
// the handler with the decoded payload. A second test drives a REAL TransportWs
// against a local `ws` echo server to prove the seam is structurally satisfied.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { WebSocketServer, WebSocket } from 'ws';
import { encode, decode, TYPE } from '@avatarsd-llc/libtracer';
import { TransportWs } from '@avatarsd-llc/libtracer-ws';
import { LibtracerClient, encodeValue, encodeSubscriber } from '../dist/index.js';

/** An in-memory ClientTransport: records sent frames, lets a test inject inbound ones. */
class FakeTransport {
  constructor() {
    this.sent = [];
    this.receiver = null;
  }
  send(frame) {
    this.sent.push(new Uint8Array(frame));
  }
  onFrame(receiver) {
    this.receiver = receiver;
  }
  /** Simulate one inbound frame arriving from the wire. */
  inject(frame) {
    if (this.receiver) this.receiver(new Uint8Array(frame));
  }
}

/** @param {Uint8Array} a @param {Uint8Array} b */
function sameBytes(a, b) {
  return a.length === b.length && a.every((x, i) => x === b[i]);
}

/** Build a ROUTER{ …, NAME "data", <wrapped> } TLV (the wrapped data is the last child). */
function routerWrap(dataTlv) {
  const name = (s) => ({
    type: TYPE.NAME,
    opt: { pl: false, ts: false, cr: false, ll: false, cw: false, tf: false },
    payload: new TextEncoder().encode(s),
    children: [],
    trailer: null,
  });
  return {
    type: TYPE.ROUTER,
    opt: { pl: true, ts: false, cr: false, ll: false, cw: false, tf: false },
    payload: new Uint8Array(0),
    children: [name('data'), dataTlv],
    trailer: null,
  };
}

test('write emits exactly the VALUE frame bytes', () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);
  client.write(new Uint8Array([0x01]));
  assert.equal(t.sent.length, 1);
  assert.ok(sameBytes(t.sent[0], encodeValue(new Uint8Array([0x01]))));
});

test('subscribe emits the SUBSCRIBER frame and delivers an injected VALUE to the handler', () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);

  const got = [];
  const sub = client.subscribe(['sensor', 'temp'], (value, tlv) => {
    got.push({ value, type: tlv.type });
  });

  // Outbound: exactly the subscriber-path payload.
  assert.equal(t.sent.length, 1);
  assert.ok(sameBytes(t.sent[0], encodeSubscriber(['sensor', 'temp'])));
  assert.deepEqual([...sub.targetPath], ['sensor', 'temp']);

  // Inbound: a VALUE delivery fires the handler with the decoded payload.
  t.inject(encodeValue(new Uint8Array([0xde, 0xad])));
  assert.equal(got.length, 1);
  assert.equal(got[0].type, TYPE.VALUE);
  assert.ok(sameBytes(got[0].value, new Uint8Array([0xde, 0xad])));

  // close() detaches locally: further inbound frames do not fire the handler.
  sub.close();
  t.inject(encodeValue(new Uint8Array([0xbe, 0xef])));
  assert.equal(got.length, 1);
});

test('inbound ROUTER envelope is shed to its wrapped VALUE before delivery', () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);
  const got = [];
  client.onValue((value) => got.push(value));

  const wrapped = encode(routerWrap(decode(encodeValue(new Uint8Array([0xab, 0xcd])))));
  t.inject(wrapped);

  assert.equal(got.length, 1);
  assert.ok(sameBytes(got[0], new Uint8Array([0xab, 0xcd])), 'shed VALUE payload');
});

test('inbound decode failures route to onError, never throw into the transport callback', () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);
  const errors = [];
  client.onError((err) => errors.push(err));

  // A truncated frame (header claims more bytes than present).
  assert.doesNotThrow(() => t.inject(new Uint8Array([0x01, 0x00, 0x05, 0x00, 0x01])));
  assert.equal(errors.length, 1);
  assert.equal(errors[0].code, 'FRAME_TRUNCATED');
});

test('a real TransportWs satisfies the ClientTransport seam (echo round-trip)', async () => {
  const wss = new WebSocketServer({ host: '127.0.0.1', port: 0 });
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('listen timeout')), 5000);
    wss.once('listening', () => {
      clearTimeout(timer);
      resolve();
    });
  });
  const port = wss.address().port;
  wss.on('connection', (sock) => {
    sock.on('message', (data, isBinary) => sock.send(data, { binary: isBinary }));
  });

  const transport = new TransportWs(`ws://127.0.0.1:${port}`, { WebSocket });
  await transport.connect();
  const client = new LibtracerClient(transport);

  const received = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('no echo within deadline')), 5000);
    client.onValue((value) => {
      clearTimeout(timer);
      resolve(value);
    });
  });

  client.write(new Uint8Array([0x07, 0x08]));
  const value = await received;
  assert.ok(sameBytes(value, new Uint8Array([0x07, 0x08])), 'echoed VALUE payload round-trips');

  await transport.close();
  await new Promise((resolve) => wss.close(resolve));
});
