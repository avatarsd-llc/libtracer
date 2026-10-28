/**
 * @file
 * @brief The receive-thread and endpoint scaffold every POSIX socket transport shares.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * posix_endpoint — the recv-thread/endpoint scaffold every POSIX socket
 * transport shares (udp / tcp / ws server / ws client). One home for the
 * stop-flag + receive-thread lifecycle and the 100 ms poll/timeout idioms that
 * let a blocking socket loop notice a clean shutdown; the transports keep what
 * is genuinely theirs (fds, receivers, counters, framing). The STREAM
 * transports additionally share the one-peer fd/teardown discipline —
 * stream_endpoint_t below owns that layer (the peer-fd atomic, the write
 * mutex, the teardown-under-write-lock ordering, the one-peer accept loop);
 * udp keeps its datagram shape (one connectionless fd, no peer teardown).
 * The MULTI-peer stream servers (tcp / ws) layer one more tier on top —
 * slot_server_t, the slot vector + accept/poll/teardown machinery and the
 * peer query trio (#871); only their framing and handshake differ, and
 * those are its two variance points. The ADR-0044 BUS FACET is a tier below
 * that again (bus_slot_server_t / flat_slot_server_t, selected by
 * stream_server_base_t), so a build with no bus module carries a listener
 * whose layout does not contain it (#1438).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "libtracer/egress.hpp"
#include "libtracer/function_ref.hpp"
#include "libtracer/inline_fn.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/static_thread.hpp"
#include "libtracer/transport.hpp"

/** @brief The POSIX scatter-gather descriptor (`<sys/uio.h>`), forward-declared
 *         so this header need not pull the system socket headers in. */
struct iovec;
/** @brief The POSIX IPv4 address (`<netinet/in.h>`), forward-declared for the same reason. */
struct in_addr;
/** @brief The POSIX poll descriptor (`<poll.h>`), forward-declared for the same reason. */
struct pollfd;

namespace tr::detail {

/**
 * @brief TEST SEAM: consulted immediately before each `send(2)`/`sendmsg(2)` the full-write
 *        helpers issue; a non-zero return makes that ONE attempt fail with that errno
 *        instead of reaching the kernel (#948).
 *
 * The write-fault classification exists for errnos a healthy host kernel never produces here
 * — a call this library got WRONG (`EOPNOTSUPP` from an lwIP `sendmsg` handed a flag it does
 * not implement, `EINVAL`, …). glibc produces none of them, so the classification's arms are
 * unreachable without injection and any host test of them would be vacuous by construction.
 * This is their failure-injection tool: return the errno to fake, or 0 to let the real
 * syscall run.
 *
 * Same shape and same rules as its neighbour @ref probe_fail_hook (and it lives in the same
 * namespace deliberately: `tr::net::detail` would SHADOW this one for every unqualified
 * `detail::` lookup inside `tr::net`, of which `transport.hpp` and `transport_tcp.cpp`
 * already have several): install it before the write that should trip it, and clear it
 * before the test returns. Consulted only when the build binds
 * `%tr::graph::default_config_t::kFaultInjection` (#1719); a default build carries no
 * branch for it and emits no definition of it.
 */
inline int (*write_fault_inject_hook)() noexcept = nullptr;

}  // namespace tr::detail

namespace tr::net {

/**
 * @brief The malformed-call write-fault tally of the POSIX stream transports (#948).
 *
 * Process-wide, because the fault it counts is a defect in libtracer's OWN syscall
 * arguments rather than a property of any one connection — the full-write helpers are
 * static and shared by every stream transport, and a non-zero count means the same bug on
 * all of them.
 */
struct write_fault_stats_t {
    /**
     * @brief How many `send`/`sendmsg` attempts failed with an errno that means "this call
     *        was malformed", not "this socket is dead".
     *
     * Non-zero is ALWAYS a libtracer defect (or an unsupported platform syscall surface):
     * the frame's bytes were still deliverable. Zero on every supported host.
     */
    std::uint64_t malformed_calls = 0;
    int last_errno = 0; /**< @brief The errno of the most recent such fault (0 = none yet) —
                                    what the defect actually was, e.g. `EOPNOTSUPP`. */
};

/**
 * @brief Read the `write_fault_stats`_t tally (relaxed; a diagnostic read, not a
 *        synchronizer).
 */
[[nodiscard]] write_fault_stats_t write_fault_stats() noexcept;

/**
 * @brief Parse the dotted-quad IPv4 @p host into @p out — `inet_pton` over a view.
 *
 * The socket transports take their peer address as a `std::string_view` (ADR-0083 Q15,
 * #1780); `inet_pton` wants a terminated string, so the text is copied into a stack buffer
 * of `INET_ADDRSTRLEN` bytes first. Nothing is allocated.
 * @retval false @p host is not a dotted quad (too long for one included); @p out is unchanged.
 */
[[nodiscard]] bool parse_ipv4(std::string_view host, ::in_addr& out) noexcept;

/**
 * @brief The default peer LIVENESS WINDOW, milliseconds — how long a peer may fail to
 *        take bytes before it is treated as broken (#838).
 *
 * The bound's provenance is an APP-PROVIDED liveness window, not a core literal: the
 * deployer injects it (`liveness_window` — see the stream transports' constructors and the
 * `tcp`/`ws` factory keys), exactly as `connect_timeout`, CAN's `peer_ttl` (ADR-0044) and
 * `httpd_ws_link_t::send_timeout_ms` are injected. An MCU derives its own bound from the
 * task watchdog (`T = TWDT / cap`, #835); a host has no watchdog to derive from, so the
 * injection arm IS the answer there.
 *
 * This value is what an unconfigured deployment gets, and it exists because "safe unless
 * you opt out" beats "buggy unless you opt in": before #838 a host stream send was
 * UNBOUNDED, so one peer that stopped reading froze the sending application thread — and
 * everything queued behind it — forever. Ten seconds is a conservative clamp on that: far
 * longer than any healthy loopback/LAN peer needs to accept one frame (so it never fires
 * in normal operation), and far shorter than "never" (so a stalled peer is detected and
 * shed while the node is still alive). It is the fallback of last resort — a deployment
 * that knows its own SLA injects that number instead.
 */
inline constexpr std::uint32_t kDefaultLivenessWindowMs = 10000;

/**
 * @brief The floor under every derived per-record send bound, milliseconds.
 *
 * Two jobs, neither of them a policy knob. (1) A derived bound is a DIVISION
 * (window ÷ peers in the round) and a division has no natural floor — the #956 lesson from
 * #837's MCU twin, where `SO_SNDTIMEO = 0` means *block forever*, so a bound that rounds
 * to zero would restore the very defect being fixed. (2) It is the same 100 ms quantum
 * @ref posix_endpoint_t::set_rcv_timeout and @ref posix_endpoint_t::poll_readable already
 * use for "one bounded wait before re-checking the world" — a write that cannot move one
 * byte inside the interval the read side already treats as a whole pass is not making
 * progress. Not a new number: the one this file already lives by.
 */
inline constexpr std::uint32_t kBoundedWaitMs = 100;

/**
 * @brief Whole rings of consecutive strikes that mark a one-peer link's connection broken
 *        (#838): offers that found the peer's STREAM lane full and whose bounded write turn
 *        moved NOTHING, counted in rings of the lane's depth. A SAMPLE lane never strikes.
 *
 * The brokenness-detector trichotomy #837 established: one ring is transient backpressure,
 * and any turn that moves a byte resets the count. Two can straddle a burst. Three in a row,
 * each turn a whole bound long and each past a full lane, means the peer is not riding out a
 * burst at all. A multi-peer server's turns never wait, so its cap is
 * `kEgressStallStrikes` (#2144). A brokenness detector, not a tunable — hence a named
 * constant and NOT a config knob.
 */
inline constexpr std::uint8_t kMaxConsecutiveStalls = 3;

/**
 * @brief The concurrent-peer admission cap of a stream server that was given none (RFC-0006).
 *
 * A deployment injects its own `max_peers`, and that number alone is the cap: a newcomer past
 * it is refused, and no admitted peer is ever evicted for it. The cap is a RAM decision, so it
 * is not derived from the liveness window: since #2144 a server's peer writes never wait, and
 * the peers it admits no longer add up to blocking. `0` asks for this default instead.
 */
inline constexpr std::size_t kDefaultMaxPeers = 100;

/**
 * @brief How a full-record write to one peer ended (#838).
 *
 * The stream transports need more than "it returned": a record that only PARTLY reached
 * the socket has desynced that stream's framing permanently (every later byte parses under
 * the wrong length), which is a different fault class from a record that never started —
 * and both are different from the peer simply being gone.
 */
enum class write_outcome_t : std::uint8_t {
    COMPLETE, /**< @brief Every byte of the record reached the socket. */
    STALLED,  /**< @brief The send bound expired with the record unfinished — the peer is
                          not taking bytes. */
    FAILED,   /**< @brief Abandoned: the socket is dead (#66 lifecycle), or the call itself
                          was rejected past its one re-attempt (counted in
                          `write_fault_stats`). */
};

/**
 * @brief The result of one full-record write — its outcome plus whether the stream
 *        survived it (#838).
 */
struct write_result_t {
    write_outcome_t outcome = write_outcome_t::COMPLETE; /**< @brief How the write ended. */
    /**
     * @brief Bytes of the record that reached the socket.
     *
     * Short of the record's length on a STALLED write, this is where the rest resumes: the
     * egress keeps the frame and its offset (#2144), so a part write is never a desync on a
     * live connection. On a FAILED one, non-zero means the peer holds a header whose payload
     * will never arrive.
     */
    std::size_t moved = 0;
};

/**
 * @brief Derive the per-record send bound for a round covering @p peers peers, ms (#838).
 *
 * One full fan-out round — one bounded record to EVERY peer in it, all of them stalled —
 * must fit inside ONE liveness window, because the round is what holds the write lock and
 * the sending application thread. So the window is divided by the number of peers actually
 * in the round (a fact in hand at the call site, not a guessed cap), and floored at
 * `kBoundedWaitMs` so the division can never yield "block forever".
 *
 * **What @p peers is for a DIRECTED send** (#1295). A directed send —
 * `bus_link_t::peer_link(name)->send()` — is a round of ONE, so passing the round's own
 * size handed it the whole window: N callers targeting N stalled peers each took a full
 * window, and because directed sends on one server then serialized on one write lock they
 * accumulated N of them. Bounded per call, unbounded per node — which is not a bound.
 * A directed send on a stream SERVER therefore divides by the server's admission cap
 * (@ref slot_server_t::max_peers), not by 1: the cap is the most peers that can ever be
 * directed at concurrently, so the sum over all of them is one window, the same claim the
 * broadcast round makes. The
 * genuinely single-peer endpoints (a dialled client, a one-peer LISTEN) still pass 1 —
 * for them the round of one IS the whole node.
 *
 * @param window_ms The peer liveness window (0 ⇒ `kDefaultLivenessWindowMs`).
 * @param peers     How many peers share this bound (0 and 1 both mean "one bound").
 * @return The per-record bound in milliseconds, never 0.
 */
[[nodiscard]] constexpr std::uint32_t derive_send_bound_ms(std::uint32_t window_ms,
                                                           std::size_t peers) noexcept {
    const std::uint32_t window = window_ms != 0 ? window_ms : kDefaultLivenessWindowMs;
    const std::uint32_t n = peers > 1 ? static_cast<std::uint32_t>(peers) : 1U;
    const std::uint32_t bound = window / n;
    return bound > kBoundedWaitMs ? bound : kBoundedWaitMs;
}

/**
 * @brief The shared recv-thread scaffold of the POSIX socket transports.
 *
 * A protected base (inherited privately by the concrete transports) owning the
 * `stop_` flag and the receive thread, plus the socket-timeout/poll idioms that
 * make a blocking loop shutdown-responsive: every blocking wait is bounded to
 * 100 ms (SO_RCVTIMEO or `poll(2)`), after which the loop re-checks `stop_`.
 *
 * **Teardown invariant (derived destructors):** call @ref stop_and_join
 * FIRST, before releasing ANY resource the thread body touches (sockets,
 * receivers, buffers) — the thread may be mid-loop until the join returns.
 *
 * Stream transports (tcp / ws) layer the shared one-peer fd/teardown
 * discipline on top via @ref stream_endpoint_t — the write-serialization and
 * teardown-under-write-lock invariants live there, with the code.
 */
class posix_endpoint_t {
   protected:
    /** @brief The receive thread's body: a stored callable with inline storage (ADR-0083
     *         Q10) — every transport's body captures `this` and at most a descriptor. */
    using thread_body_t = inline_fn_t<void()>;

