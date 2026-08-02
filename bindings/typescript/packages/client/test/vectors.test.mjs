// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief Vector-level correctness (ADR-0034 "the gate"): the client's outbound
 * payload builders MUST produce the shared conformance vectors byte-for-byte,
 * and the inbound path MUST decode VALUE vectors into the right payload.
 *
 * This pins wire-compatibility WITHOUT a live peer.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import {
  decode,
  encode,
  ERROR,
  TYPE,
  PATH_REF_ELEMENT_BYTES,
  MAX_PATH_REF_ELEMENTS,
} from '@avatarsd-llc/libtracer';
import {
  LibtracerClient,
  encodeValue,
  encodePath,
  encodeSubscriber,
  DELIVERY_DURABILITY_REQUEST,
  encodeFwd,
  encodeField,
  decodeFwd,
  replyErrorCode,
  FWD_OP,
  FWD_KIND,
  FWD_ERROR,
} from '../dist/index.js';

const HERE = dirname(fileURLToPath(import.meta.url));
/** @brief test/ -> client -> packages -> typescript -> bindings -> repo root. */
const VECTORS = join(HERE, '..', '..', '..', '..', '..', 'tests', 'conformance', 'vectors', 'v1');

/** @brief Read a vector's canonical `input.bin` bytes. */
function vector(rel) {
  return new Uint8Array(readFileSync(join(VECTORS, ...rel.split('/'), 'input.bin')));
}

/** @brief Byte-for-byte equality. @param {Uint8Array} a @param {Uint8Array} b */
function sameBytes(a, b) {
  return a.length === b.length && a.every((x, i) => x === b[i]);
}

test('encodeSubscriber matches the subscriber-path vector byte-for-byte', () => {
  const expected = vector('tlv-types/subscriber-path');
  const built = encodeSubscriber(['sensor', 'temp']);
  assert.ok(sameBytes(built, expected), `built ${Buffer.from(built).toString('hex')} != ${Buffer.from(expected).toString('hex')}`);
  // Same via the static method on the client class.
  assert.ok(sameBytes(LibtracerClient.encodeSubscriber(['sensor', 'temp']), expected));
});

/**
 * @brief RFC-0022 §3.A — the packed delivery-policy word, bound to the three
 * `subscriber/policy-*` vectors.
 *
 * These are NOT gated on a live peer: the conformance harness only proves
 * `encode(decode(input.bin)) == input.bin`, which any well-formed TLV satisfies, so a
 * binding could score `ok` on these vectors while implementing none of the field. The
 * claim is made here instead, against `encodeSubscriber`'s own bytes.
 */
test('encodeSubscriber emits the RFC-0022 delivery-policy vectors byte-for-byte', () => {
  // §5.2 — bit 5 alone: the durability request.
  const durability = vector('subscriber/policy-durability');
  assert.ok(
    sameBytes(encodeSubscriber(['client'], { deliveryPolicy: DELIVERY_DURABILITY_REQUEST }), durability),
    `durability: ${hex(encodeSubscriber(['client'], { deliveryPolicy: DELIVERY_DURABILITY_REQUEST }))}`,
  );
  assert.equal(DELIVERY_DURABILITY_REQUEST, 0x0020, 'durability_request is bit 5');

  // §5.3 — every reserved bit (6-15) set, reliability=1 under them. A sender MUST be
  // able to emit them verbatim; masking them here would change the bytes.
  const reserved = vector('subscriber/policy-reserved-bits');
  assert.ok(
    sameBytes(encodeSubscriber(['client'], { deliveryPolicy: 0xffc1 }), reserved),
    `reserved: ${hex(encodeSubscriber(['client'], { deliveryPolicy: 0xffc1 }))}`,
  );

  // §5.1 — absent. An all-zero policy emits NO SETTINGS child, so the bytes are
  // byte-identical to a pre-RFC-0022 sender's; and the `policy-absent` vector's own
  // SETTINGS child names `delivery_compact`, NOT the policy — a decoder must read the
  // word by NAME, never by position.
  const plain = encodeSubscriber(['client']);
  assert.ok(sameBytes(plain, encodeSubscriber(['client'], { deliveryPolicy: 0 })), 'zero == absent');
  const plainTlv = decode(plain);
  assert.equal(plainTlv.type, TYPE.SUBSCRIBER);
  assert.equal(plainTlv.children.length, 1, 'an all-zero policy emits no SETTINGS child');

  const absentTlv = decode(vector('subscriber/policy-absent'));
  const settings = absentTlv.children.find((c) => c.type === TYPE.SETTINGS);
  assert.ok(settings, 'the policy-absent vector DOES carry a SETTINGS child');
  const keys = settings.children.filter((c) => c.type === TYPE.NAME).map((c) => new TextDecoder().decode(c.payload));
  assert.deepEqual(keys, ['delivery_compact'], 'and it names a NEIGHBOURING key, not the policy');
});

