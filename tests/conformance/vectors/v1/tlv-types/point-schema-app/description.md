# tlv-types/point-schema-app

The RFC-0010 §B.2 **two-part `:schema` read**, byte-for-byte as the reference
`read_schema` emits it for a vertex named `temp` with one declared app field
(`kp`, access `rw`, owner descriptor `NAME "dtype" NAME "f32"`):

```
POINT (PL=1) {
  NAME "temp"                          ; vertex name
  SETTINGS (PL=1) { }                  ; synthesized protocol part — EMPTY (RFC-0022 §3.B)
  NAME "app"                           ; owner part — present iff a table is installed
  SETTINGS (PL=1) {
    NAME "kp" SETTINGS (PL=1) {
      NAME "access" VALUE "rw"         ; runtime-projected from the descriptor table
      NAME "dtype"  NAME "f32"         ; owner descriptor bytes, verbatim
    }
  }
}
```

**Regenerated for [RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)**:
the protocol part used to enumerate `deadline_ns` (u64) and `history_keep_last` (u32). §3.B
deleted `settings_t` outright, so it now enumerates **nothing** — the same (empty) set the write
gate accepts, which is what makes the synthesized part complete for the first time. The empty
`SETTINGS` is emitted rather than omitted, so the two-part record keeps its shape. The owner part
is unchanged, and this vector is what pins the two parts' ORDER and their precedence-by-position.

```
074041000200040074656d700b400000020003006170700b402a00020002006b700b4020000200060061636365737301000200727702000500647479706502000300663332
```
