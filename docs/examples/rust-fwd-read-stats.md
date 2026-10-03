# Reading a node's :stats (Rust)

A monitor reads a node's counters with one READ per seam:
`READ <any vertex>:stats.graph.delivery` answers one SETTINGS block of `NAME`/u64 pairs
(RFC-0010 Amendment 1). The example builds the request, then decodes the block a fresh node
answers.

## What to notice

- **One READ is one seam is one block.** The four counters arrive together, sampled in one
  call, so a monitor compares two whole blocks rather than counters read at different moments.
- **Look counters up by NAME.** A reader ignores names it does not know. `settings_get`
  returns `None` for one, which is how a newer node's extra counters stay harmless.
- **The answer here is built, not received.** The crate has no transport, so the example
  constructs the 113-byte block a fresh node sends. The `settings/stats-seam-block`
  conformance vector pins those bytes.

## Source

```{literalinclude} /bindings/rust/examples/fwd_read_stats.rs
:language: rust
:linenos:
```

Run it from `bindings/rust/`:

```console
$ cargo run --example fwd_read_stats
```

See also: [protocol TLVs reference, the `:stats` record](../reference/05-protocol-tlvs.md).
