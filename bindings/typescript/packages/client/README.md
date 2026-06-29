# @avatarsd-llc/libtracer-client (EXPERIMENTAL)

The libtracer **client SDK** the strawberry web-UI builds against
([#56](https://github.com/avatarsd-llc/libtracer/issues/56),
[ADR-0034](../../../../docs/adr/0034-typescript-client-sdk.md) /
[ADR-0035](../../../../docs/adr/0035-implementing-rfc-0004-remote-operation-addressing.md)).
It composes the cross-validated wire codec (`@avatarsd-llc/libtracer`) over an
**injected** transport seam (`@avatarsd-llc/libtracer-ws`'s `TransportWs` satisfies
it structurally), so it runs in the browser or Node and is testable against an
in-memory fake with no socket.

> **Experimental — `0.1.0`, pre-1.0.** The client speaks the
> [RFC-0004](../../../../docs/spec/rfcs/0004-remote-operation-addressing.md)
> remote-operation envelope (`FWD` / `FIELD`) the v1 spec §3 fast-tracks: a
> path-addressed `read` / `write` / `await` / `readField` / `subscribe`, each a
> `FWD` frame whose source-routed `FWD{REPLY}` is decoded back. Error-reply codes
> are **provisional** until the ERROR registry
> ([#8](https://github.com/avatarsd-llc/libtracer/issues/8)) pins them.

## What it does

```ts
import { LibtracerClient, encodeValue } from '@avatarsd-llc/libtracer-client';
import { TransportWs } from '@avatarsd-llc/libtracer-ws';

const transport = new TransportWs('ws://robot.local:9000');
await transport.connect();
const client = new LibtracerClient(transport);

// Path-addressed remote operations over RFC-0004 FWD — each resolves its FWD{REPLY}:
const temp = await client.read('/sensor/temp');                  // -> VALUE TLV
await client.write('/sensor/temp', encodeValue(new Uint8Array([0x01])));
const next = await client.await_('/sensor/temp', 1_000_000_000n); // block for the next write (1 s)
const subs = await client.readField('/sensor/temp', ':subscribers[]'); // -> POINT of SUBSCRIBER slots

// subscribe = a field-write of a SUBSCRIBER into :subscribers[]; deliveries fire the handler.
const unsubscribe = await client.subscribe('/sensor/temp', (value) => render(value));
unsubscribe(); // local detach

// A kind=ERROR reply rejects with a typed FwdError (.code / .codeName, e.g. "NOT_FOUND").
client.onError((err) => console.error('inbound decode error', err));
```

The pure builders (the exact bytes, transport-free) are also exported — the FWD /
FIELD envelope builders and the payload builders, each pinned to a conformance
vector:

```ts
import { encodeFwd, encodeField, FWD_OP, encodeSubscriber } from '@avatarsd-llc/libtracer-client';

encodeSubscriber(['sensor', 'temp']);                          // === subscriber-path vector
encodeFwd({ op: FWD_OP.READ, dst: ['sensor', 'temp'], src: ['client'] }); // === fwd-read vector
encodeField(':subscribers[]');                                 // === field-append vector
```

## Surfaces

| Surface | Status | Pinned by |
| --- | --- | --- |
| `read` / `write` / `await_` / `readField` / `subscribe` over `FWD` | **implemented** (RFC-0004) | `fwd-read`, `fwd-write-value`, `fwd-await-timeout`, `fwd-write-subscriber-field` |
| `encodeFwd` / `encodeField` builders + `decodeFwd` | **implemented** | `fwd-*`, `field-*`, `fwd-reply-*` |
| `encodeValue` / `encodeSubscriber` / `encodePath` payload builders | **implemented** | `value-bool-true`, `value-ll-u32`, `value-ts-abs`, `subscriber-path`, `path-sensor-temp` |
| inbound VALUE delivery (FWD{WRITE} / bare / ROUTER-shed) | **implemented** | `router-wrapped` + all vectors |
| typed `FwdError` on `kind=ERROR` reply | **implemented** (codes provisional) | `fwd-reply-error`; ERROR registry (#8) pins the code set |

## Dependencies

`@avatarsd-llc/libtracer` is a required `peerDependency` (the cross-validated
codec — [ADR-0033](../../../../docs/adr/0033-npm-subpackage-monorepo.md)).
`@avatarsd-llc/libtracer-ws` is an **optional** peer (the seam is structural; the
client never imports it — inject any `{ send, onFrame }`). ESM-only, `node >= 18`.

## Tests

```sh
npm run build && npm test   # from bindings/typescript/
```

- `test/vectors.test.mjs` — every outbound builder (`encodeValue`/`encodeSubscriber`/
  `encodePath`/`encodeFwd`/`encodeField`) matches its conformance vector
  byte-for-byte; `decodeFwd` parses the `fwd-reply-*` vectors.
- `test/roundtrip.test.mjs` — mock-transport round-trip: the exact `FWD` bytes each
  op emits, REPLY-resolves-the-op / ERROR-rejects, injected deliveries, and a real
  `TransportWs` echo.
- `test/interop.test.mjs` — **end-to-end** against a live C++ `fwd_node_server` over
  a real socket (all five ops, incl. a live subscribe delivery). Guarded on
  `LIBTRACER_FWD_NODE_SERVER`; the CI `fwd-interop` job builds the binary and runs it.
