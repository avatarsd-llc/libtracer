# Firmware/OTA-class bulk transfer is ordinary auth-gated chunked writes to an app-defined vertex — no raw transport side-channels

Status: accepted (maintainer-ratified 2026-07-03, migration design grilling for the originating production firmware — an ESP32-C6 smart-agriculture node). Builds on [ADR-0041](0041-terminus-arena-decode-span-contract.md) (terminus span contract) and [ADR-0042](0042-refcounted-receiver-seam-view-delivery.md) (injected-backend receiver seam); auth-gating per [ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md) over the [ADR-0018](0018-access-control-authorization-pluggable-subject-token.md)/[ADR-0020](0020-acl-nfsv4-style-aces-with-inheritance.md) ACL model. Companion to [ADR-0044](0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md) and [ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md).

## Context

The origin firmware's OTA update is today a **raw WebSocket side-channel**: the SPA sends binary frames shaped `[0x01][offset][bytes]` that bypass the protobuf message layer entirely, acked by a hand-rolled `OtaChunkAck`. The escape hatch existed for one reason: protobuf's union sizing made megabyte payloads impractical inside the message envelope, so bulk bytes went around it — at the cost of a discriminator byte in the framing, WS-specific flow control, and a second data path the protocol could not see, gate, or observe.

Migrating to libtracer raises the question: does the escape hatch survive (a raw mode in the WS adapter), or does bulk transfer become graph traffic? The reasons for the hatch no longer exist:

- A TLV write payload is a **span** — there is no union to size ([ADR-0005](0005-fixed-width-length-opt-ll.md) fixed-width length, `opt.LL=1` for large payloads); a multi-KB chunk is just a WRITE.
- The [ADR-0042](0042-refcounted-receiver-seam-view-delivery.md) receiver seam lands inbound frames in **injected-backend segments** — chunks are streamable straight to a flash-writing consumer with no extra copy.
- The old ~4–6 KB governor bound on chunk size is expressible **at the seam**: the injected `mem_backend_t`'s `max_segment_size()` naturally bounds what a receiving device accepts per frame.

## Decision

**Bulk transfers are plain chunked WRITEs to an app-defined vertex.** The motivating case, ESP32 OTA:

- The SPA writes firmware chunks to e.g. `/device/ota/data` — ordinary WRITEs, **auth + ACL gated** like any other write ([ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md)): only an authenticated subject with `WRITE` on the OTA subtree can push firmware, for free, from the existing model.
- The **write reply is the ack and the backpressure** — the client sends the next chunk when the previous WRITE's reply returns, replacing the hand-rolled `OtaChunkAck` with the request/reply semantics every write already has.
- **Progress is observable by subscription** — the device publishes to e.g. `/device/ota/status` (bytes written, verifying, ready-to-boot), and the SPA subscribes like it would to any vertex; no bespoke progress messages.
- **Chunk size is bounded at the seam**: the device's injected `mem_backend_t::max_segment_size()` is the natural per-frame ceiling (the old governor bound, expressed where memory policy already lives per [ADR-0042](0042-refcounted-receiver-seam-view-delivery.md) §2); a client discovers/configures it as deployment policy.

**No raw side-channels.** No discriminator byte survives in any framing; there is exactly one data path, and it is the graph.

## Considered options

- **Keep a raw side-channel in the WS adapter** (a `[0x01]`-discriminated binary mode next to TLV frames). Rejected: it is transport-specific — it dies outright on TCP and QUIC, which frame differently; it duplicates flow control the write reply already provides; it bypasses auth/ACL, forcing a parallel authorization story; and it re-creates the unobservable second data path the migration exists to eliminate.
- **A dedicated bulk-transfer wire primitive** (a STREAM/CHUNK type code with its own ack). Rejected: read/write/await is the entire data API ([ADR-0006](0006-read-write-await-api-no-connect.md)); chunked writes to a vertex compose everything a primitive would offer (ordering by reply-gating, backpressure, auth, observability) out of parts that already exist and already interoperate.

## Consequences

- OTA works **identically over TCP and QUIC**, which the WS-frame side-channel never could — the transport plane stays uniform and the [ADR-0043](0043-quic-webtransport-optional-module-msquic.md) browser path carries firmware with zero extra work.
- The OTA path is **auth-gated and observable by construction**: ACEs on `/device/ota` govern who may flash; anyone with read rights can watch `/device/ota/status`.
- The receive path is **copy-lean by construction**: chunk frames land in injected-backend segments ([ADR-0042](0042-refcounted-receiver-seam-view-delivery.md)) and stream to flash without a library-side copy; the library holds no internal buffers ([ADR-0041](0041-terminus-arena-decode-span-contract.md) §5 standing ruling).
- The origin firmware deletes its OTA side-channel, discriminator framing, and `OtaChunkAck` machinery; the SPA's updater becomes a loop of SDK writes plus one subscription.
- The pattern generalizes beyond OTA: any bulk ingest (log upload, config blob, asset push) is the same auth-gated chunked-write shape — no future case justifies a side-channel.
