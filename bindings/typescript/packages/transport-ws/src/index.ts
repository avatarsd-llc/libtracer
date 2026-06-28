// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// @avatarsd-llc/libtracer-ws — WebSocket transport for libtracer.
//
// SCAFFOLD ONLY. This file fixes the package boundary and entry point; the
// transport is not implemented yet. Per ADR-0027 a transport is a vertex behind
// a swappable seam, and ADR-0029 selects WebSocket as the first reliable
// transport. The implementation is tracked by #54 and intentionally deferred
// out of the npm-monorepo foundation slice — see
// docs/adr/0033-npm-subpackage-monorepo.md.

export const TRANSPORT = 'ws' as const;
