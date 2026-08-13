/*
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
 * bus_link_t query trio (#871); only their framing and handshake differ, and
 * those are its two variance points.
 */
#pragma once

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/transport.hpp"

/** @brief The POSIX scatter-gather descriptor (`<sys/uio.h>`), forward-declared
 *         so this header need not pull the system socket headers in. */
struct iovec;

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
 * before the test returns. Production never sets it — the cost is one predictable,
 * never-taken branch immediately ahead of a syscall.
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
 * @brief Read the @ref write_fault_stats_t tally (relaxed; a diagnostic read, not a
 *        synchronizer).
 */
[[nodiscard]] write_fault_stats_t write_fault_stats() noexcept;

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
     * Spawns via `pthread_create` (not `std::thread`): the constructor of the
     * latter THROWS on failure, which under `-fno-exceptions` (the MCU build)
     * `std::abort`s — a thread-spawn OOM on a starved node would bring the whole
     * process down instead of soft-failing. `pthread_create` returns an error
     * code; a failed spawn leaves the endpoint simply not receiving (no abort).
     *
     * @param body       The thread body (the transport's accept/recv loop).
     * @param stack_size Recv-thread stack size in bytes, or 0 for the platform
     *        default (the ONLY value that preserves prior behavior). A non-zero
     *        hint is applied via `pthread_attr_setstacksize`, honored by glibc
     *        AND the ESP-IDF pthread layer (where it maps to the FreeRTOS task
     *        stack) — the portable knob that lets an integrator right-size this
     *        thread instead of inflating `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT`
     *        for every pthread in the system. A hint below the platform floor is
     *        ignored (the default stack is used) rather than failing the spawn.
     */
    void start(std::function<void()> body, std::size_t stack_size = 0);

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
    /**
     * @brief pthread entry trampoline — runs `body_` then returns.
     *
     * @param self The owning @ref posix_endpoint_t (the `pthread_create` arg).
     * @return Always nullptr (the thread's exit value is unused).
     */
    static void* thread_entry(void* self);

    std::function<void()> body_; /**< @brief The owned thread body `thread_entry` runs. */
    pthread_t thread_{};         /**< @brief The receive thread (joined by stop_and_join;
                                            valid only while `started_`). */
    bool started_ = false;       /**< @brief Whether `thread_` holds a joinable thread
                                            (a failed/never-attempted spawn stays false). */
};