    /** @brief Constructs with no thread running and @ref stop_ clear. */
    posix_endpoint_t() = default;

    /**
     * @brief Joins a still-running thread as a last resort.
     *
     * Derived destructors must have called @ref stop_and_join already (see the
     * teardown invariant above) — by the time this runs, derived members the
     * thread touches are gone. The defensive join only covers a derived class
     * that never spawned a thread or already joined it (both no-ops).
     */
    ~posix_endpoint_t();

    posix_endpoint_t(const posix_endpoint_t&) = delete;
    posix_endpoint_t& operator=(const posix_endpoint_t&) = delete;

    /**
     * @brief Spawn the receive thread running @p body.
     *
     * Call at most once, after the socket is up and every resource @p body
     * touches is initialized. @p body must poll @ref stop_ (directly or via the
     * bounded waits below) and return promptly once it is set. Usually that is
     * the derived constructor; a transport offering the two-phase bring-up
     * (`%transport_t::start_receiving` — the owner installs its sinks BEFORE any
     * frame can be decoded) calls it from there instead, and owns the one-shot
     * latch that keeps "at most once" true.
     *
     * Spawns through @ref static_thread_t (#2288), never `std::thread`: the constructor of the
     * latter THROWS on failure, which under `-fno-exceptions` (the MCU build) `std::abort`s.
     * A refused spawn leaves the endpoint simply not receiving (no abort). On ESP-IDF the
     * thread is a static FreeRTOS task whose control block and stack are ONE block drawn from
     * @p src; on a POSIX host it is a `pthread_create` and @p src pays only a small boot
     * record.
     *
     * @param body       The thread body (the transport's accept/recv loop).
     * @param stack_size Recv-thread stack size in bytes, or 0 for the platform
     *        default (`CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT` on ESP-IDF). A non-zero hint
     *        right-sizes this thread instead of inflating that default for every pthread in
     *        the system. On POSIX a hint below the platform floor is ignored (the default
     *        stack is used) rather than failing the spawn.
     * @param src        Where the thread's memory comes from (see above); it MUST outlive
     *        @ref stop_and_join. The default is the build's net sub-pool, which on an MCU
     *        arena build is the static arena: size the arena for the stack, or pass a source.
     */
    void start(thread_body_t body, std::size_t stack_size = 0,
               mem::block_source_t& src = mem::net_source());

    /**
     * @brief Request shutdown and join the receive thread (idempotent).
     *
     * Sets `stop_` and joins the thread if one is running. MUST be the
     * FIRST act of every derived destructor — only after it returns may the
     * destructor release the resources the thread body touches.
     */
    void stop_and_join();

    /**
     * @brief Arm the 100 ms receive timeout (SO_RCVTIMEO) on @p fd.
     *
     * The idiom that keeps a blocking `recv`/`recvfrom` loop shutdown-
     * responsive: each blocked read wakes within 100 ms so the loop can
     * re-check `stop_` and resume (or exit) — one home for the constant.
     *
     * @param fd The socket to arm.
     */
    static void set_rcv_timeout(int fd);

    /**
     * @brief Arm the bounded SEND timeout (`SO_SNDTIMEO`, `kBoundedWaitMs`) on @p fd (#838).
     *
     * The egress twin of @ref set_rcv_timeout, and the syscall-level half of the #838 fix:
     * without it a `send`/`sendmsg` into a peer whose TCP receive window is full blocks
     * INDEFINITELY — with the write mutex held — so one stalled-but-not-dead peer freezes
     * the sending application thread and everything queued behind it.
     *
     * The option is deliberately the short 100 ms quantum rather than the policy bound: it
     * is what makes each blocked syscall RETURN so the software deadline
     * (@ref stream_endpoint_t::write_all_iov's @p bound_ms, derived from the liveness
     * window) can be observed. Putting the policy bound on the socket instead would bound
     * each syscall but not the record, since a stream write may need several.
     *
     * @param fd The socket to arm.
     */
    static void set_snd_timeout(int fd);

    /**
     * @brief One bounded readability wait: `poll(2)` for POLLIN with a 100 ms timeout on @p fd.
     *
     * The poll-flavored twin of @ref set_rcv_timeout for loops that wait
     * before reading. Returns the raw `poll(2)` result — `> 0` readable,
     * `0` timeout (re-check `stop_` and continue), `< 0` error.
     *
     * @param fd The socket to wait on.
     * @return The `poll(2)` return value.
     */
    static int poll_readable(int fd);

    /**
     * @brief One iteration of the poll-100ms-recheck accept loop.
     *
     * Waits up to 100 ms for @p listen_fd to become readable, then accepts.
     * Returns the accepted fd, or -1 on timeout / poll error / accept failure
     * — the caller's loop simply continues, re-checking @ref stop_ each pass.
     *
     * @param listen_fd The bound+listening socket.
     * @return The accepted connection fd, or -1 when there is none this pass.
     */
    static int poll_accept(int listen_fd);

    /**
     * @brief The shutdown flag every blocking loop polls (set by
     *        @ref stop_and_join; read with relaxed order — it is a flag, not
     *        a synchronizer; the join provides the ordering).
     */
    std::atomic<bool> stop_{false};

   private:
    /** @brief The thread entry trampoline: runs `body_` of the @ref posix_endpoint_t @p self. */
    static void thread_entry(void* self);

