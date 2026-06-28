# @avatarsd-llc/libtracer-ws

WebSocket transport for [libtracer](https://github.com/avatarsd-llc/libtracer).

> **Status: scaffold only — not implemented.** This package exists to fix the
> package boundary, name, and `exports` shape decided in
> [ADR-0033](../../../../docs/adr/0033-npm-subpackage-monorepo.md). It carries no
> functional transport code and is marked `private: true`, so it cannot be
> published until the implementation lands (tracked by
> [#54](https://github.com/avatarsd-llc/libtracer/issues/54)).

## Why a separate package

A transport is a vertex behind a swappable seam
([ADR-0027](../../../../docs/adr/0027-transport-and-connections-are-vertices.md)),
and WebSocket is the first reliable transport
([ADR-0029](../../../../docs/adr/0029-websocket-first-transport-quic-deferred-per-link.md)).
Keeping it out of the core package means a consumer that only needs the
in-process codec (`@avatarsd-llc/libtracer`) never pulls a WebSocket dependency.
Future transports (`@avatarsd-llc/libtracer-webtransport`, …) follow the same
one-package-per-transport boundary.

## Planned shape

```ts
import { decode, encode } from '@avatarsd-llc/libtracer/wire';
// import { wsTransport } from '@avatarsd-llc/libtracer-ws'; // when implemented
```

The core is a `peerDependency`, so the transport binds against whatever
cross-validated core version the consumer installs.