/**
 * @brief The one-peer fd/teardown discipline every POSIX STREAM transport
 *        shares (tcp dial+listen, ws server, ws client).
 *
 * Owns the live peer fd (@ref conn_fd_) and the write mutex (@ref write_m_),
 * and is the ONE home of the invariants that keep a concurrent `send()` and
 * the recv thread's connection teardown safe against each other:
 *
 * **Write-serialization invariant:** every write to the peer fd happens with
 * @ref write_m_ held across the WHOLE write — so (a) two senders can never
 * interleave their records on the stream, and (b) the recv thread cannot
 * close and reset the fd underneath an in-flight write. `send()` reads
 * @ref conn_fd_ INSIDE the lock, pairing with the teardown below.
 *
 * **Teardown-under-write-lock invariant:** a recv thread that closes the peer
 * fd MUST reset @ref conn_fd_ to -1 under @ref write_m_ BEFORE `close(2)`
 * (@ref teardown_peer) — so a sender never writes to (or reads) a
 * closed/reused fd.
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
    /** @brief Constructs with no peer connected (@ref conn_fd_ = -1). */
    stream_endpoint_t() = default;

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
     * disconnect (#948). The caller holds @ref write_m_ per the
     * write-serialization invariant.
     *
     * @param fd    The destination fd; a negative fd is a no-op.
     * @param bytes The bytes to write.
     */
    static void write_all(int fd, std::span<const std::byte> bytes);

    /**
     * @brief Write the gathered @p vec entries to @p fd completely as ONE record,
     *        resuming partial writes — the zero-copy scatter-gather twin of
     *        @ref write_all.
     *
     * `sendmsg(2)` (MSG_NOSIGNAL — a vanished peer must not SIGPIPE the process)
     * emits every iovec in one syscall; a stream write may stop anywhere, so the
     * loop resumes from the first unwritten byte. @p vec is READ-ONLY (#932): the
     * gather is NOT consumed, so the same array may be fanned to many fds with no
     * per-fd copy — the resume path finishes a partially-written entry with a plain
     * @ref write_all and re-gathers from the next entry boundary, which needs no
     * mutable copy of the caller's array and no scratch store on the egress path.
     * EINTR resumes, a socket-dead errno drops the rest silently, and any other
     * errno is a malformed call that is re-attempted once and counted — the same
     * ONE write-fault policy as @ref write_all (#903 / #948; link-down is #66
     * lifecycle). The caller holds @ref write_m_ per the write-serialization
     * invariant.
     *
     * @param fd  The destination fd; a negative fd is a no-op.
     * @param vec The entries to gather, in order, as ONE record.
     */
    static void write_all_iov(int fd, std::span<const ::iovec> vec);

    /**
     * @brief Write @p bytes to the live peer as one serialized record.
     *
     * The whole write-serialization invariant in one call: takes
     * @ref write_m_, reads @ref conn_fd_ inside the lock, and @ref write_all
     * s the bytes. No-op while no peer is connected.
     *
     * @param bytes One complete encoded record's bytes.
     */
    void send_all_locked(std::span<const std::byte> bytes);

    /**
     * @brief Tear the peer connection down (recv-thread side).
     *
     * The teardown-under-write-lock invariant as code: resets @ref conn_fd_
     * to -1 under @ref write_m_, THEN `close(2)` on the fd — a concurrent `send()`
     * either finished against the still-open fd or reads -1 and no-ops.
     *
     * @param fd The peer fd the recv loop was serving.
     */
    void teardown_peer(int fd);

    /**
     * @brief The one-peer accept loop (tcp LISTEN / ws server shape).
     *
     * Until `stop_`: one poll-100ms-recheck accept pass (@ref poll_accept);
     * on a new connection run @p on_accept (per-peer setup — socket options,
     * handshake; return false to reject: the fd is closed and the loop
     * re-accepts), publish the fd to @ref conn_fd_, run @p serve_peer, then
     * @ref teardown_peer and re-accept the next peer.
     *
     * @param listen_fd  The bound+listening socket.
     * @param on_accept  Per-peer setup; false rejects the connection.
     * @param serve_peer The per-connection recv loop; returns on peer
     *                   departure or `stop_`.
     */
    void run_accept_loop(int listen_fd, const std::function<bool(int)>& on_accept,
                         const std::function<void(int)>& serve_peer);

    std::mutex write_m_;           /**< @brief Serializes writes to @ref conn_fd_ (see the
                                               write-serialization invariant). */
    std::atomic<int> conn_fd_{-1}; /**< @brief The live peer connection (-1 = none). */
};

/**
 * @brief The MULTI-peer slot/poll machinery every stream SERVER shares
 *        (transport_tcp_server, transport_ws_server) — one listener, N
 *        recycled peer slots, one poll thread (#871).
 *
 * The tier above @ref stream_endpoint_t — that one owns a single peer fd, this
 * one owns a VECTOR of them. Everything the two servers used to restate
 * line-for-line lives here exactly once — the slot struct and its threading
 * rule, the bind/listen/getsockname bring-up, the free-slot-or-grow accept
 * with its @p max_peers refusal and `p<slot>` naming, the poll loop, the
 * two-phase teardown, the @ref bus_link_t query trio, and the broadcast's
 * pristine-iovec-copy-per-peer fan-out. Only the FRAMING and the HANDSHAKE
 * differ between the two servers, and those are the variance points below
 * (the `msquic_endpoint_t` shape: runtime virtuals, appropriate per ADR-0047
 * §4 because peer arrival/departure is wiring-frequency, not hot path).
 *
 * **Slot threading rule, ONE rule for both halves of a slot's lifecycle:**
 * @ref session_base_t::fd / @ref session_base_t::open are atomics MUTATED
 * only under `write_m_` — accept publishes them (fd FIRST, so "open ⇒ fd
 * valid" is an invariant, #891), teardown resets them (open first) — and read
 * by senders under that same lock, so a sender never sees a half-published
 * slot. @ref session_base_t::name is guarded by @ref peers_m_; every
 * protocol buffer a slot carries is poll-thread-only. The destructor's
 * closing sweep is the one mutation outside the lock and runs after the poll
 * thread is joined. Every access to the two atomics is `relaxed`: the lock,
 * not the memory order, is what orders them. Lock order where nested:
 * @ref peers_m_ → `write_m_`.
 *
 * @warning A derived destructor MUST call `stop_and_join()` as its FIRST act:
 *          the poll thread dispatches the variance points below into the
 *          derived object, which must still be alive when it does.
 */
