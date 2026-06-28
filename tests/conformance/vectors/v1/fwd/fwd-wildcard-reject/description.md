# fwd/fwd-wildcard-reject

FWD{ op=READ, dst=/sensor/temp, FIELD :data[*] (index_mode=WILDCARD), src=/reply-ep } — a `[*]` wildcard on a non-subscriber-path target.

Valid, round-trip-safe at the **codec** layer (the 3-core conformance machine checks `encode(decode(input))==input`, which holds). At the **resolution** layer (RFC-0004 §C / ADR-0035 slice 2) it MUST be rejected with a `kind=ERROR` reply `STATUS=ERROR(INVALID_PATH)` — `[*]` is legal only on a subscriber-path. The C++ `op_resolve_test` asserts that rejection.

```
0f403c000100010000064012000200060073656e736f720200040074656d7010400d000200040064617461010001000206400c00020008007265706c792d6570
```
