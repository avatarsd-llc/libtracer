// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

export const SPEC_VERSION = 1 as const;

// The core wire codec — a pure-TS/JS port of the C++ reference (core/src/frame.cpp),
// gated by the shared conformance vectors. See ./codec.mjs.
export {
  TYPE,
  ERROR,
  MAX_DEPTH,
  CodecError,
  decode,
  encode,
  equal,
  crc32c,
  crc16ccitt,
} from './codec.mjs';

export type { Opt, Timestamp, Crc, Trailer, Tlv } from './codec.mjs';
