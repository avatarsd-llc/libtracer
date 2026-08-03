# The host transport is a separate translation unit with a shared-nothing epoll model

Status: **accepted (design); not implemented.** Records two decisions that were previously unrecorded: *why the shipped TCP server multiplexes on one `poll()` thread* (a choice that has lived only as a code comment citing [#362](https://github.com/avatarsd-llc/libtracer/issues/362)), and *what a many-core host does instead*. Binds through [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md)'s build-time module sets; upholds [ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md) (allocation reports failure by value). **Bounded by its own erratum: the prerequisite bench now measures the threshold — limits 2 and 3 bind above ~4 000 concurrent peers, and below that width this decision is not yet paid for. See §Consequences.**

## Context

libtracer serves a single-core ESP32-C6 and a many-core host from one codebase. The TCP transport currently serves both from one file, and the way it does so is a deliberate MCU-shaped choice that no ADR records.

**The shipped server is one thread.** `transport_tcp_server::run` (`core/src/transport_tcp.cpp:521`) multiplexes the listen socket and every live peer in a single `poll()` pass, and says so:

> `// ONE poll pass multiplexes the listen socket and every live peer — no`
> `// per-peer thread (the MCU-shaped choice, #362)`

Three properties follow, and they are separable:

1. **One thread** services accept and all peers, so a 24-core host runs the transport on one core.
2. **`poll()`, not `epoll`** — the kernel rescans the whole descriptor set on every call, so cost grows with peer count even when peers are idle.
3. **The `pollfd` array is rebuilt under `peers_m_` on every pass** (`:528-540`) — O(peers) of userspace work plus a mutex acquisition per iteration, again independent of how many peers are active.

**The MCU cannot adopt the fix.** ESP-IDF's lwIP provides `select` and `poll` only; there is no `epoll_create`/`epoll_ctl` anywhere in the component, and `sockets.h` defines `lwip_poll` as `poll`. `epoll` is a Linux-host capability that cannot be pushed down. The portable file is therefore **correct for its platform**, not a compromise — and the ESP build compiles that same file (`integrations/esp-idf/libtracer/CMakeLists.txt:85-86`), resolving `<sys/socket.h>` into lwIP, which is an unconditional `PRIV_REQUIRES` for exactly that reason.

**A platform-swap mechanism already exists and already ships.** `httpd_ws_link.cpp` replaces the portable raw-socket WS server on chip targets, described in the same file (`:141-151`) as

> *"a platform link picked by which TU compiles (never an in-source `#ifdef`)"*

with `--gc-sections` dropping whichever TU the node does not reference. Nothing new has to be invented to split a transport per target.

**The host target is Linux.** All 40 CI jobs run on `ubuntu-24.04` / `ubuntu-latest`. There is no Windows or macOS target, so a cross-platform I/O abstraction has no portability value here.

## Decision

**A host-only translation unit implements the TCP server with `SO_REUSEPORT` and one `epoll` set, one accept path and one session set per thread — shared nothing.** The kernel load-balances accepts across the per-thread listen sockets. The portable `poll()`-based file stays exactly as it is and remains the MCU implementation. Selection is by which TU the build compiles, following the `httpd_ws_link.cpp` precedent — never an in-source `#ifdef`.

Shared-nothing is the substance of the decision, not an implementation detail: because no session map is shared, `peers_m_` and the per-pass descriptor rebuild have nowhere to live, so limits 2 and 3 above are removed by the same change that removes limit 1.

## Consequences

- **Two implementations of one transport must stay behaviourally identical.** This is the cost, and this project has paid it before: RFC-0014 shipped two silent misroutes because no test exercised the production wiring. Any conformance or interop assertion about the TCP transport must run against **both** TUs, or the split will diverge silently.
- **A connection is pinned to the thread that accepted it.** `SO_REUSEPORT` balances *accepts*, not *load*. A deployment with few, long-lived, very uneven peers can imbalance; one with many peers cannot meaningfully.
- **`io_uring` stays reachable.** It is the only lever measured to move the dominant term (below), so foreclosing it would be the expensive mistake.
- **`epoll` becomes ambiguous vocabulary.** `CONTEXT.md` already uses it as a *metaphor* — "`await` / `:subscribers[]` = the readiness/notify plane (`epoll`)". Once a real `epoll` exists in the transport, the glossary needs a disambiguating line.
- **UART is deliberately out of scope.** libtracer has no serial transport today (no `termios`, no `/dev/tty`). Whether to add one, and whether a library's `serial_port` is worth a dependency there, is a separate decision and is **not** settled by the rejection below.

### What this ADR does not establish

~~**No measurement shows the single poll thread is the host's limit.** `bench_tcp_fanin`'s 16→32 peer ratio measured **1.08× with overlapping ranges**, so that instrument cannot establish its own conclusion. The decision was taken as design debt — a structural single-core cap that should not be discovered later — and not as a response to an observed bottleneck. **Do not cite this ADR as evidence of a measured win.** A peer-scaling bench that can resolve the question is prerequisite to claiming one.~~

**Erratum: the prerequisite bench now exists, and it measured the threshold.** `bench/bench_tcp_peer_scaling.cpp` holds the offered load at one sender paced below saturation and sweeps connected-but-silent peers, so descriptor scanning is priced with the work held constant. Median of 5 per point:

| idle peers | delivered, against 100 000 f/s offered | sender p50 |
| ---: | ---: | ---: |
| 512 | 100 005 | 1 093 ns |
| 2 048 | 100 059 | 1 052 ns |
| 4 096 | 101 203 | 992 ns |
| **8 192** | **76 720 — 77 %** (min 72 264) | 1 052 ns |

**Limits 2 and 3 bind, and the knee is between 4 096 and 8 192 idle peers.** At 8 192 the server cannot drain what is offered while the sender's own p50 stays flat, so the shortfall is entirely server-side — 8 193 `pollfd` entries rebuilt under a mutex on every pass is what that is. Limit 1 is a separate story and is still **not** demonstrated: the active arm rises ×12.25 aggregate across 1→8 active peers, so the thread itself is not saturated there.

**What this bounds.** The decision above stands and is now paid for **at deployments above roughly 4 000 concurrent peers**. Below that width nothing in this ADR is shown to bind and the implementation is not yet justified — a host serving hundreds of peers should keep the portable file. That is a sharper claim than either "build it" or "do not", and it is the one the numbers support.

**One caution carried from the bench's own header, because it produced three confident wrong answers first.** A *narrow* sweep reports "does not bind", which was true at 512 and false as a conclusion; the width is a parameter (`LIBTRACER_BENCH_IDLE_MAX`), not a constant. And a paced arm stops being valid the moment the server cannot keep up, because the shortfall then moves into the throughput column a latency-only verdict does not read.

Related: `p999` is not a reportable statistic on this project's harness at any sample count yet tested (run-to-run 17× at n=10 000, 23.8–35.7× at n=20 000). Tail claims about any transport change must use `p99`, which is stable as a median-of-N at n ≥ 10 000.

## Considered options

- **Adopt asio for the host I/O layer.** Rejected on measurement (a four-arm interleaved echo harness: raw / asio+caller-owned / asio+libtracer `DynamicBuffer_v2` / asio+`streambuf`, 11 reps, n=20 000 samples per rep).

  *The per-frame case does not survive its own control.* Running the identical binary twice per rep — the null arm — moved throughput **1.099×** at 8 KiB and p50 **2.415×** at 64 B, i.e. more than any treatment; every timing cell overlaps. Worse for the zero-copy premise, the arm that copies **most** posted the best 8 KiB median (`streambuf`, 127.8 % of payload, 271 485 f/s) against the arm that copies least (11.2 %, 267 478 f/s) — an ordering that is impossible if copying were a term here.

  *The profile is the syscall, not the copy.* `writev`/`sendmsg` is **96.42–96.63 %** of syscall time in all four arms, which post an identical **1.063 syscalls/frame**. One send costs 920–1 780 ns; the copy the zero-copy arm removes is **2.26 ns** at 68 B and **39.9 ns** at 4 100 B — 0.13–4.3 % of a single send. For scale, one `writev` is **10.6–20.5× the entire 87 ns forward hop**. No choice of I/O engine touches that term.

  *The costs are real and the portability benefit is nil.* **+126 199 B** of static text for TCP+UDP (6 231 → 132 430 B, **21.3×**); **6.0×** compile time per TU (476 → 2 856 ms, median of 9); ~334 B per socket before any buffer (88 B `sizeof(tcp::socket)` exact, ~246 B heap) against **232 B measured for the entire shipped connection**; and 40/40 CI jobs are Linux, so the cross-platform abstraction that is asio's principal reason to exist buys nothing.

  *It cannot honour the allocation doctrine.* `DynamicBuffer_v2::grow()` returns `void`. An exhaustion probe threw in 5 of 6 arms **including a bounded slab**, which is `abort()` under `-fno-exceptions`. ADR-0065 exists because `std::pmr` cannot carry a failure signal; routing allocation through asio inherits that defect rather than fixing it. The shipped framer already answers exhaustion by value (`transport_tcp.cpp:222-238`: drain, count, never OOM).

  Recorded at this length because the proposal has been raised three times; the intent is that it not be re-litigated without new numbers on the **syscall** term.

- **A libtracer-backed asio `DynamicBuffer_v2`.** Considered specifically because `docs/reference/10-module-catalog.md` had rejected `mem_asio_streambuf` while offering only two options — copy-on-import, or forking the buffer — and never considered that `DynamicBuffer_v2` is a documented user-implementable concept. That gap in the option set is real and is corrected separately. The implementation is still rejected: **every timing cell overlaps the plain asio arm**, its only genuine edge is allocation, and the shipped library already measures **0 allocations per frame on egress** (0/0 both directions with an injected `pool_t` RX backend) without asio — at a cost of **65 536 B per connection** of ring against 232 B shipped, **282×**.

- **One shared `epoll` set with a thread pool (`EPOLLEXCLUSIVE`).** Rejected: it balances load better across uneven peers, but retains a shared session map and its lock, so limits 2 and 3 must then be solved separately. It buys balance for a problem no deployment has demonstrated, at the price of keeping the contention the shared-nothing model deletes for free.

- **Stay single-threaded; only replace `poll()` with `epoll` and drop the per-pass rebuild.** Rejected as the decision, kept as the fallback. It removes limits 2 and 3 and leaves the core cap — which is acceptable if per-peer frame rate, rather than peer count or core count, turns out to be what binds. If the prerequisite peer-scaling bench shows the single thread is not the limit, this becomes the right answer and this ADR should be revisited.

- **`io_uring` now.** Deferred, not rejected. It is the only measured lever on the 96 % syscall term — the goal being **syscalls per frame below 1.0**, via batched submission and registered buffers. It is also the largest change to buffer ownership and the most kernel-version-sensitive. The shared-nothing model above is a prerequisite shape for it rather than an obstacle.
