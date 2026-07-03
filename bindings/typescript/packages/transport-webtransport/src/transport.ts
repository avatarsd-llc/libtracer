// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// TransportWebTransport — the browser WebTransport transport for libtracer
// (ADR-0043 Phase B / ADR-0031: the browser-facing low-latency form of QUIC).
//
// It mirrors the seam of the C++ `tr::net::webtransport_transport_t`
// (core/src/transport_webtransport.cpp) from the DIAL side: the runtime's
// `WebTransport` performs the HTTP/3 extended-CONNECT session handshake, then
// this transport opens ONE bidirectional stream (`createBidirectionalStream`)
// as the frame channel and carries each libtracer TLV as a `u32-LE length ++
// frame` record (./framing.ts) — the identical stream framing the C++ TCP,
// QUIC and WebTransport transports speak, so a browser interoperates with a
// C++ `webtransport_transport_t` listener directly.
//
// Dev-time trust uses the standard WebTransport pattern: pass the SHA-256 hash
// of the server's certificate via `serverCertificateHashes` (the certificate
// must be ECDSA and valid <= 14 days — a browser rule, not ours). Deployments
// use a real certificate and omit the option.

import { FrameReassembler, encodeRecord } from './framing.js';

/** A received libtracer frame: the payload of one complete length-prefixed record. */
export type FrameReceiver = (bytes: Uint8Array) => void;

/** Invoked once when an OPEN session closes/errors, with the cause when known. */
export type CloseHandler = (cause?: Error) => void;

/** One entry of `serverCertificateHashes` (the WebCrypto dev-trust pattern). */
export interface WebTransportHash {
  /** The digest algorithm; browsers accept `"sha-256"`. */
  algorithm: string;
  /** The digest of the server certificate's DER bytes. */
  value: ArrayBuffer | ArrayBufferView;
}

/** The constructor options subset this transport forwards to `WebTransport`. */
export interface WebTransportInit {
  serverCertificateHashes?: WebTransportHash[];
}

/** The minimal reader surface of a `ReadableStream<Uint8Array>`. */
export interface StreamReaderLike {
  read(): Promise<{ done: boolean; value?: Uint8Array }>;
  cancel(reason?: unknown): Promise<unknown> | void;
}

/** The minimal writer surface of a `WritableStream<Uint8Array>`. */
export interface StreamWriterLike {
  write(chunk: Uint8Array): Promise<void>;
  close(): Promise<void>;
}

/** The minimal bidirectional-stream surface this transport relies on. */
export interface WebTransportBidiLike {
  readable: { getReader(): StreamReaderLike };
  writable: { getWriter(): StreamWriterLike };
}

/** The minimal `WebTransport` surface this transport relies on. */
export interface WebTransportLike {
  ready: Promise<unknown>;
  closed: Promise<unknown>;
  close(closeInfo?: { closeCode?: number; reason?: string }): void;
  createBidirectionalStream(): Promise<WebTransportBidiLike>;
}

/** Constructs a {@link WebTransportLike} for an `https://` URL. */
export type WebTransportCtor = new (url: string, options?: WebTransportInit) => WebTransportLike;

/** Options for {@link TransportWebTransport}. */
export interface TransportWebTransportOptions {
  /**
   * The WebTransport implementation to dial with. Defaults to the runtime
   * global `WebTransport` (browsers; Node has no native client — use the
   * mocked implementation in tests, or run the browser interop harness).
   */
  WebTransport?: WebTransportCtor;
  /**
   * Dev-time certificate pinning: the SHA-256 hash(es) of the server's DER
   * certificate (`openssl x509 -in cert.pem -outform der | openssl dgst
   * -sha256`). Browsers require an ECDSA certificate valid <= 14 days for
   * this path. Omit in deployment (real certificate).
   */
  serverCertificateHashes?: WebTransportHash[];
}

/**
 * A WebTransport-backed libtracer transport client.
 *
 * Lifecycle: construct with an `https://host:port/` URL, `await connect()`
 * (session handshake + the bidirectional frame stream), then `send()` TLV
 * bytes and receive inbound TLVs via {@link onFrame}. Satisfies the client
 * SDK's `ClientTransport` seam structurally (send / onFrame / onClose),
 * exactly like `TransportWs`.
 */
export class TransportWebTransport {
  private readonly url: string;
  private readonly ctor: WebTransportCtor;
  private readonly hashes: WebTransportHash[] | undefined;
  private wt: WebTransportLike | null = null;
  private writer: StreamWriterLike | null = null;
  private receiver: FrameReceiver | null = null;
  private closeHandler: CloseHandler | null = null;
  private closeNotified = false;

