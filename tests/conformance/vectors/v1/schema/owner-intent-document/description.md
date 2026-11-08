# schema/owner-intent-document

The [RFC-0010](../../../../../../docs/spec/rfcs/0010-owner-app-fields-and-schema.md)
Amendment 5 `:schema` read of a vertex named `temp` whose owner installed an **intent
document** and no field descriptor table:

```
POINT (PL=1) {
  NAME "temp"                     ; vertex name
  SETTINGS (PL=1) { }             ; synthesized protocol part, EMPTY (RFC-0022 §3.B)
  NAME "doc"                      ; protocol-owned POINT-level name
  VALUE <intent document>         ; the installed UTF-8 bytes, VERBATIM
}
```

The document is:

```yaml
contract: "https://vendor.example/contracts/thermo/1.0"
help: "Room temperature"
type: "f32"
unit: "Cel"
```

A document alone adds only the `doc` pair. The owner part (`NAME "app"`) is present iff a
field table is installed, so it is absent here. The runtime never parses the document: the
`VALUE` is opaque bytes (`opt.PL = 0`), and the `contract` key is the maker's, not the
runtime's.

The behaviour is bound by C++ `core/tests/intent_document_test.cpp`
(`test_conformance_vectors`, byte-exact against `read_schema`).

```
074080000200040074656d700b40000002000300646f6301006900636f6e74726163743a202268747470733a2f2f76656e646f722e6578616d706c652f636f6e7472616374732f746865726d6f2f312e30220a68656c703a2022526f6f6d2074656d7065726174757265220a747970653a2022663332220a756e69743a202243656c220a
```
