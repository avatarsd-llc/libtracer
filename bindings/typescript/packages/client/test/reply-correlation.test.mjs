// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief Reply correlation under CONCURRENCY (#1530).
 *
 * `FWD` carries no correlation id and RFC-0004 promises no reply ORDER: a request
 * forwarded across a mounted link answers in tens of milliseconds while a local one
 * answers in about one, so a later request routinely overtakes an earlier one on the
 * same link. Correlating by `pending.shift()` transposes the two answers and never
 * realigns — from the first overtake on, every caller gets the previous request's
 * reply, silently and with a well-formed RESULT.
 *
 * What the wire does carry is the responder's own endpoint in the reply's `src`
 * (RFC-0004 §B), which is the TAIL of the request's `dst` after every mount prefix was
 * stripped. These cases pin the fix on that key, and pin the FIFO fallback that keeps
 * every previously-working correlation working.
 *
 * RED/GREEN: `two_in_flight_replies_arrive_reversed` and `the_measured_1530_trace`
 * both fail against the FIFO implementation.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { encode, decode, TYPE } from '@avatarsd-llc/libtracer';
import {
  LibtracerClient,
  encodeValue,
  encodeFwd,
  FWD_OP,
  FWD_KIND,
} from '../dist/index.js';

/** @brief An in-memory ClientTransport that records what was sent. */
class FakeTransport {
  constructor() {
    this.sent = [];
    this.receiver = null;
    this.closeHandler = null;
  }
  send(frame) {
    this.sent.push(new Uint8Array(frame));
  }
  onFrame(receiver) {
    this.receiver = receiver;
  }
  onClose(handler) {
    this.closeHandler = handler;
  }
  inject(frame) {
    if (this.receiver) this.receiver(new Uint8Array(frame));
  }
}

/**
 * @brief A FWD{REPLY, RESULT} as a terminus emits it: `src` is the responder's own
 * endpoint — the request's `dst` with every mount prefix already stripped.
 */
function replyFrom(srcSegments, text) {
  return encodeFwd({
    op: FWD_OP.REPLY,
    dst: ['client'],
    src: srcSegments,
    kind: FWD_KIND.RESULT,
    payload: encodeValue(new TextEncoder().encode(text)),
  });
}

const textOf = (tlv) => new TextDecoder().decode(tlv.payload);

test('two in-flight requests whose replies arrive reversed each resolve to their own answer', async () => {
  const t = new FakeTransport();
  const c = new LibtracerClient(t);

  const slow = c.read('/net/ws-client/peer0/hw/segments'); // crosses the mount
  const fast = c.read('/system/heap/free'); // local

  // The local read overtakes the mounted one — the shape #1530 measured.
  t.inject(replyFrom(['system', 'heap', 'free'], 'heap'));
  t.inject(replyFrom(['hw', 'segments'], 'segments'));

  assert.equal(textOf(await fast), 'heap');
  assert.equal(textOf(await slow), 'segments');
});

test('the measured #1530 trace: four back-to-back reads, two of them transposed', async () => {
  const t = new FakeTransport();
  const c = new LibtracerClient(t);

  // Sent in this order, no awaits between them.
  const a = c.read('/net/ws-client/peer0/hw/variant'); // crosses the mount
  const b = c.read('/hw/variant'); // local — SAME responder endpoint as `a`
  const cc = c.read('/net/ws-client/peer0/hw/segments'); // crosses the mount
  const d = c.read('/system/heap/free'); // local

  // Answers as the device produced them: +12 / +23 / +25 / +28 ms.
  t.inject(replyFrom(['hw', 'variant'], 'mounted-variant'));
  t.inject(replyFrom(['hw', 'variant'], 'local-variant'));
  t.inject(replyFrom(['system', 'heap', 'free'], 'heap')); // overtakes `cc`
  t.inject(replyFrom(['hw', 'segments'], 'segments'));

  assert.equal(textOf(await a), 'mounted-variant');
  assert.equal(textOf(await b), 'local-variant');
  assert.equal(textOf(await cc), 'segments');
  assert.equal(textOf(await d), 'heap');
});

