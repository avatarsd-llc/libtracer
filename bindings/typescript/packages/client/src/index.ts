// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

// @avatarsd-llc/libtracer-client — the experimental libtracer client SDK (#56,
// ADR-0034). A curated composition over the cross-validated codec
// (@avatarsd-llc/libtracer, a peerDependency) and an injected transport seam
// (@avatarsd-llc/libtracer-ws's TransportWs satisfies it structurally).
//
// EXPERIMENTAL / `private` 0.0.0: only the wire byte-products v1 pins are
// implemented (VALUE / SUBSCRIBER / PATH build, frame decode + ROUTER shed +
// VALUE delivery). The path-addressed request envelope that `write(path,…)` /
// `read` / `await` / `subscribe(producer,…)` need is UNSPECIFIED in v1 and is
// deferred — see docs/adr/0034-typescript-client-sdk.md.

export const CLIENT_EXPERIMENTAL = true as const;

export { LibtracerClient } from './client.js';
export type { ClientTransport, ValueHandler, Subscription } from './client.js';

export { encodeValue, encodePath, encodeSubscriber } from './tlv.js';
export type { ValueOptions, SubscriberOptions } from './tlv.js';
