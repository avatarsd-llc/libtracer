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

/**
 * @brief A delivery: a FWD{WRITE} carrying a VALUE, addressed to a subscription's target.
 *
 * A producer addresses a delivery by the subscribe request's `src`, which is the
 * subscription's own target below the reply endpoint (the first one is `/client/sub1`).
 */
function delivery(valueBytes, dst = ['client', 'sub1'], src = ['sensor', 'temp']) {
  return encodeFwd({
    op: FWD_OP.WRITE,
    dst,
    src,
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
    src: ['client', 'q1'], // the reply endpoint plus this request's own reply token
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
  assert.ok(sameBytes(t.sent[0], encodeFwd({ op: FWD_OP.READ, dst: ['sensor', 'temp'], src: ['client', 'q1'] })));

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

  // Outbound: a FWD{WRITE, dst=/sensor/temp, FIELD=:subscribers[], src=/client/sub1,
  // payload=SUBSCRIBER{ target=/client/sub1 }}: the subscription's own target.
  assert.equal(t.sent.length, 1);
  const expected = encodeFwd({
    op: FWD_OP.WRITE,
    dst: ['sensor', 'temp'],
    field: ':subscribers[]',
    src: ['client', 'sub1'],
    payload: encodeSubscriber(['client', 'sub1']),
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

test('two subscriptions to two producers: each handler gets only its own deliveries (#2240)', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);
  const temp = [];
  const hum = [];
  const all = [];
  client.onValue((value) => all.push(value[0]));

  // Both subscribes are outstanding at once; their acks arrive out of order. The reply
  // correlation (#1530) keys on the responder's src, so each still resolves its own.
  const tempP = client.subscribe('/sensor/temp', (value) => temp.push(value[0]));
  const humP = client.subscribe('/sensor/hum', (value) => hum.push(value[0]));
  assert.equal(t.sent.length, 2);
  assert.ok(
    sameBytes(
      t.sent[1],
      encodeFwd({
        op: FWD_OP.WRITE,
        dst: ['sensor', 'hum'],
        field: ':subscribers[]',
        src: ['client', 'sub2'],
        payload: encodeSubscriber(['client', 'sub2']),
      }),
    ),
    'the second subscription targets its own path',
  );
  const ack = (dst, src) =>
    encodeFwd({ op: FWD_OP.REPLY, dst, src, kind: FWD_KIND.RESULT });
  t.inject(ack(['client', 'sub2'], ['sensor', 'hum']));
  t.inject(ack(['client', 'sub1'], ['sensor', 'temp']));
  const unsubTemp = await tempP;
  await humP;

  t.inject(delivery(new Uint8Array([0x01]), ['client', 'sub1'], ['sensor', 'temp']));
  t.inject(delivery(new Uint8Array([0x02]), ['client', 'sub2'], ['sensor', 'hum']));
  assert.deepEqual(temp, [0x01], 'the temp handler saw only temp');
  assert.deepEqual(hum, [0x02], 'the hum handler saw only hum');
  assert.deepEqual(all, [0x01, 0x02], 'onValue still observes every delivery');

  // A local unsubscribe stops only its own handler.
  unsubTemp();
  t.inject(delivery(new Uint8Array([0x03]), ['client', 'sub1'], ['sensor', 'temp']));
  t.inject(delivery(new Uint8Array([0x04]), ['client', 'sub2'], ['sensor', 'hum']));
  assert.deepEqual(temp, [0x01], 'the detached handler stays silent');
  assert.deepEqual(hum, [0x02, 0x04], 'the other subscription keeps working');
});

test('a delivery routes by its dst tail; one naming no live subscription reaches onValue only', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);
  const sub = [];
  const all = [];
  client.onValue((value) => all.push(value[0]));
  const p = client.subscribe('/sensor/temp', (value) => sub.push(value[0]));
  t.inject(resultReply());
  await p;

  // dst shrinks per hop: a node whose link is also named "client" consumes that segment,
  // so the tail /sub1 alone still names the subscription; a foreign prefix does not.
  t.inject(delivery(new Uint8Array([0x08]), ['sub1']));
  t.inject(delivery(new Uint8Array([0x09]), ['other', 'sub1']));
  assert.deepEqual(sub, [0x08], 'a shrunken dst tail routes; a foreign prefix does not');
  sub.length = 0;
  all.length = 0;

  t.inject(delivery(new Uint8Array([0x05]), ['client']));
  t.inject(delivery(new Uint8Array([0x06]), ['client', 'sub9']));
  t.inject(encodeValue(new Uint8Array([0x07])));
  assert.deepEqual(sub, [], 'no subscription handler fires for an unrouted delivery');
  assert.deepEqual(all, [0x05, 0x06, 0x07]);
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
