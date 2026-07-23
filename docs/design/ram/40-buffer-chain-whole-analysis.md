# The whole buffer chain — wifi/CAN → lwIP → wireguard → WS → libtracer

> **Status:** design analysis (pre-ADR). **Date:** 2026-07-23. **Question:** *"Can the entire chain of
> buffers — wifi tx/rx & CAN → lwIP → wireguard → websocket → libtracer — be optimized as a whole?"*
> **Method:** a 5-layer end-to-end copy/buffer audit (each layer + a whole-chain critic, with an
> adversarial verify pass on every "removable" call), grounded file:line against ESP-IDF
> (`/home/sd/esp/esp-idf`), strawberry-fw `sdkconfig.prodnode`, and libtracer core.

## The one-line answer

**No systemic copy-elimination win of consequence remains — the chain is already near-optimal on the copy
axis.** The single true whole-chain *absolute-RAM* win is **not a copy at all**: it is **right-sizing the
over-provisioned wifi + lwIP buffer pools (~20–30 KB) for the C6's single-flow control-plane role** — pure
`sdkconfig` composition, shared with v1.8.0, gated on throughput HIL. That lever is bigger than every
copy-elision idea and bigger than any single slice in the [program](00-bounded-reactor-node-profile.md).

## RX copy ledger — one ingress frame, radio → graph

| # | Boundary | Kind | Bucket |
|---|----------|------|--------|
| 1 | radio → wifi **static** rx ring | HW DMA — *not* software | shared |
| 2 | wifi static → wifi **dynamic** rx (heap) | **FUNDAMENTAL** sw copy — the DMA descriptor must be recycled to HW at once; surfaces at `wifi_netif.c:37-60`, inside the closed `esp_wifi` lib | shared |
| 3 | wifi dynamic → **lwIP pbuf** | **ZERO-COPY** — `CONFIG_LWIP_L2_TO_L3_COPY` unset (`prodnode:2163`) → `wlanif.c:168` wraps the wifi buffer as `PBUF_REF` (`esp_pbuf_ref.c:59`); `custom_free` returns it to the wifi lib | shared |
| 4 | **WireGuard decrypt** | **PHANTOM** — `CONFIG_APP_WG_ENABLED` unset (`prodnode:1023`), `cfg->enabled=false`; d2d is raw-TCP on the LAN and takes the connected route. **WG is not in the graph path.** | — |
| 5 | socket recv: pbuf → user (`pbuf_copy_partial`, `sockets.c:1029`) | **FUNDAMENTAL** — BSD `recv()` demands a contiguous owned destination. **On raw-TCP d2d this copy *is* libtracer's single wire-copy, FUSED** (`transport_tcp.cpp:229` reads straight into the owned `segment_t`). On WS it lands in httpd heap `payload` (`httpd_ws_link.cpp:338`, one alloc/frame). | shared / marginal |
| 6 | *(WS only)* httpd `payload` → owned graph value segment (`graph.cpp` store) | **REMOVABLE-in-principle but FORCED** — httpd delivers *borrowed* and frees `payload` on handler return, so `store_ref_min_bytes` cannot pin it. Value-TLV-sized (small). **On raw-TCP the segment is already owned → this copy does not exist.** | marginal |
| 7 | rope / graph | refcount subviews over the owned segment — *not* a copy | marginal |

**RX floor = two fundamental full-frame copies** (wifi DMA recycle + BSD recv), both shared with v1.8.0.
**libtracer adds exactly one marginal copy** — the graph store, the *last and smallest* in the chain.
→ **WS ingress = 3 software copies; raw-TCP d2d = 2** (recv + wire-copy + store all fuse into one owned read).

## TX copy ledger — one egress frame, graph → radio

1. graph → rope iovec — **zero-copy** gather of refcounted subviews.
2. *(WS only)* rope iovec → **one contiguous heap tx buffer** (`httpd_ws_link.cpp` queue_send memcpy) —
   removable *only* off `esp_http_server` (async `httpd_queue_work` wants one contiguous payload). **Raw-TCP
   `writev`s the rope iovec directly → this copy does not exist.**
3. user buffer → lwIP TCP send pbuf (`tcp_write`, `NETCONN_COPY` hardcoded, `sockets.c:1453`) —
   **FUNDAMENTAL**: `send()` copies into `PBUF_RAM` retained for retransmission until ACK.
4. lwIP pbuf → wifi **dynamic tx** buffer (`esp_wifi_internal_tx`, `wifi_netif.c:63-66`) — **FUNDAMENTAL**:
   no PSRAM on the C6 → `tx_by_ref` is dead code; encrypt happens in place in this DMA-capable buffer.
5. *(conditional)* chained-pbuf linearize (`wlanif.c:96-98`) — fires **only** for a pbuf *chain*; off the
   single-segment hot path, and a **hazard** any NOCOPY/segmented-TX lever would reintroduce.
6. wifi tx → radio — HW DMA.

## Ranked whole-chain levers

