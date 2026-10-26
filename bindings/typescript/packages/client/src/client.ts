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
 *   - read(path)            — FWD{ op=READ,  dst=path, src=<reply-ep>/q<n> }
 *   - write(path, valueTLV) — FWD{ op=WRITE, dst=path, src=…, payload }
 *   - await(path, ns?)      — FWD{ op=AWAIT, dst=path, src=…, await_timeout? }
 *   - readField(path, sel)  — FWD{ op=READ,  dst=path, FIELD=sel, src=… }
 *   - subscribe(prod, h)    — FWD{ op=WRITE, dst=prod, FIELD=:subscribers[],
 *                                  src=<reply-ep>/sub<n>,
 *                                  payload=SUBSCRIBER{ target=<reply-ep>/sub<n> } }
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
  FWD_ERROR,
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
   * every outbound FWD. A subscription targets its own path one segment below it
   * (`<reply-ep>/sub<n>`), used as both its `src` and its SUBSCRIBER `target`. Over a single
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

/** @brief A pending one-shot request awaiting its FWD{REPLY}. */
interface Pending {
  resolve(reply: ParsedFwd): void;
  reject(err: Error): void;
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
  /**
   * @brief The request's own reply token: the one segment after the reply endpoint in its
   * `src`, which the REPLY's `dst` carries home (see {@link LibtracerClient.correlate}).
   */
  token: string;
}

/** @brief A request that timed out: what its late REPLY can still be recognised by. */
interface Expired {
  /** @brief Its `dst` segments, for a reply that carries no token. */
  dst: string[] | null;
  /** @brief Its reply token. */
  token: string;
}

/**
 * @brief How many timed-out requests the client remembers, so their late replies are
 * dropped rather than handed to a live caller. The oldest is forgotten first.
 */
const EXPIRED_CAP = 32;

/** @brief The request deadline used when `requestTimeoutMs` is not given. */
const DEFAULT_REQUEST_TIMEOUT_MS = 10_000;

/**
 * @brief A remote await's local deadline in ms (RFC-0004 Amendment 3: the requester
 * owns it). The shorter of the request deadline and the await timeout; the request
 * deadline falls back to its 10 s default when disabled, so an await always ends.
 */
export function awaitDeadlineMs(requestTimeoutMs: number, timeoutNs?: bigint): number {
  const request =
    requestTimeoutMs > 0 && Number.isFinite(requestTimeoutMs)
      ? requestTimeoutMs
      : DEFAULT_REQUEST_TIMEOUT_MS;
  if (timeoutNs === undefined) return request;
  const own = Math.max(1, Math.ceil(Number(timeoutNs) / 1e6));
  return Math.min(request, own);
}

/**
 * @brief Settle a pending entry: clear its deadline. Every entry leaves `pending` as it
 * settles (answered, timed out, or the transport closed), so the queue holds live
 * requests only (#2330).
 */