/** @brief The decode half: the policy word is read back out of the vectors' own bytes. */
test('the delivery-policy word decodes out of the policy vectors', () => {
  /** @brief The u16 under `delivery_policy`, or null when the record names none. */
  const policyOf = (bytes) => {
    const tlv = decode(bytes);
    for (const c of tlv.children) {
      if (c.type !== TYPE.SETTINGS) continue;
      for (let i = 0; i + 1 < c.children.length; i++) {
        if (c.children[i].type !== TYPE.NAME) continue;
        if (new TextDecoder().decode(c.children[i].payload) !== 'delivery_policy') continue;
        const v = c.children[i + 1];
        return v.payload[0] | (v.payload[1] << 8);
      }
    }
    return null;
  };
  assert.equal(policyOf(vector('subscriber/policy-absent')), null, 'absent => no policy word');
  assert.equal(policyOf(vector('subscriber/policy-durability')), 0x0020);
  const rsvd = policyOf(vector('subscriber/policy-reserved-bits'));
  assert.equal(rsvd, 0xffc1);
  // The honoured bits decode from UNDER the reserved ones, and nothing leaks up.
  assert.equal(rsvd & 0x0003, 1, 'reliability = 1');
  assert.equal((rsvd >> 2) & 0x0007, 0, 'priority = 0');
  assert.equal((rsvd & 0x0020) !== 0, false, 'durability_request clear');
});

test('encodePath matches the path-sensor-temp vector byte-for-byte', () => {
  assert.ok(sameBytes(encodePath(['sensor', 'temp']), vector('path/path-sensor-temp')));
});

test('encodeValue matches the value-bool-true vector byte-for-byte', () => {
  assert.ok(sameBytes(encodeValue(new Uint8Array([0x01])), vector('tlv-types/value-bool-true')));
});

test('encodeValue (LL=1) matches the value-ll-u32 vector byte-for-byte', () => {
  const built = encodeValue(new Uint8Array([0xaa, 0xbb, 0xcc]), { longLength: true });
  assert.ok(sameBytes(built, vector('tlv-types/value-ll-u32')));
});

test('encodeValue (absolute wire timestamp) matches the value-ts-abs vector byte-for-byte', () => {
  // bytes_le 08 07 06 05 04 03 02 01 => u64 LE 0x0102030405060708.
  const built = encodeValue(new Uint8Array([0xaa, 0xbb, 0xcc]), { timestampNs: 0x0102030405060708n });
  assert.ok(sameBytes(built, vector('tlv-types/value-ts-abs')));
});

test('inbound: a VALUE vector decodes to its payload via the core codec', () => {
  const tlv = decode(vector('tlv-types/value-ll-u32'));
  assert.equal(tlv.type, TYPE.VALUE);
  assert.ok(sameBytes(tlv.payload, new Uint8Array([0xaa, 0xbb, 0xcc])));
});

test('invalid path segments are rejected before any bytes are emitted', () => {
  assert.throws(() => encodePath([]), /at least one segment/);
  assert.throws(() => encodePath(['a/b']), /reserved character/);
  assert.throws(() => encodePath(['']), /1\.\.64/);
  assert.throws(() => encodePath(['x'.repeat(65)]), /1\.\.64/);
});

