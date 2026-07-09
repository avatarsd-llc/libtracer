// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief The L2/L3 wire codec layer (`tr::wire` in the C++ reference) — exposed
 * as the tree-shakeable subpath entry `@avatarsd-llc/libtracer/wire`.
 *
 * It is a pure-TS port of core/src/frame.cpp, gated byte-for-byte by the shared
 * conformance vectors. This is the only layer the TS core implements today; the
 * L0/L1/L4 subpaths (./mem, ./view, ./graph) are reserved for when those layers
 * land in TS — see docs/adr/0033-npm-subpackage-monorepo.md.
 */
export {
  TYPE,
  ERROR,
  CodecError,
  decode,
  encode,
  equal,
  crc32c,
  crc16ccitt,
} from './codec.mjs';

export type { Opt, Timestamp, Crc, Trailer, Tlv } from './codec.mjs';
