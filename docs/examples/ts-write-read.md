# Write a remote vertex, then read it (TypeScript)

`LibtracerClient` runs over any transport with `send()` and `onFrame()`. Each `write` and
`read` is one FWD frame out and one `FWD{REPLY}` back.

The node on the other end is
[`stand-in-node.mjs`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/typescript/examples/stand-in-node.mjs),
an in-memory stand-in. The TypeScript packages have no node of their own, and the stand-in lets
the example run with no C++ build. Against a real node the client code is unchanged.

## What to notice

- **The transport is injected.** The client never opens a socket. Here it is handed an
  in-memory link, and the [dial example](ts-ws-dial.md) hands it a WebSocket without changing
  the client code.
- **A read returns the last value written.** Nothing is queued, and a second write replaces
  the first.
- **Replies are matched by path, not by order.** The reply's `src` echoes the request's `dst`,
  which is how the client pairs them without a correlation id.

## Source

```{literalinclude} /bindings/typescript/examples/write-read.mjs
:language: javascript
:linenos:
```

Run it from `bindings/typescript/`, after `npm ci && npm run build`:

```console
$ node examples/write-read.mjs
```

See also: [read and write (C++)](graph-read-write.md) · [Rust: the WRITE frame](rust-fwd-write.md).
