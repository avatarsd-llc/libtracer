# tlv-types/point-schema-app

The RFC-0010 §B.2 **two-part `:schema` read**, byte-for-byte as the reference
`read_schema` emits it for a vertex named `temp` with one declared app field
(`kp`, access `rw`, owner descriptor `NAME "dtype" NAME "f32"`):

```
POINT (PL=1) {
  NAME "temp"                          ; vertex name
  SETTINGS (PL=1) {                    ; synthesized protocol part (RFC-0022 §3.B)
    NAME "history_keep_last"   VALUE u32=1
    NAME "store_ref_min_bytes" VALUE u32=0
  }
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
the protocol part used to enumerate `deadline_ns` (u64) and `history_keep_last` (u32). It now
enumerates exactly the two knobs a vertex still owns — the same set the write gate accepts.
The owner part is unchanged.

```
07407d000200040074656d700b403c0002001100686973746f72795f6b6565705f6c61737401000400010000000200130073746f72655f7265665f6d696e5f62797465730100040000000000020003006170700b402a00020002006b700b4020000200060061636365737301000200727702000500647479706502000300663332
```
