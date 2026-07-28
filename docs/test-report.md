# Test report

```{note}
Auto-generated from a live `ctest` run by `bench/gen_test_report.py` (regenerated in CI before every Pages deploy). Not hand-maintained.
```

## Summary

| suites | passing | conformance vectors | wall time | verdict |
| --- | --- | --- | --- | --- |
| 64 | 64/64 | 39 | 7.57s | ✅ all green |

## By subsystem

| category | suites | passing |
| --- | --- | --- |
| Codec (L2/L3) | 11 | ✅ 11/11 |
| Substrate (L0/L1) | 7 | ✅ 7/7 |
| Graph (L4) | 19 | ✅ 19/19 |
| Net (FWD plane) | 12 | ✅ 12/12 |
| Transport | 8 | ✅ 8/8 |
| Examples | 7 | ✅ 7/7 |

## Suites

Each row's description is the test file's own `@brief`, read from the source at generation time, and `source` links to that file. Neither is transcribed into this generator, so neither can drift from the test.

### Codec (L2/L3)

| suite | result | time | covers | source |
| --- | --- | --- | --- | --- |
| `byteorder` | ✅ pass | 0.00s | Unit tests for the little-endian (de)serialization primitive (byteorder.hpp). | [`byteorder_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/byteorder_test.cpp) |
| `can_frames` | ✅ pass | 0.00s | transport_can PURE framing-layer test (#55). | [`can_frames_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/can_frames_test.cpp) |
| `conformance` | ✅ pass | 0.00s | Conformance harness for the seed vectors under tests/conformance/vectors/v1/. | [`conformance_runner.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/conformance_runner.cpp) |
| `frame` | ✅ pass | 0.00s | Frame-codec nesting test. | [`frame_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/frame_test.cpp) |
| `key_view` | ✅ pass | 0.00s | Unit tests for tr::wire::key_view_t (key_view.hpp) — the canonical-key NAME navigation the L4 graph dispatch and ACL-inheritance walks funnel through. | [`key_view_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/key_view_test.cpp) |
| `length_prefix_framer` | ✅ pass | 0.00s | length_prefix_framer unit test — drives the u32-length-prefix reassembly state machine directly (no QUIC connection), the whole point of extracting it from transport_quic / transport_webtransport (finding #4): prefix/body split across chunks, multiple frames per chunk, empty records, oversize => malformed, backpressure drain + resync, and reset. | [`length_prefix_framer_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/length_prefix_framer_test.cpp) |
| `rope_decode` | ✅ pass | 0.00s | Differential test for the rope-aware grammar (ADR-0048 §1): wire::validate_rope over a scatter-gather rope MUST reach the exact same verdict as wire::decode over the equivalent flat bytes — for every adversarial split, including splits that fall mid-header, mid- trailer, and mid-payload. | [`rope_decode_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/rope_decode_test.cpp) |
| `rope_decode_fuzz` | ✅ pass | 0.06s | Rope-source differential FUZZER for the wire grammar (ADR-0048 §consequences: "the differential fuzzers gain a rope-source mode — same bytes split at adversarial link boundaries MUST decode identically to the contiguous case, including mid-header splits"). | [`rope_decode_fuzz_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/rope_decode_fuzz_test.cpp) |
| `tlv_arena` | ✅ pass | 0.00s | Terminus arena decoder test (ADR-0041). | [`tlv_arena_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/tlv_arena_test.cpp) |
| `tlv_view` | ✅ pass | 0.00s | Tests for the lazy rope-backed decode view (ADR-0053): tlv_view_t over a scatter-gather rope must (a) agree with the eager decoder node-for-node when fully walked, (b) be actually LAZY — siblings of a corrupt TLV deliver, the corrupt one fails only its own verify(), bytes are shared not copied — and (c) keep its links' segments alive past the source rope (owning tier). | [`tlv_view_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/tlv_view_test.cpp) |
| `ws` | ✅ pass | 0.00s | transport_ws PROTOCOL-layer test (#54). | [`ws_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/ws_test.cpp) |

### Substrate (L0/L1)

| suite | result | time | covers | source |
| --- | --- | --- | --- | --- |
| `mem_pool_source` | ✅ pass | 0.00s | Unit tests for the bounded RECYCLING block source (`tr::mem::pool_source_t`, #597). | [`mem_pool_source_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/mem_pool_source_test.cpp) |
| `mem_source` | ✅ pass | 0.00s | Unit tests for the nothrow failable-block seam (mem_source.hpp, #551). | [`mem_source_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/mem_source_test.cpp) |
| `mem_sync_pool` | ✅ pass | 0.17s | ADR-0060 §2 — the thread-safe pool `value_backend_`: `mem::sync_pool_t` must serialize concurrent `alloc` (writer threads) with cross-thread `destroy` (a `segment` reclaimed on the reader/subscriber thread that drops the last ref). | [`mem_sync_pool_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/mem_sync_pool_test.cpp) |
| `path` | ✅ pass | 0.00s | Path parsing / validation (docs/reference/03-addressing.md). | [`path_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/path_test.cpp) |
| `rope` | ✅ pass | 0.00s | rope_t small-buffer storage + the value-consumption accessors (ADR-0053 §6). | [`rope_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/rope_test.cpp) |
| `substrate` | ✅ pass | 0.00s | L0/L1 substrate tests: refcount lifetime (the canonical intrusive_ptr orderings), zero- copy subview/concat, rope serialization equivalence (the docs/reference/02 proof obligation), the view->TLV cast claim with the lifetime gap M1 left open now closed, and the bounded pool backend. | [`substrate_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/substrate_test.cpp) |
| `substrate_no_atomic` | ✅ pass | 0.00s | L0/L1 substrate tests: refcount lifetime (the canonical intrusive_ptr orderings), zero- copy subview/concat, rope serialization equivalence (the docs/reference/02 proof obligation), the view->TLV cast claim with the lifetime gap M1 left open now closed, and the bounded pool backend. | [`substrate_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/substrate_test.cpp) |

### Graph (L4)

| suite | result | time | covers | source |
| --- | --- | --- | --- | --- |
| `acl` | ✅ pass | 0.00s | Per-endpoint :acl — storage (#81-A) + core-subset enforcement (#81, ADR-0018/ 0020/0026). | [`acl_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/acl_test.cpp) |
| `app_fields` | ✅ pass | 0.03s | RFC-0010 owner app fields: the field descriptor table, `:settings.app.*` read/write gating, the two-part `:schema`, container reads, the owner apply seam, and the announce-write convention (§C — a field write never wakes `await` and never propagates). | [`app_fields_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/app_fields_test.cpp) |
| `children` | ✅ pass | 0.00s | #82 — in-band vertex creation via a `:children[]` SPEC write (ADR-0017, ADR-0021; docs/reference/05 §0x0E). | [`children_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/children_test.cpp) |
| `delivery_drops` | ✅ pass | 0.00s | Per-cause delivery-drop counters (`graph_t::delivery_drops`). | [`delivery_drops_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/delivery_drops_test.cpp) |
| `edge_eviction` | ✅ pass | 0.00s | RFC-0009 §D extended to peer departure — subscriber-edge eviction on link teardown, and edge-slot reuse. | [`edge_eviction_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/edge_eviction_test.cpp) |
| `effective_acl` | ✅ pass | 0.13s | effective_acl_t unit test (ADR-0050, the previously-untested half) — the pure effective- ACL merge semantics driven directly with synthetic ACE lists (no graph, no locks, no wall clock): own-before-ancestors ordering, nearest-first ancestor order, INHERIT gating at merge time, any-present-ACE-closes (even expired), open-by-default over an empty merge, and DENY first-match-per-bit ordering under the full policy. | [`effective_acl_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/effective_acl_test.cpp) |
| `folded_children` | ✅ pass | 0.00s | L4 fold, Slice 0 — the folded `:children` projection is byte-identical to the materialized serialize, and is a genuine scatter-gather rope a cursor walks. | [`folded_children_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/folded_children_test.cpp) |
| `graph` | ✅ pass | 0.14s | L4 graph-runtime tests. | [`graph_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/graph_test.cpp) |
| `graph_oom_softfail` | ✅ pass | 0.00s | #477 — the store/delivery path's nothrow soft-fail discipline under OOM. | [`graph_oom_softfail_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/graph_oom_softfail_test.cpp) |
| `graph_pmr` | ✅ pass | 0.00s | #361 §5 — the ADR-0039 §1 injection seam on `graph_t`: per-write LKV allocations (control block + rope) draw from the constructor-injected `std::pmr::memory_resource`, and a default-constructed graph keeps the standard heap (zero churn). | [`graph_pmr_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/graph_pmr_test.cpp) |
| `graph_value_backend` | ✅ pass | 0.00s | ADR-0060 — the write-path value byte-buffer seam on `graph_t`: the copy-store flatten of a branch/field write draws its owned `segment` from the injected `value_backend_` (a `mem_backend_t`), not the default heap. | [`graph_value_backend_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/graph_value_backend_test.cpp) |
| `identity` | ✅ pass | 0.00s | #406 / RFC-0011 — the node identity facet: `read <vertex>:identity`. | [`identity_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/identity_test.cpp) |
| `registry_teardown` | ✅ pass | 0.00s | Registry teardown — `child_registry_t::erase` / `remove_child` / `remove_connection` (#494). | [`registry_teardown_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/registry_teardown_test.cpp) |
| `retire` | ✅ pass | 0.03s | graph_t::retire() — RFC-0009 §B/§C/§E.6 vertex retirement (#407). | [`retire_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/retire_test.cpp) |
| `security_acl` | ✅ pass | 0.00s | security_acl unit test (ADR-0050) — drives the PURE ACL policy seam directly, no graph, no locks, no wall clock: both adapters (allow_only / full), the ACE edge cases (expiry, EVERYONE@, per-bit matching, INHERIT-flag filtering, first-match-per-bit DENY ordering), and the typed parse/build round-trip that replaces the per-test byte builders. | [`security_acl_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/security_acl_test.cpp) |
| `subtree` | ✅ pass | 0.00s | RFC-0005 — subtree subscriptions (vertical bubbling), branch-write decomposition, and write-creates. | [`subtree_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/subtree_test.cpp) |
| `subtree_snapshot` | ✅ pass | 0.00s | RFC-0005 §C follow-on — the COMPOSED BRANCH READ: a plain READ of a vertex with ≥ 1 registered child serves the folded POINT tree of its registered subtree's landed LKVs. | [`subtree_read_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/subtree_read_test.cpp) |
| `vertex` | ✅ pass | 0.05s | vertex_t verb-interface unit tests — a BARE vertex, no graph_t (the point of the verb seam: the vertex's storage/readiness/edge/ACL state is testable in isolation). | [`vertex_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/vertex_test.cpp) |
| `vertex_size` | ✅ pass | 0.00s | The vertex RAM-diet regression gate (#361 §8): compile-time ceilings on `sizeof(vertex_t)` and the hot/cold split invariants, plus runtime probes that prove the cold extension block is NOT allocated for the common default leaf and IS allocated exactly when the identity needs it. | [`vertex_size_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/vertex_size_test.cpp) |

### Net (FWD plane)

| suite | result | time | covers | source |
| --- | --- | --- | --- | --- |
| `compact_cache` | ✅ pass | 0.00s | ADR-0062 increment 2 — a warm binding is fast, and it invalidates correctly. | [`compact_cache_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/compact_cache_test.cpp) |
| `fwd_compact` | ✅ pass | 0.39s | RFC-0004 / ADR-0035 slice 4 — the route-handle: ws delivery-compaction, proven over LIVE transport_ws. | [`fwd_compact_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/fwd_compact_test.cpp) |
| `fwd_fanout` | ✅ pass | 0.00s | RFC-0004 / ADR-0035 slice 4 (#136) — the PRODUCER remote fan-out. | [`fwd_fanout_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/fwd_fanout_test.cpp) |
| `fwd_frame_view` | ✅ pass | 0.00s | fwd_frame_view unit test — drives the FWD offset-dispatch cluster directly (no router, no transports), the point of extracting it from fwd_router.cpp (the length_prefix_framer precedent): first-dst-seg / op / control peeks over BOTH cursors (contiguous span + adversarially split rope), the shrunk-dst / grown-src head rebuild proved BYTE-EXACT against a reference re-encode, stack_writer clamp-to-empty overflow, and malformed rejects. | [`fwd_frame_view_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/fwd_frame_view_test.cpp) |
| `fwd_multihop` | ✅ pass | 0.30s | RFC-0004 / ADR-0035 slice 3 — multi-hop FWD forwarding + zero-copy `src` accumulation, proven over LIVE transport_ws links. | [`fwd_multihop_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/fwd_multihop_test.cpp) |
| `fwd_rope_forward` | ✅ pass | 0.00s | ADR-0053 ④b — the FWD forward hop over a MULTI-LINK rope, WITHOUT flattening. | [`fwd_rope_forward_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/fwd_rope_forward_test.cpp) |
| `mount_routing` | ✅ pass | 0.00s | The per-module mount-routing primitives (ADR-0061 / RFC-0014 S2a). | [`mount_routing_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/mount_routing_test.cpp) |
| `net_control_plane_race` | ✅ pass | 0.06s | ADR-0063 §3 — control-plane writers are serialized, and the forward reader is not. | [`net_control_plane_race_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/net_control_plane_race_test.cpp) |
| `op_resolve` | ✅ pass | 0.04s | RFC-0004 / ADR-0035 — op_resolver_t host tests, over the ADR-0041 terminus arena. | [`op_resolve_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/op_resolve_test.cpp) |
| `op_resolve_view` | ✅ pass | 0.00s | ADR-0053 §7 (3c-ii) — the DIFFERENTIAL ORACLE for the two terminus-resolver instantiations. | [`op_resolve_view_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/op_resolve_view_test.cpp) |
| `route_handle` | ✅ pass | 0.01s | route_handle_t unit test (Brick 4, ADR-0038 §3 / ADR-0039): the label state is per- connection — pmr-backed tables with a per-link mutex. | [`route_handle_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/route_handle_test.cpp) |
| `transport_vertex` | ✅ pass | 0.64s | #83 Stage-1 — transport/connection as a `/` vertex (ADR-0027), the SHELL over the live path (ADR-0037 Stage-1). | [`transport_vertex_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/transport_vertex_test.cpp) |

### Transport

| suite | result | time | covers | source |
| --- | --- | --- | --- | --- |
| `can_tx_pool` | ✅ pass | 0.03s | #383 — can_tx_pool_t ownership/backpressure host suite. | [`can_tx_pool_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/can_tx_pool_test.cpp) |
| `tcp` | ✅ pass | 2.18s | M6 TCP transport tests: length-prefix framing over a real localhost TCP stream. | [`tcp_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/tcp_test.cpp) |
| `transport_can` | ✅ pass | 0.00s | #55 (increment 2) — transport_can SocketCAN-binding tests over an in-memory fake link, so they pass in plain Docker with NO kernel CAN (vcan). | [`transport_can_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/transport_can_test.cpp) |
| `transport_can_peers` | ✅ pass | 0.16s | ADR-0044 — stateless transport-peer enumeration + transparent per-peer FWD over the CAN bus binding, proven over the in-memory fake link (no kernel CAN): | [`transport_can_peers_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/transport_can_peers_test.cpp) |
| `transport_can_vcan` | ✅ pass | 0.52s | #55 (increment 2) — REAL-bus smoke test for transport_can over Linux SocketCAN. | [`transport_can_vcan_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/transport_can_vcan_test.cpp) |
| `transport_conformance` | ✅ pass | 0.56s | Transport seam-conformance suite. | [`transport_conformance_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/transport_conformance_test.cpp) |
| `udp` | ✅ pass | 0.94s | M5 UDP transport tests: raw frame delivery over a real localhost UDP socket, and an end- to-end two-node FWD delivery through graph_t + fwd_router_t over UDP (the explicit-source- routed net plane, ADR-0040 — no bridge_t/ROUTER). | [`udp_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/udp_test.cpp) |
| `ws_transport` | ✅ pass | 0.94s | #54 — transport_ws SERVER socket-layer tests. | [`ws_transport_test.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/tests/ws_transport_test.cpp) |

### Examples

| suite | result | time | covers | source |
| --- | --- | --- | --- | --- |
| `example_in_process_pubsub` | ✅ pass | 0.05s | In-process publish/subscribe over the L4 graph — the M3 P0 node, end to end. | [`in_process_pubsub.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/in_process_pubsub.cpp) |
| `example_pubsub_fanout` | ✅ pass | 0.01s | Pub/sub fan-out — one publisher, a growing set of subscribers, and the per-delivery dispatch cost as fan-out scales. | [`pubsub_fanout.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/pubsub_fanout.cpp) |
| `example_rope_scatter` | ✅ pass | 0.01s | L1 scatter-gather — compose a multi-link `rope_t` with zero byte copies, then measure the cost of scatter-gather egress vs. the one flatten copy. | [`rope_scatter.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/rope_scatter.cpp) |
| `example_tree_of_ropes` | ✅ pass | 0.00s | The three composition axes, made visible — why a libtracer node is a *tree of ropes*, not a *rope of ropes*. | [`tree_of_ropes.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/tree_of_ropes.cpp) |
| `example_two_node_fwd` | ✅ pass | 0.01s | Two nodes over a wire — an FWD write routed between two graphs, and the end-to-end delivery latency across the "wire". | [`two_node_fwd.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/two_node_fwd.cpp) |
| `example_wire_codec` | ✅ pass | 0.02s | Wire codec deep-dive — build a structured frame, inspect its bytes, and measure encode / decode / round-trip throughput. | [`wire_codec.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/wire_codec.cpp) |
| `example_wire_roundtrip` | ✅ pass | 0.00s | L2/L3 wire codec round-trip — build a TLV, encode to bytes, decode back. | [`wire_roundtrip.cpp`](https://github.com/avatarsd-llc/libtracer/blob/main/core/examples/wire_roundtrip.cpp) |

## How every suite is verified

Beyond this Release pass, the same suites run under three more configurations in CI (`core-ci.yml`), and the net forward path carries an absolute allocation gate:

| configuration | what it proves |
| --- | --- |
| **Release** (this page) | functional correctness, byte-exact wire behavior |
| **ASan + UBSan** | no leaks, no undefined behavior, no buffer overruns |
| **TSan** | the lock-free LKV + concurrent forward paths are race-free |
| **GCC-13 + GCC-15** | the toolchain floor + the ESP on-silicon compiler |
| **16KB zero-heap gate** | the FWD forward hop allocates **0 bytes** (`bench_forward_heap`, `ZEROHEAP_MAX=0`; ADR-0038/0039) |

Cross-implementation conformance (C++ / TypeScript / Rust agree on every vector) and the live latency/throughput numbers are on the [Performance](performance.md) page.

