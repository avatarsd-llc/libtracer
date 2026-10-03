# Subscribing is a write (Rust)

A consumer subscribes by writing a `SUBSCRIBER` that names its own endpoint into the
producer's `:subscribers[]` field. Every later delivery is an ordinary `FWD{WRITE}` addressed
to that endpoint (RFC-0004 §C/§D).

## What to notice

- **One operation, two uses.** The subscribe and the delivery are both `op=WRITE`. The first
  carries the `:subscribers[]` field selector; the second is addressed to the consumer.
- **The SUBSCRIBER carries the target.** `subscriber_target_path` reads `/client` out of the
  payload, and the producer delivers there.
- **A delivery is pushed.** The producer sends it when it is written, and the consumer handles
  it like any other inbound write.

## Source

```{literalinclude} /bindings/rust/examples/fwd_subscribe.rs
:language: rust
:linenos:
```

Run it from `bindings/rust/`:

```console
$ cargo run --example fwd_subscribe
```

See also: [subscribe to one vertex (C++)](sub-callback.md) · [graph model reference](../reference/02-graph-model.md).
