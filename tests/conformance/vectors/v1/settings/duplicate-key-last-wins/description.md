# settings/duplicate-key-last-wins

The plain NAME-field family disposition (#995, mechanics from #927): a SETTINGS
whose key appears **twice**, the first occurrence wrong-typed:

```
SETTINGS (PL=1) {
  NAME "kind" VALUE 0x01     ; wrong-typed for a string key — ignored, not destructive
  NAME "kind" NAME "ws"      ; the LAST well-formed occurrence: this one wins
}
```

**Behavioural expectation:** a string read of `kind` answers `"ws"`. The walk is
pair-consuming (a value child is never re-read as the next key) and, within the
pairs matching a key, the last **well-formed** occurrence wins — a wrong-typed
occurrence is skipped and never clobbers a well-formed one, whichever order the
two arrive in. Before #995 the Rust binding resynchronised at every offset and
took the FIRST match, so these bytes read `None` there and `"ws"` at the C++
terminus (`wire::config_reader_t`).

The codec harness checks only the round-trip; the reader claim is bound per core
(see HARNESS.md): C++ `core/tests/config_reader_test.cpp`, Rust
`bindings/rust/tests/conformance_vectors.rs`.

```
0b401b00020004006b696e640100010001020004006b696e64020002007773
```
