All citations verified against the tree. The verified-claims corrections hold up against the source: `materialize` is a refcount bump on single-link (`rope.hpp:148-151`), the 4096 arena is structure storage independent of byte-source (`graph.cpp:929`), `own_wire`/`ensure_cache` carry the real ownership/contiguity flattens (`op_resolve_view.cpp:80,135-145`), and the WS TX gather already single-buffers (`httpd_ws_link.cpp:487-498`). Here is the document.

---

# Zero-copy & the flatten question — do we really need to flatten?

**A design document for libtracer's lwIP↔rope seam and the residual `materialize` fallbacks**

> **Thesis under test:** *libtracer is rope-native zero-copy by design, and the residual flatten (`materialize`) is a `span_cursor` fallback removable by completing the `rope_cursor` migration (⑤/⑥), which also eliminates the 4096-byte on-stack decode arena (the deep-thread STACK lever).*
>
> **Verdict: proven at the design layer, sharpened at the removal layer, and refuted on one conflation.** The design *is* copy-once-at-ingress, zero-copy-forever. The residual flattens *are* `span_cursor` fallbacks — but they **do not fire on the single-link common path at all** (there `materialize` is a refcount bump, not a copy), so completing ⑤/⑥ removes copies only on *fragmented / multi-link* traffic, which no current producer generates. And the 4096 on-stack arena is **not** removed by `rope_cursor`: the arena is *structure* storage (a node array + walk stacks), byte-source-independent, so eliminating it needs a *separate* streaming decode, and even then the RAM **relocates** stack→pool — only the scarce stack high-water actually drops. Two flattens are genuinely **fundamental**: the ingress ownership copy (`rope.cpp:22`) and, inside the ESP WS seam, the TX gather (`httpd_ws_link.cpp:488`).

---

## 1. The zero-copy design

libtracer's data plane is built on an RDMA-shaped invariant: **copy each byte once, at the wire, into a refcounted owned segment; thereafter every view — routed suffix, child TLV, stored value, reply — is a refcount-bumped subview, never a byte copy.** Three mechanisms realize it.

### 1.1 Rope assembly is chaining, not memcpy

A `rope_t` is an ordered list of `view_t` links, each a windowed reference into a refcounted `segment_t`. Composition never moves payload:

- `rope_t::subrope(off, len)` (`rope.hpp:184`) trims the covering links with `view_t::subview` and **shares (refcounts) exactly the segments its window touches** — "a child TLV, a routed path suffix, or a payload handed onward is a subrope of the inbound frame, never a copy of it." A path suffix after an address-shift is a `subrope`, not a slice-copy.
- Segment handles are cloned by a **relaxed refcount increment** (`segment.hpp:124-127`), never a byte copy; release is an `acq_rel` decrement that fires the backend's `destroy` at zero (`segment.hpp:138-143`). Fan-out to N subscribers is N increments.
- `vertex_t::store` holds the rope by `shared_ptr`/refcount and **copies no bytes**; durability rests entirely on the stored segments being *owned* (long-lived), which is precisely why ingress must own (§5).

### 1.2 Decode holds structure only — spans point into the caller's input

The grammar core (`grammar.hpp` `parse_header`/`walk`) is templated over a byte-source `Cursor`. The materializing decoder `decode_into` (`tlv_arena.cpp`) emits `arena_tlv_t` nodes whose `wire`/`body` are **`std::span` into the caller's input** — "structure only, never bytes" (`tlv_arena.hpp:8-9,38-47`). Decode allocates node bookkeeping, not payload. It is therefore zero-copy over its input **by construction** — provided that input is contiguous (the span constraint that §3/§4 turns on).

### 1.3 TX is iovec scatter-gather — no egress flatten (on host)

