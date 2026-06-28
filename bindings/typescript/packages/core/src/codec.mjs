// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * libtracer core wire codec — a pure-TypeScript/JavaScript port of the C++
 * reference codec in `core/src/frame.cpp`. It must match that implementation
 * byte-for-byte; correctness is gated by the SHARED conformance vectors under
 * `tests/conformance/vectors/v1/`.
 *
 * The wire format is documented in `docs/reference/01-data-format.md`:
 *   header (4 or 6 bytes) + payload + optional trailer (timestamp then CRC).
 *
 * Authored as ESM JavaScript (not `.ts`) so it runs directly under Node with no
 * build step — that keeps the polyglot conformance driver (`node harness.mjs`)
 * dependency-free. TypeScript declarations are generated from the JSDoc by
 * `tsc` for the published `@avatarsd-llc/libtracer` package.
 */

/**
 * Core type-code registry (0x01-0x10). 0x05 is retired (was LIST, ADR-0003).
 * 0x0E SPEC is the in-band vertex-creation spec. 0x0F FWD and 0x10 FIELD are the
 * remote-operation frames (RFC-0004 / ADR-0035, the v1 fast-track range
 * 0x0F-0x1F). All are structured (opt.PL=1) and handled generically by the codec.
 *
 * @readonly
 * @enum {number}
 */
export const TYPE = Object.freeze({
  VALUE: 0x01,
  NAME: 0x02,
  DESCRIPTION: 0x03,
  SUBSCRIBER: 0x04,
  PATH: 0x06,
  POINT: 0x07,
  ERROR: 0x08,
  STATUS: 0x09,
  ACL: 0x0a,
  SETTINGS: 0x0b,
  TIME: 0x0c,
  ROUTER: 0x0d,
  SPEC: 0x0e,
  FWD: 0x0f,
  FIELD: 0x10,
});

/**
 * Decode failure reasons, named after the C++ `tr::wire::error_t` enum.
 *
 * @readonly
 * @enum {string}
 */
export const ERROR = Object.freeze({
  FRAME_TRUNCATED: 'FRAME_TRUNCATED', // ran out of bytes
  FRAME_INVALID: 'FRAME_INVALID', // reserved bit, type 0x00, trailing bytes
  FRAME_CRC_FAIL: 'FRAME_CRC_FAIL', // trailer CRC mismatch
  TLV_NESTING_TOO_DEEP: 'TLV_NESTING_TOO_DEEP', // depth cap exceeded
});

/** The iterative-parser depth cap (docs/reference/01 §iterative parsing). */
export const MAX_DEPTH = 32;

/** Reserved bits 7 and 0; non-zero => FRAME_INVALID. */
const RESERVED_MASK = 0b1000_0001;

/**
 * A decode/encode error carrying one of {@link ERROR} as `.code`.
 */
export class CodecError extends Error {
  /** @param {string} code one of {@link ERROR} */
  constructor(code) {
    super(code);
    this.name = 'CodecError';
    /** @type {string} */
    this.code = code;
  }
}

/**
 * The 1-byte `opt` field, unpacked. Bits MSB->LSB: R | PL | TS | CR | LL | CW | TF | R.
 *
 * @typedef {object} Opt
 * @property {boolean} pl  bit 6: payload is structured (children, not opaque)
 * @property {boolean} ts  bit 5: trailer carries a timestamp
 * @property {boolean} cr  bit 4: trailer carries a CRC
 * @property {boolean} ll  bit 3: length width (false = u16, true = u32)
 * @property {boolean} cw  bit 2: CRC width (false = CRC-32C, true = CRC-16-CCITT)
 * @property {boolean} tf  bit 1: timestamp form (false = abs u64, true = rel i32)
 */

/**
 * A decoded wire timestamp. Absolute form is a u64 (BigInt) of ns since the Unix
 * epoch; relative form is a signed i32 ns offset from the parent timestamp.
 *
 * @typedef {object} Timestamp
 * @property {boolean} relative  false = absolute u64 ns; true = relative i32 ns
 * @property {bigint} value      ns; BigInt to preserve the full u64 range exactly
 */

