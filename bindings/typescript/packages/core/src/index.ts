// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// Barrel entry (`@avatarsd-llc/libtracer`) — re-exports every layer the TS core
// implements. For tree-shaking, the wire codec is also reachable directly at the
// layer-scoped subpath `@avatarsd-llc/libtracer/wire` (see ./wire.ts and
// docs/adr/0033-npm-subpackage-monorepo.md).

export const SPEC_VERSION = 1 as const;

// The L2/L3 wire codec — a pure-TS/JS port of the C++ reference (core/src/frame.cpp),
// gated by the shared conformance vectors. Re-exported from ./wire.ts (the subpath entry).
export {
  TYPE,
  ERROR,
  CodecError,
  decode,
  encode,
  equal,
  crc32c,
  crc16ccitt,
} from './wire.js';

export type { Opt, Timestamp, Crc, Trailer, Tlv } from './wire.js';
