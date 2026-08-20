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
| [The `dst` is a source route](route-dst-is-source-route.md) | L4 routing | the route rides in the frame; `dst` shrinks and `src` grows by a mount run, byte-exact |
| [Terminus or forward — one test decides](route-terminus-or-forward.md) | L4 routing | the leading `dst` route segment names a child, or this node is the terminus; `ERROR` is addressed |
| [One NAME, one slot](route-child-table.md) | L4 routing | `add_child`'s `[[nodiscard]] bool`; a refusal registers nothing; removal tombstones |
| [A mount run is consumed whole](route-qualified-mount.md) | L4 routing | RFC-0014 strip-K and its grow-K dual; longest prefix, per-slot width |
| [Three nodes, and a forwarder that stores nothing](route-multi-hop.md) | L4 routing | the one-hop rule again; memory bounded by topology, not by traffic |
| [The `src` you accumulated is the way home](route-reply-home.md) | L4 routing | the REPLY retraces per hop; no reply address, no correlation id |
| [A repeating flow buys its route back](route-label-compact.md) | L4 routing | ADR-0035 `advertise` binds `label ↔ route` per link; the measured byte delta |
| [A stale label is dropped and NACK'd](route-label-stale.md) | L4 routing | `clear_link`, `on_stale_label`, and why re-advertising IS the self-heal |
| [One seam, every wire technology](net-transport-seam.md) | transport plane | the three calls a kind implements; the gathered iovec fallback; zero TLV semantics |
| [A kind is a NAME, resolved twice](net-kind-catalog.md) | transport plane | `register_transport_type` + `register_module`; an unregistered kind is refused, not defaulted |
| [DIAL and LISTEN are two constructors](net-dial-and-listen.md) | transport plane, `tcp` | the role is the constructor; `ok()` is came-up, `link_up()` is now |
| [A datagram already has boundaries](net-udp-datagram.md) | transport plane, `udp` | one datagram = one frame, no framing layer; the peer is learned from ingress |
| [A stream has none, so the kind supplies them](net-tcp-stream-framing.md) | transport plane, `tcp` | the `u32-LE` prefix; coalesced writes split, split writes reassembled |
| [No frame crosses until the Upgrade completes](net-ws-upgrade.md) | transport plane, `ws` | the `101` computed from the client's nonce; the tighten-only pre-auth budget |
| [The one BUS kind](net-can-bus-peers.md) | transport plane, `can` | ADR-0044 peers synthesized from traffic; `n<node-id>` is an identity |
| [One listener, many slots](net-multi-peer-listener.md) | transport plane, `tcp`/`ws` | `p<slot>` is a POSITION — resolve per use; the printed skip ctest can SEE |

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

Ten targets need the FWD routing plane and exist only when
`LIBTRACER_NET_PLANE` is on: `two_node_fwd` and `tree_of_ropes` are declared inside
`if(LIBTRACER_NET_PLANE)` blocks (`core/examples/CMakeLists.txt:59,74`), and so are
their test registrations (`core/examples/CMakeLists.txt:87-96`); the eight `route_*` targets sit
inside a third such block at the end of the file. The option defaults to
`ON` (`core/CMakeLists.txt:63-65`), so the recipe above builds every example. Configured with
`-DLIBTRACER_NET_PLANE=OFF`, those ten binaries are never produced. For the
`route_*` group that absence is not a choice: `fwd_router_t`, `route_handle_t` and `op_resolve`
are the net plane, so at `-DLIBTRACER_NET_PLANE=OFF` the types those examples name do not exist
and there is nothing to compile, let alone to skip.

Seven more are absent from that same configuration, and they are guarded **per target** rather
than as a group, because each one's subject is a different option: `net_kind_catalog` needs
`LIBTRACER_NET_PLANE`; `net_udp_datagram` needs `LIBTRACER_TRANSPORT_UDP`; `net_dial_and_listen`,
`net_tcp_stream_framing` and `net_multi_peer_listener` need `LIBTRACER_TRANSPORT_TCP`;
`net_ws_upgrade` needs `LIBTRACER_TRANSPORT_WS`; `net_can_bus_peers` needs
`LIBTRACER_TRANSPORT_CAN`. Folding those into one guard would make a UDP-only build lose the UDP
example, which is exactly the build that wants it. Only `net_transport_seam` is unconditional —
`transport_t` and the loopback channel are the required core.

**The count is the mitigation, and it is exact.** The full default build has **70** examples. The
minimal module set (`-DLIBTRACER_NET_PLANE=OFF` plus all four transports off) runs **53**, so
`ctest --test-dir build -R example_` there is short by **seventeen**: the ten above and the seven
just listed. Both numbers are written down here so that a *further* disappearance is visible
rather than indistinguishable from a clean run — which no ctest output distinguishes on its own.

Three targets are conditional at **run** time rather than build time, which is a different hazard
with the same ending — and two of them still have it while the third does not.
[`sub_unsubscribe_from_dispatch`](sub-unsubscribe-from-dispatch) demonstrates
unsubscribing from inside a delivery — a shape `reclaim_strict_t` forbids — and
[`view_sync_pool`](view-sync-pool) binds a spin-waiting critical section, which a target that sets
`tr::mem::kSpinWaitSafe = false` may not instantiate at all. Both are always built; under a
binding an example does not apply to, it prints `skipped:` and exits `0`, so `ctest` records a
**pass for an example that demonstrated nothing**. Both knobs are bound as plain C++ rather than CMake
options, so neither CMake nor ctest can label that case; each binary announces the bound value on
its first line, and that line is the only place the distinction is visible.

[`net_multi_peer_listener`](net-multi-peer-listener) is the third, and it is the one that fixes
that. Its subject is the ADR-0044 peer-named tier, closed out by `kBusLinks = false` — again a
C++ binding CMake cannot see — so it too has to follow the binding at run time. But instead of
exiting `0` it states the skip and exits **77**, and its `add_test` carries `SKIP_RETURN_CODE 77`,
so ctest reports **`Skipped`** rather than a pass. Verified in a `kBusLinks = false` build:
`example_net_multi_peer_listener (Skipped)`, everything else green. A run-time skip is still the
last resort, but when it is unavoidable this is the shape it should take, and the two older ones
above should be converted to it rather than copied.

The eight `graph_*` targets are pure in-process L4 and are built unconditionally; run them
together with `ctest --test-dir build -R example_graph_`. The eight `wire_*`, eight `view_*`,
eight `mem_*` and eight `acl_*` targets are likewise unconditional — pure L0/L1/L2/L3/L4, no net
plane, no sockets — and run with `ctest --test-dir build -R example_wire_`, `-R example_view_`,
`-R example_mem_` and `-R example_acl_`. The eight `route_*` targets are the one group that is
build-conditional as a whole (`-R example_route_`); within a net-plane build nothing in it is
conditional at run time — no `config_override.hpp` knob is in reach, no socket is opened, no port
is bound and no thread is started, because every link is a recording stub driven synchronously.
The eight `net_*` targets (`-R example_net_`) are the group that DOES open sockets: `udp`, `tcp`
and `ws` bind real loopback ports, exactly as `udp_test`, `tcp_test` and `ws_transport_test`
already do on every CI leg. Every one of them binds port `0` and reads the kernel's answer back
from `local_port()`, so nothing collides with whatever else is running on the machine, and every
wait is a bounded condition-variable wait or a bounded poll rather than a sleep standing in for a
rendezvous. Those per-domain `ctest -R`
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

`route_*` is the third case and worth naming as its own, because it is the one where neither of
those two moves was available. Its subject *is* the net plane, so it cannot name both arms (there
is no second arm — with the plane off there is no routing to demonstrate) and it must not follow
the binding with a run-time skip (the types would not compile, and a skip that did compile would
be the vacuous pass this whole admonition exists to warn about). What is left is honest
build-time absence, and the discipline that goes with it: the group is guarded as a whole rather
than target by target, the exact number missing from a `-DLIBTRACER_NET_PLANE=OFF` run is written
down above, and every `route_*` page says on its own face that its target needs the plane. The
ranking, for whoever takes the remaining domains: **name the arms** if you can, **absent by
build** with the count recorded if you cannot, and **skip at run time with a printed line** only
when the target must exist in a configuration it cannot run in.

`net_*` is the fourth case, and it refines the last rung of that ranking rather than adding a
new one. Seven of its eight are absent by build, guarded per target because their subjects are
seven different options; one of them additionally has a knob CMake cannot see, could not name
both arms, and therefore took the last rung — but took it with `SKIP_RETURN_CODE 77`, so the
skip is a thing ctest *reports* rather than a pass nobody can tell apart. The ranking now reads:
**name the arms** → **absent by build, per target, with the count recorded** → **skip at run
time, exiting 77 so ctest says `Skipped`** → never a bare `return 0`.
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
The dst is a source route <route-dst-is-source-route>
Terminus or forward — one test decides <route-terminus-or-forward>
One NAME, one slot <route-child-table>
A mount run is consumed whole <route-qualified-mount>
Three nodes, and a forwarder that stores nothing <route-multi-hop>
The src you accumulated is the way home <route-reply-home>
A repeating flow buys its route back <route-label-compact>
A stale label is dropped and NACK'd <route-label-stale>
One seam, every wire technology <net-transport-seam>
A kind is a NAME, resolved twice <net-kind-catalog>
DIAL and LISTEN are two constructors <net-dial-and-listen>
A datagram already has boundaries <net-udp-datagram>
A stream has none, so the kind supplies them <net-tcp-stream-framing>
No frame crosses until the Upgrade completes <net-ws-upgrade>
The one BUS kind <net-can-bus-peers>
One listener, many slots <net-multi-peer-listener>
```
