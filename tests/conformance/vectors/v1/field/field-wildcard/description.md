# field/field-wildcard

FIELD{ NAME "subscribers", index_mode=WILDCARD } — :subscribers[*]

```
1040140002000B0073756273637269626572730100010002
```

The `[*]` spelling of RFC-0004 §C, and the third of the three index_mode values
(`SCALAR=0`, `ELEMENT=1`, `WILDCARD=2` — `enum class index_mode_t` at
`core/src/op_resolve_walk.hpp:313`). Three values, four wire spellings: `[N]` and
`[]` share `ELEMENT=1` and differ by whether the index VALUE is present
(RFC-0004 §C grammar; `op_resolve_walk.hpp:316-320`).

It differs from `field-append` in **exactly one byte** — the trailing index_mode
VALUE, `0x01` → `0x02` — while meaning something entirely different: append one
subscriber versus address every slot. That is why the pair exists; a decoder that
ignores the index_mode byte renders both, and `field-scalar`, identically (#587).

This vector pins the **addressing** only. `[*]` is a read/deferral selector: on a
**WRITE** it is `INVALID_PATH`, because the WRITE grammar has no wildcard axis
([#579](https://github.com/avatarsd-llc/libtracer/issues/579)). The distinction is
not academic — the wildcard sets `indexed` while leaving `index` at 0, so before
that rejection a wildcard write cleared **slot 0** and reported success.