test('the segment cap is 255 and the 1024-byte body cap binds first (RFC-0023)', () => {
  // 33 segments: rejected by the inherited cap of 32, legal now. 33 * (4 + 1) = 165 bytes.
  assert.equal(encodePath(Array(33).fill('a')).length, 4 + 33 * 5);
  // 204 segments = 1020 bytes: the byte-derived ceiling under this body encoding.
  assert.equal(encodePath(Array(204).fill('a')).length, 4 + 1020);
  // 205 = 1025 bytes: the BYTE cap fires, not the count. This client had NO byte check at
  // all before RFC-0023 §5.6 — it would have emitted a non-conforming PATH.
  assert.throws(() => encodePath(Array(205).fill('a')), /at most 1024 bytes/);
  // 256 segments: over the count cap too. The count clause becomes the binding one only
  // under RFC-0018's packed body.
  assert.throws(() => encodePath(Array(256).fill('a')), /at most 255 segments/);
});

/* ----------------------------------------------------- RFC-0004 FWD / FIELD --- */

const hex = (b) => Buffer.from(b).toString('hex');

test('encodeFwd matches the fwd-read vector byte-for-byte', () => {
  const built = encodeFwd({ op: FWD_OP.READ, dst: ['sensor', 'temp'], src: ['reply-ep'] });
  const expected = vector('fwd/fwd-read');
  assert.ok(sameBytes(built, expected), `${hex(built)} != ${hex(expected)}`);
});

test('encodeFwd matches the fwd-write-value vector byte-for-byte', () => {
  const built = encodeFwd({
    op: FWD_OP.WRITE,
    dst: ['sensor', 'temp'],
    src: ['reply-ep'],
    payload: encodeValue(new Uint8Array([0xd2, 0x04, 0x00, 0x00])), // VALUE u32 LE 1234
  });
  assert.ok(sameBytes(built, vector('fwd/fwd-write-value')), hex(built));
});

test('encodeFwd matches the fwd-await-timeout vector byte-for-byte (1 s)', () => {
  const built = encodeFwd({
    op: FWD_OP.AWAIT,
    dst: ['sensor', 'temp'],
    src: ['reply-ep'],
    awaitTimeoutNs: 1_000_000_000n,
  });
  assert.ok(sameBytes(built, vector('fwd/fwd-await-timeout')), hex(built));
});

test('encodeFwd matches the fwd-write-subscriber-field vector byte-for-byte (subscribe)', () => {
  const built = encodeFwd({
    op: FWD_OP.WRITE,
    dst: ['sensor', 'temp'],
    field: ':subscribers[]',
    src: ['reply-ep'],
    payload: encodeSubscriber(['reply-ep']),
  });
  assert.ok(sameBytes(built, vector('fwd/fwd-write-subscriber-field')), hex(built));
});

test('encodeField matches the field-indexed / field-nested / field-append vectors', () => {
  assert.ok(sameBytes(encodeField(':subscribers[3]'), vector('field/field-indexed')), 'field-indexed');
  assert.ok(sameBytes(encodeField(':settings.app'), vector('field/field-nested')), 'field-nested');
  assert.ok(sameBytes(encodeField(':subscribers[]'), vector('field/field-append')), 'field-append');
});

/** @brief The NAME segments of a decoded PATH TLV, as strings. */
function pathSegs(path) {
  return path.children.filter((c) => c.type === TYPE.NAME).map((c) => new TextDecoder().decode(c.payload));
}

/**
 * @brief Both reply vectors depict the terminus's own emission (#419 ruling (c)): the
 * request's routes swapped, so `dst` is the accumulated return route built out of whole
 * `net/<module>/<name>` mount runs and `src` is the terminus residual.
 */
const REPLY_DST = ['net', 'downlink', 'a', 'net', 'downlink', 'cli', 'reply-ep'];
const REPLY_SRC = ['sensor', 'temp'];

