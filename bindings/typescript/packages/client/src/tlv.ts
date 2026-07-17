// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief Pure, side-effect-free TLV builders for the libtracer client SDK
 * (#56, ADR-0034).
 *
 * Each function produces the EXACT wire bytes the shared conformance vectors
 * pin (`tests/conformance/vectors/v1/`) via the cross-validated core codec
 * (`@avatarsd-llc/libtracer`) — nothing here invents wire structure:
 *
 *   - encodeValue      -> VALUE TLV (0x01)        : value-bool-true / value-ll-u32 / value-ts-abs
 *   - encodePath       -> PATH TLV (0x06, PL=1)   : path-sensor-temp + spec/v1.md §3.1
 *   - encodeSubscriber -> SUBSCRIBER TLV (0x04)   : subscriber-path  (the subscribe-write payload)
 *
 * These are the payload TLVs. The path-ADDRESSED request envelope (verb +
 * destination vertex:field) is the FWD/FIELD frame RFC-0004 (spec §3) fixes —
 * it is built in `./fwd.ts`, not here; these payloads ride inside it.
 */

import { TYPE, encode } from '@avatarsd-llc/libtracer';
import type { Opt, Tlv } from '@avatarsd-llc/libtracer';

/** @brief Path-segment constraints from reference/03-addressing.md §path syntax. */
const MAX_SEGMENT_BYTES = 64;
/** @brief Reserved characters that MUST NOT appear inside a NAME segment (reference/03, /05 §NAME). */
const RESERVED_SEGMENT_CHARS = /[/:.[\]*?]/;

const utf8 = new TextEncoder();

/**
 * @brief A fully-cleared option byte (all flags false), with selected flags
 * overridden.
 *
 * Shared by the payload builders here and the FWD/FIELD builders in `./fwd.ts`.
 *
 * @param over flags to set true (e.g. `{ pl: true }` for a structured TLV)
 * @returns a complete {@link Opt}
 */
export function opt(over: Partial<Opt> = {}): Opt {
  return { pl: false, ts: false, cr: false, ll: false, cw: false, tf: false, ...over };
}

/** @brief Options for {@link encodeValue}. Mirror the wire opt/trailer fields the codec pins. */
export interface ValueOptions {
  /** @brief Use the 32-bit length field (opt.LL=1, 6-byte header). Default: 16-bit. */
  longLength?: boolean;
  /** @brief Emit a CRC trailer over the payload (+ timestamp bytes). Default: none. */
  crc?: boolean;
  /** @brief With {@link ValueOptions.crc}, use CRC-16-CCITT (opt.CW=1) instead of CRC-32C. */
  crc16?: boolean;
  /** @brief Absolute u64 wire timestamp (ns since epoch) → opt.TS=1, TF=0 (absolute). */
  timestampNs?: bigint;
  /** @brief Relative i32 wire timestamp (ns) → opt.TS=1, TF=1 (relative). Ignored if {@link ValueOptions.timestampNs} is set. */
  timestampRelNs?: number;
}

/**
 * @brief Build a VALUE TLV (`type=0x01`) carrying an opaque application payload.
 *
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
 * @brief Build a NAME TLV (`type=0x02`) for one path segment, after validating
 * it against the addressing rules (1..64 UTF-8 bytes, no reserved characters).
 *
 * @param segment the segment text
 * @returns a NAME TLV node (no trailer)
 * @throws {RangeError} if the segment is empty, over 64 bytes, or holds a reserved char
 */
export function nameTlv(segment: string): Tlv {
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
 * @brief Build a PATH TLV (`type=0x06`, PL=1) from path segments — a NAME child
 * per segment.
 *
 * Vector-pinned: `path-sensor-temp`; normative byte layout: spec/v1.md §3.1.
 *
 * @param segments the path segments, e.g. `["sensor", "temp"]` for `/sensor/temp`
 * @returns the encoded PATH TLV bytes
 * @throws {RangeError} on an empty path or an invalid segment
 */
export function encodePath(segments: string[]): Uint8Array {
  return encode(pathTlv(segments));
}

/** @brief The PATH TLV node (shared by {@link encodePath}, {@link encodeSubscriber}, and `./fwd.ts`). */
export function pathTlv(segments: string[]): Tlv {
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

/**
 * @brief Build a NAME TLV (`type=0x02`) for FREE TEXT — a SETTINGS key or a string
 * config value — WITHOUT the path-segment validation {@link nameTlv} applies.
 *
 * A NAME TLV is the wire's string carrier in two unrelated roles: a PATH's
 * addressing segment, and a SETTINGS record's key / string value. Only the first is
 * bound by the reference/03 segment rules — a config value legitimately holds the
 * reserved characters an address may not, most obviously an `addr` dotted-quad
 * (`"127.0.0.1"`), which {@link nameTlv} would reject outright. The C++ emitter draws
 * the same line: `emit_name` writes the bytes and validates nothing.
 *
 * Still bounded by the 64-byte NAME budget the addressing rules set.
 *
 * @param text the key or value text
 * @returns a NAME TLV node (no trailer)
 * @throws {RangeError} if the text is empty or over 64 UTF-8 bytes
 */
function textTlv(text: string): Tlv {
  const bytes = utf8.encode(text);
  if (bytes.length < 1 || bytes.length > MAX_SEGMENT_BYTES) {
    throw new RangeError(`NAME text ${JSON.stringify(text)} must be 1..${MAX_SEGMENT_BYTES} UTF-8 bytes (got ${bytes.length})`);
  }
  return { type: TYPE.NAME, opt: opt(), payload: bytes, children: [], trailer: null };
}

/** @brief A little-endian VALUE TLV (`type=0x01`) of `width` bytes — a SETTINGS integer value. */
function valueLe(v: number, width: number): Tlv {
  const bytes = new Uint8Array(width);
  let rest = v;
  for (let i = 0; i < width; i++) {
    bytes[i] = rest & 0xff;
    rest = Math.floor(rest / 256);
  }
  return { type: TYPE.VALUE, opt: opt(), payload: bytes, children: [], trailer: null };
}

/** @brief The link direction of a connection (`role`), per ADR-0027 / `conn_role_t`. */
export type ConnRole = 'dial' | 'listen';

/** @brief The connection SPEC a {@link encodeConnSpec} call describes (ADR-0027 / reference/13). */
export interface ConnSpecOptions {
  /** @brief The device-catalog child type: `client` (DIAL default) or `listener` (LISTEN default). */
  readonly type: 'client' | 'listener';
  /** @brief The connection NAME — the `/net/<name>` segment AND the routing key a `dst` hops through. */
  readonly name: string;
  /** @brief The link direction; overrides the type's default. */
  readonly role: ConnRole;
  /** @brief Peer port (DIAL) / bind port (LISTEN). Required — 0 is rejected by every built-in factory. */
  readonly port: number;
  /** @brief The transport-factory selector (`ws`, `tcp`, `udp`, …). Omit only for a pre-staged link. */
  readonly kind?: string;
  /** @brief Peer IPv4 dotted-quad. Required for DIAL; the built-ins are `inet_pton`-only (no DNS). */
  readonly addr?: string;
  /** @brief ws-private (LISTEN): expose the ADR-0044 bus facet, so `:children[]` lists live peers. */
  readonly peerNamed?: boolean;
  /** @brief ws-private (LISTEN): concurrent-peer admission cap (0/omitted = unbounded). */
  readonly maxPeers?: number;
}

/**
 * @brief Build a connection-creation SPEC TLV (`type=0x0e`, PL=1) — the payload of the
 * in-band `write /net:children[] += SPEC{…}` that brings a transport link up (ADR-0027,
 * reference/13 §2).
 *
 * This is the wire form of the formation write a third party (typically a web UI holding
 * delegated admin) issues on a device to create a link — the mechanism that makes a
 * device-to-device connection with no third party in the data path.
 *
 * ```
 * SPEC{ NAME "type" NAME <type>, NAME "name" NAME <name>,
 *       NAME "config" SETTINGS{ NAME "role"       VALUE u8   (0=DIAL, 1=LISTEN),
 *                               NAME "port"       VALUE u16  (LE),
 *                             [ NAME "kind"       NAME  <kind> ],
 *                             [ NAME "addr"       NAME  <addr> ],
 *                             [ NAME "peer_named" VALUE u8   ],
 *                             [ NAME "max_peers"  VALUE u32  (LE) ] } }
 * ```
 *
 * Key order matters only for readability — the C++ `config_reader_t` walk is
 * order-insensitive and ignores unknown keys (forward-compat). `peer_named` / `max_peers`
 * are **ws-private** keys parsed by the ws factory itself (ADR-0043 §5); they are ignored
 * by other kinds and on a DIAL.
 *
 * Byte-pinned against the C++ emitter in `test/conn-spec.test.mjs`.
 *
 * @param o the connection to describe
 * @returns the encoded SPEC TLV bytes — the payload of a `:children[]` field-write
 * @throws {RangeError} on a DIAL with no `addr`, or a text field over 64 UTF-8 bytes
 */
export function encodeConnSpec(o: ConnSpecOptions): Uint8Array {
  if (o.role === 'dial' && !o.addr) {
    throw new RangeError('a DIAL connection requires an addr (the built-in factories reject it as TYPE_MISMATCH)');
  }
  const cfg: Tlv[] = [
    textTlv('role'),
    valueLe(o.role === 'listen' ? 1 : 0, 1),
    textTlv('port'),
    valueLe(o.port, 2),
  ];
  if (o.kind !== undefined) cfg.push(textTlv('kind'), textTlv(o.kind));
  if (o.addr !== undefined) cfg.push(textTlv('addr'), textTlv(o.addr));
  if (o.peerNamed !== undefined) cfg.push(textTlv('peer_named'), valueLe(o.peerNamed ? 1 : 0, 1));
  if (o.maxPeers !== undefined) cfg.push(textTlv('max_peers'), valueLe(o.maxPeers, 4));

  const settings: Tlv = {
    type: TYPE.SETTINGS,
    opt: opt({ pl: true }),
    payload: new Uint8Array(0),
    children: cfg,
    trailer: null,
  };
  const spec: Tlv = {
    type: TYPE.SPEC,
    opt: opt({ pl: true }),
    payload: new Uint8Array(0),
    children: [
      textTlv('type'),
      textTlv(o.type),
      textTlv('name'),
      textTlv(o.name),
      textTlv('config'),
      settings,
    ],
    trailer: null,
  };
  return encode(spec);
}

/** @brief Options for {@link encodeSubscriber}. Optional QoS/ACL/id children are deferred (ADR-0034). */
export interface SubscriberOptions {
  // Intentionally empty for the conservative slice. The optional SUBSCRIBER
  // children (SETTINGS qos, ACL capability, NAME subscriber_id — reference/05
  // §SUBSCRIBER) are additive and land when their semantics are exercised.
}

/**
 * @brief Build a SUBSCRIBER TLV (`type=0x04`, PL=1) wrapping a target PATH —
 * the payload of a subscribe-write (reference/04 §Subscribe).
 *
 * Vector-pinned:
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
