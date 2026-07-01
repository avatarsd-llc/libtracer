# Test report

```{note}
Auto-generated from a live `ctest` run by `bench/gen_test_report.py` (regenerated in CI before every Pages deploy). Not hand-maintained.
```

## Summary

| suites | passing | conformance vectors | wall time | verdict |
| --- | --- | --- | --- | --- |
| 24 | 24/24 | 24 | 2.21s | ✅ all green |

## By subsystem

| category | suites | passing |
| --- | --- | --- |
| Codec (L2/L3) | 5 | ✅ 5/5 |
| Substrate (L0/L1) | 3 | ✅ 3/3 |
| Graph (L4) | 3 | ✅ 3/3 |
| Net (FWD plane) | 5 | ✅ 5/5 |
| Net (ROUTER plane) | 1 | ✅ 1/1 |
| Transport | 4 | ✅ 4/4 |
| Examples | 3 | ✅ 3/3 |

## Suites

### Codec (L2/L3)

| suite | result | time | covers |
| --- | --- | --- | --- |
| `byteorder` | ✅ pass | 0.00s | little-endian load/store + string-view helpers |
| `can_frames` | ✅ pass | 0.00s | CAN 29-bit ID + view_can_frames split/reassemble |
| `conformance` | ✅ pass | 0.00s | the shared cross-core vector suite (input.bin → expected) |
| `frame` | ✅ pass | 0.00s | TLV encode/decode, CRC, trailer round-trip |
| `ws` | ✅ pass | 0.00s | WebSocket RFC 6455 frame codec (mask/unmask, fragments) |

### Substrate (L0/L1)

| suite | result | time | covers |
| --- | --- | --- | --- |
| `path` | ✅ pass | 0.00s | path parse/canonicalize, PathKey, field-path |
| `substrate` | ✅ pass | 0.00s | segment/view/rope, refcount, backends |
| `substrate_no_atomic` | ✅ pass | 0.00s | the NO_ATOMIC single-core refcount build |

### Graph (L4)

| suite | result | time | covers |
| --- | --- | --- | --- |
| `acl` | ✅ pass | 0.00s | :acl structural storage (ADR-0018/0020) |
| `children` | ✅ pass | 0.00s | :children[] SPEC vertex creation (ADR-0017/#82) |
| `graph` | ✅ pass | 0.10s | roles, lock-free LKV, read/write/await, fan-out, field-write |

### Net (FWD plane)

| suite | result | time | covers |
| --- | --- | --- | --- |
| `fwd_compact` | ✅ pass | 0.39s | route-handle label compaction + self-heal (RFC-0004 §E.1) |
| `fwd_fanout` | ✅ pass | 0.00s | producer remote fan-out + delivery_compact (#136) |
| `fwd_multihop` | ✅ pass | 0.30s | multi-hop forward: dst-shrink / src-grow byte-exact |
| `op_resolve` | ✅ pass | 0.04s | terminus op resolution + zero-copy FWD{REPLY} (RFC-0004) |
| `transport_vertex` | ✅ pass | 0.02s | transport/connection as a / vertex (ADR-0027/#83) |

### Net (ROUTER plane)

| suite | result | time | covers |
| --- | --- | --- | --- |
| `bridge` | ✅ pass | 0.03s | ROUTER wrap/unwrap, dedup, hop_count, status (M4/#77) |

### Transport

| suite | result | time | covers |
| --- | --- | --- | --- |
| `transport_can` | ✅ pass | 0.00s | CAN classic + CAN-FD framing |
| `transport_can_vcan` | ✅ pass | 0.21s | SocketCAN over a vcan loopback (E2E) |
| `udp` | ✅ pass | 0.62s | UDP socket transport, two-node E2E |
| `ws_transport` | ✅ pass | 0.20s | WebSocket RFC 6455 codec + transport |

### Examples

| suite | result | time | covers |
| --- | --- | --- | --- |
| `example_in_process_pubsub` | ✅ pass | 0.05s | the in-process pub/sub example |
| `example_two_node_loopback` | ✅ pass | 0.00s | the two-node loopback example |
| `example_udp_two_node` | ✅ pass | 0.21s | the two-node UDP example |

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

