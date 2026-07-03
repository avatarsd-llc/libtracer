// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// TransportWebTransport over a MOCKED WebTransport (Node has no native
// WebTransport client, so the browser session object is simulated with web
// streams — which Node >= 18 provides globally). This covers the transport's
// own logic end to end: option pass-through (serverCertificateHashes), the
// frame-channel framing both directions across split/coalesced reads, the
// ClientTransport seam (send/onFrame/onClose), close semantics, and the
// connect deadline. The REAL browser <-> C++ server path is covered by
// interop-browser.test.mjs (puppeteer, when available) and — entirely in C++ —
// by core/tests/webtransport_test.cpp's client half.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { TransportWebTransport, encodeRecord } from '../dist/index.js';

/** A mock WebTransport session: TransformStreams stand in for the QUIC stream. */
class MockWebTransport {
  static last = null;

  constructor(url, options) {
    MockWebTransport.last = this;
    this.url = url;
    this.options = options;
    this.ready = Promise.resolve();
    this.closed = new Promise((resolve, reject) => {
      this._resolveClosed = resolve;
      this._rejectClosed = reject;
    });
    // client -> server, and server -> client.
    this._c2s = new TransformStream();
    this._s2c = new TransformStream();
    /** The "server" side: read what the client wrote, write toward it. */
    this.serverReader = this._c2s.readable.getReader();
    this.serverWriter = this._s2c.writable.getWriter();
  }

  async createBidirectionalStream() {
    return { readable: this._s2c.readable, writable: this._c2s.writable };
  }

  close() {
    this._resolveClosed({});
  }

  /** Simulate a remote/session error. */
  fail(err) {
    this._rejectClosed(err);
  }
}

/** A WebTransport whose session never becomes ready (for the deadline test). */
class NeverReadyWebTransport {
  constructor() {
    this.ready = new Promise(() => {});
    this.closed = new Promise(() => {});
  }
  async createBidirectionalStream() {
    throw new Error('unreachable');
  }
  close() {}
}

function collectFrames(transport) {
  const frames = [];
  let notify = null;
  transport.onFrame((bytes) => {
    frames.push(bytes);
    if (notify) notify();
  });
  return {
    frames,
    waitFor(n, ms = 2000) {
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`only ${frames.length}/${n} frames`)), ms);
        const check = () => {
          if (frames.length >= n) {
            clearTimeout(timer);
            resolve(frames);
          }
        };
        notify = check;
        check();
      });
    },
  };
}

test('connect passes serverCertificateHashes through to the WebTransport ctor', async () => {
  const hashes = [{ algorithm: 'sha-256', value: new Uint8Array(32) }];
  const t = new TransportWebTransport('https://robot.local:4433/', {
    WebTransport: MockWebTransport,
    serverCertificateHashes: hashes,
  });
  await t.connect();
  assert.equal(MockWebTransport.last.url, 'https://robot.local:4433/');
  assert.deepEqual(MockWebTransport.last.options.serverCertificateHashes, hashes);
  assert.ok(t.connected);
  await t.close();
});

test('send emits one length-prefixed record per frame (the C++ wire shape)', async () => {
  const t = new TransportWebTransport('https://x/', { WebTransport: MockWebTransport });
  await t.connect();
  const wt = MockWebTransport.last;

  const frame = Uint8Array.from([0xde, 0xad, 0xbe, 0xef]);
  t.send(frame);
  const { value } = await wt.serverReader.read();
  assert.deepEqual([...value], [0x04, 0x00, 0x00, 0x00, 0xde, 0xad, 0xbe, 0xef]);
  await t.close();
});

test('inbound records deliver via onFrame across split AND coalesced reads', async () => {
  const t = new TransportWebTransport('https://x/', { WebTransport: MockWebTransport });
  await t.connect();
  const wt = MockWebTransport.last;
  const sink = collectFrames(t);

  const f1 = Uint8Array.from([1, 2, 3, 4, 5]);
  const f2 = Uint8Array.from([9]);
  const f3 = Uint8Array.from([7, 7]);
  // f1 split mid-prefix and mid-body; then f2+f3 coalesced into one chunk.
  const r1 = encodeRecord(f1);
  await wt.serverWriter.write(r1.slice(0, 2));
  await wt.serverWriter.write(r1.slice(2, 6));
  await wt.serverWriter.write(r1.slice(6));
  await wt.serverWriter.write(new Uint8Array([...encodeRecord(f2), ...encodeRecord(f3)]));

  const frames = await sink.waitFor(3);
  assert.deepEqual([...frames[0]], [...f1]);
  assert.deepEqual([...frames[1]], [...f2]);
  assert.deepEqual([...frames[2]], [...f3]);
  await t.close();
});

test('send before connect throws; close fires onClose exactly once', async () => {
  const t = new TransportWebTransport('https://x/', { WebTransport: MockWebTransport });
  assert.throws(() => t.send(new Uint8Array(1)), /before connect/);

  await t.connect();
  let closes = 0;
  t.onClose(() => {
    closes += 1;
  });
  await t.close();
  await t.close(); // idempotent
  assert.equal(closes, 1);
  assert.ok(!t.connected);
});

test('a session error surfaces once through onClose with its cause', async () => {
  const t = new TransportWebTransport('https://x/', { WebTransport: MockWebTransport });
  await t.connect();
  const wt = MockWebTransport.last;

  const seen = new Promise((resolve) => t.onClose(resolve));
  wt.fail(new Error('session lost'));
  const cause = await seen;
  assert.match(cause.message, /session lost/);
  assert.ok(!t.connected);
});

test('connect rejects at the deadline when the session never becomes ready', async () => {
  const t = new TransportWebTransport('https://x/', { WebTransport: NeverReadyWebTransport });
  await assert.rejects(() => t.connect(50), /timed out after 50ms/);
  assert.ok(!t.connected);
});

test('constructing without any WebTransport implementation throws helpfully', () => {
  assert.throws(() => new TransportWebTransport('https://x/'), /No WebTransport implementation/);
});
