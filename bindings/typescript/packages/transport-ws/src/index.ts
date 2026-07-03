// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// @avatarsd-llc/libtracer-ws — WebSocket transport for libtracer (#54, ADR-0029).
//
// A transport is a vertex behind a swappable seam (ADR-0027); WebSocket is the
// first reliable transport (ADR-0029). This package carries libtracer TLV frames
// over RFC 6455 WebSocket, wire-compatible with the C++ `tr::net::transport_ws`.
//
// Two entry points:
//   - the `ws` subpath: a pure, socket-free RFC 6455 frame codec (cross-validated
//     byte-for-byte against the C++ codec); and
//   - this barrel: the `TransportWs` client plus a re-export of the codec.
// The core (`@avatarsd-llc/libtracer`) is a peerDependency (ADR-0033).

export const TRANSPORT = 'ws' as const;

export {
  Opcode,
  acceptKey,
  encodeFrame,
  encodeClientFrame,
  decodeFrame,
  sha1,
  base64,
} from './ws.js';
export type { Frame } from './ws.js';

export { TransportWs } from './transport.js';
export type {
  FrameReceiver,
  CloseHandler,
  WebSocketLike,
  WebSocketCtor,
  TransportWsOptions,
} from './transport.js';
