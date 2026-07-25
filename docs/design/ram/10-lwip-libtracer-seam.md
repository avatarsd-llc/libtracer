# The lwIP ↔ libtracer seam — architecture & optimization

*A zero-copy audit of the ingress/egress boundary between the lwIP socket layer (and `esp_http_server`) and libtracer's rope tier, targeted at the single-core ESP32-C6 (strawberry-fw). Every claim is anchored `file:line` against `/home/sd/prj/20260503_libtracer`.*

> **⚠ Second-pass errata (2026-07-23):** the re-audit corrected the ingress-copy anchoring below (this doc
> conflates `flatten()` at `rope.cpp:12` with the ingress ownership copy — the real ingress copies are the
> **recv** sites `transport_tcp.cpp:229` / `httpd_ws_link.cpp:332-338`, which C1/C4 anchor correctly;
> `rope.cpp:12` is the **multi-link durable-store flatten**, C11). Full ledger:
> [`30-second-pass-reconciliation.md`](30-second-pass-reconciliation.md) §Corrections 6.

## 0. Thesis

libtracer's ingress doctrine is RDMA-shaped: **copy each byte once at the wire, then zero-copy forever.** A frame is received into a refcounted `segment_t`; every downstream view — routed suffix, child TLV, stored value, reply span — is a refcount-bumped subview (`rope.hpp` `subrope`), never a byte copy (`segment.hpp:124-127`, a relaxed increment). The question this audit answers is not "is the design zero-copy?" (it is, by construction) but **"where does the *implementation* still flatten, is each flatten fundamental or removable, and what does removing it actually save — stack, heap, fragmentation, or latency?"**

The headline finding, stated up front so the rest can qualify it:

- **The flatten is a fallback, not a floor.** Every materialize/flatten point except the one ingress ownership copy (`rope.cpp:12`) is a *residual `span_cursor` cost* — an artifact of decoders/sinks that still demand contiguous input, not a fundamental requirement. The `rope_cursor` that removes them already ships and drives three live consumers.
- **But the savings are wildly uneven, and several widely-cited "levers" save ~0 bytes on the common path.** The 4096-byte stack arena is a *stack* win only (the RAM relocates to a pool). The terminus store flatten is already zero-copy for single-link frames (every current producer). The WS reassembly flatten only fires on fragmented messages the SPA never sends. Rendering these honestly is the point of the audit.
- **Two copies are genuinely fundamental:** the one ingress ownership copy (RDMA copy-once), and — on the device only — the `esp_http_server` TX gather-flatten, which no migration removes without abandoning the threadless httpd seam entirely.
- **The single shared blocker for the real wins is unbuilt:** the single-core, crit-section-safe bounded pool (ADR-0060 §2). Today only the multi-core `sync_pool_t` spinlock exists.

---

## 1. The RX path: wire → rope, per transport

