# @avatarsd-llc/libtracer-client (EXPERIMENTAL)

The libtracer **client SDK** the strawberry web-UI builds against
([#56](https://github.com/avatarsd-llc/libtracer/issues/56),
[ADR-0034](../../../../docs/adr/0034-typescript-client-sdk.md)). It composes the
cross-validated wire codec (`@avatarsd-llc/libtracer`) over an **injected**
transport seam (`@avatarsd-llc/libtracer-ws`'s `TransportWs` satisfies it
structurally), so it runs in the browser or Node and is testable against an
in-memory fake with no socket.

> **Experimental — `private`, `0.0.0`, not published.** This package implements
> only the wire byte-products libtracer v1 **pins** byte-for-byte. The
> path-addressed request envelope (`write(path,…)` / `read` / `await` /
> `subscribe(producerPath,…)`) is **deferred** because the v1 spec leaves the
> wire format for an addressed remote operation unspecified (`spec/v1.md` §3 is
> "to be written"). See the ADR for the full rationale and the open question.

## What it does (the pinned slice)

```ts
import { LibtracerClient } from '@avatarsd-llc/libtracer-client';
import { TransportWs } from '@avatarsd-llc/libtracer-ws';

const transport = new TransportWs('ws://robot.local:9000');
await transport.connect();
const client = new LibtracerClient(transport);

// Inbound: decode each frame (a single ROUTER wrapper is shed) and deliver VALUE payloads.
client.onValue((value) => console.log('delivery', value));
client.onError((err) => console.error('decode error', err));

// Outbound (pinned payload TLVs, emitted as one frame each):
client.write(new Uint8Array([0x01]));            // VALUE TLV
const sub = client.subscribe(['sensor', 'temp'], (value) => render(value)); // SUBSCRIBER TLV
sub.close();                                      // local detach (no unsubscribe frame yet)
```

Pure builders (the exact bytes, transport-free) are also exported and available as
static methods:

```ts
import { encodeValue, encodePath, encodeSubscriber } from '@avatarsd-llc/libtracer-client';
encodeSubscriber(['sensor', 'temp']); // === the `subscriber-path` conformance vector, byte-for-byte
```

## Pinned vs deferred

| Surface | Status | Pinned by |
| --- | --- | --- |
| `encodeValue` / `write` (VALUE) | **implemented** | `value-bool-true`, `value-ll-u32`, `value-ts-abs` |
| `encodeSubscriber` / `subscribe` (SUBSCRIBER) | **implemented** | `subscriber-path` |
| `encodePath` (PATH) | **implemented** | `path-sensor-temp`, spec §3.1 |
| inbound VALUE delivery + ROUTER shed | **implemented** | `router-wrapped` + all vectors |
| `write(path,…)` / `subscribe(producerPath,…)` / `read` / `await` / `connect` | **deferred** | unspecified — `spec/v1.md` §3 ("to be written"), no request-envelope vector |

## Dependencies

`@avatarsd-llc/libtracer` is a required `peerDependency` (the cross-validated
codec — [ADR-0033](../../../../docs/adr/0033-npm-subpackage-monorepo.md)).
`@avatarsd-llc/libtracer-ws` is an **optional** peer (the seam is structural; the
client never imports it — inject any `{ send, onFrame }`). ESM-only, `node >= 18`.

## Tests

```sh
npm run build && npm test   # from bindings/typescript/
```

- `test/vectors.test.mjs` — outbound builders match the conformance vectors
  byte-for-byte; inbound VALUE vectors decode to the right payload.
- `test/roundtrip.test.mjs` — mock-transport round-trip (`write`/`subscribe`
  bytes, injected VALUE + ROUTER-wrapped delivery, decode-error routing) plus a
  real `TransportWs` echo round-trip.