/**
 * A decoded CRC. 16-bit values are zero-extended into `value`.
 *
 * @typedef {object} Crc
 * @property {'CRC32C'|'CRC16_CCITT'} width
 * @property {number} value
 */

/**
 * @typedef {object} Trailer
 * @property {Timestamp|null} ts
 * @property {Crc|null} crc
 */

/**
 * A decoded TLV. For opaque TLVs (opt.PL=0) `payload` holds the bytes and
 * `children` is empty; for structured TLVs (opt.PL=1) `children` holds the
 * parsed sub-TLVs and `payload` is empty.
 *
 * @typedef {object} Tlv
 * @property {number} type        a {@link TYPE} code (or a user/unknown code)
 * @property {Opt} opt
 * @property {Uint8Array} payload
 * @property {Tlv[]} children
 * @property {Trailer|null} trailer
 */

/* ------------------------------------------------------------------ opt --- */

/**
 * @param {number} b raw opt byte
 * @returns {boolean} true if a reserved bit (7 or 0) is set
 */
function reservedSet(b) {
  return (b & RESERVED_MASK) !== 0;
}

/**
 * @param {number} b raw opt byte
 * @returns {Opt}
 */
function decodeOpt(b) {
  return {
    pl: (b & 0x40) !== 0,
    ts: (b & 0x20) !== 0,
    cr: (b & 0x10) !== 0,
    ll: (b & 0x08) !== 0,
    cw: (b & 0x04) !== 0,
    tf: (b & 0x02) !== 0,
  };
}

/**
 * @param {Opt} o
 * @returns {number} packed opt byte
 */
function encodeOpt(o) {
  return (
    (o.pl ? 0x40 : 0) |
    (o.ts ? 0x20 : 0) |
    (o.cr ? 0x10 : 0) |
    (o.ll ? 0x08 : 0) |
    (o.cw ? 0x04 : 0) |
    (o.tf ? 0x02 : 0)
  );
}

/* --------------------------------------------------------------- endian --- */

/**
 * Read `n` (<= 4) little-endian bytes at `off` as an unsigned Number.
 *
 * @param {Uint8Array} b
 * @param {number} off
 * @param {number} n
 * @returns {number}
 */
function readLe(b, off, n) {
  let v = 0;
  for (let i = 0; i < n; i++) v += b[off + i] * 2 ** (8 * i);
  return v;
}

/**
 * Read 8 little-endian bytes at `off` as an unsigned BigInt.
 *
 * @param {Uint8Array} b
 * @param {number} off
 * @returns {bigint}
 */
function readLe64(b, off) {
  let v = 0n;
  for (let i = 0; i < 8; i++) v += BigInt(b[off + i]) << BigInt(8 * i);
  return v;
}

/**
 * Append `n` (<= 4) little-endian bytes of unsigned Number `v`.
 *
 * @param {number[]} out
 * @param {number} v
 * @param {number} n
 */
function writeLe(out, v, n) {
  for (let i = 0; i < n; i++) out.push((v >>> (8 * i)) & 0xff);
}

/**
 * Append 8 little-endian bytes of unsigned BigInt `v`.
 *
 * @param {number[]} out
 * @param {bigint} v
 */
function writeLe64(out, v) {
  let x = BigInt.asUintN(64, v);
  for (let i = 0; i < 8; i++) {
    out.push(Number(x & 0xffn));
    x >>= 8n;
  }
}

/* ------------------------------------------------------------------ crc --- */

const CRC32C_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0x82f63b78 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