    thread_body_t body_;     /**< @brief The thread body `thread_entry` runs. */
    static_thread_t thread_; /**< @brief The receive thread (joined by stop_and_join). */
};

/**
 * @brief The one-peer fd/teardown discipline every POSIX STREAM transport
 *        shares (tcp dial+listen, ws server, ws client).
 *
 * Owns the live peer fd (@ref conn_fd_) and the link's egress (@ref egress_, ADR-0087 §2),
 * and is the ONE home of the invariants that keep a concurrent `send()` and the recv thread's
 * connection teardown safe against each other:
 *
 * **Write-serialization invariant:** every write to a peer fd is made by the sender holding
 * that peer's egress WRITER ROLE (@ref egress_send), elected by try-lock: a sender that finds
 * the peer idle writes its own record, one that finds a write in flight queues a reference
 * and returns. So two senders never interleave records on the stream and no publisher waits
 * on another publisher's write. A record the socket cuts short keeps its frame and offset,
 * and the rest goes on the peer's next turn, so a part write never desyncs the framing.
 *
 * **Teardown-under-role invariant:** a recv thread that closes the peer fd first
 * `shutdown(2)`s it (an in-flight write returns), then closes the peer's egress — which waits
 * for the writer role to be let go — and only then resets @ref conn_fd_ to -1 and
 * `close(2)`s it (@ref teardown_peer). So a sender never writes to (or reads) a closed or
 * reused fd, and the shared write mutex is gone.
 *
 * The one-peer accept loop shape (poll-100ms-recheck accept → per-peer setup
 * → serve → teardown → re-accept) shared by tcp's LISTEN mode and the ws
 * server lives here too (@ref run_accept_loop). A protected base, inherited
 * privately by the concrete stream transports; udp stays on plain
 * posix_endpoint_t — a datagram socket has no per-peer fd to tear down and
 * its single-syscall sends need no serialization.
 */
class stream_endpoint_t : protected posix_endpoint_t {
   protected:
    /** @brief Constructs with no peer connected (@ref conn_fd_ = -1); the egress draws from
     *         the process net sub-pool. */
    stream_endpoint_t() : stream_endpoint_t(mem::net_source()) {}

    /**
     * @brief Constructs with no peer connected, the egress drawing from @p tx_src.
     *
     * @param tx_src   The link's egress store: every shared frame, each (peer, vertex) ring,
     *                 and a frame's gather overflow (@ref write_frame).
     * @param one_peer This endpoint IS the one peer (a dialer, a one-peer listener), whose
     *                 write turn waits for a bound: its strike cap is `kMaxConsecutiveStalls`.
     *                 A multi-peer server passes false and keeps one peer per session; its
     *                 writes never wait, so its cap is `kEgressStallStrikes`.
     */
    explicit stream_endpoint_t(mem::block_source_t& tx_src, bool one_peer = true)
        : io_src_(&tx_src),
          egress_(tx_src, one_peer ? kMaxConsecutiveStalls : kEgressStallStrikes) {
        // Open from the start: a send before a peer connects finds no fd and sheds, counted.
        if (one_peer) egress_.open(peer_);
    }

    /**
     * @brief Closes a leftover peer fd (one the recv thread never tore down).
     *
     * Runs AFTER the derived destructor, whose first act was stop_and_join
     * (the posix_endpoint_t teardown invariant) — so no thread can race this.
     * A normally-torn-down connection already reset @ref conn_fd_ to -1 and
     * this is a no-op; it only catches a never-spawned thread (a failed dial /
     * handshake left the fd parked) so nothing double-closes.
     */
    ~stream_endpoint_t();

    stream_endpoint_t(const stream_endpoint_t&) = delete;
    stream_endpoint_t& operator=(const stream_endpoint_t&) = delete;

    /**
     * @brief Write @p bytes to @p fd completely, resuming partial writes.
     *
     * A stream write may stop anywhere; loops `send(2)` (MSG_NOSIGNAL — a
     * vanished peer must not SIGPIPE the process) until done. A signal that
     * interrupts the blocked write before any byte moved (EINTR) is RESUMED,
     * not abandoned — the connection is healthy, and a partial frame left on a
     * live framed stream would desync the peer's framing permanently (#903).
     * A socket-dead errno drops the rest silently (link-down is #66 lifecycle);
     * every OTHER errno means the call itself was malformed, is re-attempted once
     * and counted in `write_fault_stats()` rather than mistaken for a
     * disconnect (#948).
     *
     * A peer that stops taking bytes is bounded by @p bound_ms (#838): the write gives up
     * once the deadline passes, and @ref write_result_t says how far it got.
     *
     * @param fd    The destination fd; a negative fd is a no-op.
     * @param bytes The bytes to write.
     * @param bound_ms Deadline for the WHOLE record, ms; 0 = unbounded (the pre-#838
     *        behaviour, kept for sockets with no `SO_SNDTIMEO` armed — there a blocked
     *        write never returns to observe a deadline anyway).
     * @return How the write ended.
     */
    static write_result_t write_all(int fd, std::span<const std::byte> bytes,
                                    std::uint32_t bound_ms = 0);

    /**
     * @brief Write the gathered @p vec entries to @p fd as ONE record, resuming partial
     *        writes — the zero-copy scatter-gather twin of @ref write_all.
     *
     * `sendmsg(2)` (MSG_NOSIGNAL — a vanished peer must not SIGPIPE the process)
     * emits every iovec in one syscall; a stream write may stop anywhere, so the
     * loop resumes from the first unwritten byte. @p vec is READ-ONLY (#932): the
     * gather is NOT consumed, so the same array may be fanned to many fds with no
     * per-fd copy. EINTR resumes, a socket-dead errno drops the rest silently, and any
     * other errno is a malformed call that is re-attempted once and counted — the same
     * ONE write-fault policy as @ref write_all (#903 / #948; link-down is #66
     * lifecycle).
     *
     * @param fd       The destination fd; a negative fd is a no-op.
     * @param vec      The entries to gather, in order, as ONE record.
     * @param bound_ms Deadline for the whole record, ms; 0 = unbounded (see @ref write_all).
     * @param nonblock Write only what the socket takes now (`MSG_DONTWAIT`), reading no clock:
     *                 a full socket ends the write as `STALLED` with the bytes it took. The
     *                 shape of a multi-peer server, whose writes must never wait (#2144).
     * @return How the write ended, and how many bytes reached the socket.
     */
    static write_result_t write_all_iov(int fd, std::span<const ::iovec> vec,
                                        std::uint32_t bound_ms = 0, bool nonblock = false);

    /**
     * @brief One egress write turn: @p f from its byte @p off, gathered as one write — the
     *        copied head, then a kept value's links straight from the published block.
     *
     * The gather table is inline; its overflow comes from @ref io_src_ and is refused rather
     * than thrown, which writes nothing and reports a failed turn (a shed frame, never a
     * desync).
     *
     * @param fd       The destination fd.
     * @param f        The frame.
     * @param off      Bytes of @p f already on the wire.
     * @param bound_ms The turn's send bound (see @ref write_all); ignored with @p nonblock.
     * @param nonblock Write without waiting (see @ref write_all_iov).
     */
    egress_step_t write_frame(int fd, const egress_frame_t& f, std::size_t off,
                              std::uint32_t bound_ms, bool nonblock);

    /**
     * @brief One egress write turn of the caller's own gathered record (the direct write).
     * @see write_frame for the parameters.
     */
    static egress_step_t write_gather(int fd, std::span<const ::iovec> vec, std::uint32_t bound_ms,
                                      bool nonblock);

    /**
     * @brief A shared frame of the gathered record @p rec: what comes before @p value's links
     *        is copied, and @p value (whose non-empty links end @p rec) is kept by reference.
     * @return The frame, or null when the egress store refused it.
     */
    [[nodiscard]] egress_frame_t* frame_of(std::span<const ::iovec> rec,
                                           const graph::value_t* value = nullptr) const;

    /**
     * @brief Lay a retained send's record out as spans: @p head, then @p value's links.
     * @return The span count, or `parts.size() + 1` when it does not fit (the caller takes
     *         the base's copy path, `transport_t::send(head, value, lane)`).
     */
    [[nodiscard]] static std::size_t retained_parts(
        std::span<std::span<const std::byte>> parts,
        std::span<const std::span<const std::byte>> head, const graph::value_t& value) noexcept;

    /**
     * @brief Account one egress call (#838, #2144): every shed frame is a `dropped_tx_`, every
     *        turn that found the socket full and moved nothing a `stalled_tx_`.
     */
    void note_egress(const egress_result_t& r) noexcept;

    /**
     * @brief Condemn a peer that earned it: `shutdown(SHUT_RDWR)` its socket — the egress's
     *        `condemn` hook, run inside the writer role (#838, #2144).
     *
     * Shutdown, never close: it takes effect on this line, needs no cooperation from the
     * stalled socket, makes every later write fail at once and raises the readable-at-EOF the
     * recv/poll thread turns into the ordinary remote-departure teardown (the same path
     * @ref slot_server_t::close_peer uses). A negative fd is a no-op.
     */
    static void condemn_fd(int fd) noexcept;

