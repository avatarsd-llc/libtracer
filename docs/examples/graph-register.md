# Register a vertex, and address it (L4 graph)

The first thing a node does. A [`path_t`](../modules/path.md) parses its string **once** into
the canonical PATH-TLV payload — the packed segment records — and the vertex map is keyed on
those bytes, never on the string
([reference 02](../reference/02-graph-model.md) §Dispatch keyed on canonical PATH TLV bytes).
`register_vertex` returns a `vertex_handle_t`, and `find` takes the *key*, which is the same
lookup the dispatcher runs.

## What to notice

- **The key is the identity, the string is a courtesy.** The example prints the byte length
  of `/sensor/temp`'s key and resolves the handle through `g.find(temp.key())`. Two spellings
  that name one vertex must canonicalize to byte-identical PATH payload bytes.
- **Two registration doors, one fallible.** `register_vertex` takes a known-good literal and
  hard-aborts on a collision, because a collision on a compile-site literal is a source bug
  ([ADR-0056](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0056-vertex-handle-infallible-register.md)).
  `try_register_vertex` is the runtime-path form and reports `PATH_IN_USE` instead.
- **Registering `/sensor/temp` does not register `/sensor`.** The intermediate is an
  unregistered *structural placeholder* — the addressing scaffolding created it on the way to
  a deeper registration, and `find` does not answer for one ([CONTEXT.md](../../CONTEXT.md)
  §Structural vertex). That is distinct from a *structural vertex*, which is registered and
  merely carries no application datum.
- **There is no null handle.** "No such vertex" is `std::optional<vertex_handle_t>`'s empty
  state, not an invalid handle.

## Source

```{literalinclude} /core/examples/graph_register.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) · [path module](../modules/path.md) ·
[graph model reference](../reference/02-graph-model.md) ·
[addressing reference](../reference/03-addressing.md).
