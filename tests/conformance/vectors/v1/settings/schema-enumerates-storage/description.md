# settings/schema-enumerates-storage

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.5 — the synthesized `:schema` protocol part, for a default vertex named `temp` with no
owner field descriptor table:

```
POINT (PL=1) {
  NAME "temp"
  SETTINGS (PL=1) {
    NAME "history_keep_last"   VALUE u32 = 1   ; the ring trim depth
    NAME "store_ref_min_bytes" VALUE u32 = 0   ; the zero-copy store threshold (0 = off)
  }
}
```

**Exactly** the implemented storage knobs, which after RFC-0022 §3.B is the whole of the
vertex's `:settings` core namespace — so the synthesized part is now both complete and true.
It previously advertised `deadline_ns`, which nothing consumed, and omitted
`store_ref_min_bytes`, which everything did: the reported set was not even a subset of the
working set. That is the condition [#706](https://github.com/avatarsd-llc/libtracer/issues/706)
was filed about, dissolved rather than fixed.

A bare `:settings` read serves the same two knobs in the same order (plus the nested `app`
record when a descriptor table is installed), so read and write gate can never disagree.

```
074048000200040074656d700b403c0002001100686973746f72795f6b6565705f6c61737401000400010000000200130073746f72655f7265665f6d696e5f62797465730100040000000000
```