    /**
     * @brief The one-peer @ref transport_t::close_frozen_peers (#2198): a dialer's, or a
     *        one-peer listener's. Only the TX direction is owed here: a frame held on the wire
     *        that moved no byte since the last check. A frozen peer is shut down inside its
     *        writer role, so the recv thread runs the ordinary teardown, and counted in
     *        @ref frozen_closed_.
     * @return 1 if the peer was closed, else 0.
     */
    std::size_t close_frozen_one(std::uint64_t now, std::uint64_t deadline);

    /** @brief Record @p s 's bytes in @p moved, a peer's TX progress flag (#2198); returns
     *         @p s. */
    static egress_step_t note_moved(std::atomic<bool>& moved, egress_step_t s) noexcept {
        if (s.moved != 0) moved.store(true, std::memory_order_relaxed);
        return s;
    }

    /**
     * @brief Put one record on the wire toward the ONE peer through its egress (ADR-0087 §2).
     *
     * The single-peer senders' one door. A publisher that finds the peer idle takes the
     * writer role and writes its record straight from its own memory through @p direct, with
     * the fd read inside the role; one that finds a write in flight puts a reference to the
     * shared frame @p frame builds in the peer's ring and returns at once. So no publisher
     * waits on another publisher's write to a stalled peer (#1619); the writer's own wait
     * stays bounded per turn by the liveness window (#838).
     *
     * @tparam Direct Callable `egress_step_t(int fd, std::uint32_t bound_ms)`: write the
     *                caller's own record within the turn's bound.
     * @param frame  Builds the shared frame (the record copied, a kept value by reference)
     *               when the record has to wait.
     * @param lane   The ring it waits in: its vertex's, or the control lane (a default).
     */
    template <class Direct>
    void egress_send(function_ref_t<egress_frame_t*()> frame, Direct&& direct,
                     const graph::delivery_lane_t& lane = {}) {
        const std::uint32_t bound = derive_send_bound_ms(liveness_window_ms_, 1);
        int fd = -1;
        const egress_result_t r = egress_.send(
            peer_, lane, frame,
            [&] {
                fd = conn_fd_.load(std::memory_order_acquire);
                // No live peer (still dialing, or torn down) => a counted drop.
                if (fd < 0) return egress_step_t{0, false, true};
                return note_moved(tx_moved_, std::forward<Direct>(direct)(fd, bound));
            },
            [&](const egress_frame_t& f, std::size_t off) {
                fd = conn_fd_.load(std::memory_order_acquire);
                if (fd < 0) return egress_step_t{0, false, true};
                return note_moved(tx_moved_, write_frame(fd, f, off, bound, false));
            },
            [&] { condemn_fd(fd); });
        note_egress(r);
    }

    /**
     * @brief One bounded 100 ms wait on the ONE peer's socket (recv-thread side): readable,
     *        and writable too while a frame is held (@ref egress_peer_t::wants_write).
     *
     * Writability resumes the held frame here, nonblocking (`egress_t::flush`), so a record
     * the socket cut short finishes without waiting for a later send (#2144).
     *
     * @return `> 0` readable (or hung up), `0` nothing to read, `< 0` a poll error.
     */
    int poll_io(int fd);

    /**
     * @brief Tear the peer connection down (recv-thread side).
     *
     * The teardown-under-role invariant as code: `shutdown(2)` the fd so a write in flight
     * returns, close the peer's egress (waiting out its writer, releasing its frames), THEN
     * reset @ref conn_fd_ to -1 and `close(2)` the fd — a concurrent `send()` either finished
     * against the still-open fd or finds the peer closed and sheds.
     *
     * @param fd The peer fd the recv loop was serving.
     */
    void teardown_peer(int fd);

    /**
     * @brief The @ref transport_t::shut_down of a one-peer stream link: join the receive
     *        thread, then tear the live peer down (@ref teardown_peer), so a later send finds
     *        the peer closed and sheds. Idempotent.
     */
    void shut_down_stream();

    /**
     * @brief The one-peer accept loop (tcp LISTEN / ws server shape).
     *
     * Until `stop_`: one poll-100ms-recheck accept pass (@ref poll_accept);
     * on a new connection run @p on_accept (per-peer setup — socket options,
     * handshake; return false to reject: the fd is closed and the loop
     * re-accepts), publish the fd to @ref conn_fd_ and reopen the peer's egress, run
     * @p serve_peer, then @ref teardown_peer and re-accept the next peer.
     *
     * @param listen_fd  The bound+listening socket.
     * @param on_accept  Per-peer setup; false rejects the connection.
     * @param serve_peer The per-connection recv loop; returns on peer
     *                   departure or `stop_`.
     */
    void run_accept_loop(int listen_fd, function_ref_t<bool(int)> on_accept,
                         function_ref_t<void(int)> serve_peer);

    std::atomic<int> conn_fd_{-1}; /**< @brief The live peer connection (-1 = none). */
    /** @brief The injected peer liveness window, ms (0 = `kDefaultLivenessWindowMs`) —
     *         the number every per-record send bound on this endpoint derives from
     *         (`derive_send_bound_ms`). Set once at construction, read-only after. */
    std::uint32_t liveness_window_ms_ = 0;
    /** @brief A byte of ours went out to the ONE peer since the last liveness check (#2198), in
     *         the hole after @ref liveness_window_ms_.
     *         Set by the writer, taken by @ref close_frozen_one; a new session sets it. */
    std::atomic<bool> tx_moved_{true};
    /** @brief Write turns that found the peer's socket full and moved nothing (#838, #2144)
     *         — the "how often did a stalled peer refuse us" counter, each one a strike.
     *         Relaxed: a diagnostic tally, not a synchronizer. Word-wide, not 64-bit
     *         (core/STYLE.md §Introspection clause 5, #1697): a 64-bit atomic is a
     *         libatomic call on every rv32; a 32-bit target wraps after 2^32. */
    std::atomic<std::size_t> stalled_tx_{0};
    /** @brief Outbound frames this link never put on the wire, every cause: a refused build,
     *         a closed peer, a full ring, a failed write. The link's `dropped_tx`. */
    std::atomic<std::size_t> dropped_tx_{0};
    /** @brief Peers the application's liveness check closed as frozen (#2198) — every one
     *         counted. Relaxed: a diagnostic tally. */
    std::atomic<std::size_t> frozen_closed_{0};
    /** @brief The application's `now` at the last liveness check that found the ONE peer
     *         moving or owing nothing (#2198). Written only by @ref close_frozen_one. */
    std::uint64_t live_since_ = 0;
    /** @brief This link's egress store: shared frames, rings, a frame's gather overflow
     *         (@ref write_frame), and a derived class's own link buffers (#1780). */
    mem::block_source_t* io_src_;
    /** @brief The egress module (ADR-0087 §2): the source and the strike cap. */
    egress_t egress_;
    /** @brief The ONE peer's egress — a dialer's or a one-peer listener's. A multi-peer
     *         server keeps one per session instead and leaves this one closed. */
    egress_peer_t peer_;
};

/**
 * @brief The MULTI-peer slot/poll machinery every stream SERVER shares
 *        (tcp_server_transport_t, ws_server_transport_t) — one listener, N
 *        recycled peer slots, one poll thread (#871).
 *
 * The tier above @ref stream_endpoint_t — that one owns a single peer fd, this
 * one owns a VECTOR of them. Everything the two servers used to restate
 * line-for-line lives here exactly once — the slot struct and its threading
 * rule, the bind/listen/getsockname bring-up, the free-slot-or-grow accept
 * with its @p max_peers refusal and `p<slot>~<gen>` naming, the poll loop, the
 * two-phase teardown, the peer query trio, and the broadcast's
 * pristine-iovec-copy-per-peer fan-out. Only the FRAMING and the HANDSHAKE
 * differ between the two servers, and those are the variance points below
 * (the `msquic_endpoint_t` shape: runtime virtuals, appropriate per ADR-0047
 * §4 because peer arrival/departure is wiring-frequency, not hot path).
 *
 * **It is NOT a @ref bus_link_t** (#1438, the provider half of #375
 * deliverable 3). The queries a bus facet needs are all here — they are
 * questions about the slot table, which exists either way — but the FACET
 * itself (the base subobject, its `peer_rx_` slot, its two peer-lifecycle
 * notifier pairs and their vtable entries) lives one tier down in
 * @ref bus_slot_server_t, so a build that closed the ADR-0044 bus module out
 * carries a listener whose LAYOUT does not contain it. Concrete servers
 * derive from `%tr::net::stream_server_base_t`, which is that arm or
 * @ref flat_slot_server_t according to @ref tr::net::kBusLinks.
 *
 * **Slot threading rule, ONE rule for both halves of a slot's lifecycle:**
 * @ref session_base_t::fd / @ref session_base_t::open are MUTATED only on the poll
 * thread — accept publishes the fd, then opens the session's egress, then `open`
 * (#891: "open ⇒ fd valid"); teardown clears `open`, shuts the fd down, closes the
 * session's egress (which waits out its writer), and only then resets and closes the fd.
 * A sender reads the fd only INSIDE the session's egress writer role (ADR-0087 §2), which
 * the egress hands over with acquire/release, so it never sees a half-published slot or a
 * closed one. @ref session_base_t::name and the slot table's growth are guarded by
 * @ref peers_m_, which no write is ever made under (#2263): a broadcast walks the slots by
 * @ref session_base_t::next with no lock, and the poll thread, the table's only mutator,
 * reads it with none. Every protocol buffer a slot carries is poll-thread-only. The
 * destructor's closing sweep runs after the poll thread is joined. There is no shared write
 * lock: a stalled peer holds only its own role.
 *
 * @warning A derived destructor MUST call `stop_and_join()` as its FIRST act:
 *          the poll thread dispatches the variance points below into the
 *          derived object, which must still be alive when it does.
 */
class slot_server_t : public transport_t, protected stream_endpoint_t {
   public:
    /** @brief The peer-visitor shape the query trio speaks — @ref bus_link_t's, so the
     *         facet arm's overrides are the same signature and no consumer sees two. */
    using peer_visitor_t = bus_link_t::peer_visitor_t;

