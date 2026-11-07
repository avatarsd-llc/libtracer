# Coroutines are not the core execution model; a single executor over the wait module is, and an app-side coroutine adapter is allowed only within fixed limits

<!-- status: accepted -->

Status: **accepted** (2026-11-07, maintainer ruling 42, on an analysis of the coroutine execution model for single-core targets). Builds on [ADR-0087](0087-the-peer-path-is-one-frame-ingress-one-shared-egress-frame-one-wait-module-per-node.md) §3 (one wait module per node). Keeps the standing design rules of [`CLAUDE.md`](../../CLAUDE.md) §Design rules: no timers or clock reads, complexity lowered by deletion, compile-time by default, receiver pays. No wire surface moves.

## Context

A downstream ESP32-C6 consumer asked whether libtracer should move to C++20 coroutines as its execution model, so that a single-core chip stops paying for one blocking task per link.

The cost is in the links. Each link kind owns a blocking FreeRTOS task with a 12,928 B stack. On `full_node` these tasks, plus the `esp_http_server` task, hold about 65 KiB. The router, graph and store already run synchronously inside the call. Task switches cost under 1 % of a request, so the gain is RAM and zero idle wakeups, not latency.

[ADR-0087](0087-the-peer-path-is-one-frame-ingress-one-shared-egress-frame-one-wait-module-per-node.md) §3 already decides the shape that removes those tasks: "Each node has one wait module … Delivery runs to completion on whatever drives the wait module. Links stop owning threads." On MID the wait module runs on lwIP callbacks that hand work to the app's task. That shape needs no coroutines.

The analysis probed the toolchains with `-Os -fno-exceptions -fno-rtti`. The probes covered ESP GCC 15.2 for riscv32 and xtensa, host GCC 12–14, and clang 18 and 22.

- **GCC never elides the frame allocation.** GCC 15.2 riscv32 calls `operator new` for every coroutine frame at `-O2` and `-Os`. Clang elides only frames that never escape, and a frame handed to a waiter list or an executor always escapes. GCC is the only compiler ESP-IDF ships. Every coroutine instance therefore costs one allocation on every target.
- **A coroutine costs 5–7× the flash of the equivalent state machine.** On GCC 15.2 a probe coroutine took 328 B against 44 B on riscv32 (about 7×), and 271 B against 53 B on xtensa (about 5×). Nothing shares or amortises that cost.
- **Frame size is known only at run time.** C++ has no `sizeof` for a coroutine frame, and its size is fixed only after optimisation. A frame slot is therefore sized by guesswork, and an undersized slot shows up as a run-time refusal, not a compile error. Probe frames were 36–56 B on riscv32 against 16 B for a hand-written state machine.
- **`std::generator` aborts under `-fno-exceptions`.** GCC 15.2 libstdc++ ships it. It draws from the global `operator new` and aborts when that allocation fails, so it cannot answer BACKPRESSURE.
- **P2300 senders/receivers are absent from libstdc++.** GCC 15.2 libstdc++ does not ship them, and the ESP sysroot's `<execution>` names no sender.

## Decision

**1. Coroutines are not libtracer's execution model.** Core code (router, graph, store, links, egress) is not written as coroutines. The single-core gain comes from the single executor of ADR-0087 §3: the wait module, driven by the app's task or loop, with delivery run to completion inline.

**2. Three shapes that coroutines make easy are ruled out, whatever the execution model.**

- **A suspended producer is a producer-side queue.** Under coroutines, "no credit" naturally becomes "suspend the producer". The suspended frames are then a queue of pending writes on the producer side, which the backpressure rules forbid (producer never queues). Each one also pins value blocks nobody sized.
- **A ready queue is a library buffer.** Resuming waiters later from a run queue adds a buffer to the library, and replaces bounded inline delivery with scheduling latency. A waiter is resumed inline, as the ADR-0084 `{fire, ctx}` waiter already is.
- **`co_await sleep` is a timer.** A library sleep or delay awaitable reads a clock inside libtracer. Deadlines stay the application's, through a seam that takes `now` from the app's clock.

**3. A coroutine adapter for application code is allowed later, within fixed limits.** This is a standing limit. An adapter may be added only after slices S1–S3:

- S1: #2147 (one wait module per node, host epoll adapter) and #2148 (lwIP-callback adapter that hands work to the app's task);
- S2: #2596, a compile-time single-executor sync policy (`inline_sync_t`);
- S3: #2152 (own WS server on lwIP callbacks) and #2304 (self-heal without a worker thread).

If it is added, the adapter:

- is header-only, and optional;
- wraps the existing `{fire, ctx}` waiter (ADR-0084 §3), with `fire` as resume and `ctx` as the frame;
- takes every frame from a source the caller injects, and answers BACKPRESSURE when that source is exhausted;
- resumes inline, never from a library-held queue;
- adds no library sleep or send awaitables, and takes every timeout from the app's `now`.

Egress is never converted to coroutines. A coroutine for a link's protocol sequence (dial, handshake, heal) must first pass a measured, off-by-default prototype: the file's total CCN falls, flash grows by at most 1 KB, latency stays inside the A/A noise band, and frames come from the link's own source.

**4. P2300 senders/receivers are rejected for now.** The standard library on the shipped toolchains does not carry them. They would bring the most machinery for the same `pump()`, and put a framework in the public API. The question can be reopened once libstdc++ ships them.

## Considered options

- **Coroutines as the core execution model.** Rejected: the toolchain facts above, the three hazards in Decision §2, and the API lock-in. Bindings for ROS 2, C and Arduino-class builds would all be forced onto libtracer's task type.
- **Senders/receivers (P2300).** Rejected for now (Decision §4).
- **A plain poll seam with a single executor (ADR-0087 §3).** Chosen. It gives the whole single-core gain, adds about 0 B and frees 12,928 B per removed link task, and needs nothing from the toolchain.

## Consequences

- The next work is #2147 and #2148, then #2596 (S2, the single-executor sync policy), then #2152 and #2304.
- The TWAI link's 100 ms `xQueueReceive` poll and the hosted links' 100 ms receive quanta are no-timers violations. They are deleted in #2147.
- **Decisive bench, in the C6 hardware batch after #2147 and #2148 land:** before and after in one image, best-of-rounds. It passes when the libtracer-owned tasks on `full_node` fall to 0 and free at least 12,928 B per removed link, and p99 write-to-delivery latency at 64 B, 1 KiB and 4 KiB is no worse. The batch also records switches per frame, minimum-ever free heap and per-file CCN.
- Moving off `esp_http_server` in #2152 loses its TLS. Security relies on Noise ([RFC-0033](../spec/rfcs/0033-noise-nnpsk0-datagram-link-binding.md)). Before #2152 merges, check that lwIP `altcp_tls_mbedtls` builds under IDF. httpd stays as a compile-time fallback only if a TLS-only peer turns out to be a real requirement.
- Application code that wants coroutines can have them through the adapter of Decision §3, without core cost.
