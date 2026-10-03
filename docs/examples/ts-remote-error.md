# A remote failure is a typed error (TypeScript)

A request the node cannot satisfy is still answered, with `FWD{REPLY, kind=ERROR}` carrying
a `STATUS{ERROR}`. The client rejects with a `FwdError` whose `.code` is the registered wire
code.

The node on the other end is
[`stand-in-node.mjs`](https://github.com/avatarsd-llc/libtracer/blob/main/bindings/typescript/examples/stand-in-node.mjs),
an in-memory stand-in. The TypeScript packages have no node of their own, and the stand-in lets
the example run with no C++ build. Against a real node the client code is unchanged.

## What to notice

- **`NOT_FOUND` is `0x0020`.** `err.code` is the u16 from the wire and `err.codeName` its
  registry name, so a caller can branch on it.
- **Failure is a reply, not a timeout.** The promise rejects as soon as the error reply
  arrives.

## Source

```{literalinclude} /bindings/typescript/examples/remote-error.mjs
:language: javascript
:linenos:
```

Run it from `bindings/typescript/`, after `npm ci && npm run build`:

```console
$ node examples/remote-error.mjs
```

See also: [protocol TLVs reference](../reference/05-protocol-tlvs.md).
