# acl/ace-retired-bit

RFC-0036 §3.1: the rights registry holds two bits, `READ` (`0x01`) and `WRITE`
(`0x02`). This is one ALLOW ACE whose `access_mask` sets a bit outside them:

```
ACL (PL=1) {
  ACL (PL=1) {                              ; one ACE
    NAME "type"        VALUE 0x00           ; ALLOW
    NAME "subject"     VALUE "peer-a"
    NAME "access_mask" VALUE u32=0x00000004 ; the old SUBSCRIBE bit
  }
}
```

**Behavioural expectation:** every ACE reader **rejects** the record
(`TYPE_MISMATCH` / `TypeMismatch`). The bit is not masked off and the ACE is not
read as an empty grant: an ACL written for the eight-right registry would
otherwise install with a meaning its author did not write.

The codec round-trips these bytes untouched (`input.bin`, not `reject.bin`) —
the refusal is the READER's, bound per core (see HARNESS.md): C++
`core/tests/acl_test.cpp`, Rust `bindings/rust/tests/conformance_vectors.rs`.

```
0a403d000a40390002000400747970650100010000020007007375626a65637401000600706565722d6102000b006163636573735f6d61736b0100040004000000
```
