// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief Session-hardening gate (v0.1 must-fix).
 *
 * Pending requests reject when the transport closes mid-flight; a per-request
 * timeout fires (and a late reply still consumes its FIFO slot so later
 * requests stay correlated); inbound ADVERTISE/COMPACT frames surface a loud
 * CompactFlowError instead of a silent drop; and the RFC-0002 error registry
 * is complete — registered codes AND string-form (NAME tr::… path) identities
 * surface typed on FwdError.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { encode, TYPE } from '@avatarsd-llc/libtracer';
import {
  LibtracerClient,
  CompactFlowError,
  FwdError,
  FWD_ERROR,
  FWD_ERROR_PATH,
  fwdErrorPath,
  fwdErrorCodeForPath,
  encodeValue,
  encodeFwd,
  FWD_OP,
  FWD_KIND,
} from '../dist/index.js';

/** @brief An in-memory ClientTransport with a close hook, mirroring TransportWs's seam. */
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
  /** @brief Simulate one inbound frame arriving from the wire. */
  inject(frame) {
    if (this.receiver) this.receiver(new Uint8Array(frame));
  }
  /** @brief Simulate the connection closing (optionally with a cause). */
  close(cause) {
    if (this.closeHandler) this.closeHandler(cause);
  }
}

/** @brief A source-routed FWD{REPLY, RESULT} back to the default reply-ep "client". */
function resultReply(payload) {
  return encodeFwd({ op: FWD_OP.REPLY, dst: ['client'], src: ['sensor'], kind: FWD_KIND.RESULT, payload });
}

const bare = (type, payload, children = []) => ({
  type,
  opt: { pl: children.length > 0, ts: false, cr: false, ll: false, cw: false, tf: false },
  payload,
  children,
  trailer: null,
});

/** @brief STATUS{ ERROR{ <identity child> } } reply payload bytes. */
function statusError(identityChild) {
  return encode(bare(TYPE.STATUS, new Uint8Array(0), [bare(TYPE.ERROR, new Uint8Array(0), [identityChild])]));
}

function errorReply(identityChild) {
  return encodeFwd({
    op: FWD_OP.REPLY,
    dst: ['client'],
    src: ['sensor'],
    kind: FWD_KIND.ERROR,
    payload: statusError(identityChild),
  });
}

/* ------------------------------------------------ close rejects pending --- */

test('transport close mid-request rejects every pending promise with "transport closed"', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);

  const p1 = client.read('/sensor/a');
  const p2 = client.write('/sensor/b', encodeValue(new Uint8Array([1])));
  assert.equal(t.sent.length, 2);

  t.close(new Error('ECONNRESET'));
  await assert.rejects(p1, /transport closed: ECONNRESET/);
  await assert.rejects(p2, /transport closed: ECONNRESET/);

  // After a close, new requests fail fast instead of hanging.
  await assert.rejects(client.read('/sensor/c'), /transport closed/);
});

/* ------------------------------------------------------ request timeout --- */

/**
 * @brief The client's deadline timer is unref'd (it must not hold a real app's
 * event loop open), so tests that WAIT for it to fire keep the loop alive
 * themselves.
 */
async function withLiveLoop(fn) {
  const hold = setInterval(() => {}, 1000);
  try {
    await fn();
  } finally {
    clearInterval(hold);
  }
}

test('a request with no reply rejects after requestTimeoutMs', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t, { requestTimeoutMs: 25 });

  await withLiveLoop(() => assert.rejects(client.read('/sensor/never'), /timed out after 25ms/));
});

test('a late reply after a timeout consumes its FIFO slot; the next request still correlates', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t, { requestTimeoutMs: 25 });

  await withLiveLoop(() => assert.rejects(client.read('/slow'), /timed out/));

  // The second request is issued, THEN the first request's late reply arrives,
  // THEN the second's. The late reply must be dropped against the timed-out
  // slot, not resolve the second request.
  const p2 = client.read('/fast');
  t.inject(resultReply(encodeValue(new Uint8Array([0xaa])))); // late reply for /slow
  t.inject(resultReply(encodeValue(new Uint8Array([0xbb])))); // reply for /fast
  const tlv = await p2;
  assert.deepEqual([...tlv.payload], [0xbb]);
});

test('requestTimeoutMs: 0 disables the deadline', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t, { requestTimeoutMs: 0 });
  const p = client.read('/sensor/a');
  await new Promise((r) => setTimeout(r, 30));
  t.inject(resultReply(encodeValue(new Uint8Array([0x01]))));
  const tlv = await p;
  assert.deepEqual([...tlv.payload], [0x01]);
});

/* ------------------------------------------------- loud compact failure --- */

