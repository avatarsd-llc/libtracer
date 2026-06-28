# @avatarsd-llc/libtracer-ws

WebSocket transport for [libtracer](https://github.com/avatarsd-llc/libtracer) —
an RFC 6455 frame codec plus a `TransportWs` client that carries libtracer TLV
frames over WebSocket. It is **wire-compatible with the C++
`tr::net::transport_ws`** (`core/include/libtracer/ws.hpp`,
`core/src/transport_ws.cpp`): a TS client interoperates with a C++ server and
vice versa.

A transport is a vertex behind a swappable seam
([ADR-0027](../../../../docs/adr/0027-transport-and-connections-are-vertices.md)),
and WebSocket is the first reliable transport
([ADR-0029](../../../../docs/adr/0029-websocket-first-transport-quic-deferred-per-link.md)).
The core (`@avatarsd-llc/libtracer`) is a `peerDependency`
([ADR-0033](../../../../docs/adr/0033-npm-subpackage-monorepo.md)), so this
package never pulls a WebSocket dependency into a consumer that only needs the
in-process codec.

## Two entry points

```ts
// 1) The pure, socket-free RFC 6455 frame codec (cross-validated byte-for-byte
//    against the C++ codec). Use it where you frame bytes yourself.
import { acceptKey, encodeClientFrame, decodeFrame, Opcode } from '@avatarsd-llc/libtracer-ws/ws';

// 2) The TransportWs client (delegates framing to the runtime's WebSocket).
import { TransportWs } from '@avatarsd-llc/libtracer-ws';
import { encode, TYPE } from '@avatarsd-llc/libtracer';
```

## Carrying a TLV

A libtracer TLV is carried as **one RFC 6455 BINARY frame** (opcode `0x2`),
exactly as the C++ transport does. The client masks its frames (RFC 6455 §5.1);
the server's frames are unmasked. `TransportWs.send()` puts the whole TLV on the
wire as a single BINARY frame, and `onFrame()` delivers each inbound frame's
payload.

```ts
const transport = new TransportWs('ws://host:9000');
transport.onFrame((bytes) => {
  /* bytes is one inbound TLV */
});
await transport.connect();
transport.send(encode({ type: TYPE.NAME, opt: { pl: false }, payload, children: [], trailer: null }));
```

### WebSocket implementation

Framing is delegated to the runtime's `WebSocket` — native in the browser and in
Node >= 22. On older Node, pass an implementation explicitly:

```ts
import { WebSocket } from 'ws';
const transport = new TransportWs('ws://host:9000', { WebSocket });
```

The hand-rolled codec at the `/ws` subpath is what guarantees cross-implementation
agreement and is available for environments without a `WebSocket`.
