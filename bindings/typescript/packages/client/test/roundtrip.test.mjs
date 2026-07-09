// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief Mock-transport round-trip (the client "gate", now over RFC-0004 FWD):
 * drive LibtracerClient over an in-memory fake ClientTransport — no socket.
 *
 * Assert the exact FWD frame bytes `write`/`subscribe` emit, that a
 * source-routed FWD{REPLY} resolves the pending op, and that an inbound
 * delivery (a FWD{WRITE} carrying a VALUE — delivery-is-a-write, RFC-0004 §D —
 * and a bare / ROUTER-wrapped VALUE) fires the handler with the decoded
 * payload. A final test drives a REAL TransportWs against a local `ws` echo
 * server to prove the seam is structurally satisfied end to end.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { WebSocketServer, WebSocket } from 'ws';
import { encode, decode, TYPE } from '@avatarsd-llc/libtracer';
import { TransportWs } from '@avatarsd-llc/libtracer-ws';
import {
  LibtracerClient,
  encodeValue,
  encodeSubscriber,
  encodeFwd,
  FWD_OP,
  FWD_KIND,
} from '../dist/index.js';

/** @brief An in-memory ClientTransport: records sent frames, lets a test inject inbound ones. */
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
  /** @brief Simulate one inbound frame arriving from the wire. */
  inject(frame) {
    if (this.receiver) this.receiver(new Uint8Array(frame));
  }
}

/** @brief Byte-for-byte equality. @param {Uint8Array} a @param {Uint8Array} b */
function sameBytes(a, b) {
  return a.length === b.length && a.every((x, i) => x === b[i]);
}

/** @brief A source-routed FWD{REPLY, RESULT} the responder sends back (default reply-ep "client"). */
function resultReply(payload) {
  return encodeFwd({
    op: FWD_OP.REPLY,
    dst: ['client'],
    src: ['sensor', 'temp'],
    kind: FWD_KIND.RESULT,
    payload,
  });
}

/** @brief A delivery: a FWD{WRITE} carrying a VALUE, addressed back to the reply endpoint. */
function delivery(valueBytes) {
  return encodeFwd({
    op: FWD_OP.WRITE,
    dst: ['client'],
    src: ['sensor', 'temp'],
    payload: encodeValue(valueBytes),
  });
}

/** @brief Build a ROUTER{ …, NAME "data", <wrapped> } TLV (the wrapped data is the last child). */
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

test('write emits exactly a FWD{WRITE} frame and resolves on the RESULT reply', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);

  const valueTLV = encodeValue(new Uint8Array([0x01]));
  const done = client.write('/sensor/temp', valueTLV);

  // The FWD frame is put on the wire synchronously, before any reply.
  assert.equal(t.sent.length, 1);
  const expected = encodeFwd({
    op: FWD_OP.WRITE,
    dst: ['sensor', 'temp'],
    src: ['client'],
    payload: valueTLV,
  });
  assert.ok(
    sameBytes(t.sent[0], expected),
    `sent ${Buffer.from(t.sent[0]).toString('hex')} != ${Buffer.from(expected).toString('hex')}`,
  );

  // The source-routed RESULT reply resolves the pending write.
  t.inject(resultReply());
  await done;
});

test('read emits a FWD{READ} and resolves with the RESULT value', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);

  const p = client.read('/sensor/temp');
  assert.equal(t.sent.length, 1);
  assert.ok(sameBytes(t.sent[0], encodeFwd({ op: FWD_OP.READ, dst: ['sensor', 'temp'], src: ['client'] })));

  const value = encodeValue(new Uint8Array([0xd2, 0x04, 0x00, 0x00]));
  t.inject(resultReply(value));
  const tlv = await p;
  assert.equal(tlv.type, TYPE.VALUE);
  assert.ok(sameBytes(tlv.payload, new Uint8Array([0xd2, 0x04, 0x00, 0x00])));
});

