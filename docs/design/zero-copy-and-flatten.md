# Zero-copy and the residual flattens

Scope: this page describes the reference C++ implementation's own copy behaviour — every
`materialize` / `flatten` call site in `graph.cpp`, `rope.cpp`, `op_resolve_view.cpp`,
`op_resolve_walk.hpp`, `fwd_reply.cpp`, `fwd_router.cpp` and the ESP-IDF WS integration, and
which of them are
structural. It is not the protocol standard; the implementation-independent ownership model is
[reference 08 — views and ownership](../reference/08-views-and-ownership.md) and
[reference 09 — memory substrate](../reference/09-memory-substrate.md). Its ESP-IDF / lwIP
references are integration facts about the transport seam.

## Summary

On single-link traffic — one recv chunk becoming one ingress segment, which is what every
unfragmented TCP, UDP and WS producer emits — **no payload flatten fires.** `rope_t::materialize`
returns the sole link (`core/include/libtracer/rope.hpp:180`), a refcount bump, so the branch
write, the field write, the per-node parse cache, span-only delivery and COMPACT remote delivery
all resolve to reference counting rather than `memcpy`. Every payload flatten in the codebase is a
multi-link fallback.

Three copies are structural — they exist because of what the code must guarantee, not because a
byte source is the wrong shape:

