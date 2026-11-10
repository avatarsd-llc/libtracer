/**
 * @file
 * @brief The wait and endpoint scaffold every POSIX socket transport shares.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * posix_endpoint — the wait/endpoint scaffold every POSIX socket
 * transport shares (udp / tcp / ws server / ws client). One home for putting a
 * link's sockets on a wait module with no timeout (`%wait.hpp`, #2147), on the
 * node's module or a thread-per-link fallback; the transports keep what
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
#include "libtracer/sync.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/wait.hpp"

/** @brief The POSIX scatter-gather descriptor (`<sys/uio.h>`), forward-declared
 *         so this header need not pull the system socket headers in. */
struct iovec;
/** @brief The POSIX IPv4 address (`<netinet/in.h>`), forward-declared for the same reason. */
struct in_addr;

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
 * @brief The concurrent-peer admission cap of a stream server that was given none (RFC-0006).
 *
 * A deployment injects its own `max_peers`, and that number alone is the cap: a newcomer past
 * it is refused, and no admitted peer is ever evicted for it. The cap is a RAM decision alone:
 * no write waits (#2144, #2252), so the peers it admits never add up to blocking. `0` asks for
 * this default instead.
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
    STALLED,  /**< @brief The socket filled with the record unfinished — the peer is not
                          taking bytes right now. */
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
 * @brief The shared wait scaffold of the POSIX socket transports (#2147).
 *
 * A protected base (inherited privately by the concrete transports) that puts the link's
 * sockets on a @ref wait_module_t : the node's, when the link's config names one, and the link
 * then owns no thread; otherwise a private module driven by one thread of the link's own (the
 * thread-per-link fallback adapter). Either way the wait has no timeout and every callback
 * runs on the module's driving thread.
 *
 * **Teardown invariant (derived destructors):** call @ref stop_and_join FIRST, before
 * releasing ANY resource a callback touches (sockets, receivers, buffers). It detaches the
 * link's sources, which waits out a callback in flight, so none runs once it returns.
 *
 * **Lifetime rules:** a deferred teardown (a `shut_down` on a callback thread) only shuts
 * down; it doesn't close. A link's descriptors stay owned and open until its destructor
 * runs, and destructors always join.
 *
 * Stream transports (tcp / ws) layer the shared one-peer fd/teardown discipline on top via
 * @ref stream_endpoint_t — the write-serialization and teardown-under-role invariants live
 * there, with the code.
 */
class posix_endpoint_t {
   protected:
    /** @brief Constructs not yet waiting.
     *  @param node The node's wait module, or null for the thread-per-link fallback. It must
     *              outlive the link. */
    explicit posix_endpoint_t(wait_module_t* node = nullptr) noexcept : node_(node) {}

    /** @brief Stops a fallback thread as a last resort; derived destructors have already
     *         called @ref stop_and_join (see the teardown invariant above). */
    ~posix_endpoint_t();

    posix_endpoint_t(const posix_endpoint_t&) = delete;
    posix_endpoint_t& operator=(const posix_endpoint_t&) = delete;

    /**
     * @brief Begin waiting: on the node's module, or on a private one driven by a thread of
     *        the link's own. Call at most once, before the first @ref attach.
     *
     * The fallback's thread spawns through @ref static_thread_t (#2288), never `std::thread`,
     * whose constructor throws on failure (an `abort` under `-fno-exceptions`). Its private
     * module and, on ESP-IDF, its task block and stack are drawn from @p src.
     *
     * @param stack_size The fallback thread's stack in bytes, 0 for the platform default
     *                   (ignored on the node's module, whose driver is the application's).
     * @param src        The link's `link_memory_t::stack`. It MUST outlive @ref stop_and_join.
     * @retval false The private module or its thread was refused: nothing waits. The caller
     *         MUST report it, by leaving the link inert (`ok()` false) or bringing it down.
     */
    [[nodiscard]] bool start(std::size_t stack_size, mem::block_source_t& src);

    /**
     * @brief Put @p s (socket @p fd , callback @p fire with this link as context) on the wait.
     *
     * @p write arms write interest for the first turn. A write interest armed on @p s since
     * @ref start (a send held between the two) is kept, never cleared here.
     * @retval false The module refused it: the caller treats the socket as gone.
     */
    [[nodiscard]] bool attach(wait_source_t& s, int fd, wait_source_t::fire_t fire,
                              bool write = false) noexcept {
        s.fd.store(fd, std::memory_order_relaxed);
        s.fire = fire;
        s.ctx = this;
        if (write) s.write.store(true, std::memory_order_relaxed);
        return waiter().attach(s);
    }

    /** @brief The module this link waits on, or null before @ref start. */
    [[nodiscard]] wait_module_t* waiting() const noexcept {
        return wait_.load(std::memory_order_acquire);
    }

    /** @brief The module this link waits on (valid after @ref start). */
    [[nodiscard]] wait_module_t& waiter() noexcept {
        return *wait_.load(std::memory_order_acquire);
    }

    /**
     * @brief Arm write interest on @p s, from inside the egress writer role, once a write
     *        left a frame held and the peer reports it (`egress_t::send`'s `arm`). Before
     *        @ref start it does nothing: the first attach arms it.
     */
    void arm_write(wait_source_t& s) noexcept {
        if (wait_module_t* const w = wait_.load(std::memory_order_acquire)) w->want_write(s, true);
    }

    /**
     * @brief Stop waiting (idempotent): stop and join the fallback thread, then detach
     *        @ref src_ . MUST be the FIRST act of every derived destructor, and always joins:
     *        a destructor runs off every callback thread (asserted).
     *
     * The fallback's driver is joined before the detach, so on a private module the detach
     * takes no lock and a teardown made under a lock a callback takes cannot deadlock on the
     * module (the join itself still waits out a callback in flight).
     */
    void stop_and_join();

    /**
     * @brief Stop waiting for a `shut_down`: as @ref stop_and_join, except on a thread that
     *        runs link callbacks (@ref on_callback_thread), where it joins nothing and marks
     *        the link shut. A shut link's descriptors are shut down and stay open; its
     *        destructor closes them after its join.
     * @retval true The teardown is deferred: the link is shut, and nothing was joined.
     */
    [[nodiscard]] bool stop_for_shut_down();

    /** @brief Stop the fallback's thread, and join it when @p join (idempotent); a no-op on
     *         the node's module, whose driver is the application's.
     *  @param join False only on a deferred teardown (@ref stop_for_shut_down). */
    void stop_driver(bool join);

    /** @brief Give a socket @p fd back that @p slot held: closed, unless the link is shut,
     *         when it stays open in @p slot for the destructor to close. */
    void release_fd(std::atomic<int>& slot, int fd) noexcept;

    /** @brief Give a listening socket back: closed and reset to -1, unless the link is shut,
     *         when it is only shut down and the destructor closes it. */
    void release_listen(int& fd) noexcept;

    /** @brief True once a deferred teardown shut the link: its buffers and descriptors are
     *         left for the destructor. */
    [[nodiscard]] bool shut() const noexcept { return shut_.load(std::memory_order_relaxed); }

    /**
     * @brief The link's own socket's registration: a datagram socket, a dialled connection,
     *        or a listening socket. Its context is the link.
     */
    wait_source_t src_;

   private:
    /** @brief The fallback thread's body: drive the private module. */
    static void thread_entry(void* fallback);

    wait_module_t* node_; /**< @brief The node's module, or null. */
    /** @brief The module waited on, published by @ref start. */
    std::atomic<wait_module_t*> wait_{nullptr};
    /** @brief The fallback's private module and its thread, defined in the source file. */
    struct fallback_t;
    /** @brief The fallback's state, drawn from the link's `link_memory_t::stack` in
     *         @ref start; null on the node's module, which needs none of it. One pointer, so
     *         a link on the node's module pays nothing for the fallback it does not use. */
    fallback_t* fallback_ = nullptr;
    /** @brief Set by a deferred teardown (@ref stop_for_shut_down): from then on no
     *         descriptor of the link is closed before its destructor. */
    std::atomic<bool> shut_{false};
};

/**
 * @brief The one-peer fd/teardown discipline every POSIX STREAM transport
 *        shares (tcp dial+listen, ws server, ws client).
 *
 * Owns the live peer fd (@ref conn_fd_) and the link's egress (@ref egress_, ADR-0087 §2),
 * and is the ONE home of the invariants that keep a concurrent `send()` and the driving thread's
 * connection teardown safe against each other:
 *
 * **Write-serialization invariant:** every write to a peer fd is made by the sender holding
 * that peer's egress WRITER ROLE (@ref egress_send), elected by try-lock: a sender that finds
 * the peer idle writes its own record, one that finds a write in flight queues a reference
 * and returns. So two senders never interleave records on the stream and no publisher waits
 * on another publisher's write. A record the socket cuts short keeps its frame and offset,
 * and the rest goes on the peer's next turn, so a part write never desyncs the framing.
 *
 * **Teardown-under-role invariant:** a driving thread that closes the peer fd first
 * `shutdown(2)`s it (an in-flight write returns), then closes the peer's egress — which waits
 * for the writer role to be let go — and only then resets @ref conn_fd_ to -1 and
 * `close(2)`s it (@ref teardown_peer). So a sender never writes to (or reads) a closed or
 * reused fd, and the shared write mutex is gone.
 *
 * The one-peer listener shape (accept → per-peer setup → serve → teardown →
 * listen again), on the wait module, lives here too (@ref accept_one,
 * @ref relisten). A protected base, inherited
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
     * @param one_peer This endpoint IS the one peer (a dialer, a one-peer listener). A
     *                 multi-peer server passes false and keeps one peer per session. Neither
     *                 one's writes wait (#2252), so both take the strike cap
     *                 `kEgressStallStrikes`.
     */
    explicit stream_endpoint_t(mem::block_source_t& tx_src, bool one_peer = true,
                               wait_module_t* node = nullptr)
        : posix_endpoint_t(node), io_src_(&tx_src), egress_(tx_src) {
        // Open from the start: a send before a peer connects finds no fd and sheds, counted.
        if (one_peer) egress_.open(peer_);
    }

    /**
     * @brief Closes a leftover peer fd (one the driving thread never tore down).
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
     * @brief Write what the socket takes now of @p bytes to @p fd, never waiting (#2252).
     *
     * `send(2)` with `MSG_DONTWAIT` (and MSG_NOSIGNAL — a vanished peer must not SIGPIPE the
     * process), looped while the socket keeps taking bytes. A full socket ends the write as
     * `STALLED` with the bytes it took, so no caller waits on a peer that stopped reading,
     * and no write reads a clock. A socket-dead errno drops the rest silently (link-down is
     * #66 lifecycle); every OTHER errno means the call itself was malformed, is re-attempted
     * once and counted in `write_fault_stats()` rather than mistaken for a disconnect (#948).
     *
     * @param fd    The destination fd; a negative fd is a no-op.
     * @param bytes The bytes to write.
     * @return How the write ended, and how many bytes reached the socket.
     */
    static write_result_t write_all(int fd, std::span<const std::byte> bytes);

    /**
     * @brief Write the gathered @p vec entries to @p fd as ONE record, never waiting — the
     *        zero-copy scatter-gather twin of @ref write_all.
     *
     * `sendmsg(2)` emits every iovec in one syscall; a stream write may stop anywhere, so the
     * loop resumes from the first unwritten byte while the socket takes more. @p vec is
     * READ-ONLY (#932): the gather is NOT consumed, so the same array may be fanned to many
     * fds with no per-fd copy. The same ONE write-fault policy as @ref write_all (#948).
     *
     * @param fd  The destination fd; a negative fd is a no-op.
     * @param vec The entries to gather, in order, as ONE record.
     * @return How the write ended, and how many bytes reached the socket.
     */
    static write_result_t write_all_iov(int fd, std::span<const ::iovec> vec);

    /**
     * @brief One egress write turn: @p f from its byte @p off, gathered as one write — the
     *        copied head, then a kept value's links straight from the published block.
     *
     * The gather table is inline; its overflow comes from @ref io_src_ and is refused rather
     * than thrown, which writes nothing and reports a failed turn (a shed frame, never a
     * desync).
     *
     * @param fd  The destination fd.
     * @param f   The frame.
     * @param off Bytes of @p f already on the wire.
     */
    egress_step_t write_frame(int fd, const egress_frame_t& f, std::size_t off);

    /**
     * @brief One egress write turn of the caller's own gathered record (the direct write).
     * @see write_all_iov for the parameters.
     */
    static egress_step_t write_gather(int fd, std::span<const ::iovec> vec);

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
     * @brief Account one egress call (#838, #2144): every shed frame is a counted `dropped_tx`,
     * every turn that found the socket full and moved nothing a `stalled_tx_`.
     */
    void note_egress(const egress_result_t& r) noexcept;

    /**
     * @brief Condemn a peer that earned it: `shutdown(SHUT_RDWR)` its socket — the egress's
     *        `condemn` hook, run inside the writer role (#838, #2144).
     *
     * Shutdown, never close: it takes effect on this line, needs no cooperation from the
     * stalled socket, makes every later write fail at once and raises the readable-at-EOF the
     * driving thread turns into the ordinary remote-departure teardown (the same path
     * @ref slot_server_t::close_peer uses). A negative fd is a no-op.
     */
    static void condemn_fd(int fd) noexcept;

    /**
     * @brief The one-peer @ref transport_t::sweep (#2198, #2481): a dialer's, or a one-peer
     *        listener's. Only the TX direction is owed here: a frame held on the wire that
     *        moved no byte since the last check. A frozen peer is shut down inside its writer
     *        role, so the driving thread runs the ordinary teardown, and counted in
     *        @ref frozen_closed_.
     * @return When the peer is next due, 0 when it owes nothing or was closed.
     */
    std::uint64_t sweep_one(std::uint64_t now, std::uint64_t deadline);

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
     * waits on another publisher's write to a stalled peer (#1619), and the writer itself
     * never waits (#2252): what the socket did not take stays held, and the driving thread
     * resumes it on writability.
     *
     * @tparam Direct Callable `egress_step_t(int fd)`: write the caller's own record.
     * @param frame  Builds the shared frame (the record copied, a kept value by reference)
     *               when the record has to wait.
     * @param lane   The ring it waits in: its vertex's, or the control lane (a default).
     */
    template <class Direct>
    void egress_send(function_ref_t<egress_frame_t*()> frame, Direct&& direct,
                     const graph::delivery_lane_t& lane = {}) {
        int fd = -1;
        const egress_result_t r = egress_.send(
            peer_, lane, frame,
            [&] {
                fd = conn_fd_.load(std::memory_order_acquire);
                // No live peer (still dialing, or torn down) => a counted drop.
                if (fd < 0) return egress_step_t{0, false, true};
                return note_moved(tx_moved_, std::forward<Direct>(direct)(fd));
            },
            [&](const egress_frame_t& f, std::size_t off) {
                fd = conn_fd_.load(std::memory_order_acquire);
                if (fd < 0) return egress_step_t{0, false, true};
                return note_moved(tx_moved_, write_frame(fd, f, off));
            },
            [&] { condemn_fd(fd); }, [this] { arm_write(src_); });
        note_egress(r);
    }

    /**
     * @brief One readiness turn of the ONE peer's connection (@ref src_), on the driving
     *        thread: writability resumes a held frame, nonblocking (`egress_t::flush`), so a
     *        record the socket cut short finishes without waiting for a later send (#2144);
     *        readability (or a hangup) runs @p read on the socket.
     *
     * @tparam Read Callable `bool(int fd)`: read what the socket has; false when the
     *              connection must close.
     * @return False when the peer is gone: the caller runs @ref depart.
     */
    template <class Read>
    [[nodiscard]] bool peer_turn(std::uint8_t ready, Read&& read) {
        const int fd = src_.fd.load(std::memory_order_relaxed);
        if (writable(ready)) flush_one(fd);
        return !readable(ready) || std::forward<Read>(read)(fd);
    }

    /**
     * @brief Resume the ONE peer's held frame on @p fd without waiting, then disarm write
     *        interest once nothing is held (driving thread).
     */
    void flush_one(int fd);

    /** @brief After a flush, disarm write interest on @p s once @p peer holds nothing, then
     *         re-arm it if a sender held a frame meanwhile (driving thread). */
    void disarm_flushed(wait_source_t& s, const egress_peer_t& peer);

    /**
     * @brief The ONE peer departed (driving thread): stop waiting on its connection, then
     *        @ref teardown_peer it.
     */
    void depart();

    /**
     * @brief One accept on the one-peer listener, whose listening socket is @ref src_
     *        (driving thread).
     *
     * Runs @p on_accept (per-peer setup; false rejects: the socket is closed and the listener
     * keeps waiting), publishes the fd to @ref conn_fd_, moves @ref src_ from the listening
     * socket to the connection, served by @p fire, and only then reopens the peer's egress.
     * A refused registration closes the peer and puts @ref src_ back on the listening socket.
     * A listener accepts nobody else while a peer is connected: the kernel's backlog holds
     * them.
     * @ref relisten puts @ref src_ back on the listening socket after the peer departs.
     */
    void accept_one(int listen_fd, function_ref_t<bool(int)> on_accept, wait_source_t::fire_t fire);

    /** @brief After a one-peer listener's @ref depart, wait on @p listen_fd again, served by
     *         @p fire. */
    void relisten(int listen_fd, wait_source_t::fire_t fire);

    /**
     * @brief Tear the peer connection down (driving thread).
     *
     * The teardown-under-role invariant as code: `shutdown(2)` the fd so a write in flight
     * returns, close the peer's egress (waiting out its writer, releasing its frames), THEN
     * reset @ref conn_fd_ to -1 and `close(2)` the fd — a concurrent `send()` either finished
     * against the still-open fd or finds the peer closed and sheds. On a shut link the fd
     * stays open in @ref conn_fd_ for the destructor.
     *
     * @param fd The peer fd the recv loop was serving.
     */
    void teardown_peer(int fd);

    /**
     * @brief The @ref transport_t::shut_down of a one-peer stream link: join the receive
     *        thread, then tear the live peer down (@ref teardown_peer), so a later send finds
     *        the peer closed and sheds. Idempotent. A deferred teardown joins nothing and only
     *        shuts the peer down.
     */
    void shut_down_stream();

    std::atomic<int> conn_fd_{-1}; /**< @brief The live peer connection (-1 = none). */
    /** @brief A byte of ours went out to the ONE peer since the last liveness check (#2198), in
     *         the hole after @ref conn_fd_. Set by the writer, taken by @ref sweep_one; a new
     * session sets it. */
    std::atomic<bool> tx_moved_{true};
    /** @brief Write turns that found the peer's socket full and moved nothing (#838, #2144)
     *         — the "how often did a stalled peer refuse us" counter, each one a strike.
     *         Relaxed: a diagnostic tally, not a synchronizer. Word-wide, not 64-bit
     *         (core/STYLE.md §Introspection clause 5, #1697): a 64-bit atomic is a
     *         libatomic call on every rv32; a 32-bit target wraps after 2^32. */
    std::atomic<std::size_t> stalled_tx_{0};
    /** @brief This link's shed-frame counters (#2482). `dropped_tx` is every outbound frame
     *         it never put on the wire: a refused build, a closed peer, a full ring, a failed
     *         write. `dropped_rx` and `malformed_rx` are the derived kind's framing refusals. */
    drop_tally_t<> drops_;
    /** @brief Peers the application's liveness check closed as frozen (#2198) — every one
     *         counted. Relaxed: a diagnostic tally. */
    std::atomic<std::size_t> frozen_closed_{0};
    /** @brief The application's `now` at the last liveness check that found the ONE peer
     *         moving or owing nothing (#2198). Written only by @ref sweep_one. */
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
 *        recycled peer slots, all on one wait module (#871, #2147).
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
 * `session_base_t::fd` / @ref session_base_t::open are MUTATED only on the poll
 * thread — accept publishes the fd, then opens the session's egress, then `open`
 * (#891: "open ⇒ fd valid"); teardown clears `open`, shuts the fd down, closes the
 * session's egress (which waits out its writer), and only then resets and closes the fd.
 * A sender reads the fd only INSIDE the session's egress writer role (ADR-0087 §2), which
 * the egress hands over with acquire/release, so it never sees a half-published slot or a
 * closed one. @ref session_base_t::name and the slot table's growth are guarded by
 * @ref peers_m_, which no write is ever made under (#2263): a broadcast walks the slots by
 * @ref session_base_t::next with no lock, and the driving thread, the table's only mutator,
 * reads it with none. Every protocol buffer a slot carries is the driving thread's alone. The
 * destructor's closing sweep runs after the driving thread is joined. There is no shared write
 * lock: a stalled peer holds only its own role.
 *
 * @warning A derived destructor MUST call `stop_and_join()` as its FIRST act:
 *          the driving thread dispatches the variance points below into the
 *          derived object, which must still be alive when it does.
 */
class slot_server_t : public transport_t, protected stream_endpoint_t {
   public:
    /** @brief The peer-visitor shape the query trio speaks — @ref bus_link_t's, so the
     *         facet arm's overrides are the same signature and no consumer sees two. */
    using peer_visitor_t = bus_link_t::peer_visitor_t;

    /** @brief The server's shed-frame counters, summed over every peer (#932, #2482). */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        return drops_.snapshot();
    }

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

    /** @brief The concurrent-peer admission cap ENFORCED on the accept path: the injected
     *         cap, or `%kDefaultMaxPeers` for `0`, so never 0. */
    [[nodiscard]] std::size_t max_peers() const noexcept { return max_peers_; }

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
     * survives — stamped on the driving thread immediately before the flat delivery, read
     * back, on that same thread, by the router's terminus, and cleared once the frame has
     * been handed up. Outside a delivery it answers the default (invalid) handle (#1915).
     *
     * @warning Poll-thread state, meaningful ONLY inside a receive callback. It is a plain
     *          member and not an atomic on purpose: one server owns one driving thread
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
     * under the same lock before it closes the fd); the driving thread's next pass observes the
     * close and runs the IDENTICAL remote-FIN teardown, so the recycle is asynchronous (the
     * shut-down socket reports a hangup at once) and no buffer the driving thread alone
     * owns is ever touched off-thread.
     * @retval true  @p peer named an open connection and its socket was shut down.
     * @retval false @p peer names no currently-open connection.
     */
    [[nodiscard]] bool close_peer(std::string_view peer);

    /**
     * @brief The application's liveness check over every open peer (#2198,
     *        @ref transport_t::sweep).
     *
     * A peer owes TX while a frame waits on its wire, and RX while it has begun a frame or
     * a handshake and not finished it (@ref mid_frame). One that owed a direction and moved
     * no byte in it since its record fell below @p deadline is shut down exactly as
     * @ref close_peer does it, under @ref peers_m_, and counted in @ref frozen_closed. An
     * idle peer — owing nothing — is never closed.
     * @return When the earliest owing peer is next due, 0 when none owes.
     */
    std::uint64_t sweep(std::uint64_t now, std::uint64_t deadline) override;

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
     * endpoint facade object (@ref endpoint_of). Its inherited @ref wait_source_t::ctx is
     * this server, set once when the slot is made, so a facade finds its server through its
     * slot (@ref server_of). See the class-level threading rule for who may
     * touch what.
     */
    struct session_base_t : wait_source_t {
        /** @brief Constructs a free slot (no fd, not open, unnamed). Its inherited
         *         @ref wait_source_t is the session's registration with the wait module, and
         *         its `fd` is the peer socket: -1 ⇒ free slot. */
        session_base_t() = default;
        session_base_t(const session_base_t&) = delete;
        session_base_t& operator=(const session_base_t&) = delete;

        std::atomic<bool> open{false}; /**< @brief True while the session may carry frames. */
        /** @brief A byte of ours went out since the last liveness check (#2198). Set by the
         *         writer, taken by @ref sweep; an accept sets it. */
        std::atomic<bool> tx_moved{false};
        /** @brief The peer's socket was readable since the last liveness check (#2198). Set
         *         by the driving thread, taken by @ref sweep; an accept sets it. */
        std::atomic<bool> rx_moved{false};
        /** @brief The peer has begun a frame or a handshake and not finished it (#2198):
         *         @ref mid_frame as of the driving thread's last pass over it. */
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
         * ESP link's session ref carries. Written on the driving thread under @ref peers_m_
         * beside @ref name, and read on that thread by the delivery path.
         */
        peer_handle_t handle;
        /** @brief This slot's monotonic tenancy counter — the source of the generation
         *         @ref handle is minted at. Bumped once per accept (never reused, never
         *         zero), so the counter survives the retire that clears @ref handle. */
        std::uint32_t gen_seq = 0;
        /** @brief This session's egress (ADR-0087 §2, #2144): its frames waiting per vertex,
         *         its writer role and its strikes. Closed while the slot is free; reset as the
         *         slot is recycled, so a stalled peer's frames and strikes are never inherited
         *         by its successor in the slot. */
        egress_peer_t egress;
        /** @brief The application's `now` at the last liveness check that found this peer
         *         moving, or owing nothing (#2198). Written only by @ref sweep,
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
     * @param node       The node's wait module, or null for a thread of the server's own
     *                   (@ref posix_endpoint_t).
     */
    slot_server_t(std::size_t max_peers, bool peer_named,
                  mem::block_source_t& src = mem::net_source(),
                  mem::block_source_t& state_src = mem::net_source(), wait_module_t* node = nullptr)
        : stream_endpoint_t(src, false, node),
          slots_(state_src),
          max_peers_(max_peers != 0 ? max_peers : kDefaultMaxPeers),
          peer_named_(peer_named) {}

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
     *               driving thread (`ok()` stays false).
     */
    bool bind_listen(std::uint16_t bind_port);

    /** @brief Close the listen socket, leaving `ok()` false: a bind that failed, or a poll
     *         thread whose spawn was refused (#2288). */
    void close_listen() noexcept;

    /**
     * @brief Begin serving: wait on the listening socket (#2147). Call it from the DERIVED
     *        constructor, last, once every member the variance points touch is initialized.
     *
     * The listener and every session are sources of one wait module, so no per-peer thread
     * exists (#362) and the wait has no timeout.
     *
     * @param stack_size The fallback thread's stack (see @ref posix_endpoint_t::start).
     * @param stack_src  The link's `memory.stack`.
     * @retval false The wait could not begin: the caller leaves the listener inert.
     */
    [[nodiscard]] bool serve(std::size_t stack_size, mem::block_source_t& stack_src);

    /** @brief Stop waiting on the listener and on every session (idempotent): the FIRST act
     *         of a derived destructor, which hides @ref posix_endpoint_t::stop_and_join. */
    void stop_and_join();

    /** @brief @ref posix_endpoint_t::stop_for_shut_down for the listener and every session.
     *  @retval true The teardown is deferred. */
    [[nodiscard]] bool stop_for_shut_down();

    /** @brief Take every session off the wait (after the listener). */
    void detach_slots();

    /**
     * @brief Tear one slot down and free it for reuse (driving thread only).
     *
     * Two phases: stop name resolution under @ref peers_m_ (so no new sender targets the
     * dying slot), then clear `open`, shut the fd down and close the session's egress —
     * which waits out a writer in flight — BEFORE resetting the fd and `close(2)`ing it.
     * @ref on_slot_reset clears the protocol buffers, except on a shut link, whose session
     * buffers live until its destructor. The egress close and the departure seam run once
     * per session, whichever of concurrent teardowns gets there first. The departure seam
     * (RFC-0009 §D.5) fires LAST with no transport lock held — the notifier re-enters the
     * routing plane. Which seam depends on @ref peer_named() — the departed peer's own name
     * when peer-named, the whole link when flat, and then only once no open session is
     * left (#889).
     *
     * @param s The slot to recycle.
     */
    void teardown_slot(session_base_t& s);

    /**
     * @brief The @ref transport_t::shut_down of a stream server: join the driving thread, tear
     *        every session down exactly as a remote close does (@ref teardown_slot), and close
     *        the listen socket. Idempotent. A deferred teardown joins nothing and closes
     *        nothing: the sockets are shut down, and the destructor closes them.
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
     *        endpoint facade wired to the slot.
     *
     * Called under @ref peers_m_ when no free slot exists and the cap allows growth.
     */
    virtual mem::poly_ptr_t<session_base_t> make_session() = 0;

    /** @brief The directed facade @ref peer_link returns for @p s : the derived slot's own
     *         member. A virtual rather than a pointer in every slot, so a peer pays no bytes
     *         for it. */
    [[nodiscard]] virtual transport_t& endpoint_of(session_base_t& s) noexcept = 0;

    /** @brief The server @p s belongs to: its wait context, set when the slot is made. */
    [[nodiscard]] static slot_server_t& server_of(const session_base_t& s) noexcept {
        return *static_cast<slot_server_t*>(static_cast<posix_endpoint_t*>(s.ctx));
    }

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
     * the driving thread with no transport lock held. The hook owns the decision to
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
     *         server's 101; empty: none), then `open` (driving thread; the fd is already
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
     *        (@ref enumerate_peers / @ref peer_link) against the driving thread's
     *        accept/teardown. See the class-level threading rule. Never held across a write
     *        (#2263): a broadcast walks @ref first_ instead.
     */
    mutable tr::mutex_t peers_m_;
    /** @brief The peer slots: insert-only, recycled in place, never freed early. */
    mem::block_array_t<mem::poly_ptr_t<session_base_t>> slots_;
    /** @brief The slot admitted last, or null: the head of the @ref session_base_t::next
     *         chain, newest first (#2263). Stored only by the driving thread, release. */
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
     * Non-virtual and inline on purpose: it is read once per inbound frame on the driving thread,
     * where `peer_named()`'s virtual would be a per-frame dispatch. At the default binding it
     * IS `peer_named_` — one member load, the instruction sequence that was there before.
     */
    [[nodiscard]] bool bus_mode() const noexcept { return kBusLinks && peer_named_; }
    /** @brief The peer whose frame is being delivered RIGHT NOW — @ref inbound_peer's
     *         storage. Stamped by the derived server's receive loop immediately before it
     *         hands a frame up the FLAT tier, on the driving thread, and never read off it;
     *         reset to the default handle after each frame, delivered or not (#1915). */
    peer_handle_t delivering_{};

   private:
    /** @brief Admit one inbound connection into a free (or newly grown) slot, refused when
     *         the cap, the slot table or the wait module refuses it. */
    void accept_peer();

    /** @brief The listening socket's wait callback: one accept. */
    static void on_listen(void* ctx, wait_source_t& self, std::uint8_t ready);

    /** @brief A session's wait callback: writability resumes its held frame, readability
     *         serves it. */
    static void on_session(void* ctx, wait_source_t& self, std::uint8_t ready);

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
    /** @brief No peer tier to deliver an owning rope to: never delivered. */
    [[nodiscard]] static bool deliver_to_peer_rope(peer_handle_t peer, view::rope_t frame,
                                                   mem::mem_backend_t& backend) {
        (void)peer;
        (void)frame;
        (void)backend;
        return false;
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
     *         multi-link one from @p backend, the receiving link's rx source.
     *  @return Whether a sink received it (`receiver_slot_t::deliver_rope`, #2504). */
    [[nodiscard]] bool deliver_to_peer_rope(peer_handle_t peer, view::rope_t frame,
                                            mem::mem_backend_t& backend) {
        return peer_rx_.deliver_rope(peer, std::move(frame), backend);
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
