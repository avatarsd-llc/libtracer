# Subscribe to a remote producer (TypeScript)

`client.subscribe(path, handler)` writes a SUBSCRIBER into the producer's `:subscribers[]`,
and every later write to the producer is delivered to the handler.

The node on the other end is
[`stand-in-node.mjs`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/typescript/examples/stand-in-node.mjs),
an in-memory stand-in. The TypeScript packages have no node of their own, and the stand-in lets
the example run with no C++ build. Against a real node the client code is unchanged.

## What to notice

- **Two clients, one node.** The consumer subscribes and a second client writes. The
  deliveries arrive in write order.
- **`unsubscribe()` is a local detach.** It stops the handler firing; it does not clear the
  remote slot.
- **The handler is registered before the ack.** A producer may deliver before its subscribe
  reply arrives, and the client does not drop that delivery.

## Source

```{literalinclude} /bindings/typescript/examples/subscribe.mjs
:language: javascript
:linenos:
```

Run it from `bindings/typescript/`, after `npm ci && npm run build`:

```console
$ node examples/subscribe.mjs
```

See also: [subscribe to one vertex (C++)](sub-callback.md) · [Rust: subscribing is a write](rust-fwd-subscribe.md).
