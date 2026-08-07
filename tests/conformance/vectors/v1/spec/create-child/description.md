# spec/create-child

The minimal in-band vertex-creation SPEC (`type=0x0e`, `PL=1`) — the payload of a
`write /dev:children[] += SPEC{…}` (ADR-0017, ADR-0020, `docs/reference/05` §0x0E).

```
SPEC{ NAME "type" NAME "stored_value",
      NAME "name" NAME "temp" }
```

```
0e402800020004007479706502000c0073746f7265645f76616c7565020004006e616d650200040074656d70
```

| Bytes | Meaning |
| --- | --- |
| `0e 40 28 00` | SPEC, `opt.PL=1`, body length 40 |
| `02 00 04 00 74797065` | NAME `"type"` — the key |
| `02 00 0c 00 73746f7265645f76616c7565` | NAME `"stored_value"` — the catalog selector |
| `02 00 04 00 6e616d65` | NAME `"name"` — the key |
| `02 00 04 00 74656d70` | NAME `"temp"` — the new child's path component |

## Why the value TYPE is the point of this vector

The body is a run of **positional pairs**: a `NAME` key followed by its value child. For
`type` and `name` that value child is itself a **`NAME` (`0x02`), never a `VALUE`
(`0x01`)**. The terminus walk matches on the pair's value type, so a `VALUE`-typed value
is not a lenient spelling of the same thing — it is skipped, the catalog selector stays
empty, and the whole create is refused with `INVALID_PATH`. A core that wraps the two
field values in a `VALUE` round-trips its own bytes perfectly and is still rejected by
every conforming terminus, which is exactly the drift this vector exists to catch.

The pairing is also **pair-consuming** (#927): a key the receiver does not know is
skipped together with its value, so a value can never be re-read as the next key.

## What this vector gates, and where the behaviour is bound

Per [HARNESS.md](../../../HARNESS.md), a vector gates the codec only. The behaviour it
carries — that these exact bytes create `/dev/temp`, and that the `VALUE`-typed spelling
does not — is bound in `core/tests/children_test.cpp`
(`test_conformance_vectors`, `test_value_typed_spec_is_refused`).
