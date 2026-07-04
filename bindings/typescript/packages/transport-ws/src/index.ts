// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// @avatarsd-llc/libtracer-ws — WebSocket transport for libtracer (#54, ADR-0029).
//
// A transport is a vertex behind a swappable seam (ADR-0027); WebSocket is the
// first reliable transport (ADR-0029). This package carries libtracer TLV frames
// over RFC 6455 WebSocket, wire-compatible with the C++ `tr::net::transport_ws`.
//
// Two entry points, deliberately separate:
//   - this barrel (`.`): the production `TransportWs` client, which frames over the
//     runtime's own WebSocket — it does NOT pull in the hand-rolled codec; and
//   - the `./ws` subpath: the pure, socket-free RFC 6455 frame codec, cross-validated
//     byte-for-byte against the C++ codec. It is the cross-implementation-agreement
//     oracle (ws_diff_fuzz, the ws-codec tests) and the fallback for runtimes without
//     a native WebSocket — not the production framing path. It is imported explicitly
//     from `@avatarsd-llc/libtracer-ws/ws` so it tree-shakes out of every consumer that
//     only wants the transport (it is NOT re-exported here).
// The core (`@avatarsd-llc/libtracer`) is a peerDependency (ADR-0033).

export const TRANSPORT = 'ws' as const;

export { TransportWs } from './transport.js';
export type {
  FrameReceiver,
  CloseHandler,
  WebSocketLike,
  WebSocketCtor,
  TransportWsOptions,
} from './transport.js';
