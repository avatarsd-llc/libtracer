// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// FWD (0x0F) / FIELD (0x10) builders + decoder for the libtracer client SDK
// (#56, RFC-0004, ADR-0034/0035). RFC-0004 §3 pins the remote-operation envelope
// the conservative ADR-0034 slice deferred: a self-describing FWD frame carries a
// `read`/`write`/`await`/`subscribe` to a path-as-route, and a FWD{REPLY} routes
// the result back. These builders produce the EXACT bytes the shared conformance
// vectors pin (`tests/conformance/vectors/v1/fwd/`, `.../field/`) via the
// cross-validated core codec — nothing here invents wire structure.
//
// Child order (RFC-0004 §B): op, dst (PATH), FIELD? selector, src (PATH), then
//   - REPLY:  kind (VALUE u8), payload?
//   - WRITE:  payload?
//   - AWAIT:  await_timeout? (VALUE u64 ns)
//   - READ :  nothing

import { TYPE, encode, decode } from '@avatarsd-llc/libtracer';
import type { Opt, Tlv } from '@avatarsd-llc/libtracer';
import { opt, pathTlv, nameTlv } from './tlv.js';

/** FWD `op` discriminant (RFC-0004 §B — the first child, a VALUE u8). */
export const FWD_OP = Object.freeze({
  READ: 0,
  WRITE: 1,
  AWAIT: 2,
  REPLY: 3,
} as const);

/** FWD{REPLY} `kind` discriminant (RFC-0004 §D — a VALUE u8 after `src`). */
export const FWD_KIND = Object.freeze({
  RESULT: 0,
  ERROR: 1,
} as const);

/**
 * Wire ERROR codes for a `kind=ERROR` reply's `STATUS{ ERROR u8 }` payload.
 * Mirrors the C++ reference `op_resolve.cpp` `error_code()` map (provisional
 * pending the ERROR registry, RFC-0001 §C/E / #8).
 */
export const FWD_ERROR = Object.freeze({
  NOT_FOUND: 0x01,
  PERMISSION_DENIED: 0x02,
  INVALID_PATH: 0x03,
  TYPE_MISMATCH: 0x04,
  BACKPRESSURE: 0x07,
  TIMEOUT: 0x08,
  SCHEMA_NOT_FOUND: 0x0a,
  PATH_IN_USE: 0x0e,
} as const);

const FWD_ERROR_NAME: Readonly<Record<number, string>> = Object.freeze(
  Object.fromEntries(Object.entries(FWD_ERROR).map(([k, v]) => [v, k])),
);

/** The human name of a wire ERROR code (or `UNKNOWN(0x..)`). */
export function fwdErrorName(code: number): string {
  return FWD_ERROR_NAME[code] ?? `UNKNOWN(0x${code.toString(16)})`;
}

/* ----------------------------------------------------------- value nodes --- */

/** A 1-byte VALUE TLV node (the op / kind discriminants). */
function valueU8(v: number): Tlv {
  return { type: TYPE.VALUE, opt: opt(), payload: Uint8Array.of(v & 0xff), children: [], trailer: null };
}

/** A little-endian u32 VALUE TLV node (a FIELD `[N]` index). */
function valueU32(v: number): Tlv {
  const p = new Uint8Array(4);
  new DataView(p.buffer).setUint32(0, v >>> 0, true);
  return { type: TYPE.VALUE, opt: opt(), payload: p, children: [], trailer: null };
}

/** A little-endian u64 VALUE TLV node (the AWAIT `await_timeout` ns). */
function valueU64(v: bigint): Tlv {
  const p = new Uint8Array(8);
  new DataView(p.buffer).setBigUint64(0, v, true);
  return { type: TYPE.VALUE, opt: opt(), payload: p, children: [], trailer: null };
}

/* ----------------------------------------------------------- FIELD (0x10) --- */

/** One level of a `:field` selector chain (RFC-0004 §C, one NAME + optional index). */
export interface FieldLevel {
  /** The level's field name (e.g. `"subscribers"`, `"settings"`). */
  readonly name: string;
  /**
   * The index when this level is an element selector (`[N]`). When set, the level
   * is encoded as `ELEMENT` regardless of {@link FieldLevel.mode}.
   */
  readonly index?: number;
  /**
   * `SCALAR` (no brackets, default), `ELEMENT` (`[N]` / append `[]`), or `WILDCARD`
   * (`[*]` — subscriber-path targets only).
   */
  readonly mode?: 'SCALAR' | 'ELEMENT' | 'WILDCARD';
}

