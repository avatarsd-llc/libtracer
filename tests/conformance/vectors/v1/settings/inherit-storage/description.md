# settings/inherit-storage

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.6 — **storage policy inherits by copy at registration**. A parent registered with
`history_keep_last = 4`, `store_ref_min_bytes = 256`; the bytes below are what a CHILD
registered under it serves from its own `:settings`:

```
SETTINGS (PL=1) {
  NAME "history_keep_last"   VALUE u32 = 4
  NAME "store_ref_min_bytes" VALUE u32 = 256
}
```

**Behavioural expectations pinned by this vector** (§3.C):

- a child registered under an overriding parent serves these bytes;
- a child registered **later** serves them too — inheritance is per registration, not a
  one-shot at first descent, and an intermediate placeholder level carries the value down;
- a child that states its **own** policy at registration serves its own bytes instead, and
  *its* descendants inherit from **it**;
- a later `:settings` write on the parent reaches every still-inheriting descendant and stops
  at one that overrode — the override grows the subtree that opted in, and nothing else;
- a vertex whose whole ancestry is at the defaults allocates **no** extension block at all.

The value is a COPY, never an ancestor walk: `store_ref_min_bytes` is read on every write and
`history_keep_last` on every store, so both must stay a single inline load.

```
0b403c0002001100686973746f72795f6b6565705f6c61737401000400040000000200130073746f72655f7265665f6d696e5f62797465730100040000010000
```