Every inbound byte crosses two boundaries: **kernel → userspace** (the lwIP `recv` drain out of the pbuf pool — unavoidable on any BSD socket API) and **userspace → owned segment** (libtracer's one ownership copy). A transport is "ingress-optimal" when these two coincide — when `recv` writes *straight into* the owned segment. Faithfulness to that floor differs sharply across transports.

### 1.1 Raw single-peer TCP — ingress-optimal

The pull-mode stream reads the body straight into the owned segment:

```
view::segment_ptr_t seg = std::move(dec.seg);
if (!read_exact(fd, seg->bytes.data(), len)) return;   // transport_tcp.cpp:229
```

The segment is freshly `alloc`'d from the injected backend by `on_prefix` (`length_prefix_framer.hpp:118`) *before* the body read, so `recv` lands directly in owned storage. Delivered **owning** through `receiver_slot::deliver` (`receiver_slot.hpp:110`). **One kernel copy, zero userspace copies — the RDMA ideal, realized.** A pooled recv buffer *is* the owned segment here. The framer's own doc states the trade explicitly (`length_prefix_framer.hpp:76-79`): feeding chunks through `feed()` "would add a scratch-buffer copy on the hot path," so the pull loop shares the framing *rules*, not the state machine.

### 1.2 Raw UDP — ingress-optimal

Identical shape: `recvfrom(fd, rx_seg->bytes.data(), …)` (`transport_udp.cpp:154`) — one datagram → one owned segment, reused across recv timeouts, delivered owning. One kernel copy, zero userspace copies.

### 1.3 Multi-peer TCP server — one forced second copy

The single poll thread (the #362 single-thread MCU shape) cannot size a segment before it has decoded the length prefix, so it `::recv`s into a **4096-byte stack chunk** (`transport_tcp.cpp:430`) and the framer memcpy's every byte a *second* time into the segment:

```
// Fill the frame body ... (the single unavoidable copy off
// the wire into the owned segment).
std::memcpy(rx_seg_->bytes.data() + rx_off_, p, take);   // length_prefix_framer.hpp:207
```

This second userspace copy is the **tax of single-thread multiplexing**: a non-blocking multiplexed reader can't recv-into-segment before it knows the frame length. Removable only by reverting to a blocking thread-per-peer — which the single-core C6 rejects on stack grounds — or by recv'ing into a per-session growable segment and handing frames as subviews. Buffer count: 1 stack chunk (4 KiB) + N owned segments.

### 1.4 WebSocket over `esp_http_server` — the worst RX offender

The device graph `/ws` rides `esp_http_server` (registered as one more URI handler on the firmware's `:80` SPA httpd), not a raw socket. Its recv is a two-pass, copy-into-caller-buffer API:

- **Pass 1** — `httpd_ws_recv_frame(req,&frame,0)` reads only the header to learn `frame.len`.
- **Pass 2** — a **fresh per-frame heap allocation**, then a drain out of the pbuf:

```
payload.reset(new (std::nothrow) std::byte[frame.len]);   // httpd_ws_link.cpp:332
...
frame.payload = ...payload.get();
err = httpd_ws_recv_frame(req, &frame, frame.len);        // :338  memcpy pbuf → buffer
```

There is no `esp_http_server` API that hands up a borrowed pointer into the pbuf, so this drain is the **fundamental** ingress boundary copy — the same role `over_bytes()` plays in the core transport. What is *not* fundamental is that it is a **per-RX-frame `new(nothrow)`** (a fragmentation source, the ~27 KB/session leak class) delivered **borrowed** (`deliver_borrowed`, `:463-469`).

That borrowed delivery is the tap-root of the entire downstream flatten chain: because WS never hands the rope tier an *owned* segment, it forces (a) the `span_cursor` decode path, (b) the `std::array<std::byte,4096>` on-stack decode arena at `graph.cpp:929`, and (c) the `materialize` flattens at `graph.cpp:922`/`:1152`. Note carefully: on the *unfragmented* fast path (`httpd_ws_link.cpp:403-405`) the SPA sends one whole TLV per frame, so `body` is already contiguous and single-link — the flatten there is a refcount adopt, not a copy. The chain fires only because the sink still *demands* contiguity, not because bytes are actually being moved on the fast path.

**Fragmented messages are strictly worse.** Each RFC-6455 continuation runs through `asm_buf_t::append`, which regrows an exact-sized buffer and re-copies **all** prior bytes:

```
std::unique_ptr<std::byte[]> grown(new (std::nothrow) std::byte[len_ + chunk.size()]);
if (len_ != 0) std::memcpy(grown.get(), bytes_.get(), len_);        // httpd_ws_link.cpp:126
std::memcpy(grown.get() + len_, chunk.data(), chunk.size());        // :127
```

This is **O(n²) in fragment count** and a heap-fragmentation source (each grow allocates fresh, frees the prior — a transient `2·n` heap spike). The core raw transport already solved this: `ws_assembler_t` (`transport_ws.cpp:43-78`) makes each fragment one owning rope link via `over_bytes` and chains them ("chaining the fragments is pointer-linking, never a memcpy"), delivering a rope.

### 1.5 CAN via twai — one honest copy, wrongly sourced

`transport_can.cpp:357` copies each ≤8/≤64-byte slice via `over_bytes` (`mem_heap.hpp:229`) so the reassembly rope (`can_reassembly.hpp:181`) can hold subviews that outlive the transient `can_frame_data_t`. This is CAN's *legitimate* ownership copy — but it draws from the **global heap, not a pool**. The ISR additionally stages through a redundant `buf[8]` (`twai_link.cpp:171→:183`) before the FreeRTOS queue hop; `receive_from_isr` could target `out.data` directly. CAN rx is a low-value pooling target regardless (tiny bytes, ISR-copied into a fixed queue).

### 1.6 The ownership copy is fundamental *as a copy*, but not necessarily *extra*

Why must ingress copy into an *owned* segment rather than borrow the recv buffer? Because `vertex_t::store` (`vertex.hpp:856`) holds the rope by `shared_ptr`/refcount and copies **no** bytes — durability rests entirely on the stored segments being *owned* (long-lived), not *borrowed*. The recv buffer is transient (stack chunk on multi-peer TCP; per-receive `new[]` on WS) and dies when the receive call returns, while the rope outlives it (pinned in an LKV, fanned out asynchronously, awaited). A borrowed view (`mem_borrowed.hpp`, `owns_bytes=false`, *"must NOT be durably stored"*) would dangle.

But the copy need not be *extra*: on the pull path (§1.1) it already *is* the `recv`. The removable waste — multi-peer scratch (§1.3), WS borrowed-then-store (§1.4), WS reassembly (§1.4), CAN heap-not-pool (§1.5) — is all **pool-recv migration**, not a flatten problem. The DEVICE-link constraint (`rope.cpp:13` `all_host()` guard) never conflicts: recv targets are always HOST memory, so a host pool slot is a legal recv/adopt target; only a CPU-side *flatten* of a DEVICE link would fault, which `flatten` refuses up front.

---

## 2. The TX path: rope → wire

**Two different answers, split by platform.** On the host transports the egress is genuinely zero-copy up to the kernel boundary. On the ESP device it pays one forced libtracer-level flatten plus the fundamental lwIP socket copy.

### 2.1 Host transports — zero userspace copy

All three host transports override the iovec entry point and scatter-gather the untouched rope links:

```
vec[0] = ::iovec{header.data(), header_len};             // transport_ws.cpp:145  WS header (2-10 B)
for (const std::span<const std::byte>& s : iov)
    vec[n++] = ::iovec{...s.data(), s.size()};            // :150  each rope link, NO copy
```

`build_server_iov` (`transport_ws.cpp:135-151`) aliases entry-0 to the frame header and every following entry to a rope link with no copy; server frames are **unmasked** (RFC 6455 §5.1, `:234-235`) so payload bytes ride straight to `::sendmsg`. `transport_tcp` is identical with a u32-LE length prefix (`prefixed_iov_t`, `:53-80`); `transport_udp` gathers into one `sendmsg` datagram (`:83-116`). `write_all_iov` (`posix_endpoint.cpp:68-92`) drives `::sendmsg(MSG_NOSIGNAL)` and correctly re-trims across partial writes. The common FWD reply (≤~6 spans) fits the `kMaxServerIov=16` stack bound and never allocates. The only per-peer copy in a broadcast (`transport_ws.cpp:256`, `transport_tcp.cpp:342`) duplicates the ~256 B *descriptor array*, not payload — mandatory because `write_all_iov` consumes its array in place.

**Verdict: zero libtracer copy; the sole copy is the kernel skb copy every BSD socket pays.** The masking copy in `encode_client_frame` (`ws.hpp:402-405`) is real but confined to the dial-out *client* (masking mutates every byte and cannot be gathered) — the device never runs it.

### 2.2 ESP device — one forced flatten + the lwIP copy

The device pays two copies the host merges away.

**Copy 1 — the gather flatten (`httpd_ws_link.cpp:485-499`).** The rope is memcpy-collapsed into one owned heap buffer before the httpd work item is enqueued:

```
for (const auto& part : iov) total += part.size();
std::unique_ptr<std::byte[]> buf(new (std::nothrow) std::byte[total]);   // :488
...
for (const auto& part : iov)
    std::memcpy(p, part.data(), part.size());                            // :492
```

Forced by two compounding facts: `httpd_queue_work` is **asynchronous**, so the caller's rope links (borrowed into value-backend segments) are freed before `tx_work` runs; and `httpd_ws_send_frame_async` accepts only a **contiguous** `payload+len` (`httpd_ws.c:453-454`), with no `writev`/iovec form. So a multi-segment rope must be flattened to one *owned* buffer regardless of lifetime. Sized to the whole reply (≈12.7 KB observed, capped at `kMaxFrameBytes=32768`), allocated per-frame-per-peer — a broadcast to N subscribers holds N transient full-reply buffers at once. This is the exact heap-spike/fragmentation surface the `-fno-exceptions` nothrow rewrite was built to survive after the Gorshok browser-session OOM abort.

**Copy 2 — the lwIP socket copy.** `lwip_send`/`lwip_sendmsg` hard-code `NETCONN_COPY` (`sockets.c:1453,:1496`); no `send()` flag reaches `TCP_WRITE_FLAG_COPY=0`, so the bytes land in the pbuf pool regardless. **Fundamental** to the BSD socket API — escaping it means a raw `netconn`/`tcp_write` NOCOPY rewrite with manual pbuf-pinning-until-ACK, unjustified for control-plane frames.

**Important correction to the folklore:** the *double-buffer spike* (a flatten-temp buffer plus a separate tx-copy) was **already eliminated** by the `send(iov)` override at `httpd_ws_link.cpp:579-594`, which gathers **once** straight into the tx work buffer. What remains at `:488` is a **single** irreducible gather-copy, not a double buffer. And **neither egress copy is touched by the `rope_cursor` migration** — `rope_cursor` is an RX/decode lever (§4, §5); the TX sink is a hardware send API, not a `span_cursor` decoder.

---

## 3. Copy inventory

Every copy/flatten on the seam. "Saves" is the *honest, corrected* saving — several are far narrower than first-order analysis suggests.

| # | Copy / flatten | Location | Necessary? | Removes what | Honest saving | Risk |
|---|----------------|----------|-----------|--------------|---------------|------|
| C1 | Single-peer TCP recv → owned segment | `transport_tcp.cpp:229` | **Fundamental** (kernel→user + is the ownership copy, coincident) | — | — (already optimal) | — |
| C2 | UDP recvfrom → owned segment | `transport_udp.cpp:154` | **Fundamental** (same) | — | — (already optimal) | — |
| C3 | Multi-peer TCP: stack chunk → segment | `transport_tcp.cpp:430` + `length_prefix_framer.hpp:207` | **Removable** (2nd userspace copy) | 1 memcpy/frame | Latency + heap on multiplexed path; recv-into-growable-segment | Reframe non-blocking reader; thread-safety of per-session state |
| C4 | WS pass-2 recv → `new[frame.len]` | `httpd_ws_link.cpp:332,338` | **Copy fundamental** (no borrowed pbuf recv); **alloc removable** | per-frame `new(nothrow)` → pooled slot | Fragmentation + OOM-abort surface (copy stays) | ADR-0060 pool unbuilt |
| C5 | WS fragment reassembly (regrow+memcpy) | `httpd_ws_link.cpp:121,126-127` | **Removable-with-migration** | O(n²)→O(n); the transient 2·n heap spike | Real *peak-heap* drop on fragmented msgs **only**; fast path unaffected (SPA sends 1 TLV/frame). *Frag: dubious — k small segments may fragment more than 1 buffer* | Needs rope sink wired on httpd link + per-fragment owned segments; else `receiver_slot.hpp:138` re-materializes |
| C6 | CAN slice → heap segment (`over_bytes`) | `transport_can.cpp:357` | **Fundamental as copy**; source removable (heap→pool) | heap→pool draw | Fragmentation only (copy stays) | ADR-0060 pool |
| C7 | CAN ISR staging `buf[8]` | `twai_link.cpp:171→183` | **Removable** | 1 ISR copy | Tiny; ISR cleanliness | ISR-safety of direct target |
| C8 | Branch/field write `materialize` | `graph.cpp:922,1152` | **Zero-copy for single-link** (returns `links()[0]`); flatten only multi-link | 1 flatten on *multi-link* branch value | **~0 on common path** (single-link until ADR-0053 ④b); real win only in opt-in pinned-multi-link-branch case | ADR-0041 §2 sink contiguity; RFC-gated `rope_cursor` decode |
| C9 | 4096-byte on-stack decode arena | `graph.cpp:929` | **Removable-with-migration** | fixed 4096 B **stack**, branch-write path only | **Stack high-water −4096 B** (the scarce resource). **Net device RAM unchanged** — structure relocates to pool/heap. #477 throwing-overflow *relocates*, not resolved (real fix = node-counting pre-pass) | Latency (heap/pool walk stack); RFC-gated |
| C10 | Per-node `ensure_cache` flatten | `op_resolve_view.cpp:142` | **Removable-with-migration** | flatten of each *straddling* accessed TLV | Narrow: fires per-node **only when that node straddles a link**; single-link nodes are zero-copy adopt even in a fragmented frame. On straddling route TLVs, merges a *double* copy into one gather | ADR-0053 §7 shared span-node concept |
| C11 | Ownership flatten `own_wire`/`own_tlv` | `op_resolve_view.cpp:80`; `fwd_router.cpp:376` | **Fundamental** (RFC-0005/ADR-0041 §2: durable store of scatter-gather value) | — | — (mandatory ownership copy for multi-link durable store) | — |
| C12 | COMPACT delivery flatten | `fwd_router.cpp:544` | **Removable-with-migration** | 1 flatten/COMPACT delivery | Narrow: only auto-promoted leg (not hot fan-out) + multi-link + server-side `writev` link. Neutral on WS-client (masking) / base `send(iov)` default | Scatter-gather compact encoder |
| C13 | Control-child sub-rope materialize | `fwd_router.cpp:411-424` | **Removable-with-migration** | sub-rope flatten (ADVERTISE route / COMPACT payload) | **~0 net** — each materialized sub-rope is immediately re-copied by its next consumer (`wire::encode(stripped)` / `body.insert` / store-copy). Elides one small copy by fusing into the next; total bytes moved unchanged | — |
| C14 | ESP TX gather-flatten | `httpd_ws_link.cpp:488` | **Fundamental within httpd seam** | 1 gather-copy/frame/peer | Removable **only by leaving `esp_http_server`** for raw `writev`. Double-buffer spike already gone (`:579-594`). The documented *price of threadlessness* | Abandons threadless seam; breaks `send_fn` indirection (wss/custom senders); ADR on async-send ownership |
| C15 | lwIP `NETCONN_COPY` socket copy | `sockets.c:1453,1496` | **Fundamental** (BSD socket API) | — | — (raw netconn NOCOPY rewrite unjustified for control frames) | — |
| C16 | Ingress ownership copy (rope flatten) | `rope.cpp:12` | **Fundamental** (RDMA copy-once; refuses DEVICE ropes `:13`) | — | — (the one legitimate bridge-boundary copy) | — |

---

## 4. `esp_http_server` vs raw lwIP: the threadless-vs-zero-copy tradeoff on single-core C6

The device runs graph `/ws` on `esp_http_server` (adopted mode); the Linux virtual board runs `transport_ws_server`. This split is the load-bearing decision, and it is a real tradeoff, not a wash.

### 4.1 What `esp_http_server` costs

- **TX is a forced flatten (C14).** No `writev`/iovec WS send exists; every rope reply is memcpy-collapsed into one owned heap buffer per frame per peer. The threadless model compounds it: `httpd_queue_work` is async, so even a single-span reply must be copied into *owned* heap to outlive the caller's spans. The raw server holds borrowed spans across a synchronous `writev` and copies nothing.
- **RX reassembly is strictly worse (C5).** Fragmented messages go through `asm_buf_t` (O(n²) regrow) and can only `deliver_borrowed`; the raw server chains fragments as a rope and `deliver_rope`s them.

### 4.2 What it costs *less* than folklore claims

- **RX recv is *closer*, not worse.** `esp_http_server`'s two-pass recv is **one** copy (the fundamental lwIP→buffer drain, C4). The raw path is actually **two**: `recv`→stack `chunk` (`transport_ws.cpp:357`) then `chunk`→per-session `s.buf` vector (`:404`). True recv-into-pool zero-copy is unreachable for *either* transport on BSD sockets — it needs lwIP netconn/raw pbuf-refs. On RX, `esp_http_server`'s cost is a per-frame *allocation*, not an extra copy.

### 4.3 What it buys

**Threadlessness.** Adopted mode adds **zero** FreeRTOS tasks — it rides the existing `:80` SPA httpd task. The raw `transport_ws_server` needs one dedicated ~12 KB recv-task stack. Since the SPA httpd task must grow 4 KB→12 KB (`kHttpdTaskStack=12288`) to service the in-call decode/resolve/reply/batch-apply path *either way*, the **net steady-RAM win over a raw thread is ~4 KB of stack plus a second listen socket + accept machinery avoided** — meaningful on a heap-tight single-core node. RX latency also improves (decode→resolve→reply run in-call, no cross-task handoff); only TX pays a queue hop + O(total) memcpy.

### 4.4 Net

| Axis | `esp_http_server` (device) | raw `transport_ws_server` (host) |
|------|---------------------------|----------------------------------|
| New FreeRTOS task | **none** (rides SPA httpd) | one ~12 KB recv-task stack |
| Steady RAM | **~4 KB stack + 1 socket cheaper** | dedicated stack + listen socket |
| RX copies | 1 (fundamental drain) | 2 (recv→chunk→vector) |
| RX reassembly | O(n²) `asm_buf`, borrowed | O(n) rope chain, `deliver_rope` |
| TX | **forced gather-flatten + queue hop** | zero-copy `writev`, synchronous |
| TX transient heap | per-frame-per-peer full-reply buffer | none (borrowed spans) |

The trade nets **favorably on steady RAM** and **unfavorably on TX heap transients + TX latency.** For a control-plane protocol on a RAM-tight single-core node, threadlessness is the right default — provided the TX transient spike is contained (§5, O1).

---

## 5. Optimizations ranked by RAM/latency ROI

Ranked by return per unit of risk. Each carries its concrete change and the ADR/RFC/thread-safety/DEVICE-link/portability risk.

### O1 — Build the single-core crit-section bounded pool (ADR-0060 §2) — **the unblocker**

**This is the prerequisite for O2, O3, O4.** Today three (four) allocators are disjoint: WS reassembly (`new(nothrow)`, default heap), WS/TX gather buffers (`new(nothrow)`), graph values (`value_backend_` = default `heap_backend`, `graph.cpp:293`), and the on-stack decode arena. Unify WS-rx + TX-gather + graph-values into **one `pool_t`** (`mem_pool.hpp`) injected as `value_backend_` *and* the rx/tx backend. `pool_t` is fixed-slot — exactly the rope model: a large frame becomes multiple slot-segments chained into a rope, `alloc`-or-`nullptr` = the existing backpressure/drop contract.

- **Saves:** collapses the entire fragmentation + OOM-abort surface onto one bounded arena. Does **not** remove any copy (the bytes still land in a slot) — it removes *fragmentation* and the *abort*.
- **Blocker/risk:** `value_backend_` must be thread-safe (a segment self-routes reclaim on the subscriber thread that drops the last ref, concurrent with a writer's `alloc`). The only built variant is `sync_pool_t` — a **spinlock** (`mem_pool.hpp:118-165`), **wrong for single-core RISC-V** (a lower-priority slot-holder cannot run while a higher-priority task spins → deadlock). The needed variant is the **interrupt-disable critical-section pool** (`portENTER_CRITICAL`/`taskDISABLE_INTERRUPTS` around the O(1) free-list op), also ISR-safe. Selected per-target as an ADR-0047 §2 module-set trait (host keeps `sync_pool_t`/heap; ESP gets the crit-section pool) — portability clean. **UNBUILT.**
- **ROI:** highest — it is the enabling substrate for everything below.

### O2 — Deliver WS *owning* (recv into a pooled segment) — **highest structural ROI**

**The single highest-value change.** Recv the pass-2 WS payload directly into a pooled `segment_t` (`frame.payload = seg->bytes.data()`), adopt it, and `deliver` **owning** — the exact TCP/UDP shape. The unfragmented path becomes a single-link owning rope; fragmentation becomes a zero-copy rope chain (the CAN/`ws_assembler_t` model, C5).

- **Saves:** removes the per-frame `new(nothrow)` (C4 alloc), deletes `asm_buf_t` and its O(n²) reassembly (C5), and — by feeding the rope tier an owned segment — *unblocks* the collapse of the graph.cpp flatten chain (O4) to refcount bumps.
- **Honest caveat:** the reassembly win is **narrow** — it fires only on RFC-6455 fragmented messages; the SPA sends one whole TLV per unfragmented frame (`:403-405`), so the fast path already zero-copies. Real-world byte saving on the fast path ≈ 0; the win is *fragmentation/peak-heap on fragmented load* + *structural symmetry* (httpd gains the rope tier, `delivers_ropes()→true`).
- **Blocker/risk:** needs O1 (pooled segments); needs a rope sink wired on the httpd link. DEVICE-link: none — WS RX is all-host. Thread-safety: `asm_buf`/rope-building is httpd-task-only, delivery synchronous in-call.

### O3 — Pool the ESP TX gather buffer (C14) — **contains the OOM surface**

Draw the TX gather buffer (`httpd_ws_link.cpp:488`) from the O1 bounded slab instead of `new(nothrow)`. The gather-copy **stays** (fundamental within the httpd seam, §2.2/§4), but its per-frame-per-peer allocation stops fragmenting the heap and stops feeding the OOM-abort surface.

- **Saves:** fragmentation + the documented OOM-abort trigger. **Zero copies removed.**
- **Blocker/risk:** O1. The copy *itself* is removable only by leaving `esp_http_server` for a raw `writev` socket (abandons threadlessness, breaks `send_fn` indirection for wss/custom senders, needs an ADR on async-send ownership) — **not recommended** for control-plane frames.

### O4 — `rope_cursor` branch/field decode — deletes the 4096 stack arena (C8+C9)

Point `write_branch`/`field_write` at the already-shipped `tlv_view_t`/`rope_cursor` tier — the exact move `resolve_terminus_rope` already made (`fwd_router.cpp:365-390`, the interim whole-frame flatten already deleted). Convert `arena_tlv_t.wire/body` from contiguous `std::span` to offset regions `{u32 off,u32 len}`, walk over `rope_cursor` (which already validates structure+CRC over a rope with no flatten, `rope_decode.hpp:56`), and emit each landing site as `frame_rope.subrope(off,len)`. The store/deliver side (`store_value`, `fan_out`) is **already rope-native**.

- **Saves — read carefully, this is the most over-claimed lever:**
  - The **flatten (C8)** at `graph.cpp:922`/`:1152` is **zero-copy for single-link ropes today** (`materialize` returns `links()[0]`). Since ingress values are single-link until ADR-0053 ④b (`graph.cpp:914`), the flatten is a *rarely-hit fallback* for scatter-gather-reassembled frames. Net byte saving on the common path ≈ **0**.
  - The **4096 stack arena (C9)** is the real win — but it is a **stack** win only. It backs decode structure (the `pmr::vector<arena_tlv_t>` node array + walk stack), which is byte-source-independent: a `rope_cursor` is a byte *source*, not a structure store, so an equivalent node array still exists and its bytes **relocate to the injected pool/heap**. **Net device RAM unchanged; stack high-water drops by a fixed 4096 B** — worth having because stack is the binding constraint (IDF-default 4096, bumped to 8192 after a measured overflow; this array is half the default).
  - The **#477 throwing-overflow** (`graph.cpp:924-928`) is **relocated, not resolved** — it moves from stack-slab overflow to a heap/pool spill. Its true fix is a **node-counting pre-pass** that sizes the arena from `value_backend_`, orthogonal to `rope_cursor`.
- **Blocker/risk:** ADR-0041 §2 sink-node contiguity contract (the `arena_tlv_t`/`tlv_t` span node holds contiguous bytes that cannot name a straddling payload) — the rope-aware-decode sink type is **ratification-gated** (`rope_decode.hpp:17-22` SINK NOTE). Latency: `rope_cursor::byte_at` calls `locate()`, a linear scan over links (`rope_decode.hpp:124`) — but for L=1 (single-link common case) it ≈ `span_cursor` plus a trivial constant, and against the `flatten` it replaces (O(payload) memcpy + alloc) it **wins on both latency and RAM** for multi-link. No latency-vs-RAM tradeoff.
- **Best-sequenced with a node-counting pre-pass** to actually kill #477 rather than relocate it.

### O5 — Recv-into-segment on the multi-peer TCP server (C3)

Recv into a per-session growable segment and hand frames as subviews, removing the second userspace memcpy (`length_prefix_framer.hpp:207`).

- **Saves:** 1 memcpy/frame + a stack 4 KiB chunk on the multiplexed path; latency on the multi-peer server.
- **Blocker/risk:** reframe the non-blocking multiplexed reader to size storage before the prefix is known (per-session growable segment). Thread-safety of per-session state; no ADR/RFC. Lower priority — the device's hot path is WS, not multi-peer TCP.

### O6 — `rope_cursor` node-parse + scatter-gather reply head (C10)

Convert the walk to rope-native accessors (`cur.load_le`/`for_each_span`, no whole-`wire()` materialize) and emit the reply route wires as rope links, not memcpy.

- **Saves — narrow:** the per-node flatten (`op_resolve_view.cpp:142`) fires **only when that node's own subrope straddles a link** — a node wholly inside one link is a zero-copy adopt even in a fragmented frame. On straddling *route* TLVs the flatten is currently a **redundant second copy** (flatten then memcpy into the reply head), so scatter-gather emission *merges a double-copy into one gather* rather than eliminating a whole copy. The op VALUE is 5 bytes, effectively always single-link — already free.
- **Blocker/risk:** ADR-0053 §7's single templated `resolve_node` declares `wire()`/`body()` as `std::span` (`op_resolve_walk.hpp:104-119`); rope-native accessors are the pending ADR-0053 step. No hard constraint (not DEVICE/thread/latency/http/RFC) — an architectural coupling, not a local deletion. Marginal ROI vs the already-zero-copy payload.

### O7 — Pool CAN rx + drop the ISR staging buffer (C6, C7)

Source the CAN ownership copy (`transport_can.cpp:357`) from the O1 pool instead of the global heap, and target `receive_from_isr` at `out.data` directly (drop `twai_link.cpp` `buf[8]`).

- **Saves:** fragmentation (C6, copy stays); one tiny ISR copy (C7).
- **Blocker/risk:** O1 (and the crit-section pool must be ISR-safe — it is, by design). ISR-safety of the direct target. Lowest ROI (tiny bytes) but cheap and clean.

### Non-optimizations (explicitly *not* worth doing)

- **Control-child sub-rope elision (C13):** each materialized sub-rope is immediately re-copied by its next consumer (`wire::encode(stripped)`, `body.insert`, or store-copy). Eliding the flatten *fuses one small copy into the next* — total bytes moved unchanged. **~0 net.**
- **Raw `writev` to remove the ESP TX flatten (C14):** abandons the threadless httpd seam, breaks `send_fn` indirection, and lwIP still copies at the socket boundary (C15). Unjustified for control-plane frames.
- **`LWIP_NETCONN` pbuf-wrapping `mem_backend` (destroy=`pbuf_free`, the pbuf *is* the rope link):** the *largest* theoretical lever — it collapses even the fundamental ingress copy (C1/C2/C4) — but it is **non-portable** (no pbuf on the linux virtual board) and incompatible with `esp_http_server`'s WS framing. A large platform-specific migration, not a portable win.

---

## 6. Summary of verdicts

**Optimal today:** raw single-peer TCP RX (C1), UDP RX (C2), all host TX (§2.1).

**Fundamental, keep:** the one ingress ownership copy (C16/C1/C2), the lwIP `NETCONN_COPY` socket copy (C15), the multi-link durable-store ownership flatten (C11), the WS pass-2 pbuf drain *as a copy* (C4), and — within the threadless httpd seam — the ESP TX gather-flatten (C14).

**Removable, ranked:** O2 (WS owning delivery) > O1 (the pool that unblocks it) > O4 (rope_cursor decode — *stack* win, RAM neutral) > O3 (pool the TX buffer) > O5 (multi-peer recv-into-segment) > O6 (rope-native node parse) > O7 (CAN pool).

**The one honest headline:** the flatten is a fallback, not a floor — but the two most-cited "RAM levers" (the 4096 stack arena and the WS reassembly flatten) save **stack, not net heap**, and fire on **paths the current single-link/unfragmented producers rarely hit**. The genuinely load-bearing wins are (a) delivering WS *owning* to gain the rope tier and kill per-frame allocation/fragmentation, and (b) building the single-core crit-section pool that every other win depends on — **still unbuilt (ADR-0060 §2).**

*All anchors verified against `/home/sd/prj/20260503_libtracer` at branch `feat/wireshark-dissector`: `core/src/graph.cpp:922,929,924-928,1152`; `core/src/rope.cpp:12-25`; `core/src/transport_ws.cpp:135-151,234-235`; `core/src/transport_tcp.cpp:229,430`; `core/src/fwd_router.cpp:365-390`; `core/include/libtracer/length_prefix_framer.hpp:76-79,118,207`; `integrations/esp-idf/libtracer/httpd_ws_link.cpp:121-131,332,338,485-499,579-594`.*