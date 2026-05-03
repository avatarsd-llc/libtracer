# Reference 03 — Addressing

> **Status**: stub. Canonical content lives in [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) until promoted.

## What goes here when filled

- Path syntax grammar (EBNF): `/segment(/segment)*(:field(.subfield)*(\[N\])?)?`.
- Field-path resolution rules; how `subscribers[]` array index is allocated (write-append vs explicit index).
- **Address-shift slicing**: when a logical message is too large for one TLV, the publisher emits to `ep[0]..ep[N]` with a shared timestamp. Reassembly rules:
  - Same `(path, ts)` group = one logical message.
  - Out-of-order arrival is permitted; subscriber assembles by index.
  - Loss = missing index in the timestamp group; QoS decides reaction.
- Host-local vs global address scope: when a path is local-only, when it's globally visible, how the bridge layer adds host-prefix or peer-prefix.
- Worked examples: 64 KB payload as `ep[0..N]` with 4 KB slices; loss-detection on missing index 5.