/**
 * Parse a `:field` selector string into its levels. Accepts an optional leading
 * `:`; levels are split on `.`; each level may carry a trailing `[N]`, `[]`
 * (append), or `[*]` (wildcard).
 *
 * `":subscribers[]"` → `[{ name: "subscribers", mode: "ELEMENT" }]`;
 * `":subscribers[3]"` → `[{ name: "subscribers", index: 3, mode: "ELEMENT" }]`;
 * `":settings.deadline_ns"` → `[{ name: "settings" }, { name: "deadline_ns" }]`.
 *
 * @param field the selector string (with or without the leading `:`)
 * @returns the parsed levels
 * @throws {RangeError} on an empty selector or a malformed level
 */
export function parseField(field: string): FieldLevel[] {
  const body = field.startsWith(':') ? field.slice(1) : field;
  if (body.length === 0) throw new RangeError('a :field selector must name at least one level');
  return body.split('.').map((part) => {
    const m = /^([^.[\]]+)(?:\[(\*|\d*)\])?$/.exec(part);
    if (!m) throw new RangeError(`malformed :field level ${JSON.stringify(part)}`);
    const name = m[1];
    if (m[2] === undefined) return { name };
    if (m[2] === '*') return { name, mode: 'WILDCARD' as const };
    if (m[2] === '') return { name, mode: 'ELEMENT' as const };
    return { name, index: Number(m[2]), mode: 'ELEMENT' as const };
  });
}

/** Build a FIELD TLV node from its levels (RFC-0004 §C). */
function fieldTlv(levels: FieldLevel[]): Tlv {
  if (levels.length < 1) throw new RangeError('a FIELD selector needs at least one level');
  if (levels.length > 8) throw new RangeError(`a FIELD selector may have at most 8 levels (got ${levels.length})`);
  const children: Tlv[] = [];
  for (const level of levels) {
    children.push(nameTlv(level.name));
    const mode = level.mode ?? (level.index !== undefined ? 'ELEMENT' : 'SCALAR');
    if (mode === 'WILDCARD') {
      children.push(valueU8(2)); // index_mode=WILDCARD
    } else if (mode === 'ELEMENT') {
      if (level.index !== undefined) children.push(valueU32(level.index)); // [N]
      children.push(valueU8(1)); // index_mode=ELEMENT (append "[]" when no index)
    }
    // SCALAR: just the NAME (index_mode default 0).
  }
  return { type: TYPE.FIELD, opt: opt({ pl: true }), payload: new Uint8Array(0), children, trailer: null };
}

/**
 * Build a FIELD TLV (`type=0x10`, PL=1) from a selector string or pre-parsed
 * levels. Vector-pinned: `field-indexed`, `field-nested`, `field-append`.
 *
 * @param field a `:field` string (see {@link parseField}) or its levels
 * @returns the encoded FIELD TLV bytes
 */
export function encodeField(field: string | FieldLevel[]): Uint8Array {
  return encode(fieldTlv(typeof field === 'string' ? parseField(field) : field));
}

/* ------------------------------------------------------------- FWD (0x0F) --- */

/** Request to build a {@link encodeFwd} frame. `op` selects the child layout. */
export interface FwdRequest {
  /** The operation (a {@link FWD_OP} value). */
  readonly op: number;
  /** Forward route: the unresolved destination path segments (the PATH `dst`). */
  readonly dst: string[];
  /** Accumulated return route: the originator's reply endpoint segments (the PATH `src`). */
  readonly src: string[];
  /** Optional `:field` selector (string or levels). */
  readonly field?: string | FieldLevel[];
  /**
   * Optional payload — a complete TLV's encoded bytes (a VALUE to WRITE, a
   * SUBSCRIBER to subscribe, the RESULT/STATUS of a REPLY). Embedded verbatim.
   */
  readonly payload?: Uint8Array;
  /** REPLY only — the reply {@link FWD_KIND}. */
  readonly kind?: number;
  /** AWAIT only — the `await_timeout` in ns (absent ⇒ the responder's 1 s default). */
  readonly awaitTimeoutNs?: bigint;
}

/** Build the FWD child node for an embedded payload (decode the caller's TLV bytes). */
function payloadNode(payload: Uint8Array): Tlv {
  return decode(payload);
}

/**
 * Build a FWD TLV (`type=0x0F`, PL=1) — the remote-operation envelope (RFC-0004
 * §B). The children are assembled in the normative order for the request's `op`.
 * Vector-pinned: `fwd-read`, `fwd-write-value`, `fwd-await-timeout`,
 * `fwd-write-subscriber-field`, `fwd-reply-result`, `fwd-reply-error`.
 *
 * @param req the operation, routes, and optional selector/payload/kind/timeout
 * @returns the encoded FWD TLV bytes (one complete frame)
 */
