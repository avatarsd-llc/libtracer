# The refcounted receiver seam: transports MAY hand up owning frames (`view_t`), buffers come from a host-injected `mem_backend_t`, and big WRITE payloads may store as frame subviews

Status: accepted. **Implements [ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) §4** (the receiver buffer-lifetime seam) and the M5+ staged follow-on pinned in [ADR-0041](0041-terminus-arena-decode-span-contract.md) §2. Resolves [#173](https://github.com/avatarsd-llc/libtracer/issues/173) (maintainer-ratified 2026-07-03).

## Context

Today a transport's receiver delivers a **borrowed** `std::span<const std::byte>` that dies when `on_frame` returns. That is why every terminus ownership transfer must copy (the ADR-0041 §2 copy-once contract): a WRITE stores its payload by copying `node.wire` even when the payload is megabytes, because nothing refcounts the inbound frame. The frame bytes already live in a buffer the transport allocated — the copy exists only because the seam cannot express ownership.

Four design questions were framed on #173 and resolved as follows.

## Decision

### 1. Seam shape: an optional second receiver, per-link capability

`transport_t` gains:

- `set_view_receiver(std::function<void(view::view_t)>)` — the owning delivery path;
- `[[nodiscard]] virtual bool delivers_views() const { return false; }` — the capability.

A transport that can hand up owning frames overrides `delivers_views()` and calls the view receiver with a `view_t` over a refcounted segment; every other transport keeps the span receiver unchanged. `fwd_router_t::add_child` installs the receiver matching the link's capability; both paths funnel into the same routing code (offset-dispatch forward, arena terminus) — only the terminus ownership steps differ (subview vs copy).

**No adapter that wraps a borrowed span into a `view_t`.** A view whose refcount does not actually protect its bytes lies about lifetime; roping it downstream would be exactly the use-after-free class the borrowed-span contract exists to prevent. Span-only transports keep span semantics, honestly.

### 2. Buffer origin: a host-injected `mem_backend_t`, never a library buffer

A view-delivering transport receives into segments drawn from an injected **`mem_backend_t*`** (constructor / transport-factory parameter, defaulted to `mem::heap_backend()`). A bounded host passes its `pool_t` over its static slab — the L0 seam this project already has ([ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md)/[ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md) §2: one slab, whole stack). Pool exhaustion is **backpressure** (the frame is dropped, a counter ticks) — never OOM, per ADR-0039 §4. This preserves the standing maintainer ruling from ADR-0041 §5: the library holds **no internal buffers**; memory policy lives wholly at the injection seams.

`udp_transport_t` is the first implementer — one datagram = one frame = one segment (`recvfrom` straight into the segment's bytes, hand up `view_t::over(segment)`). `transport_ws` follows when its frame-assembly path is pointed at segments.

### 3. WRITE-store threshold: per-vertex `store_ref_min_bytes`, **disabled by default**

On a view-delivered terminus frame, a WRITE whose payload TLV (`node.wire`) is **≥ `settings.store_ref_min_bytes`** stores a **subview of the frame** (refcount bump, zero copy) instead of the ADR-0041 one-copy `own_tlv`; smaller payloads keep the copy (a copy under ~a few hundred bytes beats pinning — the Brick-2 lesson). The subview **pins the whole frame** until the next LKV write; the amplification is `frame_size / payload_size`, which for the dominant case (a delivery FWD whose payload is most of the frame) is ≈1.

`store_ref_min_bytes = 0` (the default) **disables referencing** — behavior is byte-identical to today. Opting in is a per-vertex `:settings` policy call because pinning amplification is a deployment property, not a library presumption. A span-delivered frame ignores the setting (nothing to reference).

The stored subview is trailer-sliced logically, not physically: the subview covers `node.wire` (trailer already excluded by the arena), and the opt byte cannot be patched in a shared frame — so a referenced store retains the wire opt byte. Readers of a stored value MUST therefore tolerate set trailer bits with absent trailer bytes **only** on the referenced path; to avoid that asymmetry the implementation MAY (and the reference implementation DOES) restrict referencing to payloads whose opt byte carries no trailer bits — a CRC/TS-carrying oversized payload falls back to the one-copy trailer-sliced store. Deliveries from our own producers are trailer-less, so the dominant case references cleanly.

### 4. M6: the seam carries a contiguous `view_t` now; the rope overload comes with rope-aware decode

M6's stream transport (length-prefix framing, partial reads) will want to hand up a **rope**. That overload is added **when rope-aware decode exists to consume it** — not speculatively (the same no-unneeded-generality call as choosing B over A in ADR-0041). Until then a stream transport reassembles each frame into one segment; `set_view_receiver` is the named extension point.

## Consequences

- New surface: `transport_t::set_view_receiver` / `delivers_views`; a `mem_backend_t*` parameter on view-delivering transports (first: `udp_transport_t`) and on the transport factory config; `settings_t::store_ref_min_bytes` (u32, default 0). All additive and defaulted — no existing caller changes.
- The big-payload WRITE path drops its last copy: receive into a pooled segment → arena-decode (spans) → store a subview. Bytes are copied **zero** times between the socket and the LKV.
- `bench_forward_heap`'s terminus window gains visibility into the referenced path (report-only, as before).
- The copy-once contract of ADR-0041 §2 is unchanged for span-delivered frames and for small/trailered payloads; this ADR adds the *sub-viewed off a refcounted owner* leg that contract always permitted.