function settle(p: Pending): void {
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
  /**
   * @brief Each live subscription's handler, keyed by its target's last segment
   * (`sub<n>`, the one segment below the reply endpoint).
   */
  private readonly subscriptions = new Map<string, ValueHandler>();
  /** @brief The number the next subscription's target segment carries. */
  private nextSubscription = 1;
  /** @brief The number the next one-shot request's reply token (`q<n>`) carries. */
  private nextRequest = 1;
  private readonly pending: Pending[] = [];
  /** @brief The most recent timed-out requests, oldest first, at most {@link EXPIRED_CAP}. */
  private readonly expired: Expired[] = [];
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
    this.requestTimeoutMs = options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;
    transport.onFrame((bytes) => this.dispatch(bytes));
    transport.onClose?.((cause) => this.handleClose(cause));
  }

  /** @brief Reject every pending request when the transport closes; fail-fast afterwards. */
  private handleClose(cause?: Error): void {
    this.closed = new Error('transport closed' + (cause ? `: ${cause.message}` : ''));
    for (const p of this.pending) {
      settle(p);
      p.reject(this.closed);
    }
    this.pending.length = 0;
    this.expired.length = 0;
  }

  /* ---------------------------------------------------------- inbound --- */

  /**
   * @brief Register a handler for inbound VALUE deliveries.
   *
   * A delivery is a FWD{WRITE} (delivery-is-a-write, RFC-0004 §D); a bare VALUE
   * or a single ROUTER wrapper is also accepted. Multiple handlers may be
   * registered; each receives every delivery, including the ones a
   * {@link subscribe} handler also receives.
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
        if (at < 0) return; // a late reply, or one with nothing outstanding to answer
        const [waiter] = this.pending.splice(at, 1);
        settle(waiter);
        waiter.resolve(parsed);
        return;
      }
      // A delivery IS a FWD{WRITE} (RFC-0004 §D): deliver its VALUE payload.
      if (parsed.op === FWD_OP.WRITE && parsed.payload && parsed.payload.type === TYPE.VALUE) {
        this.deliver(parsed.payload, parsed.dst);
      }
      return;
    }

    // Non-FWD frame: shed exactly one ROUTER envelope, then deliver a VALUE
    // (the directly-delivered / bridge-mounted path, ADR-0034 inbound rule).
    const data =
      tlv.type === TYPE.ROUTER && tlv.children.length > 0 ? tlv.children[tlv.children.length - 1] : tlv;
    if (data.type === TYPE.VALUE) this.deliver(data, null);
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
   * caller gets the previous request's reply (#1530).
   *
   * @section token The originator matches by its own reply token
   *
   * Every request's `src` is the reply endpoint plus one segment of its own: `q<n>`
   * for a one-shot, `sub<n>` for a subscribe. A REPLY is routed by that `src`, so its
   * `dst` carries the token home; `dst` shrinks per hop, so what arrives is a TAIL of
   * `<reply-ep>/<token>` — its last segment is the token and whatever precedes it must
   * end the reply endpoint, the same reading {@link deliver} applies. The token is an
   * ordinary `src` segment, not a field in `FWD`, and the receiver holds no state for
   * it ([18 §Acknowledged one-shot operations and safe retry](
   * ../../../../../docs/reference/18-composition-over-the-network.md#acknowledged-one-shot-operations-and-safe-retry)).
   *
   * @section fallback When the token is absent
   *
   * A peer that answers to its own endpoint instead of routing to `src` leaves no token
   * in `dst`. Then the reply's `src` is matched as before: the oldest outstanding
   * request whose `dst` ends with it, FIFO among the ones the wire cannot tell apart.
   * Since #2245 a reply's `src` is the target's PAIR (RFC-0029 §6.2), an escape that
   * names no segments, so such a reply falls through to the oldest entry — as does an
   * empty `src`, or one matching nothing outstanding.
   *
   * @section expired A late reply
   *
   * A request that times out leaves `pending` and is remembered in `expired` (#2330).
   * Its late reply, matched by token or, failing that, by `src` against no live request,
   * consumes that record and is dropped (`-1`). A live request is always tried first, so
   * a dead one can never take a reply from a live request to the same path.
   */
  private correlate(parsed: ParsedFwd): number {
    const fifo = this.pending.length > 0 ? 0 : -1;
    const late = (match: (e: Expired) => boolean): boolean => {
      const at = this.expired.findIndex(match);
      if (at < 0) return false;
      this.expired.splice(at, 1);
      return true;
    };
    const byToken = segmentsOf(parsed.dst);
    if (byToken !== null && byToken.length > 0 && endsWith(this.replyEndpoint, byToken.slice(0, -1))) {
      const token = byToken[byToken.length - 1];
      const at = this.pending.findIndex((p) => p.token === token);
      if (at >= 0) return at;
      if (late((e) => e.token === token)) return -1;
    }
    const src = segmentsOf(parsed.src);
    if (src === null || src.length === 0) return fifo;
    const at = this.pending.findIndex((p) => p.dst !== null && endsWith(p.dst, src));
    if (at >= 0) return at;
    return late((e) => e.dst !== null && endsWith(e.dst, src)) ? -1 : fifo;
  }

  /**
   * @brief Hand a delivered VALUE to every {@link onValue} handler, and to the one
   * subscription its `dst` names.
   *
   * A producer addresses a delivery by the return route its subscribe request
   * carried, which is that subscription's own target `<reply-ep>/sub<n>`. `dst`
   * shrinks per hop, so what arrives is a TAIL of that target: its last segment
   * names the subscription and whatever precedes it must end the reply endpoint (a
   * node whose link shares the endpoint's first segment consumes that segment as a
   * hop). A delivery whose `dst` names no live subscription (no `dst` at all, the bare
   * reply endpoint, a detached or unknown subscription, a `PATH_REF` or an escaped
   * path) reaches the {@link onValue} handlers only.
   *
   * @param value the delivered VALUE TLV
   * @param dst   the delivery's `dst` route, or `null` for a bare / ROUTER-wrapped VALUE
   */
  private deliver(value: Tlv, dst: Tlv | null): void {
    for (const handler of this.valueHandlers) handler(value.payload, value);
    const segs = segmentsOf(dst);
    if (segs === null || segs.length === 0 || !endsWith(this.replyEndpoint, segs.slice(0, -1))) return;
    this.subscriptions.get(segs[segs.length - 1])?.(value.payload, value);
  }

  private emitError(err: Error): void {
    if (this.errorHandler) this.errorHandler(err);
  }

  /**
   * @brief Send a one-shot FWD and resolve its FWD{REPLY}.
   *
   * @param build builds the request frame around the `src` it is given: the reply
   *              endpoint plus this request's own reply token
   * @param dst   the request's destination segments — the fallback key a reply's `src`
   *              echoes (see {@link correlate})
   * @param deadlineMs this request's own deadline in ms, overriding
   *              `requestTimeoutMs` (used by {@link await_})
   * @param onDeadline the rejection raised when the deadline elapses
   * @param token the reply token, when the caller already owns one (a subscription's
   *              `sub<n>`); otherwise a fresh `q<n>`
   */
  private request(
    build: (src: string[]) => Uint8Array,
    dst: string[],
    deadlineMs: number = this.requestTimeoutMs,
    onDeadline: () => Error = () =>
      new Error(`request timed out after ${deadlineMs}ms (no FWD reply)`),
    token: string = `q${this.nextRequest++}`,
  ): Promise<ParsedFwd> {
    const frame = build([...this.replyEndpoint, token]);
    return new Promise<ParsedFwd>((resolve, reject) => {
      if (this.closed) {
        reject(this.closed);
        return;
      }
      const entry: Pending = { resolve, reject, timer: null, dst, token };
      if (deadlineMs > 0 && Number.isFinite(deadlineMs)) {
        entry.timer = setTimeout(() => {
          // Evict the waiter so it cannot take a live request's reply, and remember it
          // so its own late reply is dropped (#2330).
          settle(entry);
          const at = this.pending.indexOf(entry);
          if (at >= 0) this.pending.splice(at, 1);
          this.expired.push({ dst: entry.dst, token: entry.token });
          if (this.expired.length > EXPIRED_CAP) this.expired.shift();
          entry.reject(onDeadline());
        }, deadlineMs);
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
   * @brief Read a remote vertex's value: `FWD{ op=READ, dst=path, src=<reply-ep>/q<n> }`.
   *
   * @param path the destination path (a `/`-string like `"/sensor/temp"`, or segments)
   * @returns the RESULT's value TLV (decoded)
   * @throws {FwdError} when the responder replies `kind=ERROR` (e.g. NOT_FOUND)
   */
  async read(path: string | string[]): Promise<Tlv> {
    const dst = splitPath(path);
    const reply = await this.request(
      (src) => encodeFwd({ op: FWD_OP.READ, dst, src }),
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
      (src) => encodeFwd({ op: FWD_OP.READ, dst, field: selector, src }),
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
      (src) => encodeFwd({ op: FWD_OP.WRITE, dst, src, payload: valueTLV }),
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
      (src) =>
        encodeFwd({
        op: FWD_OP.WRITE,
        dst,
        field: selector,
        src,
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
   * The deadline is this client's own (RFC-0004 Amendment 3): a responder is not
   * required to answer `TIMEOUT`, so the wait always ends locally, at the shorter
   * of `requestTimeoutMs` (10 s by default; the default is used even when the
   * option disables other requests' deadlines) and `timeoutNs`.
   *
   * Each request carries its own reply token, so an `await_` and a `read` to the same
   * vertex may be outstanding at once.
   *
   * @param path        the destination path (string or segments)
   * @param timeoutNs   the await timeout in ns, sent as `await_timeout` and used as
   *                    this call's local deadline when shorter than `requestTimeoutMs`
   * @returns the next write's value TLV
   * @throws {FwdError} with code TIMEOUT when the local deadline elapses first
   */
  async await_(path: string | string[], timeoutNs?: bigint): Promise<Tlv> {
    const dst = splitPath(path);
    const reply = await this.request(
      (src) =>
        encodeFwd({
        op: FWD_OP.AWAIT,
        dst,
        src,
        awaitTimeoutNs: timeoutNs,
      }),
      dst,
      awaitDeadlineMs(this.requestTimeoutMs, timeoutNs),
      () => new FwdError(FWD_ERROR.TIMEOUT),
    );
    const value = this.result(reply);
    if (!value) throw new FwdError(0);
    return value;
  }

  /**
   * @brief Subscribe to a remote producer: a `WRITE` of a `SUBSCRIBER{
   * target=<reply-ep>/sub<n> }` into `producer:subscribers[]` (RFC-0004 §C/§D —
   * subscribe is a field-write).
   *
   * Each call targets its own path `<reply-ep>/sub<n>` below the reply endpoint and
   * sends the write from it, so the producer addresses its deliveries there. `handler`
   * fires only for deliveries whose `dst` is that path; another subscription's
   * deliveries never reach it.
   *
   * SCOPE: this issues the SUBSCRIBER and receives VALUEs the producer directly
   * delivers to this client's reply endpoint. Full standing-stream delivery
   * additionally needs the producer auto-promote / flush seam (#136 / RFC-0004 §E)
   * — out of scope for this client slice. `unsubscribe()` is a LOCAL detach: it
   * stops firing `handler`, leaves every other subscription working, and does not
   * clear the remote slot (an indexed
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
    const key = `sub${this.nextSubscription++}`;
    const target = [...this.replyEndpoint, key];
    const subscriber = encodeSubscriber(target, opts);
    // Register the handler BEFORE awaiting the ack: a producer may stream its
    // first delivery before its subscribe REPLY is seen on the wire, and we must
    // not drop it. On a rejected subscribe we detach again.
    this.subscriptions.set(key, handler);
    try {
      const reply = await this.request(
        (src) =>
          encodeFwd({
            op: FWD_OP.WRITE,
            dst,
            field: ':subscribers[]',
            src,
            payload: subscriber,
          }),
        dst,
        this.requestTimeoutMs,
        undefined,
        key,
      );
      this.result(reply); // throws on ERROR
    } catch (err) {
      this.subscriptions.delete(key);
      throw err;
    }
    return () => {
      this.subscriptions.delete(key);
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
 * @brief Does @p path end with the segments @p tail — the reply-`src` suffix test,
 * and the test that a delivery's shrunken `dst` still names this client's endpoint.
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

/**
 * @brief A route's segments, or `null` when it names none this client can read: no route, a
 * retired `PATH_REF`, or a `PATH` carrying an escape record such as a PAIR (RFC-0029 §4).
 */
function segmentsOf(route: Tlv | null): string[] | null {
  if (route === null) return null;
  try {
    return pathSegments(route);
  } catch {
    return null;
  }
}

/** @brief Normalize a `/`-path string (or segment array) into validated segments. */
function splitPath(path: string | string[]): string[] {
  if (Array.isArray(path)) return path;
  const segs = path.split('/').filter((s) => s.length > 0);
  if (segs.length === 0) throw new RangeError(`path ${JSON.stringify(path)} has no segments`);
  return segs;
}