| # | Lever | Spans | Saves | Bucket | Do? |
|---|-------|-------|-------|--------|-----|
| **1** | **Right-size wifi+lwIP buffers as one budget** | wifi+netif+lwIP | **~20–30 KB DRAM heap** — trim `RX_BA_WIN 8→4`, `DYNAMIC_RX/TX 8→4`, `STATIC_RX 6→4`, `TCP_WND 5840→2920`; five knobs provisioned for a throughput a single-flow node never uses | absolute (shared) | **DO IT** — pure sdkconfig, but **HIL under WS + CAN storm** first; guard trimmed values vs a defaults regression |
| 2 | **Keep d2d on raw-TCP, never on browser WS** | transport + graph + lwIP | 2 marginal copies (egress gather ~12.7 KB/reply + ingress store) — *latency, not RAM* | marginal | **Already realized** — treat the WS path's 2 extra copies as the fixed cost of browser reach |
| 3 | **Keep `LWIP_L2_TO_L3_COPY=n` + CI sentinel** | wifi ↔ lwIP | protects the PBUF_REF zero-copy handoff; turning it on adds a full-frame memcpy + ~12.8 KB PBUF_RAM churn/frame | shared | **Zero-cost insurance** — assert the symbol stays unset in CI |
| 4 | Zero-copy RX (`netconn_recv_tcp_pbuf` + pbuf-backed `mem_backend`) | netconn+view+rope | removes the last full-frame copy → 0 libtracer copies on raw-TCP | marginal | **Defer** — HIGH cost (abandons BSD fd, not viable on WS, backpressures the wifi rx pool) |
| 5 | Zero-copy TX (`NETCONN_NOCOPY` + hold refs to ACK) | rope+netconn+wlanif | removes 1 egress memcpy | marginal | **Defer** — hazardous (retx-lifetime = use-after-free; re-linearizes unless single-segment ≤MSS) |
| 6 | Non-levers, ruled out | — | ~0 | — | WG bypass (already off-path); TX-by-ref (no PSRAM → hard floor); CAN rope-elision (an 8 B F64 value facade, never touches the rope). **Reclaim WG *flash* by gating `esp_wireguard` SRCS on `CONFIG_APP_WG_ENABLED`.** |

## B0 vs v1.8.0 — the config diff (2026-07-23)

Ran to answer whether B0 is a genuine *beat* of v1.8.0 or a wash. Compared `sdkconfig.defaults` (deliberate
overrides) at tag `v1.8.0` against the current defaults, plus the resolved shipped `sdkconfig.prodnode`:

| Knob | v1.8.0 intent | current intent | **shipped (resolved)** | idle-resident? |
|---|---|---|---|---|
| `ESP_WIFI_STATIC_RX_BUFFER_NUM` | 6 | 6 | 6 | **yes — equal** |
| `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 6 | 6 | **8** | no (on-demand cap) |
| `ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 6 | 6 | **8** | no (on-demand cap) |
| `ESP_WIFI_RX_BA_WIN` | IDF-default | IDF-default | **8** | no (BA reassembly) |
| `LWIP_TCP_WND_DEFAULT` / `SND_BUF` | 5840 | 5840 | 5840 | per-conn (peer-scaled) |
| `LWIP_MAX_SOCKETS` | 12 | 12 | 12 | ~equal |

Three results:

1. **v1.8.0's and the current `sdkconfig.defaults` are byte-identical for every buffer knob**, and the
   idle-*resident* pools (`STATIC_RX`, `TCP_WND`, `MAX_SOCKETS`) are equal. → The ~25 KB idle marginal is
   **genuinely libtracer, not a buffer-config difference** — the config angle independently reconfirms the
   [RAM-audit thesis](00-bounded-reactor-node-profile.md).
2. **B0 is a peak/ceiling lever, not an idle-vs-v1.8.0 lever.** The knobs it trims are *dynamic* (on-demand),
   so B0 reclaims burst DRAM ceiling + largest-block-under-load + OOM-headroom — it does **not** move idle
   heap vs v1.8.0. (An earlier draft over-claimed B0 as an idle beat; corrected.)
3. **Config-hygiene drift:** the *shipped* image runs `DYNAMIC_RX/TX=8`, `RX_BA_WIN=8` — above the `=6` its
   own `sdkconfig.defaults` intends (ESP-IDF defaults won over an already-generated full sdkconfig). **B0's
   zero-risk first step is to reconcile the shipped config back to the intended 6/6** (reverting the drift
   and restoring v1.8.0-parity on the caps at no throughput cost); trimming to 4/4 is the HIL-gated step.

## Verdict

The "2–3× upstream copy" worry is **unfounded for this config**: lwIP adds zero copies (PBUF_REF), WireGuard
is disabled (phantom), CAN never touches the rope. The RX floor is exactly **two fundamental copies** (wifi
DMA recycle + BSD recv) plus libtracer's **one** small store copy — and on raw-TCP even those fuse to two.
Every libtracer-code-side elision (store-pin, egress gather, netconn zero-copy) is a *per-frame latency*
delta that is **either already realized on raw-TCP or structurally blocked on the WS path** by
`esp_http_server`'s borrowed-recv / async-contiguous-send ownership model — chasing them is
micro-optimization. **The only systemic RAM win lives in pool sizing, not in the copy chain**, and it is
composition, not a libtracer change — which is exactly the [RAM-audit](00-bounded-reactor-node-profile.md)
thesis, now proven from the wire up.
