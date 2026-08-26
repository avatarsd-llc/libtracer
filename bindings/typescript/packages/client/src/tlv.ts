// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief Pure, side-effect-free TLV builders — and the one shared child accessor
 * that reads them back — for the libtracer client SDK (#56, ADR-0034).
 *
 * Each builder produces the EXACT wire bytes the shared conformance vectors
 * pin (`tests/conformance/vectors/v1/`) via the cross-validated core codec
 * (`@avatarsd-llc/libtracer`) — nothing here invents wire structure:
 *
 *   - encodeValue      -> VALUE TLV (0x01)        : value-bool-true / value-ll-u32 / value-ts-abs
 *   - encodePath       -> PATH TLV (0x06, PL=0)   : path-sensor-temp + spec/v1.md §3.1
 *   - encodeSubscriber -> SUBSCRIBER TLV (0x04)   : subscriber-path  (the subscribe-write payload)
 *
 * Reading back, `firstChild` is the package's single child-by-type accessor and
 * the mirror of the Rust binding's `Tlv::first_child`; it leads the file because
 * every structured read goes through it rather than indexing a position (#878).
 *
 * These are the payload TLVs. The path-ADDRESSED request envelope (verb +
 * destination vertex:field) is the FWD/FIELD frame RFC-0004 (spec §3) fixes —
 * it is built in `./fwd.ts`, not here; these payloads ride inside it.
 */

import { TYPE, encode } from '@avatarsd-llc/libtracer';
import type { Opt, Tlv } from '@avatarsd-llc/libtracer';

/**
 * @brief The first DIRECT child of `tlv` with the given type code, or `null`.
 *
 * The one child-by-type accessor this package reads structure through, and the
 * mirror of the Rust binding's `Tlv::first_child` — both cores answer a
 * structured TLV's "the X child" question by scanning direct children for the
 * first matching TYPE, never by indexing a fixed position. Open-coding
 * `children[0]?.type === TYPE.X` instead is how the two bindings drifted apart
 * on `STATUS{ … ERROR … }` (#878): it silently reads a positional rule into a
 * grammar that pins none, and the reader that indexes reports "no such child"
 * for a child that is right there.
 *
 * Direct children only — a match is never searched for inside a grandchild,
 * since a nested TLV of the same type is a DIFFERENT thing (an ERROR's own
 * detail child, say, is not the STATUS's error).
 *
 * @param tlv  the structured (`opt.PL=1`) parent to scan
 * @param type the wire type code to match — a value of the core codec's `TYPE`
 *             map (`TYPE.ERROR`, `TYPE.PATH`, …), not linked here because it is
 *             re-exported from `@avatarsd-llc/libtracer` and the docs gate
 *             refuses a cross-package link
 * @returns the first matching child, or `null` when there is none
 */
export function firstChild(tlv: Tlv | null | undefined, type: number): Tlv | null {
  return tlv?.children.find((c) => c.type === type) ?? null;
}

/** @brief Path-segment constraints from reference/03-addressing.md §path syntax. */
const MAX_SEGMENT_BYTES = 64;
/**
 * @brief Maximum segments in one PATH (reference/03 §path syntax; `core` `kMaxSegments`).
 *
 * Repriced 32 -> 255 by RFC-0023 — chosen so every per-segment quantity stays u8-representable,
 * not inherited from a parser guard. Under RFC-0018's packed body a segment costs `1 + len`, so
 * {@link MAX_PATH_BYTES} admits 512 one-byte records and this COUNT is what binds. (Before the
 * packing a segment cost `4 + len`, 204 segments filled the byte cap, and the count clause
 * could never fire.)
 */
const MAX_SEGMENTS = 255;
/**
 * @brief Maximum encoded PATH body — the concatenated packed `[u8 len][utf8]` segment records,
 * i.e. exactly the PATH TLV's own `length` field (reference/03 §path syntax, 2026-07-31
 * erratum; RFC-0018 §5; `core` `kMaxPathBytes`).
 */
const MAX_PATH_BYTES = 1024;
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
 * @brief Build a PATH TLV (`type=0x06`, PL=0) from path segments — one packed
 * `[u8 len][utf8]` record per segment (RFC-0018).
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
  if (segments.length > MAX_SEGMENTS)
    throw new RangeError(`a path may have at most ${MAX_SEGMENTS} segments (got ${segments.length})`);
  // `nameTlv` is still the SEGMENT PREDICATE — 1..64 UTF-8 bytes, no reserved character — even
  // though its NAME framing is no longer what a PATH carries. Reusing it keeps one home for
  // the addressing grammar; only the bytes it is turned into changed (RFC-0018).
  const records = segments.map((seg) => nameTlv(seg).payload);
  // The encode-time byte check the spec has always required and this client never had
  // (RFC-0023 §5.6): the 1024-byte budget is measured as the PATH TLV's own `length` field —
  // each packed record costs one length byte plus its UTF-8 bytes. Under the packing it is the
  // segment COUNT that binds first (255 one-byte records are 510 bytes), so this clause now
  // fires only on genuinely long segments.
  const bodyBytes = records.reduce((n, r) => n + 1 + r.length, 0);
  if (bodyBytes > MAX_PATH_BYTES)
    throw new RangeError(`a path's encoded body may be at most ${MAX_PATH_BYTES} bytes (got ${bodyBytes})`);
  const payload = new Uint8Array(bodyBytes);
  let at = 0;
  for (const r of records) {
    payload[at++] = r.length;
    payload.set(r, at);
    at += r.length;
  }
  return {
    type: TYPE.PATH,
    // PL = 0: a packed body is NOT a child run, and a set PL would make a generic walker read
    // the first body bytes as a TLV header and mis-frame the whole address (RFC-0018 §5).
    opt: opt(),
    payload,
    children: [],
    trailer: null,
  };
}

/**
 * @brief The `len == 0` ESCAPE record's length byte — RFC-0018 §8 / §5.4 Amendment 1.
 *
 * An escape is `00 <u8 kind> <u8 len> <len bytes>`. It is **admissible in a frame path** — a
 * forwarder steps over one whose `kind` it does not implement instead of dropping a frame it
 * is only relaying — and **rejected in canonical / key context**, because a label is not
 * canonical bytes. This client never mints one; `kind = 0x16` is reserved for RFC-0027.
 */
export const PACKED_ESCAPE_LEN = 0;

/**
 * @brief Read a PATH TLV's packed body back into its segments — the inverse of
 * {@link encodePath}, in **canonical / key** context.
 *
 * `encodePath(segs)` then `decode` then this yields `segs` again; the round trip is exact
 * because a packed record has exactly one spelling per segment (RFC-0018 §4).
 *
 * The escape is REFUSED here rather than skipped: a caller reading segments wants an ADDRESS,
 * and an address carrying a label is not one. A forwarder that only relays the frame never
 * calls this.
 *
 * @param path a decoded PATH TLV
 * @returns its segments, in order (empty for the graph root)
 * @throws {RangeError} if the TLV is not a packed PATH, the records do not tile the body, or
 *         a record is the escape
 */
export function pathSegments(path: Tlv): string[] {
  if (path.type !== TYPE.PATH || path.opt.pl || path.children.length > 0)
    throw new RangeError('not a packed PATH TLV (RFC-0018: type 0x06 with opt.PL = 0)');
  const decoder = new TextDecoder('utf-8', { fatal: true });
  const out: string[] = [];
  let at = 0;
  while (at < path.payload.length) {
    const len = path.payload[at];
    if (len === PACKED_ESCAPE_LEN)
      throw new RangeError('a PATH escape record (len == 0) is not admissible in key context');
    if (at + 1 + len > path.payload.length)
      throw new RangeError('a PATH segment record runs past the body');
    out.push(decoder.decode(path.payload.subarray(at + 1, at + 1 + len)));
    at += 1 + len;
  }
  return out;
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

/** @brief The connection SPEC a {@link encodeConnSpec} call describes (RFC-0014 §2 / reference/13). */
export interface ConnSpecOptions {
  /** @brief The connection NAME — the `/net/<module>/<name>` segment AND the routing key a `dst` hops through. */
  readonly name: string;
  /** @brief Peer port (DIAL) / bind port (LISTEN). Required — 0 is rejected by every built-in factory. */
  readonly port: number;
  /** @brief The transport-factory selector (`ws`, `tcp`, `udp`, …). Omit when the module declares
   * exactly one kind, or for a pre-staged link. */
  readonly kind?: string;
  /** @brief Peer IPv4 dotted-quad. Required by every built-in DIAL factory (`inet_pton`-only, no
   * DNS) and ignored on a LISTEN. NOT validated here — see {@link encodeConnSpec}. */
  readonly addr?: string;
  /** @brief ws-private (LISTEN): expose the ADR-0044 bus facet, so `:children[]` lists live peers. */
  readonly peerNamed?: boolean;
  /** @brief ws-private (LISTEN): concurrent-peer admission cap (0/omitted = unbounded). */
  readonly maxPeers?: number;
}

/**
 * @brief Build a connection-creation SPEC TLV (`type=0x0e`, PL=1) — the payload of the
 * in-band `write /net/<module>/conn <- SPEC{…}` that brings a transport link up
 * (RFC-0014 §2, ADR-0027, reference/13 §2).
 *
 * This is the wire form of the formation write a third party (typically a web UI holding
 * delegated admin) issues on a device to create a link — the mechanism that makes a
 * device-to-device connection with no third party in the data path.
 *
 * ```
 * SPEC{ NAME "name"   NAME <name>,
 *       NAME "config" SETTINGS{ [ NAME "kind"       NAME  <kind> ],
 *                               [ NAME "addr"       NAME  <addr> ],
 *                                 NAME "port"       VALUE u16  (LE),
 *                               [ NAME "peer_named" VALUE u8   ],
 *                               [ NAME "max_peers"  VALUE u32  (LE) ] } }
 * ```
 *
 * **There is no `type` pair and no `role` pair.** RFC-0014 S7 retired the global
 * `write /net:children[] += SPEC{…}` creation door; the surviving door is the module's own
 * creator endpoint, and the module segment of the path it is written to fixes BOTH the
 * transport and the link direction. A `role` pair here would be an ignored unknown pair,
 * not an override — so this encoder does not emit one, under that or any other spelling.
 * (Removal is the same endpoint written with a bare `NAME{<name>}`, not a SPEC.)
 *
 * Key order matters only for readability — the C++ `config_reader_t` walk is
 * order-insensitive and ignores unknown keys (forward-compat). The order emitted here is
 * the one the shared `conn/create-via-spec` conformance vector uses, so the two agree
 * byte-for-byte. `peer_named` / `max_peers` are **ws-private** keys parsed by the ws
 * factory itself (ADR-0043 §5); they are ignored by other kinds and on a DIAL.
 *
 * @note **No client-side DIAL/`addr` check.** The caller now learns the role from the
 *       endpoint it writes to, not from this record, so this encoder cannot know whether a
 *       missing `addr` is a bug (a DIAL) or correct (a LISTEN, where `addr` is ignored).
 *       Rather than reintroduce `role` under another name it emits what it is given — the
 *       same contract as the C++ `tr::net::conn_spec_t`, which validates no key against any
 *       kind either. A DIAL module handed an `addr`-less config answers `TYPE_MISMATCH`.
 *
 * Byte-pinned against the shared `conn/create-via-spec` vector in `test/vectors.test.mjs`.
 *
 * @param o the connection to describe
 * @returns the encoded SPEC TLV bytes — the payload of a `write` to `/net/<module>/conn`
 * @throws {RangeError} on a text field over 64 UTF-8 bytes
 */
export function encodeConnSpec(o: ConnSpecOptions): Uint8Array {
  const cfg: Tlv[] = [];
  if (o.kind !== undefined) cfg.push(textTlv('kind'), textTlv(o.kind));
  if (o.addr !== undefined) cfg.push(textTlv('addr'), textTlv(o.addr));
  cfg.push(textTlv('port'), valueLe(o.port, 2));
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
    children: [textTlv('name'), textTlv(o.name), textTlv('config'), settings],
    trailer: null,
  };
  return encode(spec);
}

/** @brief Options for {@link encodeSubscriber}. The ACL-capability and subscriber_id
 * children remain deferred (ADR-0034); the delivery policy is RFC-0022 §3.A. */
export interface SubscriberOptions {
  /**
   * @brief This subscription's DELIVERY policy — the packed 16-bit field of RFC-0022 §3.A,
   * carried as `SETTINGS{ NAME "delivery_policy" VALUE u16 }` (the same `SETTINGS` child
   * `delivery_compact` uses, so no new wire structure).
   *
   * Bit 0–1 reliability (0 = best-effort, 1 = reliable), bits 2–4 priority (0–7, 0 =
   * default), bit 5 `durability_request` (deliver the producer's latched last value on
   * join), bits 6–7 `delivery_class` (RFC-0025 §4.1 — see {@link DELIVERY_CLASS}), bits
   * 8–15 reserved — a sender MUST write them 0 and a receiver MUST ignore them.
   *
   * Omitted or `0` emits **no** SETTINGS child at all: absent ⇒ all-zero ⇒ today's
   * behaviour, byte-identically (the `subscriber/policy-absent` vector).
   */
  deliveryPolicy?: number;
}

/** @brief `durability_request` (RFC-0022 §3.A bit 5): ask the producer to deliver its
 * latched last value once, on join. Before RFC-0022 this was the producer's
 * `:settings.durability`, which applied to every subscriber at once. */
export const DELIVERY_DURABILITY_REQUEST = 0x0020;

/**
 * @brief `delivery_class` (RFC-0025 §4.1, bits 6–7 of the packed word): how the producer's
 * fan-out edge treats THIS subscriber's deliveries.
 *
 * `CONFLATE` is `0`, which is what a sender that predates the class wrote into those bits
 * when they were reserved — so the assignment costs no wire byte and old subscribers are
 * conflate-class by construction. Shift into place with {@link deliveryClassBits}; read one
 * back out with {@link deliveryClassOf}. This client is a codec: it carries the class, it
 * does not honour it.
 */
export const DELIVERY_CLASS = {
  /** @brief `0` — last-wins; delivery MAY coalesce to the newest value. The LKV default. */
  CONFLATE: 0,
  /** @brief `1` — every write its own event; order-preserving, never conflated. */
  IMMEDIATE: 1,
  /** @brief `2` — the wire encoding of a flush: one BATCH record (`0x80`) per flush. */
  BATCH: 2,
  /** @brief `3` — append-preserving; every write delivered in order, none conflated. */
  STREAM: 3,
} as const;

/** @brief The packed-word bits for a {@link DELIVERY_CLASS} member — `class << 6`. */
export function deliveryClassBits(cls: number): number {
  return (cls & 0x03) << 6;
}

/** @brief The delivery class carried in a packed `delivery_policy` word (bits 6–7). */
export function deliveryClassOf(policy: number): number {
  return (policy >> 6) & 0x03;
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
 * @param opts       optional children — currently the RFC-0022 §3.A delivery policy
 * @returns the encoded SUBSCRIBER TLV bytes
 */
export function encodeSubscriber(targetPath: string[], opts: SubscriberOptions = {}): Uint8Array {
  const children: Tlv[] = [pathTlv(targetPath)];
  const policy = opts.deliveryPolicy ?? 0;
  if (policy !== 0) {
    if (!Number.isInteger(policy) || policy < 0 || policy > 0xffff)
      throw new RangeError('deliveryPolicy must be a u16');
    children.push({
      type: TYPE.SETTINGS,
      opt: opt({ pl: true }),
      payload: new Uint8Array(0),
      children: [textTlv('delivery_policy'), valueLe(policy, 2)],
      trailer: null,
    });
  }
  const tlv: Tlv = {
    type: TYPE.SUBSCRIBER,
    opt: opt({ pl: true }),
    payload: new Uint8Array(0),
    children,
    trailer: null,
  };
  return encode(tlv);
}