test('decodeFwd parses the fwd-reply-result vector into op=REPLY, kind=RESULT, VALUE payload', () => {
  const parsed = decodeFwd(vector('fwd/fwd-reply-result'));
  assert.equal(parsed.op, FWD_OP.REPLY);
  assert.equal(parsed.kind, FWD_KIND.RESULT);
  assert.deepEqual(pathSegs(parsed.dst), REPLY_DST);
  assert.deepEqual(pathSegs(parsed.src), REPLY_SRC);
  assert.equal(parsed.payload.type, TYPE.VALUE);
  assert.ok(sameBytes(parsed.payload.payload, new Uint8Array([0xd2, 0x04, 0x00, 0x00])));
});

test('decodeFwd parses the fwd-reply-error vector and replyErrorCode reads NOT_FOUND', () => {
  const parsed = decodeFwd(vector('fwd/fwd-reply-error'));
  assert.equal(parsed.op, FWD_OP.REPLY);
  assert.equal(parsed.kind, FWD_KIND.ERROR);
  assert.deepEqual(pathSegs(parsed.dst), REPLY_DST);
  assert.deepEqual(pathSegs(parsed.src), REPLY_SRC);
  assert.equal(replyErrorCode(parsed), FWD_ERROR.NOT_FOUND);
});

/* ----------------------------------------------- RFC-0024 §4 PATH_REF (0x14) --- */

/** @brief Read a NEGATIVE vector's `reject.bin` bytes (HARNESS.md §negative cases). */
function rejectVector(rel) {
  return new Uint8Array(readFileSync(join(VECTORS, ...rel.split('/'), 'reject.bin')));
}

/** @brief Build a PATH_REF body from `[index, generation]` pairs — 8 LE bytes each. */
function pathRefBody(elements) {
  const body = new Uint8Array(elements.length * PATH_REF_ELEMENT_BYTES);
  const dv = new DataView(body.buffer);
  elements.forEach(([index, generation], i) => {
    dv.setUint32(i * PATH_REF_ELEMENT_BYTES, index, true);
    dv.setUint32(i * PATH_REF_ELEMENT_BYTES + 4, generation, true);
  });
  return body;
}

/** @brief Encode a PATH_REF over `[index, generation]` pairs — opt = 0x00, PL and LL both clear. */
function encodePathRef(elements) {
  return encode({
    type: TYPE.PATH_REF,
    opt: { pl: false, ts: false, cr: false, ll: false, cw: false, tf: false },
    payload: pathRefBody(elements),
    children: [],
    trailer: null,
  });
}

test('a PATH_REF encodes to the ref-1host / ref-2host / ref-3host vectors byte-for-byte', () => {
  // The hex literals ARE the pin: a codec change that no longer produces the published
  // bytes fails here even if the vector files were regenerated alongside it.
  assert.equal(hex(vector('path-ref/ref-1host')), '140008000700000003000000');
  assert.equal(hex(vector('path-ref/ref-2host')), '1400100007000000030000002a00000001000000');
  assert.equal(
    hex(vector('path-ref/ref-3host')),
    '140018000700000003000000130000000c0000002a00000001000000',
  );
  assert.ok(sameBytes(encodePathRef([[7, 3]]), vector('path-ref/ref-1host')), 'ref-1host');
  assert.ok(sameBytes(encodePathRef([[7, 3], [42, 1]]), vector('path-ref/ref-2host')), 'ref-2host');
  assert.ok(
    sameBytes(encodePathRef([[7, 3], [19, 12], [42, 1]]), vector('path-ref/ref-3host')),
    'ref-3host',
  );
});

