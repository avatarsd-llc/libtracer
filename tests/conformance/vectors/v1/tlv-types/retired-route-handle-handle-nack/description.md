# tlv-types/retired-route-handle-handle-nack

[RFC-0032](../../../../../../docs/spec/rfcs/0032-delete-compact-streams-ride-the-chain.md) §6.1 and
§12.2: one outer `0x13` frame, the retired `HANDLE_NACK`, in the layout a peer built before
RFC-0032 emitted: `{ VALUE label(u16) }`, structured (`opt.PL=1`).

```
13400600010002000100
```

`0x11` `ADVERTISE`, `0x12` `COMPACT` and `0x13` `HANDLE_NACK` are retired and not reassigned in
v1. RFC-0032 §12.2 names one vector holding all three frames. A vector is one frame
([`HARNESS.md`](../../../HARNESS.md)), so the three are banked as
`retired-route-handle-advertise`, `retired-route-handle-compact` and
`retired-route-handle-handle-nack` (RFC-0032 erratum 2026-10-10).

**What the codec must do.** Decode the frame structurally as an unknown core-range code, keep
its children as opaque structured TLVs, and re-encode it byte for byte. A reader walking a buffer
skips it by its declared length, and the frame that follows it in the same buffer still parses.
No reader gives the code a meaning.

**What a node must do** (behaviour, which the round-trip cannot check): answer exactly one bare
`ERROR{tr::schema::type_mismatch}` (`08 40 06 00 01 00 02 00 30 00`) on the arrival link, count the
frame in the `:stats.router.drops` noun `retired_rx` even when that answer is refused, create no
state, and send nothing on any other link.

**Behavioural binding:**
- C++: `core/tests/fwd_fanout_test.cpp` (`test_retired_label_frames_are_unknown`) feeds these
  bytes to a router and asserts the answer, the count and the silence on a second link.
  `core/tests/transport_alloc_softfail_test.cpp` asserts the answer allocates nothing and a
  refused answer is still counted.
- Codec, in each core: `core/tests/fwd_fanout_test.cpp`
  (`test_retired_route_handle_vectors_skip_by_length`),
  `bindings/rust/tests/conformance_vectors.rs` (`retired_route_handle_codes_skip_by_length`) and
  `bindings/typescript/packages/client/test/vectors.test.mjs` assert the unknown-code decode, the
  skip by declared length and the frame that follows.
