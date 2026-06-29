// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// @avatarsd-llc/libtracer-client — the experimental libtracer client SDK (#56,
// ADR-0034). A curated composition over the cross-validated codec
// (@avatarsd-llc/libtracer, a peerDependency) and an injected transport seam
// (@avatarsd-llc/libtracer-ws's TransportWs satisfies it structurally).
//
// EXPERIMENTAL / pre-1.0 (0.1.0). With RFC-0004 (spec §3, the remote-operation
// envelope) implemented, the client now speaks the path-addressed higher
// operations over `FWD`: read / write / await / readField / subscribe, each a
// FWD frame whose FWD{REPLY} is source-routed back. The pure payload builders
// (VALUE / SUBSCRIBER / PATH) and the FWD / FIELD builders are independently
// exported and vector-pinned — see docs/spec/rfcs/0004-remote-operation-addressing.md.

export const CLIENT_EXPERIMENTAL = true as const;

export { LibtracerClient, FwdError } from './client.js';
export type {
  ClientTransport,
  ValueHandler,
  Unsubscribe,
  ClientOptions,
} from './client.js';

export { encodeValue, encodePath, encodeSubscriber } from './tlv.js';
export type { ValueOptions, SubscriberOptions } from './tlv.js';

export {
  FWD_OP,
  FWD_KIND,
  FWD_ERROR,
  fwdErrorName,
  encodeFwd,
  encodeField,
  parseField,
  decodeFwd,
  parseFwdTlv,
  replyErrorCode,
} from './fwd.js';
export type { FwdRequest, ParsedFwd, FieldLevel } from './fwd.js';
