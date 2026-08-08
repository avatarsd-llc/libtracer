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
  encodeConnSpec,
  DELIVERY_DURABILITY_REQUEST,
  encodeFwd,
  encodeField,
  decodeFwd,
  firstChild,
  replyErrorCode,
  replyErrorPath,
  FWD_OP,
  FWD_OP_FLAG_MINT_REQUEST,
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

/**
 * @brief The cross-core acceptance rule (#878): a reply's ERROR is the FIRST ERROR child of
 * the STATUS, at whatever position — reference/05 §`0x09` pins no order over a STATUS's
 * children, and RFC-0002 §C pins position only INSIDE the ERROR. Same frame as
 * `fwd/fwd-reply-error` with the STATUS's optional DESCRIPTION written first.
 *
 * The Rust binding pins these same bytes in `tests/conformance_vectors.rs`
 * (`fwd_reply_error_after_description`); before this, only Rust read the frame correctly and
 * this core answered code 0 — the same answer it gives for a STATUS carrying no ERROR at all.
 */
test('replyErrorCode reads the ERROR at any STATUS child position (fwd-reply-error-after-description)', () => {
  const bin = vector('fwd/fwd-reply-error-after-description');
  const parsed = decodeFwd(bin);
  assert.equal(parsed.op, FWD_OP.REPLY);
  assert.equal(parsed.kind, FWD_KIND.ERROR);
  assert.deepEqual(pathSegs(parsed.dst), REPLY_DST);
  assert.deepEqual(pathSegs(parsed.src), REPLY_SRC);

  // The vector is only a gate while its ERROR is genuinely NOT the first child: assert the
  // shape before asserting the read, so a future re-blessing that reorders it cannot leave
  // this test silently passing on the easy case.
  assert.equal(parsed.payload.type, TYPE.STATUS);
  assert.equal(parsed.payload.children.length, 2);
  assert.equal(
    parsed.payload.children[0].type,
    TYPE.DESCRIPTION,
    'the ERROR must not be child 0 or this vector gates nothing',
  );
  assert.equal(parsed.payload.children[1].type, TYPE.ERROR);

  assert.equal(replyErrorCode(parsed), FWD_ERROR.NOT_FOUND);
  assert.equal(replyErrorPath(parsed), null, 'registered identity, not the string form');

  // firstChild is the shared accessor both cores answer "the X child" with; index-0 is not it.
  assert.equal(firstChild(parsed.payload, TYPE.ERROR), parsed.payload.children[1]);
  assert.equal(firstChild(parsed.payload, TYPE.PATH), null);

  // And the offending shape is buildable from this package's own surface, so it is a wire a
  // conformant peer can really send: encodeFwd embeds the STATUS bytes verbatim.
  const status = encode({
    type: TYPE.STATUS,
    opt: { ...parsed.payload.opt },
    payload: new Uint8Array(0),
    children: parsed.payload.children,
    trailer: null,
  });
  const built = encodeFwd({
    op: FWD_OP.REPLY,
    dst: REPLY_DST,
    src: REPLY_SRC,
    kind: FWD_KIND.ERROR,
    payload: status,
  });
  assert.ok(sameBytes(built, bin), hex(built));
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

/* -------------------------- encode/decode symmetry on PATH_REF (#1004) --- */

/** @brief The all-clear opt byte, with `over` applied — the raw bits a caller can set. */
function opt(over = {}) {
  return { pl: false, ts: false, cr: false, ll: false, cw: false, tf: false, ...over };
}

/**
 * @brief A raw PATH_REF node as a caller composes one. The core package exports no PATH_REF
 * builder, so the object literal IS the normal way to build one and `encode` is the only door.
 */
function rawPathRef(bits, payload, children = []) {
  return { type: TYPE.PATH_REF, opt: opt(bits), payload, children, trailer: null };
}

/** @brief A bare opaque node of `type` — a NAME child, or a FWD's leading sibling. */
function bare(type, payload) {
  return { type, opt: opt(), payload, children: [], trailer: null };
}

/** @brief A structured FWD wrapping `children`. */
function fwdWrapping(children) {
  return { type: TYPE.FWD, opt: opt({ pl: true }), payload: new Uint8Array(0), children, trailer: null };
}

test('each ill-formed PATH_REF encodes to NOTHING, and its pre-fix bytes are FRAME_INVALID', () => {
  // The property being closed (#1004), in both halves. Before the fix `encode` serialized
  // every one of these verbatim; the second assertion in each pair is what those bytes were
  // worth — this core's own decode refuses them, and so does every conformant node. Three of
  // the four pre-fix encodings ARE the published negative vectors, byte-for-byte.
  const one = pathRefBody([[7, 3]]);

  // opt.LL = 1. Pre-fix output: `1408080000000700000003000000` — ref-ll-set exactly.
  assert.equal(encode(rawPathRef({ ll: true }, one)).length, 0);
  assert.equal(hex(rejectVector('path-ref/ref-ll-set')), '1408080000000700000003000000');

  // A length that is not a whole number of elements. Pre-fix output:
  // `14000c000700000003000000aabbccdd` — ref-len-not-multiple-of-8 exactly.
  const ragged = Uint8Array.from([...one, 0xaa, 0xbb, 0xcc, 0xdd]);
  assert.equal(encode(rawPathRef({}, ragged)).length, 0);
  assert.equal(
    hex(rejectVector('path-ref/ref-len-not-multiple-of-8')),
    '14000c000700000003000000aabbccdd',
  );

  // One element over the §4.3 bound. Pre-fix output: the 2052-byte ref-256-elements, whose
  // elements are (index = i, generation = 0) — rebuilt so the pin is the published bytes.
  const over = pathRefBody(Array.from({ length: MAX_PATH_REF_ELEMENTS + 1 }, (_, i) => [i, 0]));
  assert.equal(over.length, (MAX_PATH_REF_ELEMENTS + 1) * PATH_REF_ELEMENT_BYTES);
  assert.equal(encode(rawPathRef({}, over)).length, 0);
  assert.ok(
    sameBytes(rejectVector('path-ref/ref-256-elements').subarray(4), over),
    'the pre-fix encoding IS this vector body',
  );

  // opt.PL = 1 — a caller who mistook PATH_REF for a structured type. No published vector
  // (a PL body is child TLVs, not an element array), so its pre-fix encoding is written out.
  assert.equal(encode(rawPathRef({ pl: true }, new Uint8Array(0), [bare(TYPE.NAME, one)])).length, 0);
  const preFixPl = Uint8Array.from(
    '14400c00020008000700000003000000'.match(/../g).map((b) => parseInt(b, 16)),
  );
  assert.throws(() => decode(preFixPl), (e) => e.code === ERROR.FRAME_INVALID);
});

test('a refused PATH_REF refuses its ancestors — never a shorter frame that decodes', () => {
  // The counterfactual is the point: dropping the bad child would emit `0f4005000200010061`,
  // a 9-byte FWD that decodes cleanly one component short. Silent truncation is worse than
  // emitting nothing, so the parent refuses too.
  const ragged = Uint8Array.from([...pathRefBody([[7, 3]]), 0xaa, 0xbb, 0xcc, 0xdd]);
  const sibling = bare(TYPE.NAME, Uint8Array.from([0x61]));
  const children = [sibling, rawPathRef({}, ragged)];

  // The sibling alone still encodes — the parent is refused for the PATH_REF, not for being
  // structured, so this is not a vacuous "everything is empty now" pass.
  assert.equal(hex(encode(fwdWrapping([sibling]))), '0f4005000200010061');
  assert.equal(encode(fwdWrapping(children)).length, 0);
  assert.equal(encode(fwdWrapping([fwdWrapping(children)])).length, 0, 'two levels up too');
});

test('the guard does not over-refuse: every well-formed PATH_REF encodes as it always did', () => {
  for (const name of ['ref-empty', 'ref-1host', 'ref-2host', 'ref-3host', 'ref-255-elements']) {
    const bin = vector(`path-ref/${name}`);
    assert.ok(sameBytes(encode(rawPathRef({}, bin.subarray(4))), bin), name);
  }
});

/* --------------------------------- RFC-0024 §5-§7 — the bound-path routing car --- */

test('fwd-mint-request is fwd-read with ONE bit changed — a mint ask costs no bytes', () => {
  const mint = vector('fwd/fwd-mint-request');
  const plain = vector('fwd/fwd-read');
  assert.equal(mint.length, plain.length, 'a mint request adds no bytes');
  const differing = [...mint].map((b, i) => (b === plain[i] ? -1 : i)).filter((i) => i >= 0);
  assert.equal(differing.length, 1, 'exactly one byte differs');
  assert.equal(plain[differing[0]], 0x00, "fwd-read's op byte is READ");
  assert.equal(mint[differing[0]], FWD_OP_FLAG_MINT_REQUEST, 'and the mint ask is bit 7 of it');

  // The masking rule (RFC-0024 §9.3): a forwarder switches on `op & 0x3F`, so this frame is
  // a READ everywhere but at the mint. A core reading the raw byte sees opcode 0x80.
  const mf = decodeFwd(mint);
  const pf = decodeFwd(plain);
  assert.equal(mf.op, FWD_OP.READ);
  assert.equal(mf.op, pf.op);
  assert.equal(mf.mintRequest, true);
  assert.equal(pf.mintRequest, false);
  // The mint REQUEST is canonically addressed: the canonical path IS the mint key.
  assert.equal(mf.dstBound, false);
});

test('fwd-mint-reply carries the minted binding as its LAST child', () => {
  const bin = vector('fwd/fwd-mint-reply');
  const tlv = decode(bin);
  assert.equal(tlv.type, TYPE.FWD);
  // op / dst / src / kind / payload / PATH_REF — appended, nothing displaced. Position is
  // the pin that matters: anywhere but last and every positional reader of a reply shifts.
  assert.equal(tlv.children.length, 6);
  assert.equal(tlv.children[3].payload[0], 0x00, 'kind = RESULT');
  const mintTlv = tlv.children[5];
  assert.equal(mintTlv.type, TYPE.PATH_REF);
  assert.equal(mintTlv.opt.pl, false);
  assert.equal(mintTlv.children.length, 0);
  // A terminus answers exactly one element — its own reference to the target vertex.
  assert.equal(mintTlv.payload.length / PATH_REF_ELEMENT_BYTES, 1);
  assert.ok(sameBytes(mintTlv.payload, pathRefBody([[2, 0]])), 'index=2 generation=0');
  // 4 + 8 bytes on the reply, and none anywhere else.
  assert.equal(hex(bin.subarray(bin.length - 12)), '140008000200000000000000');
});

test('acl/bound-vs-canonical-allow is the bound spelling, and it is the shorter one', () => {
  const bound = vector('acl/bound-vs-canonical-allow');
  const canonical = vector('fwd/fwd-read');
  assert.equal(
    hex(bound),
    '0f402100010001000014000800020000000000000006400c00020008007265706c792d6570',
  );
  // The byte count IS the case for the form, so it is pinned against the twin.
  assert.ok(bound.length < canonical.length, `${bound.length} < ${canonical.length}`);

  const pf = decodeFwd(bound);
  assert.equal(pf.op, FWD_OP.READ);
  assert.equal(pf.dstBound, true);
  assert.equal(pf.dst.type, TYPE.PATH_REF);
  assert.ok(sameBytes(pf.dst.payload, pathRefBody([[2, 0]])));
  // …and the canonical twin's dst is a PATH of NAME children: the two forms, side by side.
  const cf = decodeFwd(canonical);
  assert.equal(cf.dstBound, false);
  assert.equal(cf.dst.type, TYPE.PATH);
});

test('acl/bound-vs-canonical-deny denies, carries no mint, and agrees on the outcome', () => {
  const bin = vector('acl/bound-vs-canonical-deny');
  const pf = decodeFwd(bin);
  assert.equal(pf.op, FWD_OP.REPLY);
  // `src` echoes the request's dst — the bound form here, and the ONE thing that cannot
  // agree between two spellings of one address.
  assert.equal(pf.src.type, TYPE.PATH_REF);
  assert.equal(pf.kind, FWD_KIND.ERROR);
  assert.equal(replyErrorCode(pf), FWD_ERROR.PERMISSION_DENIED);

  // Nothing past the STATUS: a denial never hands back a handle to what it refused (§6.1).
  const tlv = decode(bin);
  assert.equal(tlv.children.length, 5);
  assert.ok(!tlv.children.some((c) => c.type === TYPE.PATH_REF && c !== tlv.children[2]));

  // The outcome tail is what the canonical spelling produces byte for byte — §6.3's claim.
  assert.equal(hex(bin.subarray(bin.length - 19)), '010001000109400a0008400600010002005000');
});

/* ------------------------------ RFC-0024 §3.4/§5 — the forwarder hop (car 3) --- */

test('fwd-bound-forward / fwd-bound-forwarded are one hop apart, byte for byte', () => {
  const before = vector('fwd/fwd-bound-forward');
  const after = vector('fwd/fwd-bound-forwarded');
  assert.equal(
    hex(before),
    '0f4031000100010000140010000100000000000000efbe00000700000006400c00020008007265706c792d65700100040009000000',
  );
  assert.equal(
    hex(after),
    '0f403000010001000014000800efbe0000070000000640130002000300636c69020008007265706c792d65700100040009000000',
  );

  const bf = decodeFwd(before);
  const af = decodeFwd(after);
  assert.equal(bf.op, FWD_OP.READ);
  assert.equal(af.op, bf.op, 'a hop relays the opcode it was given');
  assert.equal(bf.dstBound, true);
  assert.equal(af.dstBound, true);

  // The shrink: TWO elements arrive, ONE leaves, and the one that leaves is the NEXT host's
  // — element 0 was this host's own and is consumed, never rewritten (RFC-0024 §4.1).
  assert.equal(bf.dst.payload.length / PATH_REF_ELEMENT_BYTES, 2);
  assert.equal(af.dst.payload.length / PATH_REF_ELEMENT_BYTES, 1);
  assert.ok(sameBytes(bf.dst.payload, pathRefBody([[1, 0], [0xbeef, 7]])));
  assert.ok(sameBytes(af.dst.payload, bf.dst.payload.subarray(PATH_REF_ELEMENT_BYTES)));
  assert.equal(af.dst.opt.pl, false, 'the re-headed PATH_REF keeps PL clear — a record array');

  // The grow: `src` accumulates CANONICALLY on a bound frame, so the return route is the one
  // every canonical hop builds and a peer that never speaks the bound form still answers.
  assert.equal(bf.src.type, TYPE.PATH);
  assert.equal(af.src.type, TYPE.PATH);
  assert.equal(bf.src.children.length, 1, 'src = /reply-ep');
  assert.equal(af.src.children.length, 2, 'src = /cli/reply-ep — the inbound mount prepended');

  // Exactly 8 bytes of dst left, and the payload rode through untouched.
  assert.equal(before.length - after.length, PATH_REF_ELEMENT_BYTES - 7 /* src grew by 7 */);
  assert.equal(hex(before.subarray(before.length - 8)), hex(after.subarray(after.length - 8)));
});

/* -------------------------------- #877 — the creation SPEC's field value TYPE --- */

/**
 * @brief `spec/conn-client-ws` — `encodeConnSpec` reproduces the shared vector exactly.
 *
 * TypeScript is the parity reference for this vector: it already emitted the correct
 * shape, so this test passing is what confirms the vector encodes the right bytes rather
 * than merely the bytes one core happens to produce. `test/conn-spec.test.mjs` pins the
 * same bytes against the C++ emitter's captured output; this pins them against the file
 * every core now reads.
 *
 * The load-bearing detail is the value TYPE of each field. `type`/`name` and the string
 * settings `kind`/`addr` are NAME (0x02) nodes; only the integer settings `role`/`port`
 * are VALUE (0x01). The terminus matches each (NAME key, value) pair on the value's type,
 * so the two are not interchangeable — and since a VALUE-typed SPEC round-trips itself
 * perfectly, the codec harness cannot tell the difference. This byte pin can.
 */
test('encodeConnSpec matches the spec/conn-client-ws vector byte-for-byte', () => {
  const expected = vector('spec/conn-client-ws');
  const built = encodeConnSpec({
    type: 'client',
    name: 'up',
    role: 'dial',
    port: 8080,
    kind: 'ws',
    addr: '127.0.0.1',
  });
  assert.ok(sameBytes(built, expected), `built ${hex(built)} != ${hex(expected)}`);
});

/**
 * @brief `spec/create-child` — the minimal creation SPEC, and the type assertion on both
 * of its field values.
 *
 * The client has no builder for a bare (config-less) creation SPEC, so this decodes the
 * vector and asserts the shape every emitter must produce: two NAME-keyed pairs whose
 * VALUES are themselves NAME nodes.
 */
test('spec/create-child carries NAME-typed field values, not VALUE-typed ones', () => {
  const dec = decode(vector('spec/create-child'));
  assert.equal(dec.type, TYPE.SPEC);
  assert.equal(dec.opt.pl, true);
  assert.equal(dec.children.length, 4, 'two (key, value) pairs');
  const text = (t) => Buffer.from(t.payload).toString('utf8');
  for (const child of dec.children) {
    assert.equal(child.type, TYPE.NAME, 'every SPEC child here is a NAME — keys and values alike');
  }
  assert.equal(text(dec.children[0]), 'type');
  assert.equal(text(dec.children[1]), 'stored_value');
  assert.equal(text(dec.children[2]), 'name');
  assert.equal(text(dec.children[3]), 'temp');
});
