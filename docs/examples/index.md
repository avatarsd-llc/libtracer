# Examples

Worked, **compile-tested** examples of the C++ reference implementation. Every example
on these pages is a real source file under [`core/examples/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/examples)
that CI **builds and runs as a smoke test** on every change — the code shown is included
verbatim from that file, so it cannot drift from what actually compiles.

| Example | Layer | What it shows |
| ------- | ----- | ------------- |
| [In-process pub/sub](in-process-pubsub.md) | L4 graph | `read`/`write`/`await`, three delivery styles, zero-copy fan-out |
| [Pub/sub fan-out & dispatch cost](pubsub-fanout.md) | L4 graph | per-delivery latency as fan-out scales 1 → 8 → 64; `:schema` discovery |
| [Wire codec round-trip](wire-roundtrip.md) | L2/L3 codec | `encode` / `decode`, the CRC trailer, and zero-copy borrowed payloads |
| [Wire codec deep-dive & throughput](wire-codec.md) | L2/L3 codec | frame anatomy + encode/decode/round-trip throughput |
| [Rope scatter-gather](rope-scatter.md) | L1 views | compose a multi-link `rope_t`; `to_iovec` (zero copy) vs `flatten` (one copy) |
| [Two nodes over a wire — FWD delivery](two-node-fwd.md) | L4 + transport | `fwd_router_t` source-routing across a channel; cross-wire latency |
| [Composition axes](tree-of-ropes.md) | L1 + L4 + transport | why a node is a tree of ropes and not a rope of ropes; rope over two backends; mount = identity, not memory |
| [Register a vertex, and address it](graph-register.md) | L4 graph | `path_t` parses once; the vertex map is keyed on PATH bytes; `PATH_IN_USE`; placeholders |
| [Read and write](graph-read-write.md) | L4 graph | one store per vertex, last-writer-wins; `read` returns a reference to the published value |
| [`await`](graph-await.md) | L4 graph | the readiness plane: single-shot, at its own vertex only, `TIMEOUT` on the deadline |
| [Write-creates](graph-write-creates.md) | L4 graph | a LOCAL data write materializes its target and its missing intermediates |
| [`:children[]`](graph-children.md) | L4 graph | enumerate a parent's members, one level, through the `:` control plane |
| [Retirement](graph-retire.md) | L4 graph | logically absent, not erased: `NOT_FOUND`, live handles, the generation stamp |
| [A HANDLER vertex](graph-handler-vertex.md) | L4 graph | the role decides what a write means: `on_write` executes, `on_read` computes |
| [A STREAM vertex](graph-stream.md) | L4 graph | the bounded history ring and its owner-declared depth (no wire surface) |
| [Subscribe to one vertex](sub-callback.md) | L4 graph | `subscribe(src, callable)`, the `subscription_t` handle, and the inline delivery contract |
| [One edge, a whole subtree](sub-subtree.md) | L4 graph | RFC-0005 vertical bubbling; `own_subs` vs `has_subscribers`; provenance rides in the data |
| [Delivery terminates at the target](sub-terminal-delivery.md) | L4 graph | RFC-0007: `A → B` plus `B → C` does not relay; a cycle cannot loop |
| [The delivery policy is per subscription](sub-durability-latch.md) | L4 graph | RFC-0022 §3.A `delivery_policy_t`; `durability_request` latches the last value on join |
| [Unsubscribe & the release hook](sub-unsubscribe.md) | L4 graph | `unsubscribe(sub, release)`; when the `{fn, ctx}` pair is safe to free |
| [Unsubscribing from inside a delivery](sub-unsubscribe-from-dispatch.md) | L4 graph | ADR-0080's deferred grace point; `reclaim_local` vs `reclaim_strict` |
| [Retire drops a producer's subscriptions](sub-retire.md) | L4 graph | RFC-0009 §B re-virginize; what survives a retire, and what liveness does not do yet |
| [The TLV header and the `opt` byte](wire-tlv-header.md) | L2/L3 codec | four header bytes, the reserved `opt` bits, and who decides the length width |
| [Structured or opaque — one bit decides](wire-structured-vs-opaque.md) | L2/L3 codec | `opt.PL` over the same body bytes: two children, or one payload |
| [A `PATH` body is packed segment records](wire-packed-path.md) | L2/L3 codec | RFC-0018: one spelling per address, and the body IS the vertex-map key |
| [The escape record](wire-path-escape.md) | L2/L3 codec | RFC-0018 §5.4: stepped over by an unknowing hop, refused in canonical context |
| [The trailer: CRC and timestamp](wire-trailer.md) | L2/L3 codec | opt-in integrity at the end; `FRAME_CRC_FAIL`; the loud `opt.ts` refusal |
| [What `decode` refuses](wire-decode-refusals.md) | L2/L3 codec | the four verdicts; RFC-0006 nesting bounded by the caller's injected source |
| [`decode_into`: a flat arena](wire-arena-decode.md) | L2/L3 codec | a pre-order node array over a stack bump source — zero heap, borrowed spans |
| [`tlv_view_t`: a scattered frame](wire-lazy-view.md) | L1 + L2/L3 | ADR-0053 lazy decode over a rope split mid-header; `verify` and `materialize` |
| [The segment, and its refcount](view-segment-refcount.md) | L1 views | copy == clone; the last drop reclaims; what every other example's first line means |
| [`subview`](view-subview.md) | L1 views | the `{owner, offset, length}` window narrowed by arithmetic, not by copying |
| [`borrow`: the app's own bytes](view-borrow.md) | L0/L1 substrate | ADR-0012 transparent byte router; pointer identity vs `over_bytes`, which copies |
| [The rope](view-rope-compose.md) | L1 views | assembly is chaining; `only()`; the inline link count is a cost knob, not a limit |
| [`subrope` and the iovec egress](view-rope-subrope.md) | L1 views | a sub-range starting mid-link; `walk()`; `try_to_iovec` vs `to_iovec` |
| [A bounded backend](view-pool-backend.md) | L0/L1 substrate | a caller-owned slab; exhaustion by value; `NO_MEMORY` is transient |
| [A `DEVICE` link](view-device-rope.md) | L0/L1 substrate | a heterogeneous rope; `NOT_HOST` is permanent; `mem::transfer` declines |
| [A shared seam needs a thread-safe backend](view-sync-pool.md) | L0/L1 substrate | ADR-0060 §2 `sync_pool_t`; the `kSpinWaitSafe` run-time skip |
| [The failable block seam](mem-block-source.md) | L0 substrate | nothrow `try_alloc`, sized `release`, `nullptr` on exhaustion; writing one |
| [Two L0 seams, and the question that picks](mem-source-vs-backend.md) | L0 substrate | refcounted `segment_t` vs single-owner block — the owner count decides |
| [A bump source](mem-bump-source.md) | L0 substrate | a cursor over a caller buffer; `release` is a no-op, `reset()` is not |
| [The upstream decides what the buffer means](mem-bump-upstream.md) | L0 substrate | `heap_source()` makes it a fast path; `null_source()` makes it the bound |
| [A long-lived seam has to recycle](mem-pool-source.md) | L0 substrate | ADR-0067 `pool_source_t`; `used()` is a high-water mark, not a total |
| [Classes do not share](mem-size-classes.md) | L0 substrate | exact `(bytes, align)` classes; `classes_used()` / `overflowed()` size the span |
| [A container that fails by value](mem-block-array.md) | L0 substrate | `block_array_t`: growth returns `false`; `push_slot`; the two static asserts |
| [The `std::pmr` adapter](mem-source-resource.md) | L0 substrate | ADR-0079 `source_resource_t` — placement and bounding, never failability |
| [Open by default, and the first ACE is the lock](acl-open-by-default.md) | L4 auth / ACL | enforcement is opt-in twice; presence closes, not a DENY; the trusted local context |
| [The caller context is not the subject](acl-subject-resolver.md) | L4 auth / ACL | ADR-0018 `subject_resolver_fn_t`; the ERROR arm is a deny; the `{fn, ctx}` shape |
| [`access_mask` is a bitfield](acl-right-bits.md) | L4 auth / ACL | six gates, six single-bit grants; `WRITE_ACL` is precisely admin |
| [`EVERYONE@` is reserved both ways](acl-everyone-reserved.md) | L4 auth / ACL | the wildcard ACE, and why a resolved subject may never spell it (#908) |
| [Effective ACL = own + inherited](acl-inherit.md) | L4 auth / ACL | `kAceInherit`; a closed parent over an open child |
| [`expires_ns` is checked against your clock](acl-expiry.md) | L4 auth / ACL | ADR-0050: one merge, many verdicts; an expired ACE still closes |
| [Two evaluators, and DENY](acl-policy-profiles.md) | L4 auth / ACL | `allow_only_policy_t` vs `full_acl_policy_t`; why parse refuses a DENY |
| [An ACL is a security document](acl-parse-strict.md) | L4 auth / ACL | `parse_acl` refuses what `encode_acl` never emits; leniency widens grants |

The toctree below is the order of record; this table adds the layer and the summary.
Each example's layer column names the module that owns the types it uses — the
[C++ API reference](../modules/index.md) is where those declarations are rendered
from the headers.

Several examples print a `RESULT …` line with **latency and throughput** numbers. Those are
informational (measured on whatever build ran — CI builds the examples in debug), so CI never
flakes on timing; the canonical release-build figures live on the
[performance page](../performance.md).

:::{admonition} Build and run the examples
:class: tip

The examples build by default with the core (`LIBTRACER_BUILD_EXAMPLES`, on when
libtracer is the top-level project):

```console
$ cmake -S core -B build -DBUILD_TESTING=ON
$ cmake --build build
$ ./build/examples/in_process_pubsub
$ ./build/examples/pubsub_fanout
$ ./build/examples/wire_roundtrip
$ ./build/examples/wire_codec
$ ./build/examples/rope_scatter
$ ./build/examples/two_node_fwd
$ ./build/examples/tree_of_ropes
```

Or run them the way CI does — as ctest smoke tests that self-check and fail on any
mismatch: `ctest --test-dir build -R example_`.

Two targets need the FWD routing plane and exist only when
`LIBTRACER_NET_PLANE` is on: `two_node_fwd` and `tree_of_ropes` are declared inside
`if(LIBTRACER_NET_PLANE)` blocks (`core/examples/CMakeLists.txt:59,74`), and so are
their test registrations (`core/examples/CMakeLists.txt:87-96`). The option defaults to
`ON` (`core/CMakeLists.txt:63-65`), so the recipe above builds every example. Configured with
`-DLIBTRACER_NET_PLANE=OFF`, those two binaries are never produced and
`ctest --test-dir build -R example_` runs **two fewer** — a pass with two examples
absent rather than skipped, which no ctest output distinguishes from a clean full run.

Two targets are conditional at **run** time rather than build time, which is a different hazard
with the same ending. [`sub_unsubscribe_from_dispatch`](sub-unsubscribe-from-dispatch) demonstrates
unsubscribing from inside a delivery — a shape `reclaim_strict_t` forbids — and
[`view_sync_pool`](view-sync-pool) binds a spin-waiting critical section, which a target that sets
`tr::mem::kSpinWaitSafe = false` may not instantiate at all. Both are always built; under a
binding an example does not apply to, it prints `skipped:` and exits `0`, so `ctest` records a
**pass for an example that demonstrated nothing**. Both knobs are bound as plain C++ rather than CMake
options, so neither CMake nor ctest can label that case; each binary announces the bound value on
its first line, and that line is the only place the distinction is visible.

The eight `graph_*` targets are pure in-process L4 and are built unconditionally; run them
together with `ctest --test-dir build -R example_graph_`. The eight `wire_*`, eight `view_*`,
eight `mem_*` and eight `acl_*` targets are likewise unconditional — pure L0/L1/L2/L3/L4, no net
plane, no sockets — and run with `ctest --test-dir build -R example_wire_`, `-R example_view_`,
`-R example_mem_` and `-R example_acl_`. Those per-domain `ctest -R`
spellings are the maintained way to run a group; the explicit `./build/examples/…` list above
predates them and is deliberately left as the original seven rather than grown to every target.

Two groups have **no** conditional target of either kind — no `if(LIBTRACER_NET_PLANE)` guard and
no `if constexpr` run-time skip — so a green `ctest -R` over either is a pass for eight examples
that all demonstrated something, which is exactly the property the two paragraphs above say the
other groups cannot claim for themselves:

- `mem_*`, because L0 has no knob that can forbid one of them at all; and
- `acl_*`, which does have a knob in reach and handles it differently from a skip.
  `acl_policy_profiles` covers a surface the target's `acl_policy_t` binding selects
  ([ADR-0068](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0068-build-configuration-is-plain-cpp-config-header.md)),
  and rather than following the binding and skipping the arm it does not have, it names **both**
  policies explicitly as template arguments — both adapters are always compiled — so both arms
  run in every build and the bound choice is only *printed*. Where an example's subject is a
  build-configuration seam, naming the seam's arms beats following the binding; a skip should be
  the last resort, not the first reflex.
:::

```{toctree}
:caption: Worked examples
:hidden:
:maxdepth: 1

In-process pub/sub <in-process-pubsub>
Pub/sub fan-out & dispatch cost <pubsub-fanout>
Wire codec round-trip <wire-roundtrip>
Wire codec deep-dive & throughput <wire-codec>
Rope scatter-gather <rope-scatter>
Two nodes over a wire — FWD delivery <two-node-fwd>
Composition axes <tree-of-ropes>
Register a vertex, and address it <graph-register>
Read and write <graph-read-write>
await <graph-await>
Write-creates <graph-write-creates>
:children[] <graph-children>
Retirement <graph-retire>
A HANDLER vertex <graph-handler-vertex>
A STREAM vertex <graph-stream>
Subscribe to one vertex <sub-callback>
One edge, a whole subtree <sub-subtree>
Delivery terminates at the target <sub-terminal-delivery>
The delivery policy is per subscription <sub-durability-latch>
Unsubscribe & the release hook <sub-unsubscribe>
Unsubscribing from inside a delivery <sub-unsubscribe-from-dispatch>
Retire drops a producer's subscriptions <sub-retire>
The TLV header and the opt byte <wire-tlv-header>
Structured or opaque — one bit decides <wire-structured-vs-opaque>
A PATH body is packed segment records <wire-packed-path>
The escape record <wire-path-escape>
The trailer: CRC and timestamp <wire-trailer>
What decode refuses <wire-decode-refusals>
decode_into: a flat arena <wire-arena-decode>
tlv_view_t: a scattered frame <wire-lazy-view>
The segment, and its refcount <view-segment-refcount>
subview <view-subview>
borrow: the app's own bytes <view-borrow>
The rope <view-rope-compose>
subrope and the iovec egress <view-rope-subrope>
A bounded backend <view-pool-backend>
A DEVICE link <view-device-rope>
A shared seam needs a thread-safe backend <view-sync-pool>
The failable block seam <mem-block-source>
Two L0 seams, and the question that picks <mem-source-vs-backend>
A bump source <mem-bump-source>
The upstream decides what the buffer means <mem-bump-upstream>
A long-lived seam has to recycle <mem-pool-source>
Classes do not share <mem-size-classes>
A container that fails by value <mem-block-array>
The std::pmr adapter <mem-source-resource>
Open by default, and the first ACE is the lock <acl-open-by-default>
The caller context is not the subject <acl-subject-resolver>
access_mask is a bitfield <acl-right-bits>
EVERYONE@ is reserved both ways <acl-everyone-reserved>
Effective ACL = own + inherited <acl-inherit>
expires_ns is checked against your clock <acl-expiry>
Two evaluators, and DENY <acl-policy-profiles>
An ACL is a security document <acl-parse-strict>
```
