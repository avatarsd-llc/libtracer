# core — Reference C/C++ implementation

The reference implementation of the libtracer protocol. Targets ESP32, STM32, and bare-metal alongside hosted Linux/macOS.

## Status: pre-spec extraction removed; v0.1 rebuild pending

> **2026-06-24.** The original headers under `include/libtracer/` were a pre-spec snapshot extracted from strawberry-fw (see [ADR-0001](../docs/adr/0001-extract-reference-implementation-from-strawberry-fw.md)). They did not compile and encoded the retired v0.0 wire model (in-header XOR-16 CRC, no `PL` bit, fixed `uint32` length, `connect`/`disconnect` API, NUL-terminated NAMEs). Per the v0.1 consistency consolidation they were **deleted, to be rebuilt fresh against the spec** rather than patched.

The rebuild targets the protocol-v1 wire format and API as fixed in the ADRs and the consolidation RFC:

- 4/6-byte header, `opt` bits `PL/TS/CR/LL/CW/TF`, trailer-positioned CRC-32C ([ADR-0004](../docs/adr/0004-crc-in-optional-trailer.md)), fixed-width length ([ADR-0005](../docs/adr/0005-fixed-width-length-opt-ll.md)).
- No generic `LIST` ([ADR-0003](../docs/adr/0003-retire-list-type-code-0x05.md)); structured = `opt.PL=1` + a purpose type byte.
- `read`/`write`/`await` + field-write control surface; no `connect`/`disconnect`/`subscribe` ([ADR-0006](../docs/adr/0006-read-write-await-api-no-connect.md)).
- The error-code registry and `ERROR (0x08)` leading-child-TLV shape per [/CONTEXT.md](../CONTEXT.md) and `docs/reference/05-protocol-tlvs.md`.

The first deliverable is a P0 (in-process) skeleton that passes the seed conformance vectors under [../tests/conformance/vectors/v1/](../tests/conformance/vectors/v1/).

## Layout (target)

```text
core/
├── include/libtracer/    Public headers — users write #include <libtracer/...>
├── src/                  Implementation (added with the rebuild)
└── CMakeLists.txt
```

The directory name `include/libtracer/` is part of the public API: include paths in user code (`#include <libtracer/tracer.hpp>`) depend on it. Do not rename.

## Building

```sh
cmake -B build
cmake --build build
```

(There are no sources or tests to build yet; the interface target carries only include paths until the rebuild lands.)
