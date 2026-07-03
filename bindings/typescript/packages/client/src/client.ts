// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// LibtracerClient — the experimental browser/Node client SDK (#56, ADR-0034).
//
// It composes the cross-validated codec (@avatarsd-llc/libtracer) over an
// INJECTED transport seam (`@avatarsd-llc/libtracer-ws`'s TransportWs satisfies
// it structurally), so it is browser/Node-agnostic and testable against an
// in-memory fake with no live socket.
//
// Now that RFC-0004 fills spec §3 (the remote-operation envelope ADR-0034
// deferred), the client speaks the path-addressed higher operations over `FWD`:
//   - read(path)            — FWD{ op=READ,  dst=path, src=<reply-ep> }
//   - write(path, valueTLV) — FWD{ op=WRITE, dst=path, src=…, payload }
//   - await(path, ns?)      — FWD{ op=AWAIT, dst=path, src=…, await_timeout? }
//   - readField(path, sel)  — FWD{ op=READ,  dst=path, FIELD=sel, src=… }
//   - subscribe(prod, h)    — FWD{ op=WRITE, dst=prod, FIELD=:subscribers[],
//                                  src=…, payload=SUBSCRIBER{ target=<reply-ep> } }
// Each one-shot op sends a FWD and resolves the FWD{REPLY} the responder
// source-routes back (RESULT → the value, ERROR → a typed {@link FwdError}).
//
// Reply correlation is the transport's concern (RFC-0004 §D): over a single ws
// link the responder replies in request order, so a simple per-link FIFO of
// pending requests matches each REPLY to its request. `FWD` stays pure — no
// end-to-end correlation id.

import { TYPE, decode, CodecError } from '@avatarsd-llc/libtracer';
import type { Tlv } from '@avatarsd-llc/libtracer';
import { encodeValue, encodePath, encodeSubscriber } from './tlv.js';
import type { ValueOptions, SubscriberOptions } from './tlv.js';
import { FWD_OP, FWD_KIND, encodeFwd, parseFwdTlv, replyErrorCode, fwdErrorName } from './fwd.js';
import type { ParsedFwd, FieldLevel } from './fwd.js';

/**
 * The minimal connection seam the client drives. `TransportWs` from
 * `@avatarsd-llc/libtracer-ws` satisfies this structurally (so the client never
 * imports it — keeping the transport an optional peer).
 */
export interface ClientTransport {
  /** Put one complete libtracer TLV frame on the wire. */
  send(frame: Uint8Array): void;
  /** Register (or clear with `null`) the inbound-frame receiver. */
  onFrame(receiver: ((bytes: Uint8Array) => void) | null): void;
}

/** A delivered VALUE: its opaque payload bytes plus the decoded TLV it came from. */
export type ValueHandler = (value: Uint8Array, tlv: Tlv) => void;

/** Detach a {@link LibtracerClient.subscribe} handler (a local detach — see its docs). */
export type Unsubscribe = () => void;

/** Options for {@link LibtracerClient}. */
export interface ClientOptions {
  /**
   * The originator's reply endpoint, as path segments — the `src` route seeded on
   * every outbound FWD and the SUBSCRIBER `target` on a subscribe. Over a single
   * ws hop the responder replies back over the same link, so any 1+-segment path
   * is fine; default `["client"]`.
   */
  readonly replyEndpoint?: string[];
}

/**
 * A `kind=ERROR` FWD reply, surfaced as a typed rejection. `.code` is the
 * registered u16 wire ERROR code (see {@link FWD_ERROR}); `.codeName` its
 * symbolic name.
 */
export class FwdError extends Error {
  /** The registered u16 wire ERROR code (a {@link FWD_ERROR} value). */
  readonly code: number;
  /** The symbolic ERROR name (e.g. `"NOT_FOUND"`, `"TIMEOUT"`). */
  readonly codeName: string;
  /**
   * @param code the registered u16 code carried in the reply's
   *   `STATUS{ ERROR{ VALUE u16 } }` per RFC-0002
   */
  constructor(code: number) {
    const name = fwdErrorName(code);
    super(`FWD reply ERROR ${name} (0x${code.toString(16).padStart(4, '0')})`);
    this.name = 'FwdError';
    this.code = code;
    this.codeName = name;
  }
}

/** A pending one-shot request awaiting its FWD{REPLY} (FIFO per link). */
interface Pending {
  resolve(reply: ParsedFwd): void;
  reject(err: Error): void;
}

