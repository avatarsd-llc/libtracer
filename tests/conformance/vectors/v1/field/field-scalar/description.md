# field/field-scalar

FIELD{ NAME "subscribers" } — :subscribers (no index_mode VALUE ⇒ SCALAR)

```
10400F0002000B007375627363726962657273
```

The default form of RFC-0004 §C: the index_mode VALUE is optional, and its
absence means `SCALAR=0`. Held alongside `field-append` (`[]`) and
`field-wildcard` (`[*]`) so the three spellings of the same field name are all
pinned — they are the three cases a decoder that skips the 1-byte index_mode
collapses into one (#587).