class slot_server_t : public transport_t, public bus_link_t, protected stream_endpoint_t {
   public:
    /** @brief True if the listen socket is bound and listening. */
    [[nodiscard]] bool ok() const noexcept { return listen_fd_ >= 0; }

    /** @brief The actual bound TCP port (resolves an ephemeral 0 request). */
    [[nodiscard]] std::uint16_t local_port() const noexcept { return bound_port_; }

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
    [[nodiscard]] bus_link_t* bus() override { return peer_named_ ? this : nullptr; }

    /**
     * @brief The mode authority (#889): the `peer_named` this server was constructed with.
     *
     * The ONE answer to "which mode is this link in" — @ref bus, the two servers' per-frame
     * tier select, and the departure branch in @ref teardown_slot all key off this flag (not
     * off whether a peer sink happens to be installed), and `bus_link_t` refuses every
     * peer-named wiring call while it is false.
     */
    [[nodiscard]] bool peer_named() const noexcept override { return peer_named_; }

    /** @brief Visit the currently-OPEN peers' names, `p<slot>` (#426). */
    void enumerate_peers(const peer_visitor_t& visit) const override;

    /**
     * @brief Resolve an open peer's name to its directed sending endpoint.
     *
     * Owned by the peer's slot and pointer-valid for this server's lifetime (slots are
     * never freed, only recycled). After the peer departs its sends no-op until the slot
     * is reused.
     * @retval nullptr @p peer names no currently-open connection.
     */
    [[nodiscard]] transport_t* peer_link(std::string_view peer) override;

    /**
     * @brief Close the open peer named @p peer, freeing its slot for reuse.
     *
     * Shuts the socket down (`SHUT_RDWR`) under the sender lock order
     * (@ref peers_m_ → `write_m_`); the poll thread's next pass observes the close and
     * runs the IDENTICAL remote-FIN teardown, so the recycle is asynchronous (within one
     * poll bound) and no poll-thread-only buffer is ever touched off-thread.
     * @retval true  @p peer named an open connection and its socket was shut down.
     * @retval false @p peer names no currently-open connection.
     */
    [[nodiscard]] bool close_peer(std::string_view peer) override;

   protected:
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
        /** @brief Virtual: the base owns the slot vector and deletes derived slots. */
        virtual ~session_base_t() = default;
        session_base_t(const session_base_t&) = delete;
        session_base_t& operator=(const session_base_t&) = delete;