test('a kind=ERROR reply rejects the pending op as a typed FwdError', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);

  const p = client.read('/missing');
  // STATUS{ ERROR{ VALUE u16=0x0020 tr::path::not_found } } (RFC-0002 §C) —
  // built via the core codec.
  const status = encode({
    type: TYPE.STATUS,
    opt: { pl: true, ts: false, cr: false, ll: false, cw: false, tf: false },
    payload: new Uint8Array(0),
    children: [
      {
        type: TYPE.ERROR,
        opt: { pl: true, ts: false, cr: false, ll: false, cw: false, tf: false },
        payload: new Uint8Array(0),
        children: [
          {
            type: TYPE.VALUE,
            opt: { pl: false, ts: false, cr: false, ll: false, cw: false, tf: false },
            payload: Uint8Array.of(0x20, 0x00),
            children: [],
            trailer: null,
          },
        ],
        trailer: null,
      },
    ],
    trailer: null,
  });
  t.inject(
    encodeFwd({ op: FWD_OP.REPLY, dst: ['client'], src: ['missing'], kind: FWD_KIND.ERROR, payload: status }),
  );
  await assert.rejects(p, (err) => err.name === 'FwdError' && err.codeName === 'NOT_FOUND');
});

test('subscribe emits a FWD{WRITE} to :subscribers[] and delivers injected VALUEs to the handler', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);

  const got = [];
  const subP = client.subscribe(['sensor', 'temp'], (value, tlv) => {
    got.push({ value, type: tlv.type });
  });

  // Outbound: a FWD{WRITE, dst=/sensor/temp, FIELD=:subscribers[], payload=SUBSCRIBER{ target=/client }}.
  assert.equal(t.sent.length, 1);
  const expected = encodeFwd({
    op: FWD_OP.WRITE,
    dst: ['sensor', 'temp'],
    field: ':subscribers[]',
    src: ['client'],
    payload: encodeSubscriber(['client']),
  });
  assert.ok(
    sameBytes(t.sent[0], expected),
    `sent ${Buffer.from(t.sent[0]).toString('hex')} != ${Buffer.from(expected).toString('hex')}`,
  );

  // Ack the subscribe-write so subscribe() resolves with its local unsubscribe.
  t.inject(resultReply());
  const unsubscribe = await subP;
  assert.equal(typeof unsubscribe, 'function');

  // A delivery (FWD{WRITE}+VALUE) fires the handler with the decoded payload.
  t.inject(delivery(new Uint8Array([0xde, 0xad])));
  assert.equal(got.length, 1);
  assert.equal(got[0].type, TYPE.VALUE);
  assert.ok(sameBytes(got[0].value, new Uint8Array([0xde, 0xad])));

  // unsubscribe() detaches locally: further deliveries do not fire the handler.
  unsubscribe();
  t.inject(delivery(new Uint8Array([0xbe, 0xef])));
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

test('a real TransportWs satisfies the ClientTransport seam (FWD delivery echo)', async () => {
  const wss = new WebSocketServer({ host: '127.0.0.1', port: 0 });
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('listen timeout')), 5000);
    wss.once('listening', () => {
      clearTimeout(timer);
      resolve();
    });
  });
  const port = wss.address().port;
  // A dumb echo: bounce every frame back. The FWD{WRITE} the client sends comes
  // back as an inbound FWD{WRITE} carrying a VALUE — which the client treats as a
  // delivery (delivery-is-a-write), so onValue fires. This proves the transport
  // seam carries real frames both ways, without a full FWD responder.
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

  // The write's own FWD{REPLY} never comes back from a dumb echo, so its promise
  // stays pending; we only assert the delivery path here.
  client.write('/sensor/temp', encodeValue(new Uint8Array([0x07, 0x08]))).catch(() => {});
  const value = await received;
  assert.ok(sameBytes(value, new Uint8Array([0x07, 0x08])), 'echoed VALUE payload round-trips');

  await transport.close();
  await new Promise((resolve) => wss.close(resolve));
});
