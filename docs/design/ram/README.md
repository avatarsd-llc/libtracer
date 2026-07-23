# RAM leanness — design document set

Companion documents from the 2026-07-23 RAM-leanness investigation of libtracer on the single-core
ESP32-C6 (strawberry-fw). Read in order; each stands alone but they cross-reference.

| # | Document | Answers |
|---|----------|---------|
| 0 | [**Bounded-reactor node profile**](00-bounded-reactor-node-profile.md) | *The program.* Why the RAM goes where it does, the single-core rationale, the target architecture, and the committed slices + retirement + the buffer-budget lever. |
| 1 | [**The lwIP ↔ libtracer seam**](10-lwip-libtracer-seam.md) | *Is the seam optimized?* Full RX/TX path per transport, a 16-row copy inventory, the esp_http_server-vs-raw-lwIP threadless-vs-zero-copy tradeoff, and O1–O7 ranked optimizations. |
| 2 | [**Zero-copy & the flatten question**](20-zero-copy-and-flatten.md) | *Do we really need to flatten?* All 12 flatten points enumerated, the 4096 on-stack arena dissected, the rope_cursor migration status, and a ranked removal plan. |
| 3 | [**Second-pass reconciliation**](30-second-pass-reconciliation.md) | *Does it all hold up?* A 9-dimension adversarial re-audit against current source: what STANDS, 14 documentation/attribution corrections, and 13 new gaps (two blockers) to close before banking numbers. |
| 4 | [**The whole buffer chain**](40-buffer-chain-whole-analysis.md) | *Can the chain be optimized as a whole?* End-to-end RX/TX copy ledgers (wifi DMA → lwIP → wireguard → WS → libtracer), and the finding that the only systemic RAM win is buffer right-sizing, not copy-elision. |

> **Verdict (2026-07-23):** the substantive thesis survives a 9-dimension re-audit intact (doc 3) — corrections
> are documentation honesty + `can_en=1` pinned + couple S4 with S2, **not** design changes. The whole-chain
> audit (doc 4) then found **the single biggest RAM lever: right-sizing the wifi+lwIP buffer budget (~20–30 KB,
> sdkconfig, HIL-gated)** — *not a libtracer change*. Retirement (doc 0 §6) is a credible **~11–12 KB** but
> **0 B reclaimable today** (blocked behind consumer migration). Two measurements gate any banked number: a
> config-pinned HIL heap read and a deep-path **stress** stack census.

## The headline findings (adversarially verified)

1. **libtracer is zero-copy by design and lean.** Rope assembly is chaining-not-memcpy; decode holds
   structure only (spans into input); host TX is iovec scatter-gather. On the **single-link traffic the
   device actually runs today, no payload flatten fires at all** — `materialize` is a refcount bump.

2. **The flatten is a fallback, not a floor** — but the payoff is narrower than it looks. The removable
   payload flattens are **multi-link-only** (fragmented WS / reassembled CAN), which no current producer
   generates. They are insurance for fragmented-transport load, not a hot-path win.

3. **The one always-paid removable cost is the 4096-byte on-stack decode arena — and it is a *stack*
   win, not heap.** `rope_cursor` does **not** remove it (rope_cursor is a byte source; the arena is
   structure storage). Removing it needs a **streaming decode**; RAM relocates stack→pool (net-neutral),
   but drops ~4 KB stack high-water — and **stack is the binding constraint** on this device.

4. **Two copies are fundamental:** the ingress ownership copy (RDMA copy-once) and the ESP WS TX
   gather-flatten (the price of the threadless httpd seam). Don't chase them.

5. **The highest-value change is delivering WS *owning*** (recv into a pooled segment) — it kills the
   per-frame `new[]` / O(n²) reassembly fragmentation class and hands the graph the rope tier.

6. **Everything is gated on one unbuilt primitive:** the single-core, interrupt-disable crit-section
   bounded pool (ADR-0060 §2). `sync_pool_t` is a multi-core spinlock, wrong for a unicore kernel.

7. **Riding esp_http_server is the right seam on single-core** — threadless (saves a ~12 KB recv-task
   stack); its only real cost is a forced TX gather + queue hop, contained by pooling the gather buffer.

## Ranked, do-this-order

**Absolute-RAM first (biggest lever, no code):** `B0 wifi+lwIP buffer right-size (~20–30 KB, HIL-gated)`
(doc 4). **Then the marginal/latency slices:** `O1 single-core crit-section pool` → `O2 WS owning delivery`
→ `O4 streaming/rope decode (−4 KB stack) + node-counting pre-pass (#477 → backpressure)` → `O3 pool the TX
gather` → `O5/O6/O7 (lower ROI)` (doc 1 §5 / doc 2 §6). **Independently:** **S1 WS-for-btb** (delete the
d2d listener thread, −12 KB; at `can_en=1` this zeroes dedicated libtracer transport threads) and
**retirement** (doc 0 §6 — ~11–12 KB but a *migration program*, 0 B reclaimable until consumers move to the
graph seam). Copy-elision beyond O2/O3 is **not worth it** (doc 4): the chain is already near-optimal.