test('inbound ADVERTISE (0x11) and COMPACT (0x12) frames emit CompactFlowError on onError', () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);
  const errors = [];
  client.onError((err) => errors.push(err));
  const values = [];
  client.onValue((v) => values.push(v));

  // ADVERTISE{ VALUE label(u16), PATH } and COMPACT{ VALUE label(u16), VALUE }.
  const label = bare(TYPE.VALUE, Uint8Array.of(0x2a, 0x00));
  t.inject(encode(bare(0x11, new Uint8Array(0), [label, bare(TYPE.VALUE, Uint8Array.of(1))])));
  t.inject(encode(bare(0x12, new Uint8Array(0), [label, bare(TYPE.VALUE, Uint8Array.of(2))])));

  assert.equal(errors.length, 2);
  assert.ok(errors[0] instanceof CompactFlowError);
  assert.equal(errors[0].name, 'CompactFlowError');
  assert.equal(errors[0].frameType, 0x11);
  assert.match(errors[0].message, /ADVERTISE/);
  assert.match(errors[0].message, /not supported by this client yet/);
  assert.equal(errors[1].frameType, 0x12);
  assert.match(errors[1].message, /COMPACT/);
  assert.equal(values.length, 0, 'nothing is delivered from a compact-flow frame');
});

/* ------------------------------------------------- error registry parity --- */

test('FWD_ERROR carries the full 15-code RFC-0002 registry with tr:: paths', () => {
  const expected = {
    0x0001: 'tr::frame::truncated',
    0x0002: 'tr::frame::invalid',
    0x0003: 'tr::frame::crc_fail',
    0x0010: 'tr::tlv::nesting_too_deep',
    0x0020: 'tr::path::not_found',
    0x0021: 'tr::path::invalid',
    0x0022: 'tr::path::in_use',
    0x0030: 'tr::schema::type_mismatch',
    0x0031: 'tr::schema::not_found',
    0x0040: 'tr::flow::backpressure',
    0x0041: 'tr::flow::timeout',
    0x0042: 'tr::flow::address_shift_gap',
    0x0050: 'tr::access::denied',
    0x0060: 'tr::transport::down',
    0x0070: 'tr::version::mismatch',
  };
  assert.equal(Object.keys(FWD_ERROR).length, 15);
  assert.deepEqual(new Set(Object.values(FWD_ERROR)), new Set(Object.keys(expected).map(Number)));
  for (const [code, path] of Object.entries(expected)) {
    assert.equal(FWD_ERROR_PATH[Number(code)], path, `path of 0x${Number(code).toString(16)}`);
    assert.equal(fwdErrorPath(Number(code)), path);
    assert.equal(fwdErrorCodeForPath(path), Number(code), `code of ${path}`);
  }
});

test('a registered-code ERROR reply surfaces code, name, and tr:: path on FwdError', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);
  const p = client.read('/x');
  // STATUS{ ERROR{ VALUE u16 = 0x0060 tr::transport::down } }.
  t.inject(errorReply(bare(TYPE.VALUE, Uint8Array.of(0x60, 0x00))));
  await assert.rejects(p, (err) => {
    assert.ok(err instanceof FwdError);
    assert.equal(err.code, FWD_ERROR.TRANSPORT_DOWN);
    assert.equal(err.codeName, 'TRANSPORT_DOWN');
    assert.equal(err.path, 'tr::transport::down');
    return true;
  });
});

test('a string-form (NAME tr::… path) ERROR reply surfaces typed, not UNKNOWN', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);
  const p = client.read('/x');
  // STATUS{ ERROR{ NAME "tr::path::not_found" } } — the RFC-0002 string identity.
  t.inject(errorReply(bare(TYPE.NAME, new TextEncoder().encode('tr::path::not_found'))));
  await assert.rejects(p, (err) => {
    assert.ok(err instanceof FwdError);
    assert.equal(err.code, FWD_ERROR.NOT_FOUND, 'known path resolves back to its registered code');
    assert.equal(err.codeName, 'NOT_FOUND');
    assert.equal(err.path, 'tr::path::not_found');
    assert.doesNotMatch(err.message, /UNKNOWN/);
    return true;
  });
});

test('an unregistered string-form ERROR still exposes the path verbatim', async () => {
  const t = new FakeTransport();
  const client = new LibtracerClient(t);
  const p = client.read('/x');
  t.inject(errorReply(bare(TYPE.NAME, new TextEncoder().encode('tr::app::custom'))));
  await assert.rejects(p, (err) => {
    assert.ok(err instanceof FwdError);
    assert.equal(err.code, 0);
    assert.equal(err.path, 'tr::app::custom');
    assert.match(err.message, /tr::app::custom/);
    return true;
  });
});