test('two requests the wire cannot tell apart are correlated FIFO among themselves', async () => {
  const t = new FakeTransport();
  const c = new LibtracerClient(t);

  // Identical dst: both replies carry src=/hw/variant, so nothing on the wire
  // distinguishes them and request order is the only tiebreak there is.
  const first = c.read('/hw/variant');
  const second = c.read('/hw/variant');

  t.inject(replyFrom(['hw', 'variant'], 'one'));
  t.inject(replyFrom(['hw', 'variant'], 'two'));

  assert.equal(textOf(await first), 'one');
  assert.equal(textOf(await second), 'two');
});

test('a reply whose src matches nothing outstanding falls back to FIFO', async () => {
  const t = new FakeTransport();
  const c = new LibtracerClient(t);

  // The positive control for the fallback: a responder that echoes something the
  // client cannot match must keep behaving exactly as it did before the fix.
  const only = c.read('/sensor/temp');
  t.inject(replyFrom(['somewhere', 'else'], 'still-mine'));
  assert.equal(textOf(await only), 'still-mine');
});

test('an empty src — RFC-0004 Amendment 2 "no reply requested" — falls back to FIFO', async () => {
  const t = new FakeTransport();
  const c = new LibtracerClient(t);

  // Hand-built: this client's own `encodeFwd` refuses to MINT a zero-segment path
  // (`pathTlv` requires a segment), but it must still read one a peer sends.
  const bare = (type, payload, children = []) => ({
    type,
    opt: { pl: children.length > 0, ts: false, cr: false, ll: false, cw: false, tf: false },
    payload,
    children,
    trailer: null,
  });
  const u8 = (n) => new Uint8Array([n]);
  const frame = encode(
    bare(TYPE.FWD, new Uint8Array(0), [
      bare(TYPE.VALUE, u8(FWD_OP.REPLY)),
      decode(encodeFwd({ op: FWD_OP.READ, dst: ['client'], src: ['x'] })).children[1], // dst PATH
      bare(TYPE.PATH, new Uint8Array(0)), // src: EMPTY
      bare(TYPE.VALUE, u8(FWD_KIND.RESULT)),
      decode(encodeValue(new TextEncoder().encode('anonymous'))),
    ]),
  );

  const only = c.read('/sensor/temp');
  t.inject(frame);
  assert.equal(textOf(await only), 'anonymous');
});

/**
 * @brief Hold the event loop open across an awaited deadline.
 *
 * The request timer is `unref`'d, so with nothing else scheduled Node exits before it
 * fires and the runner reports the pending promise as a cancellation. Same helper as
 * `session.test.mjs`.
 */
async function withLiveLoop(fn) {
  const hold = setInterval(() => {}, 1000);
  try {
    await fn();
  } finally {
    clearInterval(hold);
  }
}

test('a timed-out request consumes its OWN late reply, not a live caller\'s', async () => {
  const t = new FakeTransport();
  const c = new LibtracerClient(t, { requestTimeoutMs: 25 });

  await withLiveLoop(() => assert.rejects(c.read('/slow/thing'), /timed out/));

  // A second request is now in flight; the dead one's late reply must not take it.
  const live = c.read('/fast/thing');
  t.inject(replyFrom(['slow', 'thing'], 'too-late'));
  t.inject(replyFrom(['fast', 'thing'], 'mine'));
  assert.equal(textOf(await live), 'mine');
});

test('a reply with nothing outstanding is dropped, not thrown', async () => {
  const t = new FakeTransport();
  const c = new LibtracerClient(t);
  let errs = 0;
  c.onError(() => {
    errs++;
  });
  t.inject(replyFrom(['hw', 'variant'], 'unsolicited'));
  assert.equal(errs, 0);
});
