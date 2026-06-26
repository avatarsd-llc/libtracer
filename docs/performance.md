# Performance

> Honest, reproducible numbers against [Eclipse Zenoh](https://zenoh.io). libtracer
> is measured in **two regimes** — *in-process* (dispatch cost) and *network* (real
> localhost UDP) — because they answer different questions and libtracer wins one
> while Zenoh wins the other. The full harness, methodology, and caveats live in
> [`bench/`](https://github.com/avatarsd-llc/libtracer/tree/main/bench); this page
> is the summary.

`AMD EPYC 9115 · g++ 13.3 -O3 · zenoh-c 1.9.0`. Numbers are representative single
runs, not averaged — treat them as orders of magnitude, not benchmarks of record.

```{mermaid}
flowchart TB
    subgraph ip["in-process — one process"]
        A1["publish = refcount bump<br/>(zero byte copy)"]
    end
    subgraph net["network — two processes, UDP"]
        A2["publish → encode → ROUTER<br/>→ sendto → kernel → peer"]
    end
    ip -. "measures dispatch + serialization cost" .-> Q1["libtracer's home turf"]
    net -. "measures the wire path" .-> Q2["Zenoh's home turf"]
```

## In-process — the zero-copy story

One process; a publish is a refcount bump on a shared `SegmentPtr` — the same TLV
bytes are the wire encoding *and* the in-memory value *and* the graph node, so the
hand-off moves **zero bytes**.

| payload | libtracer/inproc | zenoh/inproc | latency (libtracer p50) |
| --- | --- | --- | --- |
| 8 B | **15.5M msg/s** | 5.2M msg/s | **~0.09 µs** |
| 1024 B | **12.7M msg/s** | 5.2M msg/s | ~0.13 µs |
| 8192 B | **7.7M msg/s** | 3.7M msg/s | ~0.16 µs |

**libtracer/inproc is ~2–3× Zenoh's in-process throughput at ~90 ns latency** — the
zero-copy design paying off. (The in-process *loopback bridge* is *slower* than
Zenoh — but it's a dev/test transport, not an optimized one; the honest network
comparison is below.)

## Network — latency vs. throughput (real UDP, two processes)

Two processes over localhost UDP; one-way latency via `CLOCK_MONOTONIC`. The two
systems split the win — a textbook latency/throughput trade:

| payload | libtracer/net | zenoh/net | libtracer p50 | zenoh p50 |
| --- | --- | --- | --- | --- |
| 16 B | 352k msg/s | **3.33M msg/s** | **14.1 µs** | 65.0 µs |
| 256 B | 548k msg/s | **2.59M msg/s** | **14.4 µs** | 66.3 µs |
| 1024 B | 487k msg/s | **2.87M msg/s** | **12.5 µs** | 64.1 µs |
| 8192 B | 195k msg/s | 198k msg/s | **11.7 µs** | 69.9 µs |

- **libtracer has ~4–5× lower latency** — its per-message path is thin (encode a
  VALUE TLV → ROUTER-wrap → one `sendto`; nothing sits between the write and the wire).
- **Zenoh has ~6–9× higher small-message throughput** — it **batches** many messages
  into fewer datagrams, amortizing the syscall. libtracer currently does **one
  `sendto` per message**, so its network throughput is **syscall-bound**.
- At **8 KB** they converge — each message ≈ one datagram, so batching no longer
  helps and both are bandwidth-bound.

```{note}
Caveat: localhost UDP, best-effort. Zenoh's *default* transport is reliable TCP, so
the apples-to-apples reliable comparison waits on a libtracer reliable-stream
transport (the M6 milestone).
```

## What this surfaces

The split names libtracer's next optimization precisely: **opt-in egress batching**
(coalesce frames per datagram, or `sendmmsg`). Made *optional* — `max_delay = 0`
keeps today's low-latency path; `> 0` buys throughput — it would close most of the
throughput gap **while keeping the latency edge by default**, which Zenoh (always
batched) cannot offer. This is a deliberate design lever, declared interface-first
before implementation.

## Reproduce it

```sh
git clone https://github.com/avatarsd-llc/libtracer
cd libtracer/bench
./fetch_zenoh.sh     # vendors prebuilt zenoh-c (x86_64 linux)
./run.sh             # in-process
./run_net.sh         # network (two processes over UDP)
```

Full tables, methodology, and the bit-level rationale are in
[`bench/README.md`](https://github.com/avatarsd-llc/libtracer/blob/main/bench/README.md).
