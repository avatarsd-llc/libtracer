// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief A remote `await_` always ends locally (RFC-0004 Amendment 3).
 *
 * The requester owns an AWAIT's deadline: a responder is not required to answer
 * `TIMEOUT`, so a silent responder must not leave `await_` pending forever. The
 * local deadline is the shorter of `requestTimeoutMs` (10 s by default, also when
 * the option disables other requests' deadlines) and the call's `timeoutNs`.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { LibtracerClient, FwdError, awaitDeadlineMs } from '../dist/index.js';

/** @brief Keep the event loop alive while @p p settles: the client unrefs its deadline
 *         timers so a pending request never holds a Node process open. */
async function held(p) {
  const keep = setInterval(() => {}, 1_000);
  try {
    return await p;
  } finally {
    clearInterval(keep);
  }
}

/** @brief A transport that records sends and never answers. */
class SilentTransport {
  constructor() {
    this.sent = [];
  }
  send(frame) {
    this.sent.push(new Uint8Array(frame));
  }
  onFrame() {}
  onClose() {}
}

test('the local await deadline is the shorter of the two', () => {
  assert.equal(awaitDeadlineMs(10_000), 10_000);
  assert.equal(awaitDeadlineMs(10_000, 50_000_000n), 50);
  assert.equal(awaitDeadlineMs(100, 5_000_000_000n), 100);
  // A disabled request deadline still leaves the await its 10 s default.
  assert.equal(awaitDeadlineMs(0), 10_000);
  assert.equal(awaitDeadlineMs(Infinity), 10_000);
  assert.equal(awaitDeadlineMs(0, 20_000_000_000n), 10_000);
  assert.equal(awaitDeadlineMs(0, 30_000_000n), 30);
});

test('await_ against a silent responder rejects TIMEOUT at its own timeoutNs', async () => {
  const t = new SilentTransport();
  const client = new LibtracerClient(t);
  const t0 = Date.now();
  await assert.rejects(held(client.await_('/sensor/temp', 40_000_000n)), (err) => {
    assert.ok(err instanceof FwdError);
    assert.equal(err.codeName, 'TIMEOUT');
    return true;
  });
  const waited = Date.now() - t0;
  assert.ok(waited >= 35 && waited < 2_000, `waited ${waited} ms`);
  assert.equal(t.sent.length, 1, 'exactly one AWAIT was sent');
});

test('await_ with no timeoutNs ends at requestTimeoutMs and rejects TIMEOUT', async () => {
  const t = new SilentTransport();
  const client = new LibtracerClient(t, { requestTimeoutMs: 30 });
  await assert.rejects(held(client.await_('/sensor/temp')), (err) => {
    assert.ok(err instanceof FwdError);
    assert.equal(err.codeName, 'TIMEOUT');
    return true;
  });
});