/**
 * A WebSocket/transport-agnostic libtracer client over the RFC-0004 `FWD` plane.
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
  private readonly valueHandlers = new Set<ValueHandler>();
  private readonly pending: Pending[] = [];
  private errorHandler: ((err: Error) => void) | null = null;

  /**
   * @param transport the injected connection seam (e.g. a connected `TransportWs`).
   * @param options   optional reply-endpoint override (default `["client"]`).
   */
  constructor(transport: ClientTransport, options: ClientOptions = {}) {
    this.transport = transport;
    this.replyEndpoint = [...(options.replyEndpoint ?? ['client'])];
    transport.onFrame((bytes) => this.dispatch(bytes));
  }

  /* ---------------------------------------------------------- inbound --- */

  /**
   * Register a handler for inbound VALUE deliveries. A delivery is a FWD{WRITE}
   * (delivery-is-a-write, RFC-0004 §D); a bare VALUE or a single ROUTER wrapper is
   * also accepted. Multiple handlers may be registered; each receives every delivery.
   */
  onValue(handler: ValueHandler): void {
    this.valueHandlers.add(handler);
  }

  /** Register the sink for inbound decode failures and other client-side errors. */
  onError(handler: (err: Error) => void): void {
    this.errorHandler = handler;
  }

  /** Decode one inbound frame: correlate a REPLY, deliver a write, or shed/deliver a VALUE. */
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
        if (waiter) waiter.resolve(parsed);
        return;
      }
      // A delivery IS a FWD{WRITE} (RFC-0004 §D): deliver its VALUE payload.
      if (parsed.op === FWD_OP.WRITE && parsed.payload && parsed.payload.type === TYPE.VALUE) {
        this.deliver(parsed.payload);
      }
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

  /** Send a one-shot FWD and resolve its FWD{REPLY} (FIFO correlation). */
  private request(frame: Uint8Array): Promise<ParsedFwd> {
    return new Promise<ParsedFwd>((resolve, reject) => {
      this.pending.push({ resolve, reject });
      try {
        this.transport.send(frame);
      } catch (err) {
        this.pending.pop();
        reject(err instanceof Error ? err : new Error(String(err)));
      }
    });
  }

  /** Turn a RESULT reply into its payload TLV; throw {@link FwdError} on ERROR. */
  private result(reply: ParsedFwd): Tlv | null {
    if (reply.kind === FWD_KIND.ERROR) throw new FwdError(replyErrorCode(reply));
    return reply.payload;
  }

  /* --------------------------------------------------------- one-shot --- */

  /**
   * Read a remote vertex's value: `FWD{ op=READ, dst=path, src=<reply-ep> }`.
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
   * Read a remote vertex's `:field`: `FWD{ op=READ, dst=path, FIELD=selector, src=… }`.
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
   * Write a value to a remote vertex: `FWD{ op=WRITE, dst=path, src=…, payload }`.
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
   * Block for the next write to a remote vertex: `FWD{ op=AWAIT, dst=path, src=…,
   * await_timeout? }`. Named `await_` because `await` is reserved; an `await`
   * alias is installed on the prototype for the RFC-0004-spelled call site.
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
   * Subscribe to a remote producer: a `WRITE` of a `SUBSCRIBER{ target=<reply-ep> }`
   * into `producer:subscribers[]` (RFC-0004 §C/§D — subscribe is a field-write).
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

  /** {@link encodeValue} — the exact VALUE TLV bytes, independently of any transport. */
  static encodeValue(value: Uint8Array, opts?: ValueOptions): Uint8Array {
    return encodeValue(value, opts);
  }

  /** {@link encodePath} — the exact PATH TLV bytes. */
  static encodePath(segments: string[]): Uint8Array {
    return encodePath(segments);
  }

  /** {@link encodeSubscriber} — the exact SUBSCRIBER TLV bytes. */
  static encodeSubscriber(targetPath: string[], opts?: SubscriberOptions): Uint8Array {
    return encodeSubscriber(targetPath, opts);
  }
}

// `await` is a reserved word, so the method is `await_`; expose `client.await(...)`
// too (a member name may be a keyword) for the RFC-0004-spelled ergonomic call.
Object.defineProperty(LibtracerClient.prototype, 'await', {
  value: LibtracerClient.prototype.await_,
  writable: true,
  configurable: true,
  enumerable: false,
});

/** Normalize a `/`-path string (or segment array) into validated segments. */
function splitPath(path: string | string[]): string[] {
  if (Array.isArray(path)) return path;
  const segs = path.split('/').filter((s) => s.length > 0);
  if (segs.length === 0) throw new RangeError(`path ${JSON.stringify(path)} has no segments`);
  return segs;
}
