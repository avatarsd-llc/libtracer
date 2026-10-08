# subscriber/no-target-refused

[RFC-0009](../../../../../../docs/spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md)
§D.1, as corrected by its erratum of 2026-10-08 — **a SUBSCRIBER without `target_path` is not
an unsubscribe**:

```
SUBSCRIBER (PL=1) {
  SETTINGS (PL=1) {
    NAME "delivery_policy" VALUE u16=0x0020   ; bit 5 = durability_request
  }
}
```

The record is `subscriber/policy-durability` with its `PATH` child removed. It decodes and
round-trips: the codec has nothing to refuse.

**Behavioural expectation.** Written to `:subscribers[N]` with slot N active, the record
names no target, so it can neither replace slot N's edge nor clear it:

- the write answers `ERROR{tr::schema::type_mismatch}` (`0x0030`);
- slot N keeps its edge: `:subscribers[N]` reads back the record it held before, and the
  producer's next write is still delivered through it;
- nothing is latched, although the record requests durability: it was never admitted.

The one slot-clear sentinel is the empty `STATUS` (`09 00 00 00`, `framing/empty-status-ok`).
Reference/05 §`0x04` used to call a targetless SUBSCRIBER a "clear this slot" sentinel; no
door has treated it so since RFC-0009 §D.1 made the indexed write payload-discriminating
([#598](https://github.com/avatarsd-llc/libtracer/issues/598)).

```
04401d000b40190002000f0064656c69766572795f706f6c696379010002002000
```
