// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// RFC 6455 WebSocket frame codec — a pure-TS port of the C++ reference codec in
// core/include/libtracer/ws.hpp (`tr::net::ws`). It is socket-free and has no
// I/O; correctness is gated byte-for-byte against the same known vectors the C++
// codec is tested with (core/tests/ws_test.cpp). This is the cross-implementation
// agreement layer: a TS client framed by this codec interoperates with the C++
// `tr::net::transport_ws` server, and vice versa.
//
// Two concerns only, mirroring the C++ header:
//   1. the opening-handshake key derivation (SHA-1 + base64 -> Sec-WebSocket-Accept), and
//   2. the data-frame codec (decode one masked/unmasked frame; encode one
//      server frame unmasked, or one client frame masked, FIN=1).

/** RFC 6455 frame opcodes (the subset libtracer cares about). */
export enum Opcode {
  CONT = 0x0,
  TEXT = 0x1,
  BINARY = 0x2,
  CLOSE = 0x8,
  PING = 0x9,
  PONG = 0xa,
}

/** One decoded RFC 6455 data/control frame (payload already unmasked). */
export interface Frame {
  /** Frame opcode. */
  op: Opcode;
  /** FIN bit (true = final fragment). */
  fin: boolean;
  /** Unmasked application payload. */
  payload: Uint8Array;
}

/* --------------------------------------------------------------- sha-1 --- */

function rotl32(v: number, n: number): number {
  return ((v << n) | (v >>> (32 - n))) >>> 0;
}

/**
 * Standard SHA-1 over an arbitrary byte array (RFC 3174 / FIPS 180-1). Returns
 * the 20-byte digest. Used by {@link acceptKey} for the RFC 6455 opening
 * handshake; SHA-1 is required there for protocol compatibility, not security.
 */
export function sha1(data: Uint8Array): Uint8Array {
  let h0 = 0x67452301;
  let h1 = 0xefcdab89;
  let h2 = 0x98badcfe;
  let h3 = 0x10325476;
  let h4 = 0xc3d2e1f0;

  const bitLen = data.length * 8;

  // Padded message: original bytes, 0x80, zero pad to 56 mod 64, then the 64-bit
  // big-endian bit length.
  const msg: number[] = Array.from(data);
  msg.push(0x80);
  while (msg.length % 64 !== 56) msg.push(0x00);
  // bitLen fits in 53 bits of a Number for any realistic handshake input.
  const hi = Math.floor(bitLen / 0x100000000);
  const lo = bitLen >>> 0;
  for (let i = 7; i >= 0; i--) {
    const v = i >= 4 ? hi : lo;
    msg.push((v >>> ((i % 4) * 8)) & 0xff);
  }

  const w = new Uint32Array(80);
  for (let chunk = 0; chunk < msg.length; chunk += 64) {
    for (let i = 0; i < 16; i++) {
      w[i] =
        ((msg[chunk + i * 4 + 0] << 24) |
          (msg[chunk + i * 4 + 1] << 16) |
          (msg[chunk + i * 4 + 2] << 8) |
          msg[chunk + i * 4 + 3]) >>>
        0;
    }
    for (let i = 16; i < 80; i++) {
      w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    let a = h0;
    let b = h1;
    let c = h2;
    let d = h3;
    let e = h4;

    for (let i = 0; i < 80; i++) {
      let f = 0;
      let k = 0;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5a827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ed9eba1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8f1bbcdc;
      } else {
        f = b ^ c ^ d;
        k = 0xca62c1d6;
      }
      const tmp = (rotl32(a, 5) + (f >>> 0) + e + k + w[i]) >>> 0;
      e = d;
      d = c;
      c = rotl32(b, 30);
      b = a;
      a = tmp;
    }

    h0 = (h0 + a) >>> 0;
    h1 = (h1 + b) >>> 0;
    h2 = (h2 + c) >>> 0;
    h3 = (h3 + d) >>> 0;
    h4 = (h4 + e) >>> 0;
  }

  const out = new Uint8Array(20);
  const hs = [h0, h1, h2, h3, h4];
  for (let i = 0; i < 5; i++) {
    out[i * 4 + 0] = (hs[i] >>> 24) & 0xff;
    out[i * 4 + 1] = (hs[i] >>> 16) & 0xff;
    out[i * 4 + 2] = (hs[i] >>> 8) & 0xff;
    out[i * 4 + 3] = hs[i] & 0xff;
  }
  return out;
}

/* -------------------------------------------------------------- base64 --- */

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

/** Standard base64 encoding (RFC 4648, with '=' padding). */
export function base64(data: Uint8Array): string {
  let out = '';
  let i = 0;
  for (; i + 3 <= data.length; i += 3) {
    const n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += B64[(n >> 18) & 0x3f] + B64[(n >> 12) & 0x3f] + B64[(n >> 6) & 0x3f] + B64[n & 0x3f];
  }
  const rem = data.length - i;
  if (rem === 1) {
    const n = data[i] << 16;
    out += B64[(n >> 18) & 0x3f] + B64[(n >> 12) & 0x3f] + '==';
  } else if (rem === 2) {
    const n = (data[i] << 16) | (data[i + 1] << 8);
    out += B64[(n >> 18) & 0x3f] + B64[(n >> 12) & 0x3f] + B64[(n >> 6) & 0x3f] + '=';
  }
  return out;
}

/* ----------------------------------------------------------- accept_key --- */

const ACCEPT_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

