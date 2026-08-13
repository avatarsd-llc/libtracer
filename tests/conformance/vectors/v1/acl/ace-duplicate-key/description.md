# acl/ace-duplicate-key

The #995 **security-reader** family, the disposition opposite to the plain
NAME-field one: an ACE whose `access_mask` appears twice, narrow then wide:

```
ACL (PL=1) {
  ACL (PL=1) {                              ; one ACE
    NAME "type"        VALUE 0x00           ; ALLOW
    NAME "subject"     VALUE "peer-a"
    NAME "access_mask" VALUE u16=0x0001     ; READ
    NAME "access_mask" VALUE u16=0xFFFF     ; repeated — every right at once
  }
}
```

**Behavioural expectation:** every ACE reader **rejects** the record
(`TYPE_MISMATCH` / `TypeMismatch`). An ACL is a security document: applying the
plain family's last-wins rule here would read the ACE as the 0xFFFF grant — a
duplicate key WIDENING access — and first-wins would let a prepended duplicate
narrow a deny the same way. So repeats are refused in every tier, alongside the
family's other refusals (unknown key, non-`NAME` in a key slot, odd child
count). Same pair-consuming mechanics as #927, opposite disposition to the
config reader's forward-compat tolerance — deliberately (#906).

The codec round-trips these bytes untouched (`input.bin`, not `reject.bin`) —
the refusal is the READER's, bound per core (see HARNESS.md): C++
`core/tests/acl_test.cpp`, Rust `bindings/rust/tests/conformance_vectors.rs`.

```
0a4050000a404c0002000400747970650100010000020007007375626a65637401000600706565722d6102000b006163636573735f6d61736b01000200010002000b006163636573735f6d61736b01000200ffff
```
