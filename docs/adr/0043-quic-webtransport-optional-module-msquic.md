# QUIC + WebTransport arrive as an OPTIONAL module (`LIBTRACER_WITH_QUIC`, msquic) — the core stays dependency-free; browsers reach the graph over WebTransport

Status: accepted (maintainer-ratified 2026-07-03: "go ahead with quic + webtransport track after tcp lands"). Builds on the `transport_t` seam + [ADR-0042](0042-refcounted-receiver-seam-view-delivery.md) view delivery; the substrate for [#92](https://github.com/avatarsd-llc/libtracer/issues/92) / [ADR-0031](0031-direct-browser-to-robot-binding-and-webtransport.md) (browser ↔ robot).

## Context

M6 (TCP) covers reliable streams with zero dependencies. QUIC adds what a mobile robot fleet actually feels: **connection migration** (Wi-Fi↔LTE hop keeps the session), **stream multiplexing without cross-flow head-of-line blocking**, the **datagram extension** (RFC 9221), **TLS 1.3 built in** (the first real link-security story), and — decisively — it is **the substrate WebTransport requires**, so one investment serves both the fleet link and the browser client (#92).

There is no kernel/POSIX QUIC: a library is unavoidable. That collides with a founding property — the C++23 core has **no external dependencies** and targets 16 KB-class devices that cannot carry TLS+QUIC anyway.

## Decision

1. **Optional module, never a core dependency.** `-DLIBTRACER_WITH_QUIC=ON` (default **OFF**), the exact [ADR-0024](0024-mem-cuda-gpu-backend-heterogeneous-rope.md) `LIBTRACER_WITH_CUDA` precedent: core/CI builds stay dependency-free; the module builds where its library exists. QUIC is a **hosted** transport — the MCU class keeps UDP/CAN; nothing in core references the module.
2. **Library: msquic.** Pure C API, mature (production at scale), client+server in one library, event-driven callbacks that map cleanly onto our recv-thread/receiver model, no extra TLS library to glue (unlike ngtcp2) and no Rust toolchain in the build (unlike quiche). Revisitable if packaging friction proves high — the transport seam isolates the choice.
3. **Phase A — `quic_transport_t`** (`tr::net`, behind `transport_t` + ADR-0042): dial + one-peer listen, **one bidirectional stream per connection carrying the SAME 4-byte length-prefix framing as `tcp_transport_t`** — the M6 framing/reassembly seam is reused verbatim, frames land in injected-backend segments, view delivery on. Per-flow streams and RFC 9221 datagram mode are **staged follow-ons** (they need a flow→stream mapping policy that should be designed against a measured need, not speculatively). Certificates: the listen side takes cert+key paths; a helper generates a self-signed pair for dev. **Layering requirement (maintainer, 2026-07-03): the shared connection types stay LEAN** — `conn_settings_t` carries only the universal keys (kind/addr/port/role/keepalive); cert/key are **kind-private config keys parsed by the `quic` factory itself** from the raw SPEC config, inside the module. A device without the module contains zero QUIC schema; ordinary vertices never know a transport exists.
4. **Phase B — WebTransport endpoint** for the browser client: the HTTP/3 CONNECT handshake in front of QUIC (via msquic + a minimal H3 layer), exposing the same frame stream to the TS SDK; browser trust via `serverCertificateHashes` (the standard dev-time WebTransport pattern), a real cert in deployment. The TS side gains a `transport-webtransport` package speaking the identical length-prefixed framing. Phase B lands only after Phase A soaks — it is the #92 deliverable.
5. **Module mechanics — NO preprocessor gates (maintainer, 2026-07-03: "using any macro to include quic (or anything) is definitely wrong").** The module is a **separate link target** (`libtracer_quic`: own header/sources, links msquic); the core target contains **zero** references to it and **zero `#ifdef` conditionals** anywhere in source. The `quic` factory is registered **at runtime by the host** — `register_transport_type("quic", tr::net::quic_factory(...))`, one line in the application that links the module — through the existing catalog seam; `transport_vertex.cpp` stays QUIC-free. The CMake option decides only whether the target and its tests are *built*. Composition by linking, never by preprocessor (this supersedes the ADR-0024 compile-definition style for new modules).
6. **Docs**: reference/module pages present QUIC as what it is — the hosted, secure, migrating link — with the MCU exclusion stated plainly.

## Considered options

- **ngtcp2** — leaner and more "own the stack," but requires separately gluing a TLS backend (OpenSSL-quic/BoringSSL/picotls) and hand-rolling more of the state machine; more surface for us to maintain. Kept as the fallback if msquic packaging hurts.
- **quiche (Cloudflare)** — good library, but pulls a Rust toolchain into the C++ build for an optional module; rejected on build-friction grounds.
- **QUIC datagrams as the primary frame carrier** — rejected for Phase A: RFC 9221 datagrams are **unreliable**, and deliveries/replies assume the link delivers or reports down; the reliable stream is the correct default. Datagram mode returns as an explicit QoS opt-in follow-on.
- **Making QUIC the M6 transport instead of TCP** — rejected: TCP is dependency-free, immediately useful, and builds the framing seam QUIC reuses; QUIC as a core requirement would break the dependency-free/MCU story outright.

## Consequences

- New optional surface (module-gated): `quic_transport_t`, the `quic` factory kind, a dev-cert helper; later the WebTransport endpoint + TS `transport-webtransport` package (Phase B).
- CI: a dedicated `quic` job builds+tests the module (fetching msquic) so default jobs stay dependency-free — mirroring how CUDA stays out of CI but here the library IS CI-viable.
- The 16 KB story is untouched; the security story starts existing (TLS 1.3 on the hosted link).