    /**
     * @brief True if the listen socket is bound and listening — and, on a target that closed
     *        the bus module out, only if this server did not ask to be peer-named (#375).
     *
     * The came-up predicate `make_checked` asks (#1059), so the second limb is what turns a
     * `kBusLinks = false` build's refusal into an ordinary "this link did not come up" for
     * every door — the SPEC factory and a direct constructor alike. It is a REFUSAL rather
     * than a quiet demotion to FLAT because a demotion is not observable and this is: a
     * deployment that configured peer-named addressing on a build that carries none has a
     * configuration error, and a listener that answers `ok()` would hide it.
     *
     * At the default binding the `if constexpr` is discarded and this is `listen_fd_ >= 0`,
     * the predicate it always was — same instructions, verified by object-file `cmp`.
     */
    [[nodiscard]] bool ok() const noexcept {
        if constexpr (!kBusLinks)
            if (peer_named_) return false;
        return listen_fd_ >= 0;
    }

    /** @brief The actual bound TCP port (resolves an ephemeral 0 request). */
    [[nodiscard]] std::uint16_t local_port() const noexcept { return bound_port_; }

    /** @brief Write turns that found a peer's socket full and moved nothing, summed over
     *         every peer this server has carried (#838, #2144). `kEgressStallStrikes` of them
     *         in a row on one session close that session. */
    [[nodiscard]] std::uint64_t stalled_tx() const noexcept {
        return stalled_tx_.load(std::memory_order_relaxed);
    }

    /** @brief The injected peer liveness window this server bounds its sends by, ms —
     *         the value as configured, `0` meaning `kDefaultLivenessWindowMs` (#838). */
    [[nodiscard]] std::uint32_t liveness_window_ms() const noexcept { return liveness_window_ms_; }

    /** @brief The concurrent-peer admission cap ENFORCED on the accept path: the injected
     *         cap, or `%kDefaultMaxPeers` for `0`, so never 0. Also the denominator of
     *         @ref directed_send_bound_ms. */
    [[nodiscard]] std::size_t max_peers() const noexcept { return max_peers_; }

    /**
     * @brief The per-record send bound a DIRECTED send to one peer of this server gets, ms
     *        (#1295).
     *
     * The liveness window divided by @ref max_peers — NOT by 1. A directed send is a round
     * of one, but it is not the only one a node can have in flight: every open peer can be
     * the target of a concurrent directed send. Dividing by the cap makes the SUM over every
     * peer that could be stalled one window. Since #2144 a server's peer writes never wait,
     * so only the ws server's one blocking write, its `101`, still uses it.
     */
    [[nodiscard]] std::uint32_t directed_send_bound_ms() const noexcept {
        return derive_send_bound_ms(liveness_window_ms_, max_peers_);
    }

    /**
     * @brief The mode authority (#889): the `peer_named` this server was constructed with.
     *
     * The ONE answer to "which mode is this link in" — `bus()`, the two servers' per-frame
     * tier select, and the departure branch in @ref teardown_slot all key off this flag (not
     * off whether a peer sink happens to be installed), and `bus_link_t` refuses every
     * peer-named wiring call while it is false.
     *
     * It reads @ref bus_mode, not the constructor argument, so the one answer stays one
     * answer on a target that closed the bus module out: there the server is FLAT in every
     * respect, and @ref ok is what reports that the configuration was refused (#375).
     *
     * Not `override` here since #1438: this tier is not a @ref bus_link_t, so there is no
     * virtual to override until @ref bus_slot_server_t re-declares it. The ANSWER is
     * unchanged, and so is every caller's spelling.
     */
    [[nodiscard]] bool peer_named() const noexcept { return bus_mode(); }

    /** @brief Visit the currently-OPEN peers' names, `p<slot>~<gen>` (#426). */
    void enumerate_peers(const peer_visitor_t& visit) const;

    /**
     * @brief Resolve an inbound handle back to its peer name, `p<slot>~<gen>` (#1294).
     *
     * A pure function of the handle — its index and its tenancy generation — exactly as
     * the accept-side stamp is (ADR-0073 §2), formatted into @p scratch with no lock and no
     * slot lookup, so the router pays nothing for asking on the delivery callback that a
     * name string used to arrive on.
     */
    [[nodiscard]] std::string_view peer_name(peer_handle_t peer, std::span<char> scratch) const;

    /**
     * @brief The in-flight frame's peer — the WHO seam, answered at either setting of
     *        @ref peer_named (#375 Part 2, ADR-0082).
     *
     * A peer-named server tags every frame through the bus seam and never needs this. A FLAT
     * one cannot: it has exactly one routing identity for every peer it carries, and the
     * `p<slot>~<gen>` tag it computes is thrown away at the delivery fork. This is where that tag
     * survives — stamped on the poll thread immediately before the flat delivery, read
     * back, on that same thread, by the router's terminus, and cleared once the frame has
     * been handed up. Outside a delivery it answers the default (invalid) handle (#1915).
     *
     * @warning Poll-thread state, meaningful ONLY inside a receive callback. It is a plain
     *          member and not an atomic on purpose: one server owns one poll thread
     *          (ADR-0071's shared-nothing epoll), the store and every legitimate load happen
     *          on it, and paying for an atomic on the per-frame delivery path to make an
     *          off-thread read merely *defined* rather than *correct* buys nothing.
     */
    [[nodiscard]] peer_handle_t inbound_peer() const noexcept override { return delivering_; }

    /**
     * @brief The SUBJECT token of @p peer — `p<slot>~<gen>`, this kind's session identity.
     *
     * The same string @ref peer_name answers with, reachable without the `bus_link_t` facet;
     * see the implementation's note on why the two coincide in value and not in availability.
     */
    [[nodiscard]] std::string_view peer_subject(peer_handle_t peer,
                                                std::span<char> scratch) const override;

    /**
     * @brief Resolve an open peer's name to its directed sending endpoint.
     *
     * Owned by the peer's slot and pointer-valid for this server's lifetime (slots are
     * never freed, only recycled). After the peer departs its sends no-op until the slot
     * is reused.
     * @retval nullptr @p peer names no currently-open connection.
     */
    [[nodiscard]] transport_t* peer_link(std::string_view peer);

    /**
     * @brief The endpoint @ref peer_link resolves `p<slot>~<gen>` to, found by the handle's
     *        slot index instead of by comparing names (#1941). Session-scoped like
     *        @ref peer_link, so a departed session's handle names nobody.
     * @retval nullptr @p peer is not valid, or no open session holds it.
     */
    [[nodiscard]] transport_t* peer_link_of(peer_handle_t peer);

    /**
     * @brief Close the open peer named @p peer, freeing its slot for reuse.
     *
     * Shuts the socket down (`SHUT_RDWR`) under @ref peers_m_ (teardown un-names a slot
     * under the same lock before it closes the fd); the poll thread's next pass observes the
     * close and
     * runs the IDENTICAL remote-FIN teardown, so the recycle is asynchronous (within one
     * poll bound) and no poll-thread-only buffer is ever touched off-thread.
     * @retval true  @p peer named an open connection and its socket was shut down.
     * @retval false @p peer names no currently-open connection.
     */
    [[nodiscard]] bool close_peer(std::string_view peer);

    /**
     * @brief The application's liveness check over every open peer (#2198,
     *        @ref transport_t::close_frozen_peers).
     *
     * A peer owes TX while a frame waits on its wire, and RX while it has begun a frame or
     * a handshake and not finished it (@ref mid_frame). One that owed a direction and moved
     * no byte in it since its record fell below @p deadline is shut down exactly as
     * @ref close_peer does it, under @ref peers_m_, and counted in @ref frozen_closed. An
     * idle peer — owing nothing — is never closed.
     */
    std::size_t close_frozen_peers(std::uint64_t now, std::uint64_t deadline) override;