const CRC16_TABLE = (() => {
  const t = new Uint16Array(256);
  for (let i = 0; i < 256; i++) {
    let c = (i << 8) & 0xffff;
    for (let k = 0; k < 8; k++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff;
    t[i] = c;
  }
  return t;
})();

/**
 * CRC-32C (Castagnoli): reflected poly 0x82F63B78, init/xor 0xFFFFFFFF.
 *
 * @param {Uint8Array} data
 * @returns {number} unsigned 32-bit
 */
export function crc32c(data) {
  let c = 0xffffffff;
  for (let i = 0; i < data.length; i++) c = CRC32C_TABLE[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/**
 * CRC-16-CCITT (FALSE): poly 0x1021, MSB-first, init 0xFFFF, no final xor.
 *
 * @param {Uint8Array} data
 * @returns {number} unsigned 16-bit
 */
export function crc16ccitt(data) {
  let c = 0xffff;
  for (let i = 0; i < data.length; i++) {
    const idx = ((c >> 8) ^ data[i]) & 0xff;
    c = (CRC16_TABLE[idx] ^ (c << 8)) & 0xffff;
  }
  return c;
}

/* --------------------------------------------------------------- decode --- */

/**
 * Parse exactly one TLV at the front of `buf`, WITHOUT recursing into children.
 * Mirrors `parse_one` in frame.cpp.
 *
 * @param {Uint8Array} buf
 * @returns {{tlv: Tlv, total: number, children: Uint8Array}}
 */
function parseOne(buf) {
  if (buf.length < 4) throw new CodecError(ERROR.FRAME_TRUNCATED);

  const typeB = buf[0];
  const optB = buf[1];
  if (typeB === 0x00) throw new CodecError(ERROR.FRAME_INVALID);
  if (reservedSet(optB)) throw new CodecError(ERROR.FRAME_INVALID);

  const opt = decodeOpt(optB);
  const header = opt.ll ? 6 : 4;
  if (buf.length < header) throw new CodecError(ERROR.FRAME_TRUNCATED);

  const length = readLe(buf, 2, opt.ll ? 4 : 2);
  const tsSize = opt.ts ? (opt.tf ? 4 : 8) : 0;
  const crcSize = opt.cr ? (opt.cw ? 2 : 4) : 0;
  const total = header + length + tsSize + crcSize;
  if (buf.length < total) throw new CodecError(ERROR.FRAME_TRUNCATED);

  /** @type {Tlv} */
  const tlv = {
    type: typeB,
    opt,
    payload: new Uint8Array(0),
    children: [],
    trailer: null,
  };

  const payload = buf.subarray(header, header + length);

  if (opt.ts || opt.cr) {
    /** @type {Trailer} */
    const trailer = { ts: null, crc: null };
    if (opt.ts) {
      if (opt.tf) {
        // signed i32
        const raw = readLe(buf, header + length, 4);
        trailer.ts = { relative: true, value: BigInt(raw | 0) };
      } else {
        trailer.ts = { relative: false, value: readLe64(buf, header + length) };
      }
    }
    if (opt.cr) {
      const crcOff = header + length + tsSize;
      // CRC covers payload bytes followed by timestamp bytes (NOT the header).
      const covered = buf.subarray(header, header + length + tsSize);
      if (opt.cw) {
        const value = readLe(buf, crcOff, 2);
        if (crc16ccitt(covered) !== value) throw new CodecError(ERROR.FRAME_CRC_FAIL);
        trailer.crc = { width: 'CRC16_CCITT', value };
      } else {
        const value = readLe(buf, crcOff, 4);
        if (crc32c(covered) !== value) throw new CodecError(ERROR.FRAME_CRC_FAIL);
        trailer.crc = { width: 'CRC32C', value };
      }
    }
    tlv.trailer = trailer;
  }

  // For a structured TLV (opt.pl) decode() walks `children` (the payload region)
  // with an explicit stack; for an opaque TLV the bytes live in tlv.payload.
  if (!opt.pl) tlv.payload = payload;
  return { tlv, total, children: payload };
}

/**
 * Decode exactly one TLV that fills `input` into a TLV tree. Nesting is parsed
 * ITERATIVELY with an explicit stack (recursion is forbidden) and capped at
 * {@link MAX_DEPTH}. Trailing bytes after the root => FRAME_INVALID.
 *
 * @param {Uint8Array} input
 * @returns {Tlv}
 */
export function decode(input) {
  const root = parseOne(input);
  if (root.total !== input.length) throw new CodecError(ERROR.FRAME_INVALID); // trailing bytes
  if (!root.tlv.opt.pl) return root.tlv; // opaque root: done

  // An open structured node whose children are still being parsed. `pos` is the
  // cursor within `payload`; `total` advances the parent's cursor when it closes.
  const stack = [{ node: root.tlv, payload: root.children, pos: 0, total: root.total }];

  for (;;) {
    const top = stack[stack.length - 1];
    if (top.pos === top.payload.length) {
      // Node complete — pop and graft onto its parent (or return if root).
      const done = top;
      stack.pop();
      if (stack.length === 0) return done.node;
      const parent = stack[stack.length - 1];
      parent.node.children.push(done.node);
      parent.pos += done.total;
      continue;
    }
    // A child of `top` sits at depth == stack.length; reject at the cap.
    if (stack.length >= MAX_DEPTH) throw new CodecError(ERROR.TLV_NESTING_TOO_DEEP);
    const child = parseOne(top.payload.subarray(top.pos));
    if (child.tlv.opt.pl) {
      stack.push({ node: child.tlv, payload: child.children, pos: 0, total: child.total });
    } else {
      top.node.children.push(child.tlv);
      top.pos += child.total;
    }
  }
}

/* --------------------------------------------------------------- encode --- */

/**
 * Encode a TLV tree back to wire bytes, recomputing the trailer CRC from the
 * body (+ timestamp bytes) when opt.CR is set. Mirrors `encode` in frame.cpp.
 *
 * @param {Tlv} tlv
 * @returns {Uint8Array}
 */
export function encode(tlv) {
  /** @type {number[]} */
  let body = [];
  if (tlv.opt.pl) {
    for (const child of tlv.children) {
      const cb = encode(child);
      for (let i = 0; i < cb.length; i++) body.push(cb[i]);
    }
  } else {
    for (let i = 0; i < tlv.payload.length; i++) body.push(tlv.payload[i]);
  }

  /** @type {number[]} */
  const out = [];
  out.push(tlv.type & 0xff);
  out.push(encodeOpt(tlv.opt));
  writeLe(out, body.length, tlv.opt.ll ? 4 : 2);
  for (let i = 0; i < body.length; i++) out.push(body[i]);

  /** @type {number[]} */
  const tsBytes = [];
  if (tlv.opt.ts) {
    const t = tlv.trailer && tlv.trailer.ts ? tlv.trailer.ts : { relative: tlv.opt.tf, value: 0n };
    if (tlv.opt.tf) {
      writeLe(tsBytes, Number(BigInt.asUintN(32, BigInt(t.value))), 4);
    } else {
      writeLe64(tsBytes, BigInt(t.value));
    }
    for (let i = 0; i < tsBytes.length; i++) out.push(tsBytes[i]);
  }
  if (tlv.opt.cr) {
    const covered = Uint8Array.from(body.concat(tsBytes));
    if (tlv.opt.cw) {
      writeLe(out, crc16ccitt(covered), 2);
    } else {
      writeLe(out, crc32c(covered), 4);
    }
  }
  return Uint8Array.from(out);
}

/**
 * Structural + byte-content equality of two TLV trees.
 *
 * @param {Tlv} a
 * @param {Tlv} b
 * @returns {boolean}
 */
export function equal(a, b) {
  if (a.type !== b.type) return false;
  if (encodeOpt(a.opt) !== encodeOpt(b.opt)) return false;
  if (!trailerEqual(a.trailer, b.trailer)) return false;
  if (a.payload.length !== b.payload.length) return false;
  for (let i = 0; i < a.payload.length; i++) if (a.payload[i] !== b.payload[i]) return false;
  if (a.children.length !== b.children.length) return false;
  for (let i = 0; i < a.children.length; i++) if (!equal(a.children[i], b.children[i])) return false;
  return true;
}

/**
 * @param {Trailer|null} a
 * @param {Trailer|null} b
 * @returns {boolean}
 */
function trailerEqual(a, b) {
  if (a === null || b === null) return a === b;
  const tsEq =
    a.ts === null || b.ts === null
      ? a.ts === b.ts
      : a.ts.relative === b.ts.relative && BigInt(a.ts.value) === BigInt(b.ts.value);
  if (!tsEq) return false;
  const crcEq =
    a.crc === null || b.crc === null
      ? a.crc === b.crc
      : a.crc.width === b.crc.width && a.crc.value === b.crc.value;
  return crcEq;
}