Egress lowers a reply/forward rope straight onto `writev`/`sendmsg`. `rope_t::to_iovec` (`rope.hpp:213`) emits one `std::span` per link into the original segments. The host WS server builds `[header, link0, link1, …]` and `::sendmsg`s it with "**no flatten, no re-copy** (server frames are UNMASKED, RFC 6455 §5.1)" (`transport_ws.cpp:229`); TCP prepends a u32-LE length via `prefixed_iov_t` (`transport_tcp.cpp:53`); UDP gathers one datagram. The common reply (≤ ~6 spans) fits the stack `std::array<::iovec, kMaxServerIov+1>` (`transport_ws.cpp:237`) — zero heap, zero payload copy. **The only host TX copy is the kernel skb copy every BSD socket pays.**

### 1.4 The one ingress ownership copy

The single deliberate copy is `rope_t::flatten`'s ingress twin: `recv` moves kernel→user into an **owned** segment (`rope.cpp:11-25`, and the pull-path `read_exact` at `transport_tcp.cpp:167-171`). This copy is fundamental (§5) and correct: `flatten` refuses a non-host rope up front (`rope.cpp:13`, `all_host()` guard) because a DEVICE link is not CPU-addressable and a host memcpy would fault.

**Design verdict:** rope assembly, decode-structure, and host TX are all zero-copy as designed. The grounded facts hold. What follows is the honest accounting of *where a copy still lands*, and whether each is a fallback or a floor.

---

## 2. Every flatten point, enumerated

Each entry: the site, whether it fires on the **single-link common path** (every current TCP / unfragmented-WS producer) vs only on **multi-link** (fragmented WS / reassembled CAN) frames, the necessity verdict, and whether `rope_cursor` ⑤/⑥ removes it.