    /** @brief Peers the application's liveness check closed as frozen, summed over every
     *         peer this server has carried (#2198). */
    [[nodiscard]] std::size_t frozen_closed() const noexcept {
        return frozen_closed_.load(std::memory_order_relaxed);
    }

   protected:
    /** @brief Bytes a session's `p<slot>~<gen>` name holds, terminator included: `p`, the
     *         widest slot index, `~` and the widest generation. */
    static constexpr std::size_t kPeerNameChars = 24;

    /**
     * @brief The protocol-agnostic half of ONE peer slot.
     *
     * Slots are never destroyed while the server lives — recycled in place on departure
     * — so the endpoint facade @ref peer_link hands out stays pointer-valid for the
     * server's lifetime. A derived server extends this with its own framing state
     * (a length-prefix framer, a WS reassembler + byte buffers) and owns the concrete
     * @ref peer_endpoint facade object. See the class-level threading rule for who may
     * touch what.
     */
    struct session_base_t {
        /** @brief Constructs a free slot (no fd, not open, unnamed). */
        session_base_t() = default;
        session_base_t(const session_base_t&) = delete;
        session_base_t& operator=(const session_base_t&) = delete;

        std::atomic<int> fd{-1};       /**< @brief The peer socket; -1 ⇒ free slot. */
        std::atomic<bool> open{false}; /**< @brief True while the session may carry frames. */
        /** @brief A byte of ours went out since the last liveness check (#2198). Set by the
         *         writer, taken by @ref close_frozen_peers; an accept sets it. */
        std::atomic<bool> tx_moved{false};
        /** @brief The peer's socket was readable since the last liveness check (#2198). Set
         *         by the poll thread, taken by @ref close_frozen_peers; an accept sets it. */
        std::atomic<bool> rx_moved{false};
        /** @brief The peer has begun a frame or a handshake and not finished it (#2198):
         *         @ref mid_frame as of the poll thread's last pass over it. */
        std::atomic<bool> rx_owed{false};
        /** @brief The peer's routable NAME, `p<slot>~<gen>` — the slot index and this
         *         tenancy's generation (ADR-0073 §2, #426): stamped at accept, moved out by
         *         teardown (the eviction seam), so a reused slot gets a FRESH name and a
         *         route held for the previous session names nobody. A legal path
         *         segment, unlike the old `<ip>:<port>`. It identifies a SESSION, not a
         *         device — a reconnecting peer may land in a different slot; device-stable
         *         identity is a named link (RFC-0014). */
        char name[kPeerNameChars] = {};
        /** @brief @ref name as a view; empty while the slot is unnamed. */
        [[nodiscard]] std::string_view name_view() const noexcept { return name; }
        /** @brief This session's identity HANDLE, `(slot index, generation)` — what the
         *         peer-receiver seam tags every inbound frame with (#1294).
         *
         * Minted at accept and RETIRED at teardown (`generation = 0`, i.e. not
         * @ref peer_handle_t::valid), so a handle minted against the session that used to
         * hold this slot never matches its successor — the same validate-on-use stamp the
         * ESP link's session ref carries. Written on the poll thread under @ref peers_m_
         * beside @ref name, and read on that thread by the delivery path.
         */
        peer_handle_t handle;
        /** @brief This slot's monotonic tenancy counter — the source of the generation
         *         @ref handle is minted at. Bumped once per accept (never reused, never
         *         zero), so the counter survives the retire that clears @ref handle. */
        std::uint32_t gen_seq = 0;
        /** @brief The directed facade @ref peer_link returns — the derived slot's own
         *         member, registered here by @ref make_session. */
        transport_t* peer_endpoint = nullptr;
        /** @brief This session's egress (ADR-0087 §2, #2144): its frames waiting per vertex,
         *         its writer role and its strikes. Closed while the slot is free; reset as the
         *         slot is recycled, so a stalled peer's frames and strikes are never inherited
         *         by its successor in the slot. */
        egress_peer_t egress;
        /** @brief The application's `now` at the last liveness check that found this peer
         *         moving, or owing nothing (#2198). Written only by @ref close_frozen_peers,
         *         under @ref peers_m_. */
        std::uint64_t live_since = 0;
        /** @brief The slot admitted before this one, or null: the chain @ref broadcast walks
         *         without @ref peers_m_ (#2263). Stored once, before this slot is published
         *         at the chain's head, and never again: slots are recycled in place, never
         *         freed, so a walker that loaded a slot can always follow it. */
        std::atomic<session_base_t*> next{nullptr};

       protected:
        /** @brief Destroyed only as the derived slot it is: the slot table's
         *         `%tr::mem::poly_ptr_t` records that slot's destructor (#2022), so the base
         *         carries no vtable and no deleting destructor. */
        ~session_base_t() = default;
    };

    /**
     * @brief Constructs inert: no listen socket, no slots, no thread.
     *
     * @param max_peers  Requested concurrent-peer admission cap; a deployment-injected
     *                   bound (RFC-0006) — a connection beyond it is accepted and
     *                   immediately closed (a clean refusal, not a hung SYN). `0` takes
     *                   `%kDefaultMaxPeers`; any other value is enforced as given. Read
     *                   the enforced value back from @ref max_peers.
     * @param peer_named Expose the @ref bus_link_t facet (see @ref bus).
     * @param src        The store every session's egress rings, the shared frames and the
     *                   egress gathers are drawn from (the link's `memory.io`). Must outlive
     *                   the server.
     * @param state_src  The store the slot table, every session and each poll pass's tables
     *                   are drawn from (the link's `memory.state`, #1780). Must outlive the
     *                   server.
     * @param liveness_window_ms The app-provided peer liveness window, ms (0 =
     *                   `kDefaultLivenessWindowMs`) — see @ref directed_send_bound_ms (#1295).
     */
    slot_server_t(std::size_t max_peers, bool peer_named, std::uint32_t liveness_window_ms = 0,
                  mem::block_source_t& src = mem::net_source(),
                  mem::block_source_t& state_src = mem::net_source())
        : stream_endpoint_t(src, false),
          slots_(state_src),
          max_peers_(max_peers != 0 ? max_peers : kDefaultMaxPeers),
          peer_named_(peer_named) {
        liveness_window_ms_ = liveness_window_ms;
    }

    /**
     * @brief Closes the listen socket and sweeps every slot's fd.
     *
     * Runs AFTER the derived destructor, whose first act was `stop_and_join` — the poll
     * thread is gone, so nothing races this sweep and no virtual is dispatched from it.
     */
    ~slot_server_t();

    /**
     * @brief The shared bring-up: socket + SO_REUSEADDR + bind + listen(SOMAXCONN) +
     *        getsockname, publishing @ref listen_fd_ and @ref bound_port_.
     *
     * SOMAXCONN is the OS's own accept-queue bound — admission is per-connection in the
     * accept path (the @p max_peers deployment cap), never a synthetic backlog.
     *
     * @param bind_port TCP port to listen on (host byte order; 0 → ephemeral, resolved
     *                  into @ref local_port).
     * @retval false The socket could not be bound/listened; the caller must NOT spawn the
     *               poll thread (`ok()` stays false).
     */
    bool bind_listen(std::uint16_t bind_port);

    /**
     * @brief The ONE poll thread body: one `poll(2)` pass multiplexes the listen socket
     *        and every live peer — no per-peer thread (the MCU-shaped choice, #362),
     *        bounded to 100 ms so the loop stays shutdown-responsive.
     *
     * Spawn it from the DERIVED constructor (`start([this] { run(); }, recv_stack)`),
     * last, once every member the variance points touch is initialized.
     */
    void run();

    /**
     * @brief Tear one slot down and free it for reuse (poll thread only).
     *
     * Two phases: stop name resolution under @ref peers_m_ (so no new sender targets the
     * dying slot), then clear `open`, shut the fd down and close the session's egress —
     * which waits out a writer in flight — BEFORE resetting the fd and `close(2)`ing it.
     * @ref on_slot_reset clears the protocol buffers, and the departure seam
     * (RFC-0009 §D.5) fires LAST with no transport lock held — the notifier re-enters the
     * routing plane. Which seam depends on @ref peer_named() — the departed peer's own name
     * when peer-named, the whole link when flat, and then only once no open session is
     * left (#889).
     *
     * @param s The slot to recycle.
     */
    void teardown_slot(session_base_t& s);

    /**
     * @brief The @ref transport_t::shut_down of a stream server: join the poll thread, tear
     *        every session down exactly as a remote close does (@ref teardown_slot), and close
     *        the listen socket. Idempotent.
     */
    void shut_down_slots();

