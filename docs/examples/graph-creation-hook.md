# Creation — refused by default, opted in per parent (L4 graph)

A data write to an address that does not resolve answers `NOT_FOUND` and creates nothing,
locally and from a peer alike
([RFC-0030](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0030-host-api-walks-the-graph-reply-is-a-remote-write.md)
§7.1). An application that wants a write to create, below one parent, installs a **creation
hook** there with `graph_t::set_creation_hook` (§7.2). The hook is shown the missing child's
key, the writer's subject and the payload, and it registers the child itself, typed as the
application decides, or refuses.

## What to notice

- **Refusal is the default everywhere.** The first write fails with `NOT_FOUND` and leaves no
  intermediate level behind. A remote `FWD{WRITE}` gets the same answer; that arm is pinned in
  `core/tests/op_resolve_test.cpp`.
- **The hook is compiled out by default.** `config_t::kCreationHooks` is `false` on every
  profile, so a lean build has no hook slot: `set_creation_hook` answers `SCHEMA_NOT_FOUND`
  and every miss stays refused. The example checks that and stops there in such a build. Set
  `static constexpr bool kCreationHooks = true;` in your `libtracer/config_override.hpp` to
  opt in; on ESP-IDF, set `CONFIG_LIBTRACER_CREATION_HOOKS=y` instead.
- **The parent's `CREATE` right is checked before the hook runs.** A writer the parent's ACL
  denies gets `PermissionDenied`, and the hook never sees the write.
- **One level per hook.** The hook on `/zone` creates `/zone/a`, which carries no hook of its
  own, so the next level, `/zone/a/b`, is refused. `mkdir -p` holds only where every level
  opted in.
- **The `:` control plane never creates.** A field write to a nonexistent vertex is
  `NOT_FOUND`, because there is no vertex to control.
- **Appearance is the first write.** A child the hook creates appears as the write that
  caused it, so a subtree subscriber above it sees the child arrive without any dedicated
  event type ([reference 02](../reference/02-graph-model.md) §Observing structural change).

## Source

```{literalinclude} /core/examples/graph_creation_hook.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[graph model reference](../reference/02-graph-model.md) §Vertex lifecycle ·
[retirement](graph-retire.md) (the other half of the lifecycle).
