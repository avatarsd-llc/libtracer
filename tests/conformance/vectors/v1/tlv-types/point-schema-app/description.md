# tlv-types/point-schema-app

The RFC-0010 §B.2 **two-part `:schema` read**, byte-for-byte as the reference
`read_schema` emits it for a vertex named `temp` with one declared app field
(`kp`, access `rw`, owner descriptor `NAME "dtype" NAME "f32"`):

```
POINT (PL=1) {
  NAME "temp"                          ; vertex name
  SETTINGS (PL=1) {                    ; synthesized protocol part (unchanged shape)
    NAME "deadline_ns"       VALUE u64=0
    NAME "history_keep_last" VALUE u32=1
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

```
074079000200040074656d700b40380002000b00646561646c696e655f6e7301000800000000000000000002001100686973746f72795f6b6565705f6c6173740100040001000000020003006170700b402a00020002006b700b4020000200060061636365737301000200727702000500647479706502000300663332
```