    /**
     * @brief Fan one already-encoded gathered record to EVERY open peer through its egress
     *        (ADR-0087 §2, #2144).
     *
     * An idle peer takes @p rec straight from the caller's gather (read-only, #932); a peer
     * with a write in flight, or whose socket takes only part of it, gets a reference to ONE
     * shared frame built at most once for the whole fan-out (the framing copied, @p value kept
     * by reference). Every write is nonblocking, so a stalled peer costs the round one refused
     * syscall and never delays the peers behind it; its frames wait in its own ring for
     * @p lane, and only ITS strikes condemn it. Shed frames and stalls are counted.
     *
     * The walk takes no lock (#2263): it follows @ref session_base_t::next, so accept,
     * teardown, the receive loop and every other sender run beside it. A session that departs
     * mid-walk has its egress closed, which refuses the claim (a counted shed) or waits out a
     * write already inside it; a session admitted mid-walk may take the record, as it would
     * from a broadcast that began a moment later.
     *
     * @param rec   The whole record (framing first, payload after), for the direct writes.
     * @param value The value whose links end @p rec, kept by reference in a shared frame
     *              (@ref frame_of), or null: the frame copies all of @p rec.
     * @param lane  The vertex the record comes from, or the control lane (a default).
     */
    void broadcast(std::span<const ::iovec> rec, const graph::value_t* value = nullptr,
                   const graph::delivery_lane_t& lane = {});

    /**
     * @brief Send one record to ONE session through its egress — the directed facades'
     *        door. Parameters as @ref broadcast; a departed session sheds, counted.
     */
    void send_to(session_base_t& s, std::span<const ::iovec> rec,
                 const graph::value_t* value = nullptr, const graph::delivery_lane_t& lane = {});

    /**
     * @name Variance points (runtime virtuals — ADR-0047 §4 wiring-frequency).
     * @{
     */

    /**
     * @brief Allocate one fresh slot of the derived server's session type, with its
     *        @ref session_base_t::peer_endpoint facade wired to this server.
     *
     * Called under @ref peers_m_ when no free slot exists and the cap allows growth.
     */
    virtual mem::poly_ptr_t<session_base_t> make_session() = 0;

    /**
     * @brief Per-accept setup: socket options and the slot's protocol buffers, run after
     *        the slot is named and before its fd is published.
     *
     * @param s  The slot being admitted (named, not yet published).
     * @param fd The accepted socket.
     * @return The slot's INITIAL `open` value — true where the protocol has no handshake
     *         (a raw stream peer is open the moment it is accepted), false where the
     *         session only carries frames past a handshake the framing hook completes
     *         (WS holds `open` until its 101 is on the wire).
     */
    virtual bool on_accept(session_base_t& s, int fd) = 0;

    /**
     * @brief @p s 's socket is readable: read it and frame what arrives — the derived
     *        server's frame reader, or its handshake parser.
     *
     * The read is the derived server's, so each kind receives straight into the buffer
     * its framing draws (ADR-0087 §1) rather than through a chunk of this tier's. Runs on
     * the poll thread with no transport lock held. The hook owns the decision to
     * @ref teardown_slot, for a peer that closed as for a framing violation.
     *
     * @param s  The slot whose socket polled readable.
     * @param fd Its socket.
     */
    virtual void on_readable(session_base_t& s, int fd) = 0;

    /**
     * @brief Reset the slot's protocol buffers as it is recycled (teardown side).
     *
     * @param s The slot being freed; its fd is already closed.
     */
    virtual void on_slot_reset(session_base_t& s) = 0;

    /**
     * @brief @p s 's peer has begun a frame or a handshake it has not finished — the RX half
     *        of what a liveness check finds owed (#2198). Poll thread, after
     *        @ref on_readable. Default: never.
     */
    [[nodiscard]] virtual bool mid_frame(const session_base_t& s) const {
        (void)s;
        return false;
    }

    /**
     * @brief TEST SEAM dispatch: run on the accept path with the fd published and the slot
     *        not yet open (its egress still closed).
     *
     * Default: nothing. A derived server overrides it to fire its own hook pointer — the
     * instant a test holds open to prove the two stores are atomic to senders (#891).
     */
    virtual void on_slot_publishing() {}
    /** @} */

    /**
     * @brief Announce @p s as a live, named session — the arrival half of the seam whose
     *        departure half is `teardown_slot`'s `notify_peer_down` (#1223 step 2).
     *
     * Called from the POLL THREAD at the moment the slot becomes usable to senders, which is
     * kind-specific and therefore not a single site: a raw stream peer is live the instant it
     * is accepted, a WS peer only once its `101` is on the wire — the same two transitions
     * `open` itself is stored at, so arrival and departure bracket exactly the same interval.
     * A FLAT (not @ref peer_named) server announces nothing: it has one routing identity for
     * every peer it carries, so there is no per-session identity to announce.
     *
     * Fired with NO transport lock held, per the bus facet's arrival-notifier contract — the
     * notifier re-enters the routing plane and takes graph locks.
     */
    void publish_peer_up(const session_base_t& s);

    /** @brief Open @p s to senders: its egress, then @p first as its first frame (a WS
     *         server's 101; empty: none), then `open` (poll thread; the fd is already
     *         published). */
    void open_session(session_base_t& s, std::span<const std::byte> first = {});

    /**
     * @name The peer-LIFECYCLE seam (#1438) — the only two places this tier needs the facet.
     *
     * `publish_peer_up` and `teardown_slot` run HERE, in the tier that owns the slot table,
     * but the notifiers they end in are `bus_link_t`'s protected members and this tier is
     * no longer a `bus_link_t`. These two hooks are the join: inert in the base, overridden
     * by @ref bus_slot_server_t to fire `notify_peer_up` / `notify_peer_down`.
     *
     * Virtual rather than the static seam the per-frame tier select uses, because these are
     * the base tier's own call sites and a base cannot resolve a derived name statically.
     * The cost is two vtable slots per concrete server and one indirect call per peer
     * ARRIVAL and DEPARTURE — wiring frequency, explicitly the tier ADR-0047 §4 admits a
     * runtime virtual at — and zero bytes per listener, since the vptr is already there.
     * @{
     */
    /** @brief Announce an arrival to the facet (no-op without one). */
    virtual void announce_peer_up(peer_handle_t handle, std::string_view peer) {
        (void)handle;
        (void)peer;
    }
    /** @brief Announce a departure to the facet (no-op without one). */
    virtual void announce_peer_down(peer_handle_t handle, std::string_view peer) {
        (void)handle;
        (void)peer;
    }
    /** @} */

    /**
     * @brief Guards the slot vector's growth and every slot's NAME — the cross-thread reads
     *        (@ref enumerate_peers / @ref peer_link) against the poll thread's
     *        accept/teardown. See the class-level threading rule. Never held across a write
     *        (#2263): a broadcast walks @ref first_ instead.
     */
    mutable std::mutex peers_m_;
    /** @brief The peer slots: insert-only, recycled in place, never freed early. */
    mem::block_array_t<mem::poly_ptr_t<session_base_t>> slots_;
    /** @brief The slot admitted last, or null: the head of the @ref session_base_t::next
     *         chain, newest first (#2263). Stored only by the poll thread, release. */
    std::atomic<session_base_t*> first_{nullptr};
    int listen_fd_ = -1;           /**< @brief The bound+listening socket (-1 = not bound). */
    std::uint16_t bound_port_ = 0; /**< @brief The resolved bound port (see local_port()). */
    /** @brief The ENFORCED admission cap (RFC-0006), resolved once at construction — never
     *         0. */
    std::size_t max_peers_ = 1;
    /** @brief Expose the bus facet — a wiring-time deployment choice. Stored HERE rather
     *         than in the facet arm because @ref ok reads it to refuse a peer-named server
     *         on a build that carries no facet at all (#375), and that refusal has to work
     *         in the arm where the facet does not exist. */
    bool peer_named_ = false;

    /**
     * @brief The constructed mode AS THIS BUILD CAN HONOUR IT — the one predicate every
     *        peer-named branch in this class and its two derived servers reads (#375).
     *
     * `peer_named_` is the REQUEST; this is the request conjoined with whether the target
     * carries a bus module at all (`tr::graph::default_config_t::kBusLinks`). The two differ
     * in exactly one build, the one that closed the module out, and there this is constant
     * `false` — so the per-frame tier select, the departure seam, the arrival seam and
     * `bus()` all collapse to their FLAT arms at compile time and the peer-named halves are
     * never emitted. That build cannot reach those arms at run time either, because @ref ok
     * refuses such a server outright; the folding is what makes the refusal FREE rather than
     * merely safe.
     *
     * Non-virtual and inline on purpose: it is read once per inbound frame on the poll thread,
     * where `peer_named()`'s virtual would be a per-frame dispatch. At the default binding it
     * IS `peer_named_` — one member load, the instruction sequence that was there before.
     */
    [[nodiscard]] bool bus_mode() const noexcept { return kBusLinks && peer_named_; }
    /** @brief The peer whose frame is being delivered RIGHT NOW — @ref inbound_peer's
     *         storage. Stamped by the derived server's receive loop immediately before it
     *         hands a frame up the FLAT tier, on the poll thread, and never read off it;
     *         reset to the default handle after each frame, delivered or not (#1915). */
    peer_handle_t delivering_{};

