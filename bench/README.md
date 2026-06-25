# libtracer ↔ Zenoh benchmark

A side-by-side **speed and latency** comparison of libtracer and
[Eclipse Zenoh](https://zenoh.io) for a 1-publisher / 1-subscriber workload.

> [!IMPORTANT]
> **This is an _in-process_ benchmark, not a network benchmark.** libtracer's
> socket transport isn't built yet (it lands in **M5** — today the only transport
> is an in-process loopback). So these numbers compare **dispatch + serialization
> overhead inside one process** — they do **not** measure wire/network throughput,
> which is Zenoh's whole domain. Read the [interpretation](#interpretation) before
> quoting any figure. The harness is structured so the *same* benchmark re-runs
> over a real socket once M5 exists (a `socket` mode drops in next to `loopback`).

## What is measured

Three in-process paths, same payload sizes (8 B → 8 KiB), same message counts:

| Path | What it exercises |
| --- | --- |
| `libtracer/inproc` | The **zero-copy graph** path (M3): `write` → subscriber callback. A refcount-bump `View` handoff, synchronous, no serialization. libtracer's design sweet spot. |
| `libtracer/loopback` | The **M4 bridge**: `write` → encode + **ROUTER**-wrap → in-memory queue (cross-thread) → unwrap + dedup-check + decode → deliver. Real messaging work, minus the socket. |
| `zenoh/inproc` | Zenoh **peer mode**, publisher + subscriber on one key in one `Session` (intra-session local delivery) — the closest Zenoh analogue. |

- **Throughput** — `kThroughputMsgs` published back-to-back; `received / elapsed`.
- **Latency** — one message at a time (publish, wait for receipt, repeat); **p50 / p99 / mean**.
- Both sides build at **`-O3`** (`CMAKE_BUILD_TYPE=Release`) for parity; the app
  payload size (not the on-wire envelope) is used for MB/s.

## Run it

```sh
./fetch_zenoh.sh   # vendors prebuilt zenoh-c 1.9.0 + zenoh-cpp (x86_64 linux)
./run.sh           # builds both at -O3 and prints the table
```

Without `fetch_zenoh.sh`, only the libtracer numbers print.

## Results

`AMD EPYC 9115 · g++ 13.3 -O3 · zenoh-c 1.9.0 · 100k msgs throughput / 10k latency`

```
 payload path                         msgs/s       MB/s        p50        p99       mean
----------------------------------------------------------------------------------------
      8B libtracer/inproc         15,457,537      123.7      0.09µ      0.09µ      0.09µ
      8B libtracer/loopback        1,695,441       13.6      3.38µ     11.68µ      3.80µ
      8B zenoh/inproc              5,207,970       41.7      0.24µ      0.28µ      0.26µ
     64B libtracer/inproc         15,474,999      990.4      0.09µ      0.09µ      0.09µ
     64B libtracer/loopback          592,218       37.9     12.71µ     16.84µ      9.89µ
     64B zenoh/inproc              5,332,346      341.3      0.23µ      0.24µ      0.23µ
   1024B libtracer/inproc         12,662,854    12966.8      0.13µ      0.18µ      0.13µ
   1024B libtracer/loopback          386,301      395.6     13.56µ     21.59µ     13.99µ
   1024B zenoh/inproc              5,209,408     5334.4      0.24µ      0.25µ      0.24µ
   8192B libtracer/inproc          7,664,714    62789.3      0.15µ      0.23µ      0.16µ
   8192B libtracer/loopback          475,330     3893.9      3.58µ     10.58µ      3.95µ
   8192B zenoh/inproc              3,678,678    30135.7      0.33µ      0.35µ      0.33µ
```

## Interpretation

- **`libtracer/inproc` is ~2–3× Zenoh's in-process throughput at ~90 ns latency.**
  That is the **zero-copy** story paying off: a publish is a refcount bump on a
  `View` + a synchronous callback — **no serialization, no buffer copy, no thread
  hop**. For *in-process* telemetry/tracing on one node (the P0 use case), this is
  the benefit libtracer is built for, and it shows.
- **`libtracer/loopback` is _slower_ than Zenoh here — and that's the honest part.**
  It pays for encode + ROUTER envelope + a cross-thread queue + decode + dedup,
  while Zenoh's intra-session path is a mature, highly-tuned local fast-path. The
  loopback is a *dev/test* transport, not an optimized one.
- **Neither side crosses a socket.** Over a real network — Zenoh's home turf, and
  libtracer's M5 — the picture changes: serialization and the OS path dominate, and
  libtracer's zero-copy scatter-gather egress (rope → `iovec`, no flatten) is the
  lever to test. **That comparison is not valid until M5.** Treat the in-process
  numbers as "dispatch cost," not "messaging throughput."

**Where libtracer's benefit lies (today):** the same TLV bytes are the wire
encoding *and* the in-memory value, so an in-process hand-off moves **zero bytes**
— ideal for high-rate on-node tracing on MCUs and gateways. Where it does **not**
yet compete: networked pub/sub at scale (no transport, discovery, QoS, or security
yet — see the [module roadmap](../docs/reference/10-module-catalog.md)).

## Reproducibility / caveats

- Single machine, single process; results vary with CPU, allocator, and load.
- libtracer allocates a fresh heap `View` per message (parity with Zenoh's
  per-`put` copy); its zero-copy advantage (reusing an existing `View`) is *not*
  exploited here, so the inproc numbers are conservative for libtracer.
- The loopback dedup recent-set is disabled in the throughput run (unique
  timestamps per message would otherwise be the only de-dup key).
- Zenoh vendored binaries are **not** committed (third-party); `fetch_zenoh.sh`
  pulls them. Pinned to 1.9.0.