  /**
   * @param url     The `https://host:port/path` WebTransport endpoint to dial.
   * @param options Transport options; see {@link TransportWebTransportOptions}.
   */
  constructor(url: string, options: TransportWebTransportOptions = {}) {
    const ctor =
      options.WebTransport ?? (globalThis as { WebTransport?: WebTransportCtor }).WebTransport;
    if (!ctor) {
      throw new Error(
        'No WebTransport implementation available: run in a browser (or pass ' +
          'options.WebTransport). Node has no native WebTransport client.',
      );
    }
    this.url = url;
    this.ctor = ctor;
    this.hashes = options.serverCertificateHashes;
  }

  /**
   * Register (or clear) the inbound-frame receiver. Mirrors the C++
   * `set_receiver`: invoked once per complete length-prefixed record with that
   * record's frame bytes.
   */
  onFrame(receiver: FrameReceiver | null): void {
    this.receiver = receiver;
  }

  /**
   * Register (or clear) the connection-closed notifier (the `ClientTransport`
   * seam's optional close hook). Invoked at most once per session, when an
   * OPEN session closes — remotely, on error, or via {@link close}.
   */
  onClose(handler: CloseHandler | null): void {
    this.closeHandler = handler;
  }

  /** Fire the close notifier exactly once for the current session. */
  private notifyClose(cause?: Error): void {
    if (this.closeNotified) return;
    this.closeNotified = true;
    if (this.closeHandler) this.closeHandler(cause);
  }

  /** True once the session and its frame stream are up and not yet closed. */
  get connected(): boolean {
    return this.wt !== null && this.writer !== null;
  }

  /**
   * Establish the WebTransport session (HTTP/3 extended CONNECT) and open the
   * ONE bidirectional frame stream.
   *
   * @param deadlineMs Reject if the session is not ready within this many
   *                   milliseconds (default 5000). Uses events, never a sleep.
   * @returns A promise that resolves when frames can flow.
   */
  async connect(deadlineMs = 5000): Promise<void> {
    const init: WebTransportInit = {};
    if (this.hashes) init.serverCertificateHashes = this.hashes;
    const wt = new this.ctor(this.url, init);

    let timer: ReturnType<typeof setTimeout> | undefined;
    const deadline = new Promise<never>((_, reject) => {
      timer = setTimeout(() => {
        try {
          wt.close();
        } catch {
          /* ignore */
        }
        reject(new Error(`TransportWebTransport.connect timed out after ${deadlineMs}ms`));
      }, deadlineMs);
    });

    try {
      await Promise.race([wt.ready, deadline]);
      const stream = await Promise.race([wt.createBidirectionalStream(), deadline]);
      this.writer = stream.writable.getWriter();
      this.wt = wt;
      this.closeNotified = false; // arm the close notifier for this session

      // The session-closed watcher: resolves on clean close, rejects on error.
      wt.closed.then(
        () => {
          this.wt = null;
          this.writer = null;
          this.notifyClose();
        },
        (err: unknown) => {
          this.wt = null;
          this.writer = null;
          this.notifyClose(err instanceof Error ? err : new Error(String(err)));
        },
      );

      // The read loop: reassemble length-prefixed records off the frame stream.
      void this.readLoop(stream.readable.getReader());
    } catch (err) {
      throw err instanceof Error
        ? err
        : new Error(`TransportWebTransport.connect failed: ${String(err)}`);
    } finally {
      clearTimeout(timer);
    }
  }

  /** Pump the frame stream through the reassembler until it ends. */
  private async readLoop(reader: StreamReaderLike): Promise<void> {
    const reassembler = new FrameReassembler();
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        if (!value) continue;
        // A malformed prefix throws here — the stream is desynced (see below).
        const frames = reassembler.push(value);
        for (const frame of frames) {
          if (this.receiver) this.receiver(frame);
        }
      }
    } catch (err) {
      // Desynced framing or a stream error: the session is unusable.
      try {
        this.wt?.close();
      } catch {
        /* ignore */
      }
      this.notifyClose(err instanceof Error ? err : new Error(String(err)));
    }
  }

  /**
   * Send one complete libtracer TLV as one length-prefixed record on the frame
   * stream. Throws if not connected; a transmit failure surfaces through the
   * session's close notifier (the underlying write is asynchronous).
   */
  send(frame: Uint8Array): void {
    if (!this.writer) throw new Error('TransportWebTransport.send called before connect()');
    // Backpressure is delegated to the stream's internal queue; a rejected
    // write means the session died — the closed watcher notifies.
    void this.writer.write(encodeRecord(frame)).catch(() => {
      /* surfaced via wt.closed */
    });
  }

  /** Close the session. Resolves once the session has fully closed. */
  async close(): Promise<void> {
    const wt = this.wt;
    if (!wt) return;
    this.wt = null;
    this.writer = null;
    try {
      wt.close();
    } catch {
      /* ignore */
    }
    try {
      await wt.closed;
    } catch {
      /* a close-triggered rejection is still a completed close */
    }
    this.notifyClose();
  }
}
