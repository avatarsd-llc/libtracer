// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// Pure, side-effect-free TLV builders for the libtracer client SDK (#56,
// ADR-0034). Each function produces the EXACT wire bytes the shared conformance
// vectors pin (`tests/conformance/vectors/v1/`) via the cross-validated core
// codec (`@avatarsd-llc/libtracer`) — nothing here invents wire structure:
//
//   - encodeValue      -> VALUE TLV (0x01)        : value-bool-true / value-ll-u32 / value-ts-abs
//   - encodePath       -> PATH TLV (0x06, PL=1)   : path-sensor-temp + spec/v1.md §3.1
//   - encodeSubscriber -> SUBSCRIBER TLV (0x04)   : subscriber-path  (the subscribe-write payload)
//
// These are the only client byte-products v1 pins. The path-ADDRESSED request
// envelope (verb + destination vertex:field) is unspecified (spec/v1.md §3 is
// "to be written") and is deferred per ADR-0034 — it is NOT built here.

import { TYPE, encode } from '@avatarsd-llc/libtracer';
import type { Opt, Tlv } from '@avatarsd-llc/libtracer';

/** Path-segment constraints from reference/03-addressing.md §path syntax. */
const MAX_SEGMENT_BYTES = 64;
/** Reserved characters that MUST NOT appear inside a NAME segment (reference/03, /05 §NAME). */
const RESERVED_SEGMENT_CHARS = /[/:.[\]*?]/;

const utf8 = new TextEncoder();

/** A fully-cleared option byte (all flags false). */
function opt(over: Partial<Opt> = {}): Opt {
  return { pl: false, ts: false, cr: false, ll: false, cw: false, tf: false, ...over };
}

/** Options for {@link encodeValue}. Mirror the wire opt/trailer fields the codec pins. */
export interface ValueOptions {
  /** Use the 32-bit length field (opt.LL=1, 6-byte header). Default: 16-bit. */
  longLength?: boolean;
  /** Emit a CRC trailer over the payload (+ timestamp bytes). Default: none. */
  crc?: boolean;
  /** With {@link ValueOptions.crc}, use CRC-16-CCITT (opt.CW=1) instead of CRC-32C. */
  crc16?: boolean;
  /** Absolute u64 wire timestamp (ns since epoch) → opt.TS=1, TF=0 (absolute). */
  timestampNs?: bigint;
  /** Relative i32 wire timestamp (ns) → opt.TS=1, TF=1 (relative). Ignored if {@link ValueOptions.timestampNs} is set. */
  timestampRelNs?: number;
}

/**
 * Build a VALUE TLV (`type=0x01`) carrying an opaque application payload.
 * Vector-pinned: `value-bool-true`, `value-ll-u32`, `value-ts-abs`.
 *
 * @param value the opaque payload bytes (the publisher/subscriber agree on the shape out-of-band)
 * @param opts  optional length-width / CRC / wire-timestamp selectors
 * @returns the encoded VALUE TLV bytes (one complete frame)
 */
export function encodeValue(value: Uint8Array, opts: ValueOptions = {}): Uint8Array {
  const hasAbs = opts.timestampNs !== undefined;
  const hasRel = !hasAbs && opts.timestampRelNs !== undefined;
  const hasTs = hasAbs || hasRel;

  const tlv: Tlv = {
    type: TYPE.VALUE,
    opt: opt({ ll: !!opts.longLength, cr: !!opts.crc, cw: !!opts.crc16, ts: hasTs, tf: hasRel }),
    payload: value,
    children: [],
    trailer: hasTs
      ? {
          ts: hasAbs
            ? { relative: false, value: opts.timestampNs as bigint }
            : { relative: true, value: BigInt(opts.timestampRelNs as number) },
          crc: null,
        }
      : null,
  };
  return encode(tlv);
}

/**
 * Build a NAME TLV (`type=0x02`) for one path segment, after validating it
 * against the addressing rules (1..64 UTF-8 bytes, no reserved characters).
 *
 * @param segment the segment text
 * @returns a NAME TLV node (no trailer)
 * @throws {RangeError} if the segment is empty, over 64 bytes, or holds a reserved char
 */
function nameTlv(segment: string): Tlv {
  if (RESERVED_SEGMENT_CHARS.test(segment)) {
    throw new RangeError(`path segment ${JSON.stringify(segment)} contains a reserved character (/ : . [ ] * ?)`);
  }
  const bytes = utf8.encode(segment);
  if (bytes.length < 1 || bytes.length > MAX_SEGMENT_BYTES) {
    throw new RangeError(`path segment ${JSON.stringify(segment)} must be 1..${MAX_SEGMENT_BYTES} UTF-8 bytes (got ${bytes.length})`);
  }
  return { type: TYPE.NAME, opt: opt(), payload: bytes, children: [], trailer: null };
}

/**
 * Build a PATH TLV (`type=0x06`, PL=1) from path segments — a NAME child per
 * segment. Vector-pinned: `path-sensor-temp`; normative byte layout: spec/v1.md §3.1.
 *
 * @param segments the path segments, e.g. `["sensor", "temp"]` for `/sensor/temp`
 * @returns the encoded PATH TLV bytes
 * @throws {RangeError} on an empty path or an invalid segment
 */
export function encodePath(segments: string[]): Uint8Array {
  return encode(pathTlv(segments));
}

/** The PATH TLV node (shared by {@link encodePath} and {@link encodeSubscriber}). */
function pathTlv(segments: string[]): Tlv {
  if (segments.length < 1) throw new RangeError('a path must have at least one segment');
  if (segments.length > 32) throw new RangeError(`a path may have at most 32 segments (got ${segments.length})`);
  return {
    type: TYPE.PATH,
    opt: opt({ pl: true }),
    payload: new Uint8Array(0),
    children: segments.map(nameTlv),
    trailer: null,
  };
}

/** Options for {@link encodeSubscriber}. Optional QoS/ACL/id children are deferred (ADR-0034). */
export interface SubscriberOptions {
  // Intentionally empty for the conservative slice. The optional SUBSCRIBER
  // children (SETTINGS qos, ACL capability, NAME subscriber_id — reference/05
  // §SUBSCRIBER) are additive and land when their semantics are exercised.
}

/**
 * Build a SUBSCRIBER TLV (`type=0x04`, PL=1) wrapping a target PATH — the
 * payload of a subscribe-write (reference/04 §Subscribe). Vector-pinned:
 * `subscriber-path` = `SUBSCRIBER{ PATH{ NAME "sensor", NAME "temp" } }`.
 *
 * The `targetPath` is the SUBSCRIBER's `target_path` child: where the producer
 * dispatches matched writes (reference/05 §SUBSCRIBER).
 *
 * @param targetPath the delivery target path segments
 * @param _opts      reserved for future optional children (none in this slice)
 * @returns the encoded SUBSCRIBER TLV bytes
 */
export function encodeSubscriber(targetPath: string[], _opts: SubscriberOptions = {}): Uint8Array {
  const tlv: Tlv = {
    type: TYPE.SUBSCRIBER,
    opt: opt({ pl: true }),
    payload: new Uint8Array(0),
    children: [pathTlv(targetPath)],
    trailer: null,
  };
  return encode(tlv);
}