| Structural copy | Site | Why it cannot go |
|---|---|---|
| Ingress ownership | `rope_t::flatten` (`core/src/rope.cpp:22`), `read_exact` into the accepted segment (`core/src/transport_tcp.cpp:266`) | A transient recv buffer cannot be borrowed by a rope that outlives the receive call |
| Mutation ownership | `own_wire` (`core/src/op_resolve_view.cpp:136`) | A mutated multi-link value must own a contiguous, patchable, trailer-cleared segment |
| WS TX gather | `httpd_ws_link_t::queue_send` (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`; destination is a pre-allocated tx work slot — no slot free means a counted drop, not a heap item) | `httpd_ws_send_frame_async` takes one contiguous buffer and `httpd_queue_work` runs later, after the rope links are gone |

A fourth copy is bounded rather than structural: reply-route synthesis (`tlv_sliced`,
`core/src/fwd_reply.hpp:98`) emits rewritten route wires — tens of bytes, never
payload-scaled.

The 4096-byte decode arena (`core/src/graph.cpp:1269`) is **structure storage, not a payload copy**:
it backs the `arena_tlv_t` node array and the grammar walk stacks, whose existence is independent
of where the field bytes come from. The rope cursor is a byte source, so it does not remove the
arena; only a streaming decode does, and that **relocates** the bytes from stack to pool rather
than eliminating them. Net device RAM is unchanged; what drops is the scarce stack high-water.

---

## 1. Ownership invariant

The data plane copies each byte once, at the wire, into a refcounted owned segment; thereafter
every view — routed suffix, child TLV, stored value, reply — is a refcount-bumped subview. The
model itself is described implementation-independently in
[reference 08](../reference/08-views-and-ownership.md). What is specific to this implementation is
where the mechanism lives:

- **Composition shares segments.** `rope_t::subrope(off, len)`
  (`core/include/libtracer/rope.hpp:216`) trims the covering links with `view_t::subview` and
  refcounts exactly the segments its window touches. Segment handles clone by a relaxed increment
  (`core/include/libtracer/segment.hpp:124-126`); release is an `acq_rel` decrement that fires the
  backend's `destroy` at zero (`:137-141`). Fan-out to N subscribers is N increments.
- **Decode holds structure only.** `decode_into` emits `arena_tlv_t` nodes whose `wire` / `body`
  are `std::span` into the caller's input — "the arena holds structure only, never bytes"
  (`core/include/libtracer/tlv_arena.hpp:8-9`, node type at `:30`). Decode allocates node
  bookkeeping, never payload, and is zero-copy over its input provided that input is contiguous.
  That contiguity constraint is what §3 and §4 turn on.
- **Egress scatter-gathers.** `rope_t::to_iovec` (`core/include/libtracer/rope.hpp:245`) emits one
  span per link into the original segments. The host WS server builds `[header, link0, link1, …]`
  and `sendmsg`s it with "no flatten, no re-copy (server frames are UNMASKED, RFC 6455 §5.1)"
  (`core/src/transport_ws.cpp:264`); TCP prepends a u32-LE length via `prefixed_iov_t`
  (`core/src/transport_tcp.cpp:56`). With `kMaxServerIov = 16` (`core/src/transport_ws.cpp:150`),
  the common reply (≤ ~6 spans) fits the stack `std::array<::iovec, kMaxServerIov + 1>`
  (`core/src/transport_ws.cpp:272`) — zero heap, zero payload copy. The only host TX copy is the
  kernel skb copy every BSD socket pays.
- **Flatten refuses a heterogeneous rope.** A DEVICE link is not CPU-addressable, so a host memcpy
  would fault; `flatten` checks `all_host()` up front and returns an empty view
  (`core/src/rope.cpp:15`).

---

## 2. Flatten call sites

Each row: the site, whether it fires on the **single-link** path (every unfragmented producer) or
only on **multi-link** frames (fragmented WS, reassembled CAN), whether it is structural or a
fallback, and whether the rope-cursor migration removes it. The circled numerals are row
identifiers for the rest of this page.

| # | Site | Single-link? | Multi-link? | Kind | Removed by the rope cursor? |
|---|------|:--:|:--:|---------|---------|
| ① | Ingress ownership — `flatten` (`core/src/rope.cpp:22`), pull-path `read_exact` into the accepted segment (`core/src/transport_tcp.cpp:266`) | yes (it *is* the recv) | yes | Structural | No — orthogonal; it is the ingress floor |
| ② | Branch write — `value.materialize(*value_backend_)` (`core/src/graph.cpp:1256`) | no — refcount bump | yes (one flatten to feed the span cursor) | Fallback | Multi-link leg: yes, via a rope-native branch decode |
| ③ | Field write — the twin of ② (`core/src/graph.cpp:1502`) | no — refcount bump | yes | Fallback | Same as ② |
| ④ | 4096-byte decode arena (`core/src/graph.cpp:1269-1270`) | yes — paid on every branch write | yes | Structure scratch, not a payload copy | **No** — see §3; the rope cursor is a byte source, not a structure store |
| ⑤ | `own_wire` mutation ownership — `sub.flatten(backend())` (`core/src/op_resolve_view.cpp:136`) | no — a single link is still COPIED, through the same backend (`core/src/op_resolve_view.cpp:146`, #793) | yes — flattens the multi-link subrope | Structural for *mutated* values | No — this step *is* the ownership copy; it still owns |
| ⑥ | Per-node parse contiguity — `ensure_cache` → `wire().materialize(backend())` (`core/src/op_resolve_view.cpp:248-254`) | no — a single-link node adopts | only per **straddling** node | Fallback, span-node-shaped | Yes — rope-native node accessors remove it |
| ⑦ | `deliver_rope` span fallback (`core/include/libtracer/receiver_slot.hpp:138`) | no | yes — only when no rope sink is installed | Fallback — the cost of a span-only sink | Yes — installing the rope sink removes it; see §4.1 |
| ⑧ | WS RX reassembly — `asm_buf_t` regrow-and-memcpy (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) | no — unfragmented delivers borrowed (scratch-backed, no per-frame alloc for fitting frames) | yes — O(n²) across fragments | Fallback | Enables ⑦'s removal; the copy itself is a pool-recv question, not a cursor one |
| ⑨ | WS TX gather — memcpy into a pooled tx work slot in `queue_send` (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`; `new (nothrow)` only for an oversize payload — an exhausted pool drops and counts, #949) | copy per frame per peer; alloc only on the oversize arm | yes | Structural within the `esp_http_server` seam | **No** — TX-side; the cursor is irrelevant |
| ⑩ | COMPACT remote delivery — `deliver_remote` (`core/src/fwd_router.cpp:1895`, materialize at `:1929`, from the router's injected backend) | no — adopt | yes — auto-promotion leg only | Fallback, narrow | Yes — a scatter-gather compact encoder |
| ⑪ | Control-child strip — `on_control_rope` (`core/src/fwd_router.cpp:1541`, sub-rope materialize at `:1552` and `:1525`, from the router's injected backend) | no | only a multi-link ADVERTISE / COMPACT sub-rope | Fallback, and fused rather than eliminated — the next consumer re-encodes anyway | Yes, with a near-zero saving |
| ⑫ | Reply-route synthesis — `tlv_sliced` (`core/src/fwd_reply.hpp:98`, called at `core/src/fwd_reply.cpp:113-114`) | yes | yes | Bounded frame synthesis: the route wires are rewritten | No — these are emitted bytes, not a copy of payload |

On the single-link path the only copies that fire are ① (the recv floor), ④ (the structure arena),
⑨ (the `esp_http_server` WS TX gather) and ⑫ (bounded route synthesis). ①, ⑨ and ⑫ are structural
or bounded, which leaves **④ as the one always-paid removable cost on that path — and it is not a
payload copy.** Every payload flatten (②③⑤⑥⑦⑧⑩⑪) is multi-link-only. Completing the rope-cursor
migration is therefore insurance against fragmented-transport load, not a single-link win.

---

## 3. The 4096-byte decode arena

### 3.1 Two costs at one site

```
core/src/graph.cpp:1256   const view_t head = value.materialize(*value_backend_);   // A: the flatten
core/src/graph.cpp:1257   if (head.empty() && value.total_length() != 0) return std::unexpected(status_t::BACKPRESSURE);
core/src/graph.cpp:1269   std::array<std::byte, 4096> stack;                        // B: the arena
core/src/graph.cpp:1270   mem::bump_source_t src(stack, *ctl_);
core/src/graph.cpp:1272   wire::decode_into(head.bytes(), src);
```

**Cost A, the flatten (`:1252`)** is zero-copy for a single-link rope — `materialize` returns
`links()[0]`, a refcount bump (`core/include/libtracer/rope.hpp:180`) — and memcpys only a
multi-link rope, drawing from the injected `value_backend_`. An exhausted pool yields an empty
head, which `graph.cpp:1257` surfaces as `BACKPRESSURE` rather than letting the decoder read it back as a
malformed value. Because ingress values are single-link until the rope-native branch decode lands,
Cost A does not fire on single-link traffic. It is a fallback.

**Cost B, the arena (`graph.cpp:1269`)** is the `std::array<std::byte, 4096>` backing `decode_into`'s node
array (`std::pmr::vector<arena_tlv_t>`) plus the grammar walk stack and the open-node stack. It is
structure-only scratch, allocated on every branch write regardless of link count, on the deepest
thread — the httpd/WS receive task.

`sizeof(arena_tlv_t)` is **48 bytes on an LP64 host** (compiled against
`core/include/libtracer/tlv_arena.hpp:38-65`; the two `std::span` members dominate at 16 bytes
each), so 4096 bytes holds roughly 85 nodes before the node vector alone exhausts the slab, and
fewer once the walk stacks take their share. On a 32-bit target both spans halve and the node is
correspondingly smaller; that figure has not been compiled here and is not asserted.

### 3.2 Why the rope cursor does not remove the arena

`rope_cursor` (`core/include/libtracer/rope_decode.hpp:57`) is a **byte source**. It lets the
grammar read fields off a scatter-gather rope by stitching straddling headers a byte at a time and
feeding the CRC link by link, satisfying the same `Cursor` concept as `span_cursor`. But
`decode_into` does not only read bytes — it stores structure: a random-accessible `arena_tlv_t`
array that `parse_branch_node` (`core/src/graph.cpp:246`) walks via `end` / `first_child`. That
node array is byte-source-independent. Swapping `span_cursor` for `rope_cursor` changes where field
bytes come from, not the fact that a node array and walk stacks must exist.

`core/include/libtracer/rope_decode.hpp:17` states the same constraint from the decoder's side:

> SINK NOTE: this validates STRUCTURE + CRC over a rope; it does not yet materialize a rope frame
> into a tlv_t / arena node, because both sink node types hold a borrowed contiguous std::span that
> cannot name a straddling payload (ADR-0041 §2). Producing sink nodes from a rope is the
> ratification-gated follow-on (the rope-aware-decode sink-type proposal).

So the arena and the flatten are two moves, not one:

1. **A rope-native node type** — the ratification-gated sink type of
   [ADR-0052, rope-aware decode sink node type](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0052-rope-aware-decode-sink-node-type.md):
   convert `arena_tlv_t.wire` / `body` from `std::span` to an offset region `{u32 off, u32 len}` and
   have `parse_branch_node` emit `frame_rope.subrope(off, len)`. This removes **Cost A** for
   multi-link frames. The node array still exists and still needs backing storage.
2. **A streaming decode** — a separate rewrite driving `parse_branch_node` off `grammar::walk`
   post-order callbacks (open / leaf / close), never materializing the node array. Only this removes
   **Cost B**.

The rope cursor is a precondition for move 1 and irrelevant to move 2.

### 3.3 Stack, not heap

Move 2 does not eliminate the RAM; it **relocates** it. The node array and walk stacks must live
somewhere during decode, and taking them off `stack.data()` pushes those transient bytes into the
injected pool or heap. **Net device RAM is unchanged.** What drops is the scarce stack high-water:
a flat 4096 bytes — a `std::array` reserves its whole frame slot whether filled or not — off the
deep receive task.

A stack budget for that task counts four such buffers, not one. The decode arena is the only one
this document covers; the other three are transport receive and chunk scratch, each a 4096-byte
`std::array` — `core/src/transport_tcp.cpp:224` (the backpressure drain),
`core/src/transport_ws.cpp:629` (the WS client's receive loop), and
`core/src/posix_endpoint.cpp:442` — the ONE per-chunk scratch both multi-peer servers now
share, since #871 folded their duplicated poll loops into `slot_server_t::service_peer` (it
was two buffers, one apiece, before that). They are not decode arenas and carry no structure,
but they occupy the same frames and none of the four has a measured per-task high-water.

That receive task is the binding constraint on a single-core, RAM-constrained node. In the ESP-IDF
WS integration, servicing a graph request in-call was measured overflowing an 8 KB stack and needing
~12 KB on the raw WS receive thread, against the 4 KB `esp_http_server` default; the integration
therefore sizes the task at `kRequiredHttpdStack = 12288`
(`integrations/esp-idf/libtracer/include/libtracer_esp/httpd_ws_link.hpp:160` — a PUBLIC constant
since #955, because only the port-binding ctor can apply it and an adopting embedder must size the
task itself; the 8 KB the deep path was measured overflowing is the other half of the same
measurement, at `integrations/esp-idf/libtracer/httpd_ws_link.cpp:71`, and the 4 KB platform
default is named at
`integrations/esp-idf/libtracer/include/libtracer_esp/httpd_ws_link.hpp:52`). Against a 4096-byte
default the arena is a full half of the frame, and it is the single largest consumer on the write
path. A 4 KB stack-high-water reclaim is the real saving even though total RAM is flat.

### 3.4 Arena exhaustion

The overflow leg does not draw from a throwing upstream. `core/src/graph.cpp:1269-1270` reads

```
std::array<std::byte, 4096> stack;
mem::bump_source_t src(stack, *ctl_);
```

`bump_source_t` (`core/include/libtracer/mem_source.hpp:184`) carves from that stack buffer and,
past it, falls back to the graph's injected control seam `ctl_`
(`core/include/libtracer/graph.hpp:330`, `control_source()`), whose default is the NOTHROW heap
source. Capability is unchanged — a branch tree larger than the slab still decodes — and
**exhaustion is a value, not an abort**: the write soft-fails as `TYPE_MISMATCH`
(`core/src/graph.cpp:1273`), which is *not* `BACKPRESSURE` — this decode cannot distinguish "the
value did not parse" from "the arena ran out" and does not try, so the block seam's reject belongs
to the operation rather than to the seam. A bounded node that injects its own control source gets
the arena overflow drawn from that store too. No node-counting pre-pass exists, and none is
needed ([#477], [#588]).

Streaming the decode (§3.2 move 2) changes where the walk stack is drawn from, not whether
exhaustion is representable. The general failable-allocation contract is
[failable allocation and backpressure](allocation-and-backpressure.md).

---

## 4. The rope cursor

### 4.1 Rope-native consumers

`rope_cursor` drives four live consumers with no flatten:

- `check_frame` and `validate_rope` (`core/src/rope_decode.cpp:32`, `:46`) validate structure and
  CRC straight over a rope.
- The lazy `tlv_view_t` tier walks children one header at a time off a refcounted subrope.
- `on_control_rope` / `peek_control` (`core/src/fwd_router.cpp:1541`, `:1492`) read a control frame's
  label off the rope and materialize only the sub-rope a re-encoding consumer needs contiguous
  (`:1552`, `:1525`) — out of the router's injected `flat` backend, and a refused flatten drops the
  frame rather than delivering an empty value (#730).
- The FWD request terminus: `resolve_terminus_rope`
  (`core/include/libtracer/fwd_router.hpp:786-794`) adopts a fragmented request as
  `tlv_view_t::over(rope)` and resolves it through `op_resolver_t::resolve(tlv_view_t)`.

The forward hop scatter-gathers a multi-link frame over the rope cursor with no flatten; the egress
gathers each region's per-link sub-spans into a `block_array_t` drawn from the injected `rx_`, and
exhaustion drops the frame rather than throwing (`core/include/libtracer/fwd_router.hpp:838-846`,
[#596]).

Two limits on that tier are load-bearing. First, **a single-link rope never reaches
`resolve_terminus_rope`** — `on_frame_rope_impl` short-circuits it deliberately into the
single-link view path (`core/src/fwd_router.cpp:1181`, the check at `:1187-1192`). Second, the tier earns its place only on large, lightly
fragmented frames: at 64 KB across 2 links it is ~12% ahead of flatten-then-arena, and behind it
everywhere smaller (`core/include/libtracer/fwd_router.hpp:767-770`, recorded as an erratum to
[ADR-0053, lazy rope-backed decode view](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md)).
That figure carries no host, sample count or spread in the source that records it, so it is a
direction, not a budget.

Row ⑦ has changed shape rather than disappearing. The span fallback in
`receiver_slot_t::deliver_rope` (`core/include/libtracer/receiver_slot.hpp:138`) still exists for a
slot with no rope sink installed, but the router does not flatten on the reply path: a REPLY that
reaches its originator is handed to the sink rope-native
(`core/src/fwd_router.cpp:1230-1234`). The contract at
`core/include/libtracer/fwd_router.hpp:418-422` states it — the router performs no decode and no
flatten, a rope-delivered reply reaches the sink zero-copy, a sink that wants contiguous bytes
holds `const view_t m = reply.materialize()`, and only a multi-link reply pays one flatten, on
demand. The escape hatch is the consumer's, not the router's.

### 4.2 Sites the rope cursor does not reach

1. **Rope-native branch and field decode** (§3.2 move 1) — point the branch and field write paths at
   a rope-aware node type instead of `decode_into` + `materialize`. Removes ②③.
2. **Streaming branch decode** (§3.2 move 2) — removes ④'s on-stack arena.
3. **Rope-native node accessors for the walk** — `ensure_cache`
   (`core/src/op_resolve_view.cpp:248-254`) flattens each *accessed* node whose own subrope
   straddles a link. Converting `wire()` / `body()` from `std::span` to rope-native readers (fields
   via `load_le` / `for_each_span`) plus a scatter-gather reply head removes ⑥.

None of these reaches ⑤. `own_wire` (`core/src/op_resolve_view.cpp:136`) is structural: a *mutated*
multi-link value must own a contiguous, patchable, trailer-cleared segment
([ADR-0041, terminus arena decode span contract](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0041-terminus-arena-decode-span-contract.md)
§2), so it flattens once. The zero-copy `pin_wire` subrope applies only to the opt-in verbatim
referenced STORE, not to ops that patch bytes.

### 4.3 Complexity of a rope walk

`span_cursor::byte_at` is O(1). `rope_cursor::byte_at` calls `locate()`, a linear scan over links,
so `load_le(n)` is n such scans, a header read costs O(header_bytes × L) and a CRC feed costs
O(payload + L), for L links.

- For a **single-link rope** — the common case, one recv chunk becoming one ingress segment, where
  `materialize` returns `links()[0]` — L = 1, so `rope_cursor` ≈ `span_cursor` plus a trivial
  constant. There is no regression.
- For a **multi-link rope** the header pointer-chase is bounded by fragment count × a 4–6-byte
  header, and the payload feed is asymptotically identical to the memcpy it replaces, but read-only
  and without allocating a destination.
- Against the alternative it removes — `flatten()` is O(payload) read *and* write, plus a pool or
  stack allocation — the rope walk wins on **latency** (no write pass, no allocation) and on **RAM**
  (no destination buffer).

There is no latency-versus-RAM trade-off for the decode walk. The one genuine cost is code size:
`rope_cursor` lives in a separate translation unit so a span-only target never instantiates it
([ADR-0048, one wire grammar, chunk cursor, rope-aware decode](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0048-one-wire-grammar-chunk-cursor-rope-aware-decode.md)
§1). That is a build-configuration trait, not a runtime trade-off.

---

## 5. The ingress copy

The ingress ownership copy (①) is structural as a copy, but it is not always an *extra* copy.

**Why it must own.** `vertex_t::store` refcounts rather than copies, so durability rests on stored
segments being long-lived. The receive buffer is transient: the ESP-IDF WS link reads a fitting
frame into a once-per-link reusable scratch, falling back to an exact-size
`new (std::nothrow) std::byte[frame.len]` only for oversized frames
(`httpd_ws_link_t::on_data_frame`), and either buffer is reused or dies when the receive
call returns, while the rope outlives it — pinned in a last-known-value slot, fanned out
asynchronously, awaited. A borrowed view of that buffer would dangle. Ingress must therefore land
bytes in an owned segment.

**Why the pull path pays nothing extra.** The TCP `serve` loop reads the body straight into the
accepted segment: `read_exact(fd, seg->bytes.data(), len)` (`core/src/transport_tcp.cpp:266`,
`read_exact` defined at `:204`) fills a segment freshly allocated from the injected backend by
`length_prefix_framer::on_prefix`. The pooled receive target *is* the owned segment — one kernel
copy and zero user-space copies. The in-source rationale names the trade explicitly: feeding recv
chunks through `feed()` "would add one" copy, so the pull loop shares framing *rules* with the
chunk-fed transports rather than their state machine (`core/src/transport_tcp.cpp:241-246`). The
only stack scratch left on this path is `drain()`'s 4096-byte backpressure discard buffer
(`core/src/transport_tcp.cpp:224`), which runs when a frame is dropped, not when one is delivered.

**Where the pull-path shape is not followed**, the residual costs are pool-recv questions, not
flatten questions:

- **The ESP-IDF WS link delivers borrowed** (`httpd_ws_link_t::deliver`),
  forcing a downstream ownership copy at store. The per-frame `new[]` itself is gone for fitting
  frames (they read into the once-per-link scratch); the residual is the shape: landing the
  payload in a bounded rx pool, adopting it and delivering owning — the TCP/UDP shape — would, by
  feeding the rope tier an owned segment, let the branch and field decode collapse to refcount
  bumps once the sink is rope-native.
- **WS reassembly (⑧)** regrows exact-size per fragment
  (`integrations/esp-idf/libtracer/httpd_ws_link.cpp:410-420`), which is O(n²) in total bytes
  copied. Chaining each fragment as an owning rope link makes it O(n) owning copies — the CAN model,
  which is what the host `transport_ws.cpp` does.

**The DEVICE-link constraint never conflicts with pool-recv.** Receive targets are always host
memory, so a host pool slot is a legal recv-and-adopt target; only a CPU-side *flatten* of a DEVICE
link would fault, and `flatten` refuses that up front (`core/src/rope.cpp:15`).

**Removing even the ownership copy** requires `LWIP_NETCONN` plus a pbuf-wrapping `mem_backend`
(`destroy = pbuf_free`, the pbuf *is* the rope link). That collapses ①, but it is non-portable — no
pbuf on a Linux host — and incompatible with `esp_http_server`'s WS framing. For a BSD-socket API,
① stays.

**Every pool-recv change shares one precondition, and it is built.** A segment
self-routes reclaim on whichever subscriber thread drops the last reference, concurrent with a
writer's `alloc`, so the receive backend must be thread-safe. `synchronized_pool_t<Sync>`
(`core/include/libtracer/mem_pool.hpp:170`) is that backend, with the critical section as a
compile-time policy
([ADR-0060, LKV copy store and injected value backend](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0060-lkv-copy-store-injected-value-backend.md)
§2, selected per target as a module-set trait
([ADR-0047, build-time closed module sets](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md)
§2): a host keeps the spinlock `sync_pool_t`, and a single-core priority-preemptive target — where
a lower-priority slot-holder cannot run while a higher-priority task spins — takes the
interrupt-disable `tr::esp::critical_pool_t` from the ESP-IDF component
(`integrations/esp-idf/libtracer/include/libtracer_esp/critical_pool.hpp`; it needs FreeRTOS
headers, so it is not in `core/`). It is opt-in construction: no seam defaults to a pool.

---

## 6. Removal candidates, ranked by value

Ranked by value on single-link traffic for a single-core, RAM-constrained node, with the saving and
the gate.

| Rank | Change | Removes | Saving | Fires on the single-link path? | Gate |
|:--:|--------|---------|-------------|:--:|-------------|
| **1** | Streaming branch decode (walk-callback driven, no node array) | ④, the on-stack 4096-byte arena | ~4 KB stack high-water off the deepest task, the binding constraint. RAM relocates stack→pool rather than disappearing. | yes — always paid | Latency: the walk stack becomes pool- or heap-drawn |
| **2** | WS receive into a pooled segment, deliver owning | ⑧'s per-frame `new[]`; the borrowed-then-store copy | One allocation per WS frame per session removed, and the rope tier gets an owned segment | yes — every WS frame | Changes the WS delivery contract; the thread-safe pool it draws from already exists (§5) |
| **3** | Rope-native branch and field node type | ②③, the multi-link branch/field flatten | Nothing on single-link, which is a refcount bump; one flatten becomes a refcount bump on fragmented POINT writes | **no — multi-link only** | Ratification-gated sink type (ADR-0041 §2); needs rank 1 to matter |
| **4** | Rope-native walk accessors plus a scatter-gather reply head | ⑥, the per-straddling-node `ensure_cache` | On straddling route TLVs, fuses flatten and memcpy into one gather | **no — multi-link only** | Converts the shared span-based `resolve_node` concept — non-local |
| **5** | Rope-chaining WS reassembler (the host `ws_assembler_t` shape) | ⑧'s O(n²) regrow | O(n²) → O(n) owning copies; no 2×n transient heap peak. Not zero-copy, and fragmentation may worsen (k small segments). | **no — fragmented only** | Needs rank 2 or the rope sink wired, or ⑦ re-materializes |
| **6** | Scatter-gather COMPACT encoder | ⑩'s delivery flatten | Real only on server-side `writev` links, on the auto-promotion leg; neutral on a masked WS client | no | Low value |
| — | `LWIP_NETCONN` pbuf-as-rope-link | ①, the ingress ownership copy | Collapses even the structural copy | yes | Non-portable (no pbuf on a Linux host); incompatible with `esp_http_server` framing |
| — | Raw `sendmsg` WS TX, leaving `esp_http_server` | ⑨, the TX gather | One full-payload allocation and memcpy per frame per peer | yes | Abandons the threadless HTTP-server seam; breaks the `send_fn` indirection; lwIP still copies at the socket |

### Structural copies

- **① the ingress ownership copy** — a transient receive buffer cannot be borrowed by a rope that
  outlives it. On the pull path it *is* the recv: one kernel copy, zero user-space copies.
  Removable only by the non-portable `LWIP_NETCONN` route.
- **⑤ `own_wire`** — a mutated multi-link value must own a patchable contiguous segment (ADR-0041
  §2). This step is the ownership copy, not its removal.
- **⑨ the WS TX gather** — `httpd_ws_send_frame_async` takes one contiguous `{payload, len}` and
  `httpd_queue_work` is asynchronous, so the rope links are gone before the send runs; the reply
  must be flattened into one owned buffer. `httpd_ws_link_t::queue_send` gathers once, straight
  into a pre-allocated tx work slot claimed lock-free (nothrow heap only for an oversize
  payload; an exhausted pool drops and counts, #949), and the directed reply path hands it the
  reply rope's iovec
  with no intermediate flatten temporary. One irreducible gather-copy remains — now
  allocation-free in steady state. It is the price
  of threadlessness — riding the existing HTTP-server task, adding no FreeRTOS task — and is
  removable only by leaving the seam. The rope cursor is irrelevant to it: the sink is a send API,
  not a decoder.
- **⑫ reply-route synthesis** — `tlv_sliced` (`core/src/fwd_reply.hpp:98`) emits rewritten
  route wires. Bounded at tens of bytes, never payload-scaled.

[#477]: https://github.com/avatarsd-llc/libtracer/issues/477
[#588]: https://github.com/avatarsd-llc/libtracer/issues/588
[#596]: https://github.com/avatarsd-llc/libtracer/issues/596
