// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief LibtracerClient — the experimental browser/Node client SDK (#56,
 * ADR-0034).
 *
 * It composes the cross-validated codec (@avatarsd-llc/libtracer) over an
 * INJECTED transport seam (`@avatarsd-llc/libtracer-ws`'s TransportWs satisfies
 * it structurally), so it is browser/Node-agnostic and testable against an
 * in-memory fake with no live socket.
 *
 * Now that RFC-0004 fills spec §3 (the remote-operation envelope ADR-0034
 * deferred), the client speaks the path-addressed higher operations over `FWD`:
 *   - read(path)            — FWD{ op=READ,  dst=path, src=<reply-ep> }
 *   - write(path, valueTLV) — FWD{ op=WRITE, dst=path, src=…, payload }
 *   - await(path, ns?)      — FWD{ op=AWAIT, dst=path, src=…, await_timeout? }
 *   - readField(path, sel)  — FWD{ op=READ,  dst=path, FIELD=sel, src=… }
 *   - subscribe(prod, h)    — FWD{ op=WRITE, dst=prod, FIELD=:subscribers[],
 *                                  src=…, payload=SUBSCRIBER{ target=<reply-ep> } }
 * Each one-shot op sends a FWD and resolves the FWD{REPLY} the responder
 * source-routes back (RESULT → the value, ERROR → a typed {@link FwdError}).
 *
 * Reply correlation is the transport's concern (RFC-0004 §D): `FWD` stays pure —
 * there is no end-to-end correlation id. What the wire DOES carry is the
 * responder's own endpoint in the reply's `src` (RFC-0004 §B: "the `src` child of
 * a REPLY … is set to the responder's own endpoint — the vertex that produced the
 * result"), and this client correlates on that, FIFO only among requests it cannot
 * tell apart. See {@link LibtracerClient} §correlation.
 */

import { TYPE, decode, CodecError } from '@avatarsd-llc/libtracer';
import type { Tlv } from '@avatarsd-llc/libtracer';
import { encodeValue, encodePath, encodeSubscriber, pathSegments } from './tlv.js';
import type { ValueOptions, SubscriberOptions } from './tlv.js';
import {
  FWD_OP,
  FWD_KIND,
  encodeFwd,
  parseFwdTlv,
  replyErrorCode,
  replyErrorPath,
  fwdErrorName,
  fwdErrorPath,
  fwdErrorCodeForPath,
} from './fwd.js';
import type { ParsedFwd, FieldLevel } from './fwd.js';

/**
 * @brief The minimal connection seam the client drives.
 *
 * `TransportWs` from `@avatarsd-llc/libtracer-ws` satisfies this structurally
 * (so the client never imports it — keeping the transport an optional peer).
 */
export interface ClientTransport {
  /** @brief Put one complete libtracer TLV frame on the wire. */
  send(frame: Uint8Array): void;
  /** @brief Register (or clear with `null`) the inbound-frame receiver. */
  onFrame(receiver: ((bytes: Uint8Array) => void) | null): void;
  /**
   * @brief Register (or clear with `null`) the connection-closed notifier — invoked once
   * when the underlying connection closes or errors out, with the cause when one
   * is known. OPTIONAL: a transport without it never notifies the client of a
   * close, so pending requests only fail by timeout.
   */
  onClose?(handler: ((cause?: Error) => void) | null): void;
}

/** @brief A delivered VALUE: its opaque payload bytes plus the decoded TLV it came from. */
export type ValueHandler = (value: Uint8Array, tlv: Tlv) => void;

/** @brief Detach a {@link LibtracerClient.subscribe} handler (a local detach — see its docs). */
export type Unsubscribe = () => void;

/** @brief Options for {@link LibtracerClient}. */
export interface ClientOptions {
  /**
   * @brief The originator's reply endpoint, as path segments — the `src` route seeded on
   * every outbound FWD and the SUBSCRIBER `target` on a subscribe. Over a single
   * ws hop the responder replies back over the same link, so any 1+-segment path
   * is fine; default `["client"]`.
   */
  readonly replyEndpoint?: string[];
  /**
   * @brief Per-request deadline in milliseconds: a one-shot op whose FWD{REPLY} has not
   * arrived within this window rejects with a timeout {@link Error}. Default
   * 10 000 ms; pass `0` (or `Infinity`) to disable the deadline.
   */
  readonly requestTimeoutMs?: number;
}

/**
 * @brief A `kind=ERROR` FWD reply, surfaced as a typed rejection.
 *
 * `.code` is the registered u16 wire ERROR code (see {@link FWD_ERROR});
 * `.codeName` its symbolic name.
 */
export class FwdError extends Error {
  /** @brief The registered u16 wire ERROR code (a {@link FWD_ERROR} value). */
  readonly code: number;
  /** @brief The symbolic ERROR name (e.g. `"NOT_FOUND"`, `"TIMEOUT"`). */
  readonly codeName: string;
  /**
   * @brief The canonical `tr::<concept>::<error>` namespace path (RFC-0002 §A) — from
   * the frozen registry for a registered code, or carried verbatim by a
   * string-form (NAME identity) ERROR reply. `null` when neither is known.
   */
  readonly path: string | null;
  /**
   * @brief Build the typed rejection from either wire identity form.
   * @param code the registered u16 code carried in the reply's
   *   `STATUS{ ERROR{ VALUE u16 } }` per RFC-0002 (0 when absent)
   * @param path the string-form `tr::…` identity when the reply carried a
   *   `STATUS{ ERROR{ NAME utf8-path } }` instead of a registered code
   */
  constructor(code: number, path?: string | null) {
    // A string-form identity that names a registered error resolves back to its
    // code, so both wire forms surface identically (never UNKNOWN).
    const resolved = code === 0 && path ? fwdErrorCodeForPath(path) : code;
    const trPath = path ?? fwdErrorPath(resolved);
    const name = resolved === 0 && trPath ? trPath : fwdErrorName(resolved);
    super(`FWD reply ERROR ${name} (0x${resolved.toString(16).padStart(4, '0')})`);
    this.name = 'FwdError';
    this.code = resolved;
    this.codeName = name;
    this.path = trPath;
  }
}

/** @brief ADVERTISE (`0x11`) — a route-handle label binding (RFC-0004 route handles). */
const TYPE_ADVERTISE = 0x11;
/** @brief COMPACT (`0x12`) — a label-compacted delivery (RFC-0004 route handles). */
const TYPE_COMPACT = 0x12;

/**
 * @brief An inbound ADVERTISE (`0x11`) / COMPACT (`0x12`) frame reached this
 * client, which does not implement the RFC-0004 compact (route-handle) delivery
 * flow yet.
 *
 * Routed to {@link LibtracerClient.onError} so the failure is diagnosable
 * instead of a silent drop; the sender should fall back to plain FWD delivery.
 */
export class CompactFlowError extends Error {
  /** @brief The offending wire type code (`0x11` ADVERTISE or `0x12` COMPACT). */
  readonly frameType: number;
  /**
   * @brief Name the offending frame kind in the error message.
   * @param frameType the inbound frame's wire type code
   */
  constructor(frameType: number) {
    const kind = frameType === TYPE_ADVERTISE ? 'ADVERTISE' : 'COMPACT';
    super(
      `inbound ${kind} (0x${frameType.toString(16).padStart(2, '0')}) dropped: ` +
        'compact (route-handle) delivery is not supported by this client yet',
    );
    this.name = 'CompactFlowError';
    this.frameType = frameType;
  }
}

/** @brief A pending one-shot request awaiting its FWD{REPLY}. */
interface Pending {
  resolve(reply: ParsedFwd): void;
  reject(err: Error): void;
  /**
   * @brief True once the promise has settled (timed out / transport closed).
   *
   * A settled entry stays in the queue so its own late REPLY still consumes its
   * slot rather than being handed to a live caller.
   */
  settled: boolean;
  /** @brief The per-request deadline timer (cleared on settle), when one is armed. */
  timer: ReturnType<typeof setTimeout> | null;
  /**
   * @brief The request's `dst` segments — the correlation key a REPLY's `src` echoes.
   *
   * `null` when the client cannot state them (it never is today: every one-shot op
   * here builds `dst` from a `PATH`), in which case this entry is only reachable by
   * the FIFO fallback.
   */
  dst: string[] | null;
}

/** @brief Settle a pending entry: clear its deadline and mark it consumed. */
function settle(p: Pending): void {
  p.settled = true;
  if (p.timer !== null) {
    clearTimeout(p.timer);
    p.timer = null;
  }
}

/**
 * @brief A WebSocket/transport-agnostic libtracer client over the RFC-0004
 * `FWD` plane.
 *
 * Construct over any {@link ClientTransport}; issue {@link read} / {@link write} /
 * {@link await_} / {@link readField} (resolving the responder's FWD{REPLY}), or
 * {@link subscribe} to register a SUBSCRIBER and receive its deliveries. Inbound
 * decode errors are routed to {@link onError}, never thrown across the transport's
 * receive callback.
 */
export class LibtracerClient {
  private readonly transport: ClientTransport;
  private readonly replyEndpoint: string[];
  private readonly requestTimeoutMs: number;
  private readonly valueHandlers = new Set<ValueHandler>();
  private readonly pending: Pending[] = [];
  private errorHandler: ((err: Error) => void) | null = null;
  private closed: Error | null = null;

  /**
   * @brief Bind to the injected transport and start receiving frames.
   * @param transport the injected connection seam (e.g. a connected `TransportWs`).
   * @param options   optional reply-endpoint / request-timeout overrides.
   */
  constructor(transport: ClientTransport, options: ClientOptions = {}) {
    this.transport = transport;
    this.replyEndpoint = [...(options.replyEndpoint ?? ['client'])];
    this.requestTimeoutMs = options.requestTimeoutMs ?? 10_000;
    transport.onFrame((bytes) => this.dispatch(bytes));
    transport.onClose?.((cause) => this.handleClose(cause));
  }

  /** @brief Reject every pending request when the transport closes; fail-fast afterwards. */
  private handleClose(cause?: Error): void {
    this.closed = new Error('transport closed' + (cause ? `: ${cause.message}` : ''));
    for (const p of this.pending) {
      if (p.settled) continue;
      settle(p);
      p.reject(this.closed);
    }
    this.pending.length = 0;
  }

  /* ---------------------------------------------------------- inbound --- */

  /**
   * @brief Register a handler for inbound VALUE deliveries.
   *
   * A delivery is a FWD{WRITE} (delivery-is-a-write, RFC-0004 §D); a bare VALUE
   * or a single ROUTER wrapper is also accepted. Multiple handlers may be
   * registered; each receives every delivery.
   */
  onValue(handler: ValueHandler): void {
    this.valueHandlers.add(handler);
  }

  /** @brief Register the sink for inbound decode failures and other client-side errors. */
  onError(handler: (err: Error) => void): void {
    this.errorHandler = handler;
  }

  /** @brief Decode one inbound frame: correlate a REPLY, deliver a write, or shed/deliver a VALUE. */
  private dispatch(bytes: Uint8Array): void {
    let tlv: Tlv;
    try {
      tlv = decode(bytes);
    } catch (err) {
      this.emitError(err instanceof Error ? err : new CodecError(String(err)));
      return;
    }

    if (tlv.type === TYPE.FWD) {
      let parsed: ParsedFwd;
      try {
        parsed = parseFwdTlv(tlv);
      } catch (err) {
        this.emitError(err instanceof Error ? err : new Error(String(err)));
        return;
      }
      if (parsed.op === FWD_OP.REPLY) {
        const at = this.correlate(parsed);
        if (at < 0) return; // a reply with nothing outstanding to answer
        const [waiter] = this.pending.splice(at, 1);
        // A settled waiter (timed out) still consumes its slot; its late reply is
        // dropped rather than handed to a live caller.
        if (!waiter.settled) {
          settle(waiter);
          waiter.resolve(parsed);
        }
        return;
      }
      // A delivery IS a FWD{WRITE} (RFC-0004 §D): deliver its VALUE payload.
      if (parsed.op === FWD_OP.WRITE && parsed.payload && parsed.payload.type === TYPE.VALUE) {
        this.deliver(parsed.payload);
      }
      return;
    }

    // The compact (route-handle) flow is not implemented here: surface it loudly
    // rather than silently dropping the delivery (v0.1 client limitation).
    if (tlv.type === TYPE_ADVERTISE || tlv.type === TYPE_COMPACT) {
      this.emitError(new CompactFlowError(tlv.type));
      return;
    }

    // Non-FWD frame: shed exactly one ROUTER envelope, then deliver a VALUE
    // (the directly-delivered / bridge-mounted path, ADR-0034 inbound rule).
    const data =
      tlv.type === TYPE.ROUTER && tlv.children.length > 0 ? tlv.children[tlv.children.length - 1] : tlv;
    if (data.type === TYPE.VALUE) this.deliver(data);
  }

  /**
   * @brief Which outstanding request this REPLY answers — its index, or `-1` for none.
   *
   * @section correlation Why not FIFO
   *
   * FIFO correlation is only correct while at most one request is outstanding.
   * `FWD` carries no correlation id, and nothing in RFC-0004 promises reply order:
   * a request that is FORWARDED across a mounted link answers in tens of
   * milliseconds while a local one answers in about one, so a later request
   * routinely OVERTAKES an earlier one on the same link. Under `shift()` the first
   * overtake transposes two answers and the queue never realigns — every later
   * caller gets the previous request's reply. Nothing errors: the caller receives a
   * well-formed `RESULT` for somebody else's path (#1530).
   *
   * @section key What the wire does carry
   *
   * RFC-0004 §B: a REPLY "does not accumulate `src`; the `src` child of a REPLY is
   * still required and is set to the **responder's own endpoint** (the vertex that
   * produced the result)". `dst` SHRINKS per hop, so what the terminus resolved —
   * and echoes — is the TAIL of the request's `dst` after every mount prefix was
   * stripped: a request to `/net/ws-client/peer0/hw/variant` answers `src=hw/variant`,
   * and so does a local request to `/hw/variant`. The key is therefore a SUFFIX
   * match, not an equality, and it is genuinely ambiguous between those two.
   *
   * So: the oldest outstanding request whose `dst` ends with the reply's `src`, which
   * is FIFO among the ones the wire cannot tell apart — exactly the tiebreak #1530
   * prescribes, and it resolves that issue's measured four-request trace correctly.
   *
   * @section fallback When the key is unusable
   *
   * An empty `src` (RFC-0004 Amendment 2's "no reply requested"), a `PATH_REF` src on
   * a reply to a bound operation, a label escape, or a `src` matching nothing
   * outstanding all fall back to the oldest entry — today's behaviour exactly, so
   * this change can only ever improve a correlation, never break one that worked.
   */
  private correlate(parsed: ParsedFwd): number {
    const fifo = this.pending.length > 0 ? 0 : -1;
    let src: string[];
    try {
      src = pathSegments(parsed.src);
    } catch {
      return fifo; // a PATH_REF / escaped src is not an address this client can match
    }
    if (src.length === 0) return fifo;
    for (let i = 0; i < this.pending.length; i++) {
      const dst = this.pending[i].dst;
      if (dst !== null && endsWith(dst, src)) return i;
    }
    return fifo;
  }

  private deliver(value: Tlv): void {
    for (const handler of this.valueHandlers) handler(value.payload, value);
  }

  private emitError(err: Error): void {
    if (this.errorHandler) this.errorHandler(err);
  }

  /**
   * @brief Send a one-shot FWD and resolve its FWD{REPLY}.
   *
   * @param frame the encoded request frame
   * @param dst   the request's destination segments — the key its reply's `src`
   *              echoes (see {@link correlate})
   */
  private request(frame: Uint8Array, dst: string[]): Promise<ParsedFwd> {
    return new Promise<ParsedFwd>((resolve, reject) => {
      if (this.closed) {
        reject(this.closed);
        return;
      }
      const entry: Pending = { resolve, reject, settled: false, timer: null, dst };
      if (this.requestTimeoutMs > 0 && Number.isFinite(this.requestTimeoutMs)) {
        entry.timer = setTimeout(() => {
          // Leave the settled entry in the FIFO (see Pending) — its slot is
          // consumed by the late reply, if one ever arrives.
          settle(entry);
          entry.reject(new Error(`request timed out after ${this.requestTimeoutMs}ms (no FWD reply)`));
        }, this.requestTimeoutMs);
        // Don't hold a Node event loop open for a pending deadline (no-op in browsers).
        (entry.timer as { unref?: () => void }).unref?.();
      }
      this.pending.push(entry);
      try {
        this.transport.send(frame);
      } catch (err) {
        this.pending.pop();
        settle(entry);
        reject(err instanceof Error ? err : new Error(String(err)));
      }
    });
  }

  /** @brief Turn a RESULT reply into its payload TLV; throw {@link FwdError} on ERROR. */
  private result(reply: ParsedFwd): Tlv | null {
    if (reply.kind === FWD_KIND.ERROR) {
      // Registered-code and string-form (NAME tr::… path) identities both
      // surface typed — a known path resolves back to its code, never UNKNOWN.
      throw new FwdError(replyErrorCode(reply), replyErrorPath(reply));
    }
    return reply.payload;
  }

  /* --------------------------------------------------------- one-shot --- */

  /**
   * @brief Read a remote vertex's value: `FWD{ op=READ, dst=path, src=<reply-ep> }`.
   *
   * @param path the destination path (a `/`-string like `"/sensor/temp"`, or segments)
   * @returns the RESULT's value TLV (decoded)
   * @throws {FwdError} when the responder replies `kind=ERROR` (e.g. NOT_FOUND)
   */
  async read(path: string | string[]): Promise<Tlv> {
    const dst = splitPath(path);
    const reply = await this.request(
      encodeFwd({ op: FWD_OP.READ, dst, src: this.replyEndpoint }),
      dst,
    );
    const value = this.result(reply);
    if (!value) throw new FwdError(0); // RESULT with no payload — malformed read reply
    return value;
  }

  /**
   * @brief Read a remote vertex's `:field`: `FWD{ op=READ, dst=path,
   * FIELD=selector, src=… }`.
   *
   * A whole-array field (e.g. `:subscribers[]`) resolves to a POINT of the slot TLVs.
   *
   * @param path     the destination path (string or segments)
   * @param selector the `:field` string (e.g. `":subscribers[]"`) or pre-parsed levels
   * @returns the RESULT's payload TLV (a slot TLV, or a POINT for a whole array)
   * @throws {FwdError} when the responder replies `kind=ERROR`
   */
  async readField(path: string | string[], selector: string | FieldLevel[]): Promise<Tlv> {
    const dst = splitPath(path);
    const reply = await this.request(
      encodeFwd({ op: FWD_OP.READ, dst, field: selector, src: this.replyEndpoint }),
      dst,
    );
    const value = this.result(reply);
    if (!value) throw new FwdError(0);
    return value;
  }

  /**
   * @brief Write a value to a remote vertex: `FWD{ op=WRITE, dst=path, src=…, payload }`.
   *
   * @param path     the destination path (string or segments)
   * @param valueTLV a complete VALUE TLV's bytes (e.g. from {@link encodeValue})
   * @returns once the responder acks `kind=RESULT`
   * @throws {FwdError} when the responder replies `kind=ERROR`
   */
  async write(path: string | string[], valueTLV: Uint8Array): Promise<void> {
    const dst = splitPath(path);
    const reply = await this.request(
      encodeFwd({ op: FWD_OP.WRITE, dst, src: this.replyEndpoint, payload: valueTLV }),
      dst,
    );
    this.result(reply); // throws on ERROR; RESULT carries no payload
  }

  /**
   * @brief Write into a remote vertex's FIELD: `FWD{ op=WRITE, dst=path, field=selector,
   * src=…, payload }` — the write counterpart of {@link readField}.
   *
   * The append form (`":children[]"` — a whole-array ELEMENT selector with no index) is
   * how a vertex is CREATED in band: `writeField('/cfg', ':children[]', spec)` mints a
   * `stored_value` child on a remote device (ADR-0017), and
   * `writeField('/sensor/temp', ':subscribers[]', encodeSubscriber(…))` binds a
   * consumer-initiated subscription (ADR-0026). Neither was expressible from this client
   * before: {@link write} takes no selector, so every field-addressed write — the entire
   * in-band formation plane — was out of reach.
   *
   * A CONNECTION is the one thing this door no longer creates. RFC-0014 S7 retired
   * `writeField('/net', ':children[]', …)`: a transport link is now brought up by a
   * WHOLE-VERTEX write to its module's creator endpoint —
   * `write(['net', '<module>', 'conn'], encodeConnSpec(…))`. Removal is the same endpoint
   * written with a bare `NAME{<name>}` payload instead of a SPEC (RFC-0014 §2,
   * reference/13 §2). The module segment is what fixes the transport and the role, which is
   * why the payload carries neither. Generic `:children[]` creation is unaffected.
   *
   * Vector-pinned by `fwd-write-subscriber-field`.
   *
   * @param path     the destination path (string or segments)
   * @param selector the field selector (e.g. `":children[]"`, or parsed {@link FieldLevel}s)
   * @param valueTLV a complete payload TLV's bytes (e.g. from {@link encodeConnSpec})
   * @returns once the responder acks `kind=RESULT`
   * @throws {FwdError} when the responder replies `kind=ERROR`
   */
  async writeField(
    path: string | string[],
    selector: string | FieldLevel[],
    valueTLV: Uint8Array,
  ): Promise<void> {
    const dst = splitPath(path);
    const reply = await this.request(
      encodeFwd({
        op: FWD_OP.WRITE,
        dst,
        field: selector,
        src: this.replyEndpoint,
        payload: valueTLV,
      }),
      dst,
    );
    this.result(reply); // throws on ERROR; RESULT carries no payload
  }

  /**
   * @brief Block for the next write to a remote vertex: `FWD{ op=AWAIT,
   * dst=path, src=…, await_timeout? }`.
   *
   * Named `await_` because `await` is reserved; an `await` alias is installed
   * on the prototype for the RFC-0004-spelled call site.
   *
   * @param path        the destination path (string or segments)
   * @param timeoutNs   the await timeout in ns (absent ⇒ the responder's 1 s default)
   * @returns the next write's value TLV
   * @throws {FwdError} with code TIMEOUT when the responder's deadline elapses
   */
  async await_(path: string | string[], timeoutNs?: bigint): Promise<Tlv> {
    const dst = splitPath(path);
    const reply = await this.request(
      encodeFwd({
        op: FWD_OP.AWAIT,
        dst,
        src: this.replyEndpoint,
        awaitTimeoutNs: timeoutNs,
      }),
      dst,
    );
    const value = this.result(reply);
    if (!value) throw new FwdError(0);
    return value;
  }

  /**
   * @brief Subscribe to a remote producer: a `WRITE` of a `SUBSCRIBER{
   * target=<reply-ep> }` into `producer:subscribers[]` (RFC-0004 §C/§D —
   * subscribe is a field-write).
   *
   * Once the responder acks, `handler` fires for every inbound VALUE delivery.
   *
   * SCOPE: this issues the SUBSCRIBER and receives VALUEs the producer directly
   * delivers to this client's reply endpoint. Full standing-stream delivery
   * additionally needs the producer auto-promote / flush seam (#136 / RFC-0004 §E)
   * — out of scope for this client slice. `unsubscribe()` is a LOCAL detach: it
   * stops firing `handler` but does not clear the remote slot (an indexed
   * `:subscribers[N]` field-write, which REPLACES that slot rather than adding
   * to it — RFC-0009 §D.1).
   *
   * @param producerPath the producer vertex path (string or segments)
   * @param handler      invoked with each inbound VALUE delivery
   * @param opts         the SUBSCRIBER's optional children — notably RFC-0022 §3.A's
   *                     `deliveryPolicy`, whose bit 5 asks the producer to deliver its
   *                     latched last value on join (omitted ⇒ today's behaviour)
   * @returns a function that locally detaches `handler`
   * @throws {FwdError} when the subscribe-write is rejected
   */
  async subscribe(
    producerPath: string | string[],
    handler: ValueHandler,
    opts: SubscriberOptions = {},
  ): Promise<Unsubscribe> {
    const dst = splitPath(producerPath);
    const subscriber = encodeSubscriber(this.replyEndpoint, opts);
    // Register the handler BEFORE awaiting the ack: a producer may stream its
    // first delivery before its subscribe REPLY is seen on the wire, and we must
    // not drop it. On a rejected subscribe we detach again.
    this.valueHandlers.add(handler);
    try {
      const reply = await this.request(
        encodeFwd({
          op: FWD_OP.WRITE,
          dst,
          field: ':subscribers[]',
          src: this.replyEndpoint,
          payload: subscriber,
        }),
        dst,
      );
      this.result(reply); // throws on ERROR
    } catch (err) {
      this.valueHandlers.delete(handler);
      throw err;
    }
    return () => {
      this.valueHandlers.delete(handler);
    };
  }

  /* ------------------------------------------- pure builders (static) --- */

  /** @brief {@link encodeValue} — the exact VALUE TLV bytes, independently of any transport. */
  static encodeValue(value: Uint8Array, opts?: ValueOptions): Uint8Array {
    return encodeValue(value, opts);
  }

  /** @brief {@link encodePath} — the exact PATH TLV bytes. */
  static encodePath(segments: string[]): Uint8Array {
    return encodePath(segments);
  }

  /** @brief {@link encodeSubscriber} — the exact SUBSCRIBER TLV bytes. */
  static encodeSubscriber(targetPath: string[], opts?: SubscriberOptions): Uint8Array {
    return encodeSubscriber(targetPath, opts);
  }
}

/**
 * @brief `await` is a reserved word, so the method is `await_`; expose
 * `client.await(...)` too (a member name may be a keyword) for the
 * RFC-0004-spelled ergonomic call.
 */
Object.defineProperty(LibtracerClient.prototype, 'await', {
  value: LibtracerClient.prototype.await_,
  writable: true,
  configurable: true,
  enumerable: false,
});

/**
 * @brief Does @p path end with the segments @p tail — the reply-`src` suffix test.
 *
 * A suffix rather than an equality because `dst` shrinks per hop (RFC-0004 §B): the
 * responder's own endpoint is what is left of the request's `dst` once every mount
 * prefix has been stripped.
 */
function endsWith(path: string[], tail: string[]): boolean {
  if (tail.length > path.length) return false;
  const at = path.length - tail.length;
  for (let i = 0; i < tail.length; i++) if (path[at + i] !== tail[i]) return false;
  return true;
}

/** @brief Normalize a `/`-path string (or segment array) into validated segments. */
function splitPath(path: string | string[]): string[] {
  if (Array.isArray(path)) return path;
  const segs = path.split('/').filter((s) => s.length > 0);
  if (segs.length === 0) throw new RangeError(`path ${JSON.stringify(path)} has no segments`);
  return segs;
}