test('a decoded PATH_REF is an opaque record array — no children, count = length / 8', () => {
  const tlv = decode(vector('path-ref/ref-3host'));
  assert.equal(tlv.type, TYPE.PATH_REF);
  // PL = 0 is a MUST (RFC-0024 §4.2): the body is fixed-stride records, not child TLVs.
  assert.equal(tlv.opt.pl, false);
  assert.equal(tlv.children.length, 0);
  // There is no count field on the wire — the count IS length / 8 (§4.3).
  assert.equal(tlv.payload.length / PATH_REF_ELEMENT_BYTES, 3);
  const dv = new DataView(tlv.payload.buffer, tlv.payload.byteOffset, tlv.payload.byteLength);
  assert.deepEqual(
    [0, 1, 2].map((i) => [dv.getUint32(i * 8, true), dv.getUint32(i * 8 + 4, true)]),
    [[7, 3], [19, 12], [42, 1]],
  );
});

test('the ref-255-elements vector is the normative maximum: 255 elements, 2044 bytes', () => {
  const bin = vector('path-ref/ref-255-elements');
  assert.equal(bin.length, 4 + MAX_PATH_REF_ELEMENTS * PATH_REF_ELEMENT_BYTES);
  // Head and tail rather than 4088 hex characters: an off-by-one in the bound moves these.
  assert.equal(hex(bin.subarray(0, 12)), '1400f8070000000000000000');
  assert.equal(hex(bin.subarray(bin.length - 8)), 'fe000000f2060000');
  const elements = Array.from({ length: 255 }, (_, i) => [i, (i * 7) % 65536]);
  assert.ok(sameBytes(encodePathRef(elements), bin));
});

test('the structurally-invalid PATH_REF vectors are FRAME_INVALID, never a partial parse', () => {
  // A length that is not a whole number of elements has no reading at all: the count is not
  // on the wire, so a body of 1.5 elements cannot be framed (§4.3).
  const ragged = rejectVector('path-ref/ref-len-not-multiple-of-8');
  assert.equal(hex(ragged), '14000c000700000003000000aabbccdd');
  assert.throws(() => decode(ragged), (e) => e.code === ERROR.FRAME_INVALID);

  // One element over the §4.3 bound.
  const over = rejectVector('path-ref/ref-256-elements');
  assert.equal(over.length, 4 + 256 * PATH_REF_ELEMENT_BYTES);
  assert.throws(() => decode(over), (e) => e.code === ERROR.FRAME_INVALID);

  // opt.PL = 1: ref-2host's body with one option bit set. A generic PL=1 walker would read
  // the first element's index bytes as a TLV header and mis-frame the whole body (§4.2).
  const plSet = rejectVector('path-ref/ref-pl-set');
  assert.equal(hex(plSet), '1440100007000000030000002a00000001000000');
  assert.throws(() => decode(plSet), (e) => e.code === ERROR.FRAME_INVALID);
  const cleared = Uint8Array.from(plSet);
  cleared[1] = 0x00;
  assert.ok(sameBytes(cleared, vector('path-ref/ref-2host')), 'one bit apart from ref-2host');

  // opt.LL = 1: ref-1host's element under the 6-byte header and the u32 length. The u32
  // width is unreachable below a 2040-byte cap, so §4.2 forbids the bit rather than
  // ignoring it — one route, one byte spelling. Its own case because the LL clause is
  // independent of the PL one: dropping either leaves the other three rules satisfied.
  const llSet = rejectVector('path-ref/ref-ll-set');
  assert.equal(hex(llSet), '1408080000000700000003000000');
  assert.throws(() => decode(llSet), (e) => e.code === ERROR.FRAME_INVALID);
});

test('an empty PATH_REF body is well-formed — the §4.3 bound is an upper one', () => {
  // H = 0: the envelope alone. 0 % 8 == 0 and 0 <= 2040, and there is no "at least one
  // element" clause; a route naming no vertex is the router's to refuse (§5), not the
  // codec's. This is the low end of the range ref-255/ref-256-elements bound above.
  const empty = vector('path-ref/ref-empty');
  assert.equal(hex(empty), '14000000');
  const tlv = decode(empty);
  assert.equal(tlv.type, TYPE.PATH_REF);
  assert.equal(tlv.payload.length, 0);
  assert.equal(tlv.children.length, 0);
  assert.ok(sameBytes(encodePathRef([]), empty), 'ref-empty');
});
