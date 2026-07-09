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
 * Reply correlation is the transport's concern (RFC-0004 §D): over a single ws
 * link the responder replies in request order, so a simple per-link FIFO of
 * pending requests matches each REPLY to its request. `FWD` stays pure — no
 * end-to-end correlation id.
 */

import { TYPE, decode, CodecError } from '@avatarsd-llc/libtracer';
import type { Tlv } from '@avatarsd-llc/libtracer';
import { encodeValue, encodePath, encodeSubscriber } from './tlv.js';
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

/** @brief A pending one-shot request awaiting its FWD{REPLY} (FIFO per link). */
interface Pending {
  resolve(reply: ParsedFwd): void;
  reject(err: Error): void;
  /**
   * @brief True once the promise has settled (timed out / transport closed).
   *
   * A settled entry stays in the FIFO so a late REPLY still consumes its slot —
   * keeping every later request correlated to the right reply.
   */
  settled: boolean;
  /** @brief The per-request deadline timer (cleared on settle), when one is armed. */
  timer: ReturnType<typeof setTimeout> | null;
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
        const waiter = this.pending.shift();
        // A settled waiter (timed out) still consumes its FIFO slot; its late
        // reply is dropped so later requests stay correlated.
        if (waiter && !waiter.settled) {
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

  private deliver(value: Tlv): void {
    for (const handler of this.valueHandlers) handler(value.payload, value);
  }

  private emitError(err: Error): void {
    if (this.errorHandler) this.errorHandler(err);
  }

  /** @brief Send a one-shot FWD and resolve its FWD{REPLY} (FIFO correlation). */
  private request(frame: Uint8Array): Promise<ParsedFwd> {
    return new Promise<ParsedFwd>((resolve, reject) => {
      if (this.closed) {
        reject(this.closed);
        return;
      }
      const entry: Pending = { resolve, reject, settled: false, timer: null };
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
    const reply = await this.request(
      encodeFwd({ op: FWD_OP.READ, dst: splitPath(path), src: this.replyEndpoint }),
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
    const reply = await this.request(
      encodeFwd({ op: FWD_OP.READ, dst: splitPath(path), field: selector, src: this.replyEndpoint }),
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
    const reply = await this.request(
      encodeFwd({ op: FWD_OP.WRITE, dst: splitPath(path), src: this.replyEndpoint, payload: valueTLV }),
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
    const reply = await this.request(
      encodeFwd({
        op: FWD_OP.AWAIT,
        dst: splitPath(path),
        src: this.replyEndpoint,
        awaitTimeoutNs: timeoutNs,
      }),
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
   * `:subscribers[N]` field-write, additive).
   *
   * @param producerPath the producer vertex path (string or segments)
   * @param handler      invoked with each inbound VALUE delivery
   * @returns a function that locally detaches `handler`
   * @throws {FwdError} when the subscribe-write is rejected
   */
  async subscribe(producerPath: string | string[], handler: ValueHandler): Promise<Unsubscribe> {
    const subscriber = encodeSubscriber(this.replyEndpoint);
    // Register the handler BEFORE awaiting the ack: a producer may stream its
    // first delivery before its subscribe REPLY is seen on the wire, and we must
    // not drop it. On a rejected subscribe we detach again.
    this.valueHandlers.add(handler);
    try {
      const reply = await this.request(
        encodeFwd({
          op: FWD_OP.WRITE,
          dst: splitPath(producerPath),
          field: ':subscribers[]',
          src: this.replyEndpoint,
          payload: subscriber,
        }),
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

/** @brief Normalize a `/`-path string (or segment array) into validated segments. */
function splitPath(path: string | string[]): string[] {
  if (Array.isArray(path)) return path;
  const segs = path.split('/').filter((s) => s.length > 0);
  if (segs.length === 0) throw new RangeError(`path ${JSON.stringify(path)} has no segments`);
  return segs;
}