export function encodeFwd(req: FwdRequest): Uint8Array {
  const children: Tlv[] = [valueU8(req.op), pathTlv(req.dst)];
  if (req.field !== undefined) {
    children.push(fieldTlv(typeof req.field === 'string' ? parseField(req.field) : req.field));
  }
  children.push(pathTlv(req.src));

  if (req.op === FWD_OP.REPLY) {
    children.push(valueU8(req.kind ?? FWD_KIND.RESULT));
    if (req.payload !== undefined) children.push(payloadNode(req.payload));
  } else if (req.op === FWD_OP.WRITE) {
    if (req.payload !== undefined) children.push(payloadNode(req.payload));
  } else if (req.op === FWD_OP.AWAIT) {
    if (req.awaitTimeoutNs !== undefined) children.push(valueU64(req.awaitTimeoutNs));
  }

  const fwd: Tlv = {
    type: TYPE.FWD,
    opt: opt({ pl: true }),
    payload: new Uint8Array(0),
    children,
    trailer: null,
  };
  return encode(fwd);
}

/* --------------------------------------------------------------- decoding --- */

/** A decoded FWD frame, its children parsed positionally (RFC-0004 §B order). */
export interface ParsedFwd {
  /** The operation (a {@link FWD_OP} value). */
  readonly op: number;
  /** The `dst` PATH child. */
  readonly dst: Tlv;
  /** The optional FIELD selector child (or `null`). */
  readonly field: Tlv | null;
  /** The `src` PATH child. */
  readonly src: Tlv;
  /** The reply {@link FWD_KIND} (REPLY frames only, else `null`). */
  readonly kind: number | null;
  /** The payload TLV child (WRITE payload / REPLY result / RESULT VALUE), or `null`. */
  readonly payload: Tlv | null;
  /** The AWAIT `await_timeout` ns (else `null`). */
  readonly awaitTimeoutNs: bigint | null;
}

const u8 = (t: Tlv): number => (t.payload.length > 0 ? t.payload[0] : 0);

/** Parse an already-decoded FWD {@link Tlv} positionally (RFC-0004 §B). */
export function parseFwdTlv(fwd: Tlv): ParsedFwd {
  if (fwd.type !== TYPE.FWD) throw new RangeError(`not a FWD TLV (type 0x${fwd.type.toString(16)})`);
  const ch = fwd.children;
  let i = 0;
  if (ch[i]?.type !== TYPE.VALUE) throw new RangeError('FWD: missing op VALUE child');
  const op = u8(ch[i++]);
  if (ch[i]?.type !== TYPE.PATH) throw new RangeError('FWD: missing dst PATH child');
  const dst = ch[i++];
  let field: Tlv | null = null;
  if (ch[i]?.type === TYPE.FIELD) field = ch[i++];
  if (ch[i]?.type !== TYPE.PATH) throw new RangeError('FWD: missing src PATH child');
  const src = ch[i++];

  let kind: number | null = null;
  let payload: Tlv | null = null;
  let awaitTimeoutNs: bigint | null = null;
  if (op === FWD_OP.REPLY) {
    if (ch[i]?.type === TYPE.VALUE) kind = u8(ch[i++]);
    if (i < ch.length) payload = ch[i++];
  } else if (op === FWD_OP.WRITE) {
    if (i < ch.length) payload = ch[i++];
  } else if (op === FWD_OP.AWAIT) {
    if (ch[i]?.type === TYPE.VALUE && ch[i].payload.length >= 8) {
      awaitTimeoutNs = new DataView(ch[i].payload.buffer, ch[i].payload.byteOffset, 8).getBigUint64(0, true);
      i++;
    }
  }
  return { op, dst, field, src, kind, payload, awaitTimeoutNs };
}

/**
 * Decode a complete FWD frame's bytes into its parsed children.
 *
 * @param bytes one complete FWD TLV frame
 * @returns the parsed FWD
 * @throws if the bytes do not decode, or are not a FWD
 */
export function decodeFwd(bytes: Uint8Array): ParsedFwd {
  return parseFwdTlv(decode(bytes));
}

/** The wire ERROR code of a `kind=ERROR` reply's `STATUS{ ERROR u8 }` payload (or 0). */
export function replyErrorCode(reply: ParsedFwd): number {
  const status = reply.payload;
  if (status && status.type === TYPE.STATUS && status.children[0]?.type === TYPE.ERROR) {
    return u8(status.children[0]);
  }
  return 0;
}

// (re-export Opt so consumers building raw nodes do not need the core import.)
export type { Opt };