/**
 * Compute the RFC 6455 Sec-WebSocket-Accept value for a client key:
 * `base64(sha1(clientKey + GUID))`. The server returns this in the 101 Switching
 * Protocols response to prove it spoke RFC 6455.
 *
 * @param clientKey The raw Sec-WebSocket-Key header value sent by the client.
 */
export function acceptKey(clientKey: string): string {
  const combined = clientKey + ACCEPT_GUID;
  const bytes = new Uint8Array(combined.length);
  for (let i = 0; i < combined.length; i++) bytes[i] = combined.charCodeAt(i) & 0xff;
  return base64(sha1(bytes));
}

/* ------------------------------------------------------------- framing --- */

/**
 * Encode one server->client RFC 6455 frame: FIN=1, given opcode, UNMASKED.
 * Server frames MUST NOT be masked (RFC 6455 §5.1). The length uses the smallest
 * legal encoding (7-bit, then 126 + 2-byte marker, then 127 + 8-byte marker).
 */
export function encodeFrame(op: Opcode, payload: Uint8Array): Uint8Array {
  const out: number[] = [];
  out.push(0x80 | (op & 0x0f)); // FIN=1
  const len = payload.length;
  if (len < 126) {
    out.push(len); // MASK=0
  } else if (len <= 0xffff) {
    out.push(126);
    out.push((len >> 8) & 0xff);
    out.push(len & 0xff);
  } else {
    out.push(127);
    pushU64Be(out, len);
  }
  for (let i = 0; i < len; i++) out.push(payload[i]);
  return Uint8Array.from(out);
}

/**
 * Encode one client->server RFC 6455 frame: FIN=1, given opcode, MASKED.
 * Client frames MUST be masked (RFC 6455 §5.1): the MASK bit is set, a 4-byte
 * masking key is emitted big-endian after the length, and every payload byte is
 * XOR'd with `maskKey[i % 4]`. `maskKey` is a 32-bit value whose 4 big-endian
 * bytes form the RFC 6455 key; it need not be cryptographically strong, only
 * varied.
 */
export function encodeClientFrame(op: Opcode, payload: Uint8Array, maskKey: number): Uint8Array {
  const out: number[] = [];
  out.push(0x80 | (op & 0x0f)); // FIN=1
  const len = payload.length;
  if (len < 126) {
    out.push(0x80 | len); // MASK=1
  } else if (len <= 0xffff) {
    out.push(0x80 | 126);
    out.push((len >> 8) & 0xff);
    out.push(len & 0xff);
  } else {
    out.push(0x80 | 127);
    pushU64Be(out, len);
  }
  const mk = [
    (maskKey >>> 24) & 0xff,
    (maskKey >>> 16) & 0xff,
    (maskKey >>> 8) & 0xff,
    maskKey & 0xff,
  ];
  for (const m of mk) out.push(m);
  for (let i = 0; i < len; i++) out.push(payload[i] ^ mk[i % 4]);
  return Uint8Array.from(out);
}

/**
 * Decode exactly one RFC 6455 frame from the front of `buf`. Handles the FIN
 * bit, opcode, the MASK bit with its 4-byte key (client->server frames are
 * masked; the payload is unmasked here), and the 7 / 16 / 64-bit extended length
 * encodings. Overflow-safe: a 64-bit over-long length on a short buffer yields
 * need-more (`null`), never an out-of-bounds read.
 *
 * @returns `null` if `buf` does not yet hold a complete frame (need more bytes);
 *          otherwise the decoded frame and the number of bytes consumed.
 */
export function decodeFrame(buf: Uint8Array): { frame: Frame; consumed: number } | null {
  if (buf.length < 2) return null;

  const b0 = buf[0];
  const b1 = buf[1];
  const fin = (b0 & 0x80) !== 0;
  const op = (b0 & 0x0f) as Opcode;
  const masked = (b1 & 0x80) !== 0;
  let len = b1 & 0x7f;

  let pos = 2;
  if (len === 126) {
    if (buf.length < pos + 2) return null;
    len = (buf[pos] << 8) | buf[pos + 1];
    pos += 2;
  } else if (len === 127) {
    if (buf.length < pos + 8) return null;
    // Accumulate as a Number; an over-long (e.g. ~2^64) length stays huge and
    // fails the bounds check below rather than wrapping, so no OOB read occurs.
    len = 0;
    for (let i = 0; i < 8; i++) len = len * 256 + buf[pos + i];
    pos += 8;
  }

  const mask = [0, 0, 0, 0];
  if (masked) {
    if (buf.length < pos + 4) return null;
    for (let i = 0; i < 4; i++) mask[i] = buf[pos + i];
    pos += 4;
  }

  // Overflow-safe length check (mirrors the C++ `len > buf.size() - pos`).
  if (len > buf.length - pos) return null;

  const payload = new Uint8Array(len);
  for (let i = 0; i < len; i++) {
    let byte = buf[pos + i];
    if (masked) byte ^= mask[i % 4];
    payload[i] = byte;
  }

  return { frame: { op, fin, payload }, consumed: pos + len };
}

/** Append the 8 big-endian bytes of a (<= 2^53) length to `out`. */
function pushU64Be(out: number[], len: number): void {
  const hi = Math.floor(len / 0x100000000);
  const lo = len >>> 0;
  for (let i = 7; i >= 0; i--) {
    const v = i >= 4 ? hi : lo;
    out.push((v >>> ((i % 4) * 8)) & 0xff);
  }
}
