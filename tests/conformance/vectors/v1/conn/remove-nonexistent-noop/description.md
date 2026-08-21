# conn/remove-nonexistent-noop

RFC-0014 §2's `remove-nonexistent-noop`: a `NAME` write to `/net/<module>/conn` naming a
connection that does **not** resolve — never created, or already retired — is a **no-op
success**, not an error.

```
NAME{ "never" }
```

```
020005006e65766572
```

| Bytes | Meaning |
| --- | --- |
| `02 00 05 00` | NAME, `opt = 0`, payload length 5 |
| `6e65766572` | `"never"` — a name this module has no connection under |

## The vector triple, and why it is three vectors

`conn/remove-via-name`, this case, and `conn/remove-reserved-rejected` are the **same TLV
type with three different payload strings** and three different outcomes: retire, no-op
success, and refusal. That is deliberate, and it is the whole shape of the remove door — the
decision is made by what the name resolves to **at the endpoint**, not by anything a codec can
see. Three separate directories, because an implementation can get any one of them wrong while
the other two still pass.

Why success rather than `tr::path::not_found`: a teardown whose reply is lost must be safe to
retry. Answering an error here would make a retried remove indistinguishable from a genuine
failure and push idempotence onto every orchestrator. `graph_t::retire()`'s own idempotence
covers only the already-resolved-but-unregistered handle, so this leg belongs to the endpoint,
which is why it is specified here at all.

## What this vector gates, and where the behaviour is bound

Codec only, per [HARNESS.md](../../../../HARNESS.md) — the bytes are inert; the claim is entirely
about the answer. Bound in `core/tests/transport_vertex_test.cpp`,
`test_conformance_vectors`: the write succeeds, and — the non-vacuous half — the live
connection created alongside it is **still there** afterwards, so "no-op" means no operation
rather than an operation that hit the wrong target.
