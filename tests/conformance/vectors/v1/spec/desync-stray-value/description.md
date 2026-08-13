# spec/desync-stray-value

The #995 walk-parity witness: a creation SPEC whose children begin with a stray
non-`NAME` where the first key belongs:

```
SPEC (PL=1) {
  VALUE 0xEE                  ; a non-NAME in a key slot — the pair stream is lost here
  NAME "type" NAME "stored_value"
  NAME "name" NAME "temp"
}
```

**Behavioural expectation:** the pair-consuming walk STOPS at child 0 — the
pairing is unknowable past a desync, and resynchronizing at every offset is the
defect #927 removed. Both fields therefore read **absent**, the catalog selector
stays empty, and the terminus refuses the create with `INVALID_PATH`, nothing
created. A reader that resyncs reads `("stored_value", "temp")` out of these
bytes — a spelling no terminus accepts, which is exactly what the Rust binding
did before #995.

The codec harness checks only the round-trip; the reader claim is bound per core
(see HARNESS.md): C++ `core/tests/children_test.cpp`, Rust
`bindings/rust/tests/conformance_vectors.rs`.

```
0e402d0001000100ee020004007479706502000c0073746f7265645f76616c7565020004006e616d650200040074656d70
```
