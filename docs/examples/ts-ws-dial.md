# Dial a WebSocket link (TypeScript)

`TransportWs` dials a WebSocket listener, and each libtracer frame crosses as one BINARY
message. The listener here is a loopback `ws` server on port 0 in front of the stand-in node.

The node on the other end is
[`stand-in-node.mjs`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/typescript/examples/stand-in-node.mjs),
an in-memory stand-in. The TypeScript packages have no node of their own, and the stand-in lets
the example run with no C++ build. Against a real node the client code is unchanged.

## What to notice

- **`connect()` resolves when the socket is open.** Only then can frames cross. The client
  code is the same as in the in-memory examples.
- **Port 0, read back.** The kernel picks the port and the example reads it from the
  listener, so nothing collides with whatever else runs on the machine.
- **A browser dials the same way.** On a runtime with a global `WebSocket` (a browser, or
  Node 22 and later) the `{ WebSocket }` option can be left out.

## Source

```{literalinclude} /bindings/typescript/examples/ws-dial.mjs
:language: javascript
:linenos:
```

Run it from `bindings/typescript/`, after `npm ci && npm run build`:

```console
$ node examples/ws-dial.mjs
```

See also: [DIAL and LISTEN (C++)](net-dial-and-listen.md) · [the WebSocket upgrade (C++)](net-ws-upgrade.md).
