# Composition axes (L1 + L4 + transport)

A libtracer node is a *tree of ropes*, not a *rope of ropes*. The tempting fused
model — a single memory chain that **is** the graph, folds into the TLV tree, and
grows every time a transport is attached — collapses three things the reference
implementation keeps **orthogonal** ([CONTEXT.md](https://github.com/avatarsd-llc/libtracer/blob/main/CONTEXT.md)
§"Two compositions", §"Graph (address) composition"), and that orthogonality is
the zero-copy story. The three axes below are exercised independently, and the
example asserts they never merge.

## The three axes

- **Memory composition (L1, `tr::view`)** — a [`rope_t`](../modules/views.md) is an
  ordered chain of `view_t` windows over refcounted segments (ADR-0053). Its links
  may live in *different* backends at once: here link 0 is heap-allocated and link 1
  **borrows** caller-owned memory, in one chain, with zero byte copies. A rope is
  scoped to *one payload* — there is no process-wide rope.
- **Address composition (L4, `tr::graph`)** — the vertex tree is its own Composite
  of `vertex_t` linked by parent/children (ADR-0057). Each leaf *holds one rope* in
  its value slot; storing the two-link rope keeps it two-link (the tree never
  flattens the memory chain), and a second vertex holds a wholly separate rope.
  **Tree-of-ropes, not rope-of-ropes.**
- **A transport is an identity, not memory** — mounting a transport (ADR-0027) via
  an in-band `/net:children[]` write adds exactly one addressable vertex at
  `/net/<module>/<name>` — here `/net/can/link0`, the module taken from the
  `provide_link` staging key (`transport_vertex_t::provide_link`,
  `core/src/transport_vertex.cpp:431`). The transport's real bytes live *outside*
  the graph, in the FWD router's demux; no per-peer vertex or memory is added
  (ADR-0044). Attaching a bus does not "grow the rope."

## What to notice

- **One rope, two backends** — `link_count() == 2`, with `btag == HEAP` on link 0 and
  `btag == BORROWED` on link 1; `to_iovec()[1].data()` points straight *into* the
  caller's buffer, proving the borrow never copied.
- **The store is zero-copy** — after `write` then `read`, the value is still a
  two-link rope and the borrowed link's backend tag survives: the L4 tree threads the
  L1 rope through untouched.
- **No global rope** — two vertices resolve to two independent ropes; the vertex tree
  is walked by *path* (parent/children), never by rope links.
- **Mount = identity** — a fresh `/net/can/link0` resolves as an address but holds no
  value at all: the read returns `NOT_FOUND`. Only once the link reports state does
  the vertex hold a value, and that value is a **single-link** rope carrying a
  one-byte link-state VALUE TLV (`link_state_value`,
  `core/src/transport_vertex.cpp:90`) — categorically not a chained payload. The live
  transport is found in `router.registry().by_name("net/can/link0")`, outside the
  graph.

The transport half runs over the in-process `loopback_channel_t` so the example is
deterministic and needs no hardware; the same `provide_link` seam accepts a real
`transport_can` bus link on a Linux host with a (v)CAN interface, and the structural
claims asserted here are identical. The transport axis is compiled only with the FWD
net plane enabled (`LIBTRACER_NET_PLANE`).

## Source

```{literalinclude} /core/examples/tree_of_ropes.cpp
:language: cpp
:linenos:
```

See also: [views module](../modules/views.md) ·
[graph model reference](../reference/02-graph-model.md) ·
[transports & connections as vertices (ADR-0027)](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0027-transport-and-connections-are-vertices.md).
