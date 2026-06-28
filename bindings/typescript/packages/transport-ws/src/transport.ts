// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// TransportWs — the dial-out WebSocket transport for libtracer (#54, ADR-0029).
//
// It mirrors the seam of the C++ `tr::net::transport_ws_client`
// (core/src/transport_ws.cpp): a `send(bytes)` that puts a complete libtracer
// TLV onto the wire as ONE binary WebSocket frame, plus a receiver registration
// for inbound frames. A libtracer TLV is carried as a single RFC 6455 BINARY
// frame (opcode 0x2): the C++ transport sends the whole TLV as one masked client
// frame and reassembles inbound frames the same way, so this transport stays
// wire-compatible with the C++ `transport_ws_server`.
//
// Framing is delegated to the runtime's WebSocket (native in the browser and in
// Node >= 22; the `ws` package in older Node). That implementation performs the
// RFC 6455 masking/framing a client must do — identical on the wire to the
// hand-rolled codec in ./ws.ts, which exists for cross-implementation agreement
// tests and for environments without a WebSocket.

/** A received libtracer frame: the unmasked payload of one inbound BINARY frame. */
export type FrameReceiver = (bytes: Uint8Array) => void;

/** The minimal WHATWG-WebSocket surface this transport relies on. */
export interface WebSocketLike {
  binaryType: string;
  send(data: Uint8Array): void;
  close(code?: number, reason?: string): void;
  onopen: ((ev: unknown) => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
  onclose: ((ev: unknown) => void) | null;
  onerror: ((ev: unknown) => void) | null;
}

/** Constructs a {@link WebSocketLike} for a `ws://` / `wss://` URL. */
export type WebSocketCtor = new (url: string) => WebSocketLike;

/** Options for {@link TransportWs}. */
export interface TransportWsOptions {
  /**
   * The WebSocket implementation to dial with. Defaults to the runtime global
   * `WebSocket` (browser, Node >= 22). Pass the `ws` package's `WebSocket` when
   * running on a Node without a global one.
   */
  WebSocket?: WebSocketCtor;
}

/** Normalize an inbound WebSocket message payload to a `Uint8Array` of its bytes. */
function toBytes(data: unknown): Uint8Array | null {
  if (data instanceof Uint8Array) {
    // Copy out of the (possibly pooled) backing buffer so callers own their bytes.
    return new Uint8Array(data);
  }
  if (data instanceof ArrayBuffer) {
    return new Uint8Array(data.slice(0));
  }
  if (ArrayBuffer.isView(data)) {
    const v = data as ArrayBufferView;
    return new Uint8Array(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength));
  }
  return null; // text / unsupported payloads are ignored (BINARY-only transport)
}

/**
 * A WebSocket-backed libtracer transport client.
 *
 * Lifecycle: construct with a URL, `await connect()`, then `send()` TLV bytes and
 * receive inbound TLV bytes via {@link onFrame}. Each `send()` emits exactly one
 * BINARY WebSocket frame carrying the whole TLV, matching the C++ transport.
 */
export class TransportWs {
  private readonly url: string;
  private readonly ctor: WebSocketCtor;
  private ws: WebSocketLike | null = null;
  private receiver: FrameReceiver | null = null;

  /**
   * @param url     The `ws://host:port` (or `wss://`) endpoint to dial.
   * @param options Transport options; see {@link TransportWsOptions}.
   */
  constructor(url: string, options: TransportWsOptions = {}) {
    const ctor = options.WebSocket ?? (globalThis as { WebSocket?: WebSocketCtor }).WebSocket;
    if (!ctor) {
      throw new Error(
        'No WebSocket implementation available: pass options.WebSocket (e.g. the `ws` package) ' +
          'or run on a runtime with a global WebSocket (browser / Node >= 22).',
      );
    }
    this.url = url;
    this.ctor = ctor;
  }

  /**
   * Register (or clear) the inbound-frame receiver. Mirrors the C++
   * `transport_ws_client::set_receiver`. The callback is invoked once per inbound
   * BINARY frame with that frame's payload bytes.
   */
  onFrame(receiver: FrameReceiver | null): void {
    this.receiver = receiver;
  }

  /** True once the underlying WebSocket has opened and not yet closed. */
  get connected(): boolean {
    return this.ws !== null;
  }

  /**
   * Open the WebSocket and complete the RFC 6455 opening handshake.
   *
   * @param deadlineMs Reject if the socket has not opened within this many
   *                   milliseconds (default 5000). Uses events, never a sleep.
   * @returns A promise that resolves when the connection is open.
   */
  connect(deadlineMs = 5000): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      let settled = false;
      const ws = new this.ctor(this.url);
      ws.binaryType = 'arraybuffer';

      const timer = setTimeout(() => {
        if (settled) return;
        settled = true;
        try {
          ws.close();
        } catch {
          /* ignore */
        }
        reject(new Error(`TransportWs.connect timed out after ${deadlineMs}ms`));
      }, deadlineMs);

      ws.onopen = () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        this.ws = ws;
        resolve();
      };
      ws.onmessage = (ev) => {
        const bytes = toBytes(ev.data);
        if (bytes && this.receiver) this.receiver(bytes);
      };
      ws.onerror = () => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timer);
        reject(new Error('TransportWs.connect failed (WebSocket error)'));
      };
      ws.onclose = () => {
        this.ws = null;
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        reject(new Error('TransportWs.connect closed before opening'));
      };
    });
  }

  /**
   * Send one complete libtracer TLV as a single BINARY WebSocket frame. The
   * underlying WebSocket applies the RFC 6455 client masking. Throws if not
   * connected.
   */
  send(frame: Uint8Array): void {
    if (!this.ws) throw new Error('TransportWs.send called before connect()');
    this.ws.send(frame);
  }

  /** Close the connection. Resolves once the socket has fully closed. */
  close(): Promise<void> {
    return new Promise<void>((resolve) => {
      const ws = this.ws;
      if (!ws) {
        resolve();
        return;
      }
      ws.onclose = () => {
        this.ws = null;
        resolve();
      };
      ws.close();
    });
  }
}
