# Research — M6 reliable stream transport + egress batching (interface-first)

> **Status:** research / design note (not published). Declares the interfaces for
> the next build work **before implementation**, per the interface-first rule, and
> frames the two decisions to grill: **TCP vs QUIC** and **where the batch knob
> lives**. Companion to the [bench finding](../../bench/README.md#network-results)
> (libtracer is `sendto`-syscall-bound) and the
> [ROS note](./ros2-adapter.md) (a bridge needs reliable QoS = M6).

```{admonition} Update — the throughput gap is closed (the batching half is resolved)
:class: important
The egress-batching gap below has been **resolved differently and better** than the
`BatchingTransport` timer-decorator proposed here. Instead of a *temporal* batch (a
Nagle-style timer that costs latency), libtracer batches **structurally**: a
composite endpoint's value is a rope already batched in memory, shipped in **one
`sendmsg(iovec)`** via `transport_t::send(iov)` (the scatter-gather seam, shipped in
the perf work — see [Performance](../performance.md) and `bench/bench_scatter`). One
syscall carries K values, so throughput scales with composition size *at flat
latency* — libtracer now beats zenoh-c on **both** throughput and latency. The
`BatchingTransport` timer-decorator is therefore **superseded** (kept below only as
the considered-and-rejected alternative). The remaining M6 work is purely the
**reliable byte-stream transport** (TCP/QUIC + `FrameReassembler`) for ROS
`RELIABLE` QoS and WAN — that part stands.
```

## The two gaps M6 closes

The M5 UDP transport is **datagram = frame** (flat decode, best-effort). The bench
showed two consequences:

1. **No reliability** — only ROS `BEST_EFFORT`; no reliable stream for QoS or WAN.
2. **Throughput is syscall-bound** — one `sendto` per message; Zenoh batches and
   wins small-message throughput ~6–9×.

M6 addresses both with **one new idea each**, and both land behind the **unchanged
`Transport` seam** (`send(span)` / `set_receiver(cb)` — graph and bridge never move):

| Gap | New idea | Where |
| --- | --- | --- |
| Reliability over a byte stream | **length-prefix framing + partial-read reassembly** | `StreamTransport` |
| Throughput | **optional egress batching** (coalesce frames, opt-in) | `BatchingTransport` *decorator* |

## The elegant part — transports compose as decorators

The seam is two methods, so cross-cutting behavior wraps *any* transport without a
single change above it. Batching, and later encryption / metering / tracing, are
**decorators**, not forks of each transport:

```{mermaid}
flowchart LR
    BR["Bridge"] -->|send / receive| DEC
    subgraph DEC["composable Transport stack"]
        B["BatchingTransport<br/>(coalesce, opt-in)"] --> E["EncryptedTransport<br/>(future)"] --> S["StreamTransport (TCP/QUIC)<br/>or UdpTransport (M5)"]
    end
    S -->|fd / quic stream| K["kernel"]
```

`Bridge` holds a `Transport&`; whether that's a bare `UdpTransport`, a
`StreamTransport`, or `BatchingTransport(StreamTransport(...))` is a wiring choice.
No new virtual surface, no boilerplate per combination — the win the seam was
designed for (ADR-0013: the module ABI is implementation-defined).

## Interface 1 — `StreamTransport` (reliable, framed)

A stream delivers **bytes, not datagrams**: a frame may span several reads, and one
read may carry several frames. So egress prepends a length, and ingress reassembles.

```cpp
namespace tracer {

// A reliable byte-stream transport (TCP now; a QUIC stream later — same shape).
// Wire framing: each frame is written as [u32 little-endian length][frame bytes].
class StreamTransport : public Transport {
   public:
    StreamTransport(int connected_fd);                 // owns the fd
    void send(std::span<const std::byte> frame) override;       // write len-prefix + frame
    void set_receiver(Receiver receiver) override;
   private:
    void run();                                         // recv thread → reassembler → receiver
    FrameReassembler reassembler_;
    // … fd, thread, stop flag (as UdpTransport) …
};

}  // namespace tracer
```

### The tricky bit — `FrameReassembler`

A tiny state machine that turns an arbitrary byte chunk stream into whole frames.
Its interface stays trivial; it owns the carry buffer for a frame split across reads:

```cpp
// Consumes arbitrary byte chunks, invokes `on_frame` once per complete frame.
// Header = u32 LE length; body follows. Carries a partial frame between pushes.
class FrameReassembler {
   public:
    explicit FrameReassembler(std::size_t max_frame = 1u << 20);  // guard runaway lengths
    template <class OnFrame>
    void push(std::span<const std::byte> chunk, OnFrame&& on_frame);
   private:
    std::vector<std::byte> carry_;   // bytes of an incomplete frame
    std::size_t need_ = 0;           // 0 = still reading the length prefix
};
```

This is the **first real consumer of rope-aware decode**: instead of copying the
carry + chunk into one contiguous buffer, the complete frame can be presented as a
**`Rope`** of `{carry segment, chunk segment}` and the TLV decoder walks the links
with zero copy (M2 built `Rope`/`View` for exactly this; it has had no consumer
until now). **Grill:** build the zero-copy rope path in M6, or ship the simple
carry-copy first and optimize later?

## Interface 2 — `BatchingTransport` (throughput, opt-in)

A decorator that coalesces several frames into one downstream `send`, amortizing the
syscall. **`max_delay = 0` ⇒ pass-through (today's low-latency behavior); `> 0` ⇒
throughput mode.** That opt-in is libtracer's edge over Zenoh, which *always* pays
the batching latency.

```cpp
namespace tracer {

struct BatchPolicy {
    std::size_t max_bytes = 64 * 1024;          // flush when the batch would exceed MTU/window
    std::chrono::microseconds max_delay{0};     // 0 = never hold a frame (pass-through)
};

// Wraps any Transport; coalesces frames (each still length-prefixed) up to the
// policy, then forwards one combined buffer to `inner`. Ingress splits via the
// same FrameReassembler, so batching needs no new receive logic.
class BatchingTransport : public Transport {
   public:
    BatchingTransport(Transport& inner, BatchPolicy policy);
    void send(std::span<const std::byte> frame) override;   // append; flush on cap/timer
    void set_receiver(Receiver receiver) override;          // delegates to inner
    void flush();                                           // force-emit the current batch
   private:
    Transport& inner_;
    BatchPolicy policy_;
    std::vector<std::byte> batch_;   // length-prefixed frames, guarded
    // … flush timer / mutex …
};

}  // namespace tracer
```

Because coalesced frames are **length-prefixed**, the receive side is *already* the
`FrameReassembler` — batching and stream-framing share one mechanism. Portable
(works on lwIP: pack into one `pbuf`); `sendmmsg`/UDP-GSO remain optional Linux
accelerators *inside* a transport, not part of this interface.

## Open questions (for tomorrow's grill)

1. **TCP vs QUIC for M6's first stream.** TCP: simplest, portable (incl. lwIP),
   ships fastest, matches Zenoh's *default* for an apples-to-apples reliable bench.
   QUIC: encrypted + multiplexed + no head-of-line blocking, but pulls in a library
   and is heavier on MCUs. *Recommendation: TCP first (the framing/reassembler is
   identical and reusable for QUIC later).*
2. **Where the batch knob lives.** Per-`Transport` (ctor `BatchPolicy`) vs
   per-vertex QoS (`:settings.batch_max_bytes` / `batch_max_delay_us`). *Recommendation:
   `BatchingTransport` ctor for v1 (one knob per link); per-vertex QoS later if a
   real mixed-traffic need appears — don't over-build the control surface now.*
3. **Rope-aware decode now or deferred?** M6 reassembly is its first true consumer.
   *Recommendation: ship carry-copy first (correct, simple), then a follow-up swaps
   in the rope path and the bench proves the zero-copy win — keep the PRs small.*
4. **Does `BatchingTransport` belong in `core/` or as an opt-in module?** It's
   ~100 lines and pure composition over the seam → `core/`, beside the transports.

## Recommendation / sequence

- **M6.0** — `FrameReassembler` + `StreamTransport` (TCP), len-prefix framing,
  partial-read reassembly. Reuses the M5 recv-thread shape; carry-copy decode.
  Tests + TSan. The reliable comparison vs Zenoh-TCP drops into `run_net.sh`.
- **M6.1** — `BatchingTransport` decorator + `BatchPolicy`; extend the bench with a
  batched mode and **prove the throughput jump** (expected: clears Zenoh while a
  `max_delay=0` lane keeps the latency win).
- **M6.2** — swap the reassembler's carry-copy for the **`Rope` zero-copy path**;
  bench the delta. First real exercise of M2's rope machinery.

Once grilled, the TCP-vs-QUIC choice + the batch-knob placement become an ADR (both
are hard-to-reverse, surprising-without-context trade-offs — ADR-worthy).
