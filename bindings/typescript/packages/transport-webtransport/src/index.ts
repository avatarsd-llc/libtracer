// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// @avatarsd-llc/libtracer-webtransport — WebTransport transport for libtracer
// (ADR-0043 Phase B, the #92 browser->robot path recorded in ADR-0031).
//
// A transport is a vertex behind a swappable seam (ADR-0027); WebTransport is
// the browser-reachable form of QUIC (ADR-0031 ranks it above plain QUIC for
// the browser and above WebSocket for streams). This package carries libtracer
// TLV frames over ONE WebTransport bidirectional stream with 4-byte u32-LE
// length-prefix framing, wire-compatible with the C++
// `tr::net::webtransport_transport_t` (the libtracer_quic module).
//
// Two entry points:
//   - the `framing` subpath: the pure, socket-free length-prefix record codec
//     (shared with the C++ TCP/QUIC/WebTransport stream framing); and
//   - this barrel: the `TransportWebTransport` client plus a re-export.
// The core (`@avatarsd-llc/libtracer`) is a peerDependency (ADR-0033).

export const TRANSPORT = 'webtransport' as const;

export {
  PREFIX_BYTES,
  MAX_FRAME,
  MalformedPrefixError,
  encodeRecord,
  FrameReassembler,
} from './framing.js';

export { TransportWebTransport } from './transport.js';
export type {
  FrameReceiver,
  CloseHandler,
  WebTransportHash,
  WebTransportInit,
  StreamReaderLike,
  StreamWriterLike,
  WebTransportBidiLike,
  WebTransportLike,
  WebTransportCtor,
  TransportWebTransportOptions,
} from './transport.js';
