# core — Reference C++ implementation

The reference implementation of the libtracer protocol. Targets ESP32, STM32, and bare-metal alongside hosted Linux/macOS.

## Status: protocol-v1 rebuild in progress — M1 (wire codec) + M2 (L0/L1 substrate) landed

> **2026-06-24.** The original headers under `include/libtracer/` were a pre-spec snapshot extracted from strawberry-fw (see [ADR-0001](../docs/adr/0001-extract-reference-implementation-from-strawberry-fw.md)). They did not compile and encoded the retired v0.0 wire model (in-header XOR-16 CRC, no `PL` bit, fixed `uint32` length, `connect`/`disconnect` API, NUL-terminated NAMEs). Per the protocol-v1 consistency consolidation they were **deleted, to be rebuilt fresh against the spec** rather than patched.

The rebuild targets the protocol-v1 wire format and API as fixed in the ADRs:

- 4/6-byte header, `opt` bits `PL/TS/CR/LL/CW/TF`, trailer-positioned CRC-32C ([ADR-0004](../docs/adr/0004-crc-in-optional-trailer.md)), fixed-width length ([ADR-0005](../docs/adr/0005-fixed-width-length-opt-ll.md)).
- No generic `LIST` ([ADR-0003](../docs/adr/0003-retire-list-type-code-0x05.md)); structured = `opt.PL=1` + a purpose type byte.
- `read`/`write`/`await` + field-write control surface; no `connect`/`disconnect`/`subscribe` ([ADR-0006](../docs/adr/0006-read-write-await-api-no-connect.md)).
- The module ABI is implementation-defined ([ADR-0013](../docs/adr/0013-v1-scope-boundaries.md)); this implementation is modern C++23.

### Milestones

- **M1 — wire codec (landed).** The L2/L3 layer: frame header / `opt` / trailer, CRC-32C and CRC-16-CCITT, the borrowed (zero-copy) `Tlv` model, and `decode`/`encode`. Passes all four seed conformance vectors under [../tests/conformance/vectors/v1/](../tests/conformance/vectors/v1/) via `tests/conformance_runner.cpp`.
- **M2 — L0/L1 substrate (landed).** The refcounted `Segment` (intrusive `SegmentPtr`, the canonical intrusive_ptr orderings from [02-graph-model.md](../docs/reference/02-graph-model.md) §required atomic operations; `LIBTRACER_NO_ATOMIC` for single-threaded/Cortex-M0), the user-implementable `MemBackend` seam with three backends (`mem_heap`, `mem_borrowed` live/transparent-router, `mem_pool` bounded fixed-slab), the zero-copy `View`/`Rope`, and the `view_as_tlv` cast that ties a TLV back to a view ([08-views-and-ownership.md](../docs/reference/08-views-and-ownership.md)). The refcount is what makes M1's borrowed `Tlv` safe to hold past its source buffer. Validated by `tests/substrate_test.cpp` (also built `-DLIBTRACER_NO_ATOMIC`), clean under ASan+UBSan.
- **M3 — L4 in-process graph.** Vertex map, dispatcher, `read`/`write`/`await` + field-write, an in-process pub/sub example — the full P0 node. Rope-aware (link-walking) zero-copy decode lands here too.

## Layout

```text
core/
├── include/libtracer/    Public headers
│   ├── tlv.hpp crc.hpp frame.hpp     M1 — wire codec (L2/L3)
│   ├── backend.hpp segment.hpp       M2 — L0 seam + refcounted segment
│   ├── mem_heap.hpp mem_borrowed.hpp mem_pool.hpp   M2 — L0 backends
│   ├── view.hpp rope.hpp             M2 — L1 zero-copy view/rope + cast
│   └── tracer.hpp                    umbrella include
├── src/                  Implementation — frame.cpp, mem_heap.cpp, mem_pool.cpp, rope.cpp
├── tests/                conformance_runner.cpp + substrate_test.cpp + CMake
├── CHANGELOG.md          public-API change log
└── CMakeLists.txt
```

The directory name `include/libtracer/` is part of the public API: include paths in user code (`#include <libtracer/tracer.hpp>`) depend on it. Do not rename.

## Building

Requires a C++23 compiler (GCC 13+ / Clang 16+, for `std::expected`).

```sh
cmake -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

`ctest` runs the conformance suite (`conformance_runner`) against the seed vectors: each is validated by roundtrip (`encode(decode(input.bin)) == input.bin`) plus a programmatic golden encode/decode — no JSON parser, since the bytes are self-describing.