   private:
    /**
     * @brief Admit one inbound connection into a free (or newly grown) slot.
     *
     * @param pfds   The poll thread's fd set; grown here to hold a new slot before it is
     *               admitted, and a refused growth refuses the newcomer (#2156).
     * @param pslots The session list parallel to @p pfds, grown the same way.
     */
    void accept_peer(mem::block_array_t<pollfd>& pfds, mem::block_array_t<session_base_t*>& pslots);

    /** @brief One readable pass on @p s: `recv` a chunk, or tear the slot down. */
    void service_peer(session_base_t& s);

    /** @brief @p s reported writability: resume its held frame and drain its lanes
     *         (@ref egress_t::flush). */
    void flush_peer(session_base_t& s);

    /** @brief One record to @p s through its egress, every write nonblocking: @p rec direct
     *         when @p s is idle, else the shared frame @p frame builds, in @p lane. */
    void send_one(session_base_t& s, function_ref_t<egress_frame_t*()> frame,
                  std::span<const ::iovec> rec, const graph::delivery_lane_t& lane);

    /**
     * @brief True while ANY slot is still open — the flat mode's "is the link still up"
     *        question (#889), asked by @ref teardown_slot after the departing slot has
     *        already been closed, so it never counts itself.
     */
    [[nodiscard]] bool any_open_session() const;
};

/**
 * @brief The stream-server arm WITHOUT the ADR-0044 bus facet — the layout a target that
 *        closed the bus module out actually gets (#1438).
 *
 * Adds nothing to @ref slot_server_t but the INERT half of the per-frame peer-delivery
 * seam. Those members exist so the two servers' tier select is ONE piece of source under
 * both bindings; they are unreachable here, because the select is guarded by
 * `slot_server_t::bus_mode()`, which this arm only ever compiles as the constant `false`.
 *
 * The seam is deliberately NOT virtual, unlike the two peer-lifecycle hooks: it is read
 * once per inbound FRAME, and the concrete servers derive from `%tr::net::stream_server_base_t`,
 * so which arm's members they see is a compile-time fact. Nothing is dispatched.
 */
class flat_slot_server_t : public slot_server_t {
   protected:
    using slot_server_t::slot_server_t;
    /** @brief Destroyed only as the concrete server it is, never through this base (#2022):
     *         the derived destructor's `stop_and_join` must run first. */
    ~flat_slot_server_t() = default;

    /**
     * @name The per-frame peer-delivery seam, inert (see @ref bus_slot_server_t for the live
     *       half). Every one of these is dead code under this arm's own `bus_mode()`.
     * @{
     */
    /** @brief No peer tier to deliver an owning view to. */
    static void deliver_to_peer(peer_handle_t peer, view::view_t frame) {
        (void)peer;
        (void)frame;
    }
    /** @brief No peer tier to deliver an owning rope to. */
    static void deliver_to_peer_rope(peer_handle_t peer, view::rope_t frame,
                                     mem::mem_backend_t& backend) {
        (void)peer;
        (void)frame;
        (void)backend;
    }
    /** @} */
};

/**
 * @brief The stream-server arm WITH the ADR-0044 bus facet — @ref slot_server_t plus the
 *        `bus_link_t` base subobject, its `peer_rx_` slot and its two peer-lifecycle
 *        notifier pairs (#1438).
 *
 * Everything a bus facet needs to ANSWER already lives one tier up, because every one of
 * those questions is a question about the slot table: `p<slot>~<gen>` is a pure function of
 * the slot's handle either way. What lives HERE is the facet itself — the identity
 * `transport_t::bus()` hands out, the peer-named receiver slot, and the arrival/departure
 * notifiers — so the four `bus_link_t` pure virtuals below are one-line forwards to the
 * implementations they always had, not a second copy of them.
 *
 * Re-declaring them here is also what keeps the names UNAMBIGUOUS: with the same signature
 * reachable through @ref slot_server_t and through `bus_link_t`, an unqualified
 * `server.enumerate_peers(...)` would otherwise be an ambiguous lookup; the override in the
 * most-derived tier hides both.
 */
class bus_slot_server_t : public slot_server_t, public bus_link_t {
   public:
    /** @brief The one visitor shape — declared here so the name reaching this tier through
     *         two bases (identical types, ambiguous LOOKUP) resolves to one declaration. */
    using peer_visitor_t = bus_link_t::peer_visitor_t;

    /**
     * @brief The @ref bus_link_t facet (ADR-0044) when constructed `peer_named`, else
     *        `nullptr`. With the facet the router tags inbound frames per peer (each
     *        peer gets its own return-route identity and a `dst` segment routes back to
     *        that one session); without it the link keeps point-to-point hop naming —
     *        inbound frames carry the registered child NAME and `send()` fans out to
     *        every open peer.
     * @note Departure eviction (RFC-0009 §D.5) follows the same split: peer-named mode
     *       evicts just the departed peer's edges (`notify_peer_down(name)`), while FLAT
     *       mode reports the whole link down (`notify_down()`) — but only when the LAST
     *       open session departs (#889). A flat link has ONE routing identity for all its
     *       peers (the registered child NAME), so firing that on a mid-life close would
     *       evict the surviving peers' edges too.
     */
    [[nodiscard]] bus_link_t* bus() override { return bus_mode() ? this : nullptr; }

    /** @brief The mode authority as the facet's own virtual — @ref slot_server_t::peer_named,
     *         so "which mode is this link in" still has exactly one answer (#889). */
    [[nodiscard]] bool peer_named() const noexcept override { return slot_server_t::peer_named(); }

    /** @brief The facet's spelling of @ref slot_server_t::enumerate_peers. */
    void enumerate_peers(const peer_visitor_t& visit) const override {
        slot_server_t::enumerate_peers(visit);
    }

    /** @brief The facet's spelling of @ref slot_server_t::peer_name. */
    [[nodiscard]] std::string_view peer_name(peer_handle_t peer,
                                             std::span<char> scratch) const override {
        return slot_server_t::peer_name(peer, scratch);
    }

    /** @brief The facet's spelling of @ref slot_server_t::peer_link. */
    [[nodiscard]] transport_t* peer_link(std::string_view peer) override {
        return slot_server_t::peer_link(peer);
    }

    /** @brief The facet's spelling of @ref slot_server_t::peer_link_of. */
    [[nodiscard]] transport_t* peer_link_of(peer_handle_t peer) override {
        return slot_server_t::peer_link_of(peer);
    }

    /** @brief The facet's spelling of @ref slot_server_t::close_peer. */
    [[nodiscard]] bool close_peer(std::string_view peer) override {
        return slot_server_t::close_peer(peer);
    }

   protected:
    using slot_server_t::slot_server_t;
    /** @brief Destroyed only as the concrete server it is, never through this base (#2022):
     *         the derived destructor's `stop_and_join` must run first. */
    ~bus_slot_server_t() = default;

    /** @brief Arrival: fire the facet's peer-up notifier (#1223 step 2). */
    void announce_peer_up(peer_handle_t handle, std::string_view peer) override {
        notify_peer_up(handle, peer);
    }

    /** @brief Departure: fire the facet's peer-down notifier (RFC-0009 §D.5). */
    void announce_peer_down(peer_handle_t handle, std::string_view peer) override {
        notify_peer_down(handle, peer);
    }

    /**
     * @name The per-frame peer-delivery seam, live — the tier select in both servers'
     *       receive loops reaches `peer_rx_` through exactly these.
     * @{
     */
    /** @brief Deliver an owning view up the peer-named tier. */
    void deliver_to_peer(peer_handle_t peer, view::view_t frame) {
        peer_rx_.deliver(peer, std::move(frame));
    }
    /** @brief Deliver an owning rope up the peer-named tier; a span-only sink materializes a
     *         multi-link one from @p backend, the receiving link's rx source. */
    void deliver_to_peer_rope(peer_handle_t peer, view::rope_t frame, mem::mem_backend_t& backend) {
        peer_rx_.deliver_rope(peer, std::move(frame), backend);
    }
    /** @} */
};

/**
 * @brief What a concrete stream server derives from: the arm this BUILD binds (#1438).
 *
 * The provider half of the #375 deliverable-3 fold. `tr::net::bus_of` stopped the routing
 * plane from ASKING for a bus on a target that has none; this stops a listener on that
 * target from CARRYING the facet it could never hand out. One `std::conditional_t`, chosen
 * by @ref tr::net::kBusLinks, and the flat listener's layout simply does not contain
 * `bus_link_t` — no base subobject, no second vptr, no `peer_rx_`, no notifier pairs, and
 * none of the vtable entries that hang off them.
 *
 * At the default binding this IS @ref bus_slot_server_t and nothing about a listener moves.
 */
using stream_server_base_t = std::conditional_t<kBusLinks, bus_slot_server_t, flat_slot_server_t>;

}  // namespace tr::net
