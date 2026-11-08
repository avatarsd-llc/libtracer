# schema/owner-intent-document-app

The [RFC-0010](../../../../../../docs/spec/rfcs/0010-owner-app-fields-and-schema.md)
Amendment 5 `:schema` read of a vertex named `temp` with **both** an intent document and a
one-field descriptor table (`kp`, access `rw`, **empty** descriptor bytes):

```
POINT (PL=1) {
  NAME "temp"
  SETTINGS (PL=1) { }               ; synthesized protocol part, EMPTY
  NAME "doc"  VALUE <document>      ; after the protocol part, before the owner part
  NAME "app"                        ; owner part, unchanged by the amendment
  SETTINGS (PL=1) {
    NAME "kp" SETTINGS (PL=1) {
      NAME "access" VALUE "rw"      ; runtime-projected
    }                               ; no owner descriptor bytes: the document describes kp
  }
}
```

The document is:

```yaml
contract: "https://vendor.example/contracts/thermo/1.0"
help: "Room temperature controller"
fields:
  kp:
    type: "f32"
    help: "Proportional gain"
```

Amendment 5's §B.1 lets a field the document describes under `fields` carry empty
descriptor bytes; its record then holds only the projected `access`. The owner part is
byte-identical to what the same table serves without a document.

The behaviour is bound by C++ `core/tests/intent_document_test.cpp`
(`test_conformance_vectors`, byte-exact against `read_schema`).

```
0740d4000200040074656d700b40000002000300646f6301009800636f6e74726163743a202268747470733a2f2f76656e646f722e6578616d706c652f636f6e7472616374732f746865726d6f2f312e30220a68656c703a2022526f6f6d2074656d706572617475726520636f6e74726f6c6c6572220a6669656c64733a0a20206b703a0a20202020747970653a2022663332220a2020202068656c703a202250726f706f7274696f6e616c206761696e220a020003006170700b401a00020002006b700b40100002000600616363657373010002007277
```
