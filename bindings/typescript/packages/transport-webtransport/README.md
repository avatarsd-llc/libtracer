# @avatarsd-llc/libtracer-webtransport

WebTransport transport for [libtracer](https://github.com/avatarsd-llc/libtracer)
— a browser `WebTransport` client that carries libtracer TLV frames over **one
bidirectional stream** with **4-byte u32-LE length-prefix framing**. It is
**wire-compatible with the C++ `tr::net::webtransport_transport_t`**
(`core/include/libtracer/transport_webtransport.hpp`, the `libtracer_quic`
module): a browser connects directly to a node's graph
([ADR-0043](../../../../docs/adr/0043-quic-webtransport-optional-module-msquic.md)
Phase B — the [#92](https://github.com/avatarsd-llc/libtracer/issues/92) /
[ADR-0031](../../../../docs/adr/0031-direct-browser-to-robot-binding-and-webtransport.md)
browser↔robot path).

WebTransport is the browser-reachable form of QUIC (ADR-0031): TLS 1.3, no TCP
head-of-line blocking, and the substrate for later datagram/multi-stream modes.
The framing is the same M6 stream framing the C++ TCP/QUIC transports speak —
the prefix is *transport* framing, not part of the TLV. The core
(`@avatarsd-llc/libtracer`) is a `peerDependency`
([ADR-0033](../../../../docs/adr/0033-npm-subpackage-monorepo.md)).

## Two entry points

```ts
// 1) The pure, socket-free length-prefix record codec.
import { encodeRecord, FrameReassembler } from '@avatarsd-llc/libtracer-webtransport/framing';

// 2) The TransportWebTransport client (drives the runtime's WebTransport).
import { TransportWebTransport } from '@avatarsd-llc/libtracer-webtransport';
import { encode, TYPE } from '@avatarsd-llc/libtracer';
```

## Usage (browser)

```ts
const transport = new TransportWebTransport('https://robot.local:4433/', {
  // DEV ONLY — pin the self-signed server certificate. Browsers require an
  // ECDSA certificate valid <= 14 days for this path; deployments use a real
  // certificate and omit the option.
  serverCertificateHashes: [{ algorithm: 'sha-256', value: certHashBytes }],
});
transport.onFrame((bytes) => console.log('frame', bytes));
await transport.connect();
transport.send(encode(myTlv));
```

Compute the dev hash from the server's PEM:

```sh
openssl x509 -in cert.pem -outform der | openssl dgst -sha256 -binary | base64
```

`TransportWebTransport` satisfies the client SDK's `ClientTransport` seam
structurally (`send` / `onFrame` / `onClose`), exactly like `TransportWs` —
`new LibtracerClient(transport)` works unchanged.

## Runtimes and testing

Node has **no native WebTransport client**, so this package's unit tests run
against a mocked `WebTransport` (web streams), and the wire itself is proven
end-to-end in C++ (`core/tests/webtransport_test.cpp` implements the client
half of the handshake). A real-browser interop harness
(`test/interop-browser.test.mjs`) drives chrome-headless via puppeteer against
the C++ `wt_interop_server` echo binary; it skips gracefully unless
`LIBTRACER_WT_INTEROP_SERVER` is set **and** puppeteer is installed.