        std::atomic<int> fd{-1};       /**< @brief The peer socket; -1 ⇒ free slot. */
        std::atomic<bool> open{false}; /**< @brief True while the session may carry frames. */
        /** @brief The peer's routable NAME, `p<slot>` — a pure function of the slot index
         *         (ADR-0073 §2, #426): stamped at accept, moved out by teardown (the
         *         eviction seam), so a reused slot gets the SAME name back. A legal path
         *         segment, unlike the old `<ip>:<port>`. It identifies a SESSION, not a
         *         device — a reconnecting peer may land in a different slot; device-stable
         *         identity is a named link (RFC-0014). */
        std::string name;
        /** @brief The remote `<ip>:<port>` — DIAGNOSTIC only, never a name and never in
         *         the graph (#584 owns any future per-peer facet). Refreshed per accept. */
        std::string endpoint_str;
        /** @brief The directed facade @ref peer_link returns — the derived slot's own
         *         member, registered here by @ref make_session. */
        transport_t* peer_endpoint = nullptr;
    };

    /**
     * @brief Constructs inert: no listen socket, no slots, no thread.
     *
     * @param max_peers  Concurrent-peer admission cap; 0 = unbounded (host default). A
     *                   deployment-injected bound (RFC-0006) — a connection beyond it is
     *                   accepted and immediately closed (a clean refusal, not a hung SYN).
     * @param peer_named Expose the @ref bus_link_t facet (see @ref bus).
     */
    slot_server_t(std::size_t max_peers, bool peer_named)
        : max_peers_(max_peers), peer_named_(peer_named) {}

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
     * dying slot), then reset `open`/`fd` under `write_m_` BEFORE `close(2)` (so an
     * in-flight send either finished against the still-open fd or observes the reset).
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
     * @brief Fan one already-encoded gathered record to EVERY open peer.
     *
     * `write_all_iov` reads its gather without consuming it (#932), so every peer writes
     * straight from @p rec — no per-peer copy, and no scratch store that could exhaust
     * and drop the frame. Takes @ref peers_m_ → `write_m_`, the header lock order.
     *
     * @param rec The assembled record (framing entry first, payload spans after).
     */
    void broadcast_iov(std::span<const ::iovec> rec);

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
    virtual std::unique_ptr<session_base_t> make_session() = 0;

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
     * @brief Per-readable-chunk framing: hand @p len bytes just read off @p s 's socket
     *        to the derived server's reassembler (or its handshake parser).
     *
     * Runs on the poll thread with no transport lock held. The hook owns the decision to
     * @ref teardown_slot on a framing violation; a peer that simply closed is torn down
     * by the caller before this is reached.
     *
     * @param s    The slot the bytes arrived on.
     * @param data The chunk (borrowed; valid only for this call).
     * @param len  The chunk length, always > 0.
     */
    virtual void on_readable(session_base_t& s, const std::byte* data, std::size_t len) = 0;

    /**
     * @brief Reset the slot's protocol buffers as it is recycled (teardown side).
     *
     * @param s The slot being freed; its fd is already closed.
     */
    virtual void on_slot_reset(session_base_t& s) = 0;

    /**
     * @brief TEST SEAM dispatch: run inside the accept-side `write_m_` hold, with the fd
     *        published and the slot ONE store from open.
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

    /**
     * @brief Guards the slot vector and every slot's NAME — the cross-thread reads
     *        (@ref enumerate_peers / @ref peer_link) against the poll thread's
     *        accept/teardown. See the class-level threading rule; lock order where
     *        nested: this → `write_m_`.
     */
    mutable std::mutex peers_m_;
    /** @brief The peer slots: insert-only, recycled in place, never freed early. */
    std::vector<std::unique_ptr<session_base_t>> slots_;
    int listen_fd_ = -1;           /**< @brief The bound+listening socket (-1 = not bound). */
    std::uint16_t bound_port_ = 0; /**< @brief The resolved bound port (see local_port()). */
    std::size_t max_peers_ = 0;    /**< @brief Admission cap; 0 = unbounded (RFC-0006). */
    bool peer_named_ = false;      /**< @brief Expose bus() — a wiring-time deployment choice. */

   private:
    /** @brief Admit one inbound connection into a free (or newly grown) slot. */
    void accept_peer();

    /** @brief One readable pass on @p s: `recv` a chunk, or tear the slot down. */
    void service_peer(session_base_t& s);

    /**
     * @brief True while ANY slot is still open — the flat mode's "is the link still up"
     *        question (#889), asked by @ref teardown_slot after the departing slot has
     *        already been closed, so it never counts itself.
     */
    [[nodiscard]] bool any_open_session() const;
};

}  // namespace tr::net