| # | Site | Fires on single-link? | Fires on multi-link? | Verdict | Removed by ⑤/⑥? |
|---|------|:--:|:--:|---------|---------|
| ① | **Ingress ownership** `rope.cpp:22` / `transport_tcp.cpp:171` | **yes** (the recv itself) | yes | **Fundamental** (as a copy). Not extra on the pull path — it *is* the recv. | No — orthogonal (it's the ingress floor) |
| ② | **Branch-write** `graph.cpp:922` `value.materialize(*value_backend_)` | **no — refcount bump** | yes (one flatten to feed span_cursor) | **Fallback.** Zero-copy today on the common path. | Multi-link leg: yes (via a rope-native branch decode) |
| ③ | **Field-write** `graph.cpp:1152` (twin of ②) | **no — refcount bump** | yes | **Fallback.** | Same as ② |
| ④ | **4096 on-stack decode arena** `graph.cpp:929` | **yes — always paid on branch writes** | yes | **Structure scratch, not a payload copy.** The real stack lever. | **No — see §3. `rope_cursor` is a byte source, not a structure store.** |
| ⑤ | **`own_wire` ownership** `op_resolve_view.cpp:80` `sub.flatten()` | no — `over_bytes(sub.only())` single copy | yes — flatten multi-link | **Fundamental** for *mutated* multi-link values (ADR-0041 §2 patchable owned segment). | No — ⑤ *is* this ownership scatter-gather; it still owns |
| ⑥ | **Per-node parse contiguity** `op_resolve_view.cpp:135-145` `ensure_cache` → `wire().materialize()` | no — single-link node = adopt | only per **straddling** node | **Fallback**, span-node-shaped. | Yes — rope-native node accessors remove it |
| ⑦ | **`deliver_rope` span fallback** `receiver_slot.hpp:138` | no (single-link hands borrowed) | yes — if no rope sink installed | **Fallback** — the honesty cost of a span-only sink. | Yes — wiring the rope sink removes it |
| ⑧ | **WS RX reassembly** `httpd_ws_link.cpp:121-127` `asm_buf` regrow-and-memcpy | no — unfragmented delivers borrowed (`:403-404`) | yes — O(n²) across fragments | **Fallback.** Rope-chaining replaces it (O(n) owning copies). | Enables ⑦'s removal; the copy itself is pool-recv, not ⑤/⑥ |
| ⑨ | **WS TX gather** `httpd_ws_link.cpp:488` `new[] + memcpy` | **yes — per frame per peer** | yes | **Fundamental within the esp_http_server seam** (async work item + contiguous-only send API). | **No — TX-side, `rope_cursor` is irrelevant.** |
| ⑩ | **COMPACT delivery** `deliver_remote` (fwd_router) | no — `materialize` adopt | yes — non-hot auto-promotion leg only | **Fallback**, narrow. | Yes — scatter-gather compact encoder |
| ⑪ | **Control-child strip** `on_control_rope` (fwd_router) | no | only multi-link ADVERTISE/COMPACT sub-rope | **Fallback**, and *fused not eliminated* — the next consumer re-copies anyway. | Yes, but near-zero saving |
| ⑫ | **Reply route synthesis** `op_resolve_walk.hpp:386` `tlv_sliced` | yes | yes | **Fundamental** (frame synthesis: the route wires are rewritten). Bounded — tens of bytes, never payload-scaled. | No — this is emitted bytes, not a copy of payload |

Reading the table: **on the single-link common path the only flattens that fire are ① (the recv floor), ④ (the structure arena), ⑨ (ESP WS TX), and ⑫ (bounded route synthesis).** ①, ⑨, ⑫ are fundamental. That leaves **④ as the one always-paid, removable cost on the common path** — and it is not a payload copy at all. Every *payload* flatten (②③⑤⑥⑦⑧⑩⑪) is multi-link-only.

This is the precise, honest form of the thesis: **the flatten is a fallback, and on the traffic the device actually sees today it does not run.** Completing ⑤/⑥ is insurance against fragmented-transport load, not a hot-path win.

---

## 3. The `graph.cpp:929` on-stack 4096 arena — the flatten's stack cost, and what removing it actually saves

This is where recon and even the analysis draft conflate two independent things. Separating them is the finding.

### 3.1 Two costs at one site, wrongly fused

```
core/src/graph.cpp:922   const view_t head = value.materialize(*value_backend_);   // Cost A: the flatten
core/src/graph.cpp:929   std::array<std::byte, 4096> stack;                        // Cost B: the arena
core/src/graph.cpp:930   std::pmr::monotonic_buffer_resource mr(stack.data(), ...);
core/src/graph.cpp:931   ... wire::decode_into(head.bytes(), mr);
```

**Cost A (the flatten, `:922`)** is zero-copy for a single-link rope — `materialize` returns `links()[0]`, a refcount bump (`rope.hpp:148-151`) — and memcpys only a **multi-link** rope. The in-code comment is explicit: *"ingress values are single-link until ④b … a multi-link POINT pays one flatten here (the interim until the ④b rope-cursor decode)."* Because no current producer emits multi-link branch values, **Cost A does not fire today.** It is a genuine fallback.

**Cost B (the arena, `:929`)** is the `std::array<std::byte,4096>` backing `decode_into`'s **node array** (`std::pmr::vector<arena_tlv_t>`, ~48 B/node ⇒ ~80 nodes), plus the grammar walk stack and open-node stack. It is **structure-only scratch, allocated on every branch write regardless of link count**, on the deepest thread (the httpd/WS receive task).

### 3.2 Why `rope_cursor` alone does **not** remove the arena

This is the refutation the thesis needs. `rope_cursor` (`rope_decode.hpp:56`) is a **byte source** — it lets the grammar read fields off a scatter-gather rope by stitching straddling headers a byte at a time and feeding CRC link-by-link. It satisfies the identical `Cursor` concept as `span_cursor`. But `decode_into` does not just *read* bytes — it *stores structure*: a random-accessible `arena_tlv_t` array that `parse_branch_node` walks via `a[i].end` / `next_sibling` / `first_child` (`graph.cpp:189+`). That node array is **byte-source-independent**: swapping `span_cursor` for `rope_cursor` changes where field bytes come from, not that a node array + walk stacks must exist. The `rope_decode.hpp:17-21` SINK NOTE says exactly this: it "validates STRUCTURE + CRC over a rope; **it does not yet materialize a rope frame into a tlv_t / arena node**, because both sink node types hold a borrowed contiguous `std::span` that cannot name a straddling payload (ADR-0041 §2)."

So the migration order is really two moves, not one:

1. **Rope-native node type** (the ratification-gated sink type): convert `arena_tlv_t.wire/body` from `std::span` to an offset region `{u32 off, u32 len}` and have `parse_branch_node` emit `frame_rope.subrope(off,len)`. This removes **Cost A** for multi-link frames — but the node array still exists and still needs backing storage.
2. **Streaming decode** (a *separate* rewrite): drive `parse_branch_node` off `grammar::walk` post-order callbacks (open/leaf/close), never materializing the node array. **Only this removes Cost B's on-stack 4096.**

`rope_cursor` is a *precondition* for move 1, and irrelevant to move 2.

### 3.3 What removing the arena actually saves: stack, not heap

Even move 2 does not *eliminate* the RAM — it **relocates** it. The node array + walk stacks must live somewhere during decode; taking them off `stack.data()` pushes those transient bytes into the injected pool/heap. **Net device RAM is unchanged.** What drops is the **scarce stack high-water**: a flat 4096 bytes (a `std::array` reserves its whole frame slot whether filled or not) off the deep receive task. On a single-core C6 where the httpd task's IDF-default stack was 4096 — bumped to 8192 (`kHttpdTaskStack=12288` on the adopted path) after a *measured* overflow — this array is a full half of the original default and the single largest consumer on the write path. **Stack is the binding constraint, so a 4 KB stack-high-water drop is the real win** even though total RAM is flat.

### 3.4 The #477 throwing-overflow leg relocates, it does not resolve

The arena's overflow draws from the monotonic's **throwing** default upstream (`graph.cpp:924-928`) — an `abort()` under `-fno-exceptions`, the one leg not on the injected-resource soft-fail path. Streaming the decode moves this from an inline-slot-overflow-to-heap into a heap-drawn walk stack — **it relocates the failure mode, it does not fix it.** The code comment names the real fix: *"converting the overflow leg needs a node-counting pre-pass"* — size the arena from `value_backend_` after counting nodes, so exhaustion soft-fails as BACKPRESSURE like the payload path already does (`graph.cpp:923`). This is **orthogonal to `rope_cursor`.**

**Section verdict:** the thesis clause "*which also eliminates the 4096 on-stack decode arena*" is **half true, and for the wrong reason.** `rope_cursor` removes the multi-link *flatten* (Cost A). The *arena* (Cost B) needs a streaming decode, which relocates RAM stack→pool, netting only — but genuinely — a 4 KB stack-high-water reclaim. The #477 abort needs a node-counting pre-pass, not either.

---

## 4. The `rope_cursor` migration — what's left, and the latency of walking a rope

### 4.1 What already shipped

`rope_cursor` is not hypothetical. It drives **three live rope-native consumers with no flatten**: `validate_rope`/`check_frame` (`rope_decode.cpp`), the lazy `tlv_view_t` tier (children walked one header at a time off a refcounted subrope), and `on_control_rope`/`peek_control` (fwd_router). Crucially, **the FWD request terminus already migrated**: `resolve_terminus_rope` (`fwd_router.hpp:289-324`) adopts a fragmented request as `tlv_view_t::over(rope)` and resolves through `op_resolver_t::resolve(tlv_view_t)` — "*the interim flatten this replaces is deleted.*" The forward hop already scatter-gathers a multi-link frame over the rope cursor (ADR-0053 ④b) with no flatten. **⑤ (scatter-gather reply/store) and ⑥ (rope-native control + terminus request) are substantially done; the header doc calling the sink an unfinished "⑤/⑥ follow-on" (`fwd_router.hpp:268-270`) now *lags* the code and should be corrected.**

### 4.2 What is genuinely left

1. **Rope-native branch/field decode** (§3 move 1): point `write_branch`/`field_write` at a rope-aware node type instead of `decode_into`+`materialize`. Removes ②③.
2. **Streaming branch decode** (§3 move 2): removes ④'s on-stack arena.
3. **Rope-native node accessors** for the walk (⑥): `ensure_cache` (`op_resolve_view.cpp:135-145`) flattens each *accessed* node whose own subrope straddles a link. Convert `wire()`/`body()` from `std::span` to rope-native readers (fields via `cur.load_le` / `for_each_span`) plus a scatter-gather reply head (route wires emitted as rope links, not `tlv_sliced` memcpy). Removes ⑥.

Note what is **not** removed by any of these: **⑤ `own_wire` (`op_resolve_view.cpp:80`) is fundamental** — a *mutated* multi-link value must own a contiguous, patchable, trailer-cleared segment (ADR-0041 §2), so it flattens once. The zero-copy `pin_wire` subrope applies **only** to the opt-in *verbatim referenced STORE*, not to ops that patch bytes. ⑤/⑥ *is* the ownership scatter-gather; it stores its one ownership copy by design.

### 4.3 Latency: walking a rope vs a contiguous buffer — a win, not a tradeoff

The prompt asks for the latency-vs-RAM tradeoff of a rope walk. **On the common case there is no tradeoff — the rope walk is strictly better on both axes.**

- `span_cursor::byte_at` is O(1); `rope_cursor::byte_at` calls `locate()`, a **linear scan over links** (`rope_decode.hpp`), so `load_le(n)` is `n` such scans and a header read costs `O(header_bytes × L)`, a CRC feed `O(payload + L)`.
- For a **single-link rope** — the overwhelming common case (one recv chunk → one ingress segment, `materialize` returns `links()[0]`) — `L = 1`, so `rope_cursor ≈ span_cursor` plus a trivial constant. No regression.
- For a **multi-link rope**, the header pointer-chase is bounded by fragment count × a 4–6-byte header; the payload feed is asymptotically identical to the memcpy it replaces **but read-only and without allocating a destination.**
- Against the alternative it removes — `flatten()` = O(payload) read **and** write memcpy **plus** a pool/stack allocation — the rope walk wins on **latency** (no write pass, no alloc) **and** **RAM** (no destination buffer). 

**There is no latency-vs-RAM tradeoff for the decode walk.** The only genuine cost is code size / instantiation (`rope_cursor` is a separate TU so span-only MCUs never link it — ADR-0048 §1), which is a build-configuration trait, not a runtime tradeoff.

---

## 5. The ingress copy — fundamental, or removable via pool-recv?

The ingress ownership copy (①) is **fundamental as a copy, but not necessarily an *extra* copy.**

**Why it must own.** `store` refcounts rather than copies (§1.1), so durability rests on stored segments being *owned* (long-lived). The recv buffer is transient: on the multi-peer TCP server a `std::array<std::byte,4096>` stack scratch (`transport_tcp.cpp:187`); on ESP WS a per-receive `new(nothrow) std::byte[frame.len]` (`httpd_ws_link.cpp:332`). Both die when the receive call returns, while the rope outlives them (pinned in an LKV, fanned out asynchronously, awaited). A borrowed view of that buffer would dangle. **So ingress must land bytes in an owned segment — that is a copy, and it is fundamental.**

**Why it is not extra on the pull path.** The single-peer TCP pull path recv's **straight into the owned segment**: `read_exact(fd, seg->bytes.data(), len)` (`transport_tcp.cpp:167-171`) fills a segment freshly `alloc`'d from the injected backend. **A pooled recv buffer is already adopted as the owned segment** — the RDMA ideal, realized. Ingress here is one *kernel* copy and *zero* user-space copies. `length_prefix_framer` even documents the trade: feeding chunks through `feed()` "*would add a scratch-buffer copy on the hot path*," so the pull loop shares framing *rules*, not the state machine.

**The removable waste is where the pull-path shape is *not* followed** — all pool-recv migrations, none of them flatten problems:

- **Multi-peer TCP server** pays a *second* user-space memcpy (kernel→stack chunk → segment) because a non-blocking multiplexed reader cannot size the segment before decoding the length prefix. This is the tax of the #362 single-poll-thread MCU shape; removable only by reverting to thread-per-peer (rejected on single-core) or a per-session growable segment.
- **ESP WS delivers borrowed** (`httpd_ws_link.cpp:467-469`), forcing a downstream ownership copy at store. Removal: land the payload in the bounded rx pool, adopt it, deliver **owning** (the TCP/UDP shape). This is the single highest-value RX change — it removes the per-frame `new[]` (⑧'s alloc), and by feeding the rope tier an owned segment it lets the whole branch/field decode collapse to refcount bumps *once the sink is rope-native*.
- **WS reassembly (⑧)** regrows O(n²); rope-chaining each fragment as an owning link makes it O(n) owning copies (the CAN model, and exactly what host `transport_ws.cpp` already does).

**The DEVICE-link constraint never conflicts with pool-recv:** recv targets are always HOST memory, so a host pool slot is a legal recv/adopt target; only a CPU-side *flatten* of a DEVICE link faults, which `flatten` refuses up front (`rope.cpp:13`).

**Can even the ownership copy go?** Only with `LWIP_NETCONN` + a pbuf-wrapping `mem_backend` (`destroy = pbuf_free`, the pbuf *is* the rope link). That collapses ①, but it is **non-portable** (no pbuf on the Linux virtual board) and **incompatible with esp_http_server's WS framing** — a large platform-specific migration, not a portable win. **For a BSD-socket API, ① stays.**

**The blocker for every pool-recv fix is the same and it is unbuilt:** a segment self-routes reclaim on the subscriber thread that drops the last ref, concurrent with a writer's `alloc`, so `value_backend_`/rx-backend must be thread-safe. The only built pool variant is `sync_pool_t`, a **spinlock** — wrong for single-core RISC-V (a lower-priority slot-holder cannot run while a higher-priority task spins). The needed variant is the **interrupt-disable critical-section pool** (ADR-0060 §2), selected per-target as an ADR-0047 §2 module-set trait (host keeps `sync_pool_t`/heap; ESP gets the crit-section pool). **UNBUILT.**

---

## 6. Ranked removal plan

Ranked by **value on the traffic the device sees today** (single-link, single-core, RAM-tight C6), with the honest saving and the gating risk.

| Rank | Change | Removes | Real saving | Fires on common path? | Gate / risk |
|:--:|--------|---------|-------------|:--:|-------------|
| **1** | **Streaming branch decode** (walk-callback driven, no node array) | ④ on-stack 4096 arena | **~4 KB stack high-water** off the deepest task (the binding constraint). RAM relocates stack→pool, not eliminated. | **yes — always paid** | Latency (walk-stack now heap/pool-drawn); does **not** fix #477 |
| **2** | **Node-counting pre-pass** to size the arena from `value_backend_` | #477 throwing-overflow abort | Turns an `abort()` into a BACKPRESSURE soft-fail — robustness, not bytes | yes (overflow leg) | Orthogonal to rope_cursor; small, high-value |
| **3** | **Single-core crit-section bounded pool** (ADR-0060 §2) | *prerequisite* for 4–6 | Unblocks all pool-recv; unifies WS-rx + graph-values + arena into one slab (defrag + OOM-abort collapse) | — | Thread-safety correctness; per-target module-set trait |
| **4** | **WS recv into pooled segment, deliver owning** | ⑧ per-frame `new[]`; borrowed-then-store copy | Removes the ~27 KB/session alloc/frag class; feeds rope tier | yes (every WS frame) | Needs #3; ADR note on WS delivery contract |
| **5** | **Rope-native branch/field node type** (⑤/⑥ sink) | ②③ multi-link branch/field flatten | Zero on single-link (already a refcount bump); one flatten→refcount on fragmented POINT writes | **no — multi-link only** | Ratification-gated (ADR-0041 §2 sink-type); needs #1 to matter |
| **6** | **Rope-native walk accessors + scatter-gather reply head** | ⑥ per-straddling-node `ensure_cache` | On straddling route TLVs, fuses flatten+memcpy into one gather; tiny selector/NAME flattens gone | **no — multi-link only** | Converts the shared span-based `resolve_node` concept — non-local |
| **7** | **Rope-chaining WS reassembler** (host `ws_assembler_t` shape) | ⑧ O(n²) regrow | O(n²)→O(n) owning copies; heap-peak drop (no 2×n transient). *Not* zero-copy; frag may worsen (k small segs). | **no — fragmented only** | Needs rope sink wired (#4/⑤) or ⑦ re-materializes |
| **8** | **Scatter-gather compact encoder** | ⑩ COMPACT delivery flatten | Real only on server-side `writev` links, non-hot auto-promotion leg; neutral on masked WS client | no | Low value |
| — | **`LWIP_NETCONN` pbuf-as-rope-link** | ① the ingress ownership copy | Collapses even the fundamental copy | yes | **Non-portable** (no pbuf on Linux board), incompatible with esp_http_server framing — large, platform-specific |
| — | **ESP WS TX raw `::sendmsg`** (leave esp_http_server) | ⑨ TX gather flatten | Removes one full-payload alloc+memcpy per frame per peer; kills the co-resident heap spike blamed for the OOM abort | yes | Abandons the threadless SPA-httpd seam; breaks `send_fn` indirection (wss/custom senders); lwIP still copies at the socket |

### What is genuinely fundamental (do not chase)

- **① the ingress ownership copy** — a transient recv buffer cannot be borrowed by an outliving rope. On the pull path it already *is* the recv (one kernel copy, zero user-space). Removable only by non-portable `LWIP_NETCONN`.
- **⑤ `own_wire`** — a *mutated* multi-link value must own a patchable contiguous segment (ADR-0041 §2). ⑤/⑥ is this ownership copy, not its removal.
- **⑨ the ESP WS TX gather** — `httpd_ws_send_frame_async` takes one contiguous `{payload,len}` and `httpd_queue_work` is async (the rope links die before the send runs), so the reply *must* be flattened into one owned buffer. The `send(iov)` override (`httpd_ws_link.cpp:476-498`) already gathers **once** straight into the tx work buffer — the double-buffer spike is already gone; one irreducible gather-copy remains. It is the documented **price of threadlessness** (riding the `:80` SPA httpd task, zero new FreeRTOS task), removable only by leaving the seam. **`rope_cursor` is irrelevant to it — the sink is a hardware send API, not a decoder.**
- **⑫ reply-route synthesis** — bounded frame synthesis (rewritten routes, tens of bytes), never payload-scaled.

### The one-line answer to "do we really need to flatten?"

**On the traffic the device runs today, no payload flatten fires at all** — `materialize` is a refcount bump on every single-link value, TX scatter-gathers on host, decode holds structure only. The flattens that *do* fire are ① (the recv floor, fundamental), ⑨ (ESP WS TX, fundamental within the seam), and ④ (the structure arena — the one removable always-paid cost, and it is *stack scratch, not a payload copy*). Everything else is a **multi-link fallback** that completing ⑤/⑥ converts to refcount bumps — real insurance for fragmented-transport / CAN load, but **not** a hot-path win, and **not** the same lever as reclaiming the 4 KB stack. The thesis is right that the flatten is a fallback; the precision it was missing is that **the biggest single lever (the 4096 stack) is unlocked by a streaming decode, not by `rope_cursor`, and it relocates RAM rather than eliminating it — the win is stack high-water, which on this device is exactly the RAM that matters.**

---

*File anchors verified against the tree: `core/src/graph.cpp:905-935,1145-1160`; `core/src/rope.cpp:11-25`; `core/include/libtracer/rope.hpp:140-215`; `core/include/libtracer/segment.hpp:124-143`; `core/src/op_resolve_view.cpp:70-105,135-145`; `core/include/libtracer/rope_decode.hpp:10-25,50-60`; `core/include/libtracer/receiver_slot.hpp:130-141`; `core/include/libtracer/fwd_router.hpp:265-324`; `core/src/transport_ws.cpp:135,229,237`; `core/src/transport_tcp.cpp:53,167-200`; `integrations/esp-idf/libtracer/httpd_ws_link.cpp:45,55,102-127,319-338,401-421,467-498,556`.*