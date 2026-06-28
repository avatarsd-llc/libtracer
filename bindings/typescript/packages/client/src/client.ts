// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// LibtracerClient — the experimental browser/Node client SDK (#56, ADR-0034).
//
// It composes the cross-validated codec (@avatarsd-llc/libtracer) over an
// INJECTED transport seam (`@avatarsd-llc/libtracer-ws`'s TransportWs satisfies
// it structurally), so it is browser/Node-agnostic and testable against an
// in-memory fake with no live socket.
//
// Scope is deliberately bounded to what v1 PINS byte-for-byte (ADR-0034):
//   - outbound : build a VALUE / SUBSCRIBER payload TLV and emit it as one frame.
//   - inbound  : decode each frame (shedding one ROUTER wrapper) and deliver
//                VALUE payloads to handlers.
// The path-ADDRESSED request envelope (verb + destination vertex:field +
// correlation) that `write(path,…)` / `read` / `await` / `subscribe(producer,…)`
// would need is UNSPECIFIED in v1 (spec/v1.md §3 is "to be written") and is
// therefore DEFERRED, not faked here.

import { TYPE, decode, CodecError } from '@avatarsd-llc/libtracer';
import type { Tlv } from '@avatarsd-llc/libtracer';
import { encodeValue, encodePath, encodeSubscriber } from './tlv.js';
import type { ValueOptions, SubscriberOptions } from './tlv.js';

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

/** A live subscription handle. `close()` stops local delivery to its handler. */
export interface Subscription {
  /** The target path segments this subscription delivers to. */
  readonly targetPath: readonly string[];
  /**
   * Stop delivering to this subscription's handler. NOTE: this is a LOCAL detach
   * only — it does not emit an unsubscribe frame, because clearing a remote
   * `:subscribers[N]` slot needs the path-addressed envelope that v1 leaves
   * unspecified (ADR-0034 §3).
   */
  close(): void;
}

/**
 * A WebSocket/transport-agnostic libtracer client.
 *
 * Construct over any {@link ClientTransport}, register {@link onValue} (or
 * {@link subscribe}) handlers, and {@link write} / {@link subscribe} to emit the
 * pinned payload frames. Inbound decode errors are routed to {@link onError},
 * never thrown across the transport's receive callback.
 */
export class LibtracerClient {
  private readonly transport: ClientTransport;
  private readonly valueHandlers = new Set<ValueHandler>();
  private errorHandler: ((err: Error) => void) | null = null;

  /** @param transport the injected connection seam (e.g. a connected `TransportWs`). */
  constructor(transport: ClientTransport) {
    this.transport = transport;
    transport.onFrame((bytes) => this.dispatch(bytes));
  }

  /* ---------------------------------------------------------- inbound --- */

  /**
   * Register a handler for inbound VALUE deliveries (a single ROUTER wrapper is
   * shed first, mirroring the bridge shedding rule). Multiple handlers may be
   * registered; each receives every delivery.
   */
  onValue(handler: ValueHandler): void {
    this.valueHandlers.add(handler);
  }

  /** Register the sink for inbound decode failures and other client-side errors. */
  onError(handler: (err: Error) => void): void {
    this.errorHandler = handler;
  }

  /** Decode one inbound frame and fan it out to the VALUE handlers. Never throws. */
  private dispatch(bytes: Uint8Array): void {
    let tlv: Tlv;
    try {
      tlv = decode(bytes);
    } catch (err) {
      this.emitError(err instanceof Error ? err : new CodecError(String(err)));
      return;
    }
    // Shed exactly one ROUTER envelope: the wrapped data TLV is its last child
    // (reference/05 §ROUTER — the `NAME "data"` marker tags it last).
    const data =
      tlv.type === TYPE.ROUTER && tlv.children.length > 0
        ? tlv.children[tlv.children.length - 1]
        : tlv;
    if (data.type === TYPE.VALUE) {
      for (const handler of this.valueHandlers) handler(data.payload, data);
    }
  }

  private emitError(err: Error): void {
    if (this.errorHandler) this.errorHandler(err);
  }

  /* --------------------------------------------------------- outbound --- */

  /**
   * Emit a VALUE TLV (the data-write payload) as one frame. PINNED by the VALUE
   * vectors. The producer reached is the vertex the transport connection is
   * bound to (the mount/bridge model); the in-band path-addressed write envelope
   * is deferred (ADR-0034).
   *
   * @param value the opaque payload bytes
   * @param opts  optional length-width / CRC / wire-timestamp selectors
   */
  write(value: Uint8Array, opts?: ValueOptions): void {
    this.transport.send(encodeValue(value, opts));
  }

  /**
   * Emit a SUBSCRIBER TLV (the subscribe-write payload) as one frame and register
   * `handler` for inbound VALUE deliveries. PINNED by the `subscriber-path`
   * vector (outbound) and the VALUE decode (inbound).
   *
   * @param targetPath the delivery target path segments (the SUBSCRIBER's `target_path`)
   * @param handler    invoked with each inbound VALUE delivery
   * @param opts       reserved for future optional SUBSCRIBER children (none yet)
   * @returns a {@link Subscription} whose `close()` detaches the handler locally
   */
  subscribe(targetPath: string[], handler: ValueHandler, opts?: SubscriberOptions): Subscription {
    const frame = encodeSubscriber(targetPath, opts);
    this.valueHandlers.add(handler);
    this.transport.send(frame);
    const segments = [...targetPath];
    return {
      targetPath: segments,
      close: () => {
        this.valueHandlers.delete(handler);
      },
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
