# Vertices

## In one paragraph

A vertex is a named, addressable position in the graph: something can be written to it, read
from it, awaited on and subscribed to. Its owner registers it with one of three roles. A stored
vertex keeps the last value written, a STREAM vertex keeps a bounded history in its own ring, and
a handler vertex keeps nothing and hands each read and write to the owner's code, which is how a
mirror, a computed value, a proxy or a live register is built. Beside its value a vertex has `:`
fields, its control surface: `:subscribers`, `:acl`, `:settings` and `:schema`. The wire never
says which role a vertex has; a peer sees only what reads and writes do. Leaving the graph is
retirement, which makes the path read exactly as if it had never existed.

## The picture

```{mermaid}
:caption: "Legend: purple = application, green = vertex, orange = ring."
flowchart LR
    accTitle: Three vertex roles
    accDescr: The owner application registers a vertex with a role. A stored vertex holds one last value. A STREAM vertex holds a bounded ring of entries. A handler vertex holds nothing and calls the owner's on_read and on_write code.
    O([owner]):::app -->|"register, role"| K{"role?"}
    K -->|"STORED_VALUE"| S("stored: one last value"):::vertex
    K -->|"STREAM"| T("stream"):::vertex
    T --- R[("bounded ring")]:::ring
    K -->|"HANDLER"| H("handler: holds nothing"):::vertex
    H -->|"on_read, on_write"| O
```

## When you need it

Whenever you lay out a device or a service as paths. Pick the role by what a reader should get:
the latest value (stored), every recent value (STREAM), or whatever your code computes at that
moment (handler). Register the structure your node owns at start-up; let peers create children
only through a creator endpoint or a creation hook you install.

## How it works

**Registration.** `graph_t::register_vertex(path, role, handlers)` adds a vertex and returns a
handle; `try_register_vertex` reports failure as a value instead. A vertex's appearance is its
first write, so subtree subscribers see it arrive. A write to a path nobody registered answers
`tr::path::not_found` and creates nothing, from the host API and from the wire alike.

**Roles.**

| Role | `read` returns | `write` does | State |
| --- | --- | --- | --- |
| `STORED_VALUE` | the latest value | replaces it | one value |
| `STREAM` | the latest; `history` returns the ring | appends to the ring | N values, bounded in bytes |
| `HANDLER` | what `on_read` composes | calls `on_write` | none in the graph |

A branch, a vertex with registered children, reads as the folded tree of its subtree.

**Owner declarations.** `graph_t::set_policy` declares what only the owner knows: ring depth,
whether a stored value copies or shares the received bytes, delivery mode, app fields. A
declaration reaches exactly the vertex it names. Nothing is inherited from an ancestor, and none
of it has a wire surface.

**Creation from outside.** A peer creates by writing a `SPEC` record to a creator endpoint, a
vertex that exists, and the owner registers the child. An owner may instead install a creation
hook on a parent, so that a write to a missing child calls the owner's code to register it; the
hook is a compile-time option, `config_t::kCreationHooks`, off on every profile by default.

**Retirement.** `graph_t::retire` re-virginizes a subtree in place. Each vertex unwinds its
subscription edges and reads `not_found` again, and its generation moves on, so a stale PAIR
([addresses](../wire/address.md)) no longer reaches it. The vertex object is not freed, so
outstanding handles stay valid. A detached handler block is parked until the embedder calls
`graph_t::collect()` at a point it knows no reader holds one. A later registration may revive the
path; it inherits nothing from the retired vertex, its ACL included.

## Worked example

The `SPEC` record a peer appends to a creator endpoint's `:children[]` to ask for one stored
child named `temp`. It is a run of `NAME` pairs: a key, then its value.

```{hex-walk} spec/create-child
:caption: The minimal in-band creation request.
str: key
str: the child's role
str: key
str: the child's name
```

## Cost

### Wire bytes

None to register or retire locally. A remote creation is one `FWD{WRITE}` carrying a `SPEC`
like the 44-byte one above. A vertex's path is its key, so the bytes that address it on the wire
are the `1 + len` per segment of [addresses](../wire/address.md), whatever its value's size: a
1 KiB or a 16 KiB value at `/sensor/temp` uses the same 16-byte `PATH`.

### Allocations

One vertex per registration, drawn from the graph's injected block source; a cold extension block
only when the vertex is a STREAM, carries handlers, holds app fields or an `:acl`, or has a policy
declared. Retirement frees nothing on the spot: handles stay valid, and parked handler blocks wait
for `collect()`.

### Latency

Not benched on this page. Lookup by a canonical key is a byte compare of the frame's own `PATH`
body; a PAIR is an index and a generation compare. `vertex_test` and `retire_test` pin behaviour,
not time.

### Flash and RAM per target

| Target | RAM per vertex | Note |
| --- | --- | --- |
| NARROW | 64 B (`kMaxVertexBytes32`) | plus the extension block where one is needed |
| MID | 64 B | same |
| WIDE | 88 B (`kMaxVertexBytes64`) | same |

Both sizes are asserted in `vertex.hpp` on every build and pinned with no headroom, so a member
added to `vertex_t` fails the build. Flash is not split out per module; the gated Cortex-M0 node
includes the graph.

## What the library does not do

- It does not create a vertex on a write to a missing path unless the owner opted a parent in.
- It does not announce a retirement. A subscriber learns it by re-reading.
- It does not free a retired vertex's handler block on its own; `collect()` is the embedder's call.
- It does not expose a vertex's role, ring depth or share hint on the wire.

## Pitfalls

- **Symptom:** a bus node's memory grows with peer churn. **Cause:** link teardown parks a handler
  block per peer vertex and nothing calls `collect()`. **Fix:** call `graph_t::collect()` from a
  point where no reader holds a seam, and watch `parked_seam_count()`.
- **Symptom:** a revived path refuses a client that the old one admitted. **Cause:** the revived
  vertex takes its live ancestor's ACL, never the retired one's. **Fix:** grant again after
  revival.
- **Symptom:** a peer's writes to new child paths answer `not_found`. **Cause:** no creation hook
  or creator endpoint covers that parent. **Fix:** install one, or register the children yourself.

## Where the rules live

**Normative source:** [v1 §3](../../spec/v1.md#3-wire-format), which incorporates the creation
record in [reference 05 §0x0E SPEC](../../reference/05-protocol-tlvs.md#0x0e--spec) and the data-API
semantics of [reference 05](../../reference/05-protocol-tlvs.md#type-code-allocation-summary).

**Exercised by:** `create-child`, `spec-name-in-use`, `vertex_test`, `vertex_size_test`,
`retire_test`.

Next concept: [Propagate and FOLD](propagate.md). Go deeper:
[reference 11 §Vertex roles](../../reference/11-vertex-roles-and-aggregation.md#vertex-roles-kinds-of-things-a-path-resolves-to),
then [reference 02 §Vertex lifecycle](../../reference/02-graph-model.md#vertex-lifecycle-registration-creation-retirement-revival),
then the [graph module](../../modules/graph.md).
