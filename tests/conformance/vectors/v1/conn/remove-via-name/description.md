# conn/remove-via-name

RFC-0014 §2's `remove-via-NAME`: the payload of a `write /net/<module>/conn` that retires the
connection vertex `/net/<module>/up` and un-routes its link.

```
NAME{ "up" }
```

```
020002007570
```

| Bytes | Meaning |
| --- | --- |
| `02 00 02 00` | NAME, `opt = 0`, payload length 2 |
| `7570` | `"up"` — the connection's leaf NAME |

## Six bytes, and no new wire primitive

Removal needed a wire path (`graph_t::retire()`'s own contract says *"there is no wire
operation that reaches here — a peer goes through the creator endpoint"*). RFC-0014 gives it
one without adding a verb: **create and remove collapse onto the same control vertex and are
distinguished by the written TLV's TYPE.** `SPEC` creates (`conn/create-via-spec`), `NAME`
removes, and everything else is refused (`conn/bad-payload-type`).

Three consequences the byte string makes concrete:

- The operation is an ordinary `WRITE` (`0x02` in the access mask), **not** `DELETE` (`0x10`).
  RFC-0009 §A.2 records `DELETE` as reserved-and-unused for protocol v1 and §Discussion 1
  explicitly rejected a `DELETE`-gated remote retire; RFC-0014 §5 follows it.
- The path carries the module, the payload carries the name. Nothing here says *which*
  transport — the endpoint's own path does.
- These same six bytes written to a vertex that is **not** a creator endpoint are an ordinary
  value write, not a control (RFC-0014 §2, last bullet). The TYPE selects the operation only
  at the endpoint.

## What this vector gates, and where the behaviour is bound

Codec only, per [HARNESS.md](../../../../HARNESS.md). The behaviour — the vertex is retired, the
link leaves the router's demux table, and a repeat is a no-op success rather than an error —
is bound in `core/tests/transport_vertex_test.cpp`, `test_conformance_vectors`, with
`test_conn_endpoint_name_removes` covering the same door from the builder side.
