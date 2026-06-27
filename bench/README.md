# libtracer ↔ Zenoh benchmark

A side-by-side **speed and latency** comparison of libtracer and
[Eclipse Zenoh](https://zenoh.io), swept across a parameter **matrix** (payload
size × subscriber fan-out × topic count) plus a mixed workload — so the result is a
*response surface*, not a single point.

The current numbers and figures are in **[RESULTS.md](RESULTS.md)** (snapshot) and
**[figures/](figures/)** (2D/3D plots). This README is the methodology.

## Harnesses

| script | what it measures |
| --- | --- |
| `run.sh` | **in-process** sweep: dispatch + (optionally) serialization cost, on one process. |
| `run_net.sh` | **network**: two processes over real localhost **UDP** (the kernel path). |
| `grid.sh` | **response-surface grid** → `grid.csv` → 2D/3D figures via `plot.py`. |
| `bench_scatter` | **scatter-gather egress**: one `sendmsg(iovec)` ships a K-value composite rope. |

```sh
./fetch_zenoh.sh   # vendors prebuilt zenoh-c 1.9.0 + zenoh-cpp (x86_64 linux; not committed)
./run.sh           # in-process matrix
./run_net.sh       # network (two processes over UDP)
python3 -m venv .venv && ./.venv/bin/pip install matplotlib numpy
./grid.sh          # grid.csv + figures/
```

Without `fetch_zenoh.sh`, only the libtracer numbers print.

## What is measured

The swept axes (`bench_common.hpp`): payload **1..8192 B**, fan-out
**1/8/128/1024/8192** subscribers per endpoint, endpoints **1..8192** topics. To
keep the comparison fair and the wall-clock bounded, each run targets a roughly
constant number of *deliveries* (high fan-out does proportionally fewer publishes).

Module compositions are surfaced as distinct `mode`s — "different approaches to
craft libtracer":

| mode | what it exercises |
| --- | --- |
| `inproc` | zero-copy graph dispatch (`write` → subscriber callback), heap-allocated view per message. |
| `inproc-borrow` | the **zero-copy loaned path** — a borrowed view, *zero alloc, zero copy*. |
| `inproc-path` | write **by path** (registry hash + lookup per publish) — the "many topics" cost. |
| `loopback` | the M4 bridge: encode + ROUTER-wrap + cross-thread queue + decode. |
| `mixed` | 128 topics, varied fan-out + payloads. |
| `net` | two processes over real UDP (`run_net.sh`). |
| `scatter` | composite rope shipped in one `sendmsg(iovec)` (`bench_scatter`). |
| `eptype-lean` | ep-type axis: minimal sink (see below). |
| `eptype-lean-cached` | ep-type axis: loaned / `out_cache` read (see below). |
| `eptype-stream` | ep-type axis: `STREAM`-role vertex (see below). |

### ep-type (endpoint-dispatch-class) axis (#96 / ADR-0032)

A fourth axis over the *dispatch class* a write takes to an endpoint, measured on one
fixed workload (**64 B, fan-out 1, 1 endpoint**) so the three classes are directly
comparable. Each emits a `RESULT` line whose `mode` names the class (same 12-field
shape, so `collate.py` / `perf_gate.py` still parse). **The names are provisional**
(the class boundaries are what matter); the map to the underlying paths is:

| ep-type | maps to | what it exercises |
| --- | --- | --- |
| `eptype-lean` | minimal sink (`inproc`) | plain write+deliver to a `STORED_VALUE` vertex, heap view per publish. |
| `eptype-lean-cached` | loaned / `out_cache` (`inproc-borrow`) | the zero-alloc loaned read path — a borrowed view (zero alloc, zero copy). |
| `eptype-stream` | `STREAM` role | each write appends to the bounded history ring (retention work) *then* fans out — strictly more work than lean. |

Expected cost ordering: **lean-cached** (fastest / zero-alloc) < **lean** < **stream**
(heaviest — pays history retention on every write). lean / lean-cached reuse the
existing `inproc` / `inproc-borrow` code paths, re-emitted under the `eptype-*` tag (the
original lines still print).

- **Throughput** — back-to-back publishes; `deliveries / elapsed`.
- **Latency** — one publish at a time (publish, wait for receipt, repeat); p50/p99/mean.
- Both sides build at **`-O3`** (Release) for parity; the app payload size (not the
  on-wire envelope) is used for MB/s.

## Results summary

See **[RESULTS.md](RESULTS.md)** for the tables and **[figures/](figures/)** for the
plots. Headline (libtracer vs zenoh-c, this machine):

- **In-process:** ~2.5× → ~6.4× throughput and ~2.6× → ~6.6× lower p50 as fan-out
  grows 1 → 8192; the `inproc-borrow` (loaned) path is flat ~80 ns even at 8 KB.
- **Network latency:** ~14 µs vs ~64 µs p50 (≈4.5× lower) — libtracer sends one
  datagram per message immediately.
- **Network throughput:** via **scatter-gather composition** (`transport_t::send(iov)`),
  one `sendmsg` ships a K-value rope → 5.1M values/s @ ~3 µs (K=8) up to 46.6M @
  ~12 µs (K=256), vs zenoh-c 3.5M @ 62 µs. **libtracer wins both axes.**

## Why libtracer wins — and the honest caveats

- **In-process:** the same TLV bytes are the wire encoding *and* the in-memory value
  *and* the graph node, so a publish moves **zero bytes** (a refcount bump). zenoh's
  intra-session path, though well-tuned, still runs its full sample machinery.
- **Network throughput is *structural*, not a timer.** zenoh raises throughput with
  a **batching timer** — which is why its latency is ~4.5× worse. libtracer batches
  by **composition**: a composite endpoint's value is a rope already batched in
  memory, shipped zero-copy in one `sendmsg(iovec)` — no Nagle timer, so no latency
  penalty. (The plain one-`sendto`-per-message path remains for lowest single-message
  latency.)
- **`loopback` is *slower* than zenoh** — it's a dev/test transport (encode + ROUTER
  + cross-thread queue + decode), not an optimized one; not the network comparison.
- **Caveats:** single machine; localhost UDP is best-effort (drops shrink counts);
  zenoh's default reliable-TCP comparison waits on libtracer's reliable-stream
  transport (the remaining **M6** work). Numbers are representative single runs.
