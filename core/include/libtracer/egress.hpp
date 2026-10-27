/**
 * @file
 * @brief The one egress module every stream link adapts (ADR-0087 §2, #2144): one shared
 *        refcounted frame per send, one ring of frame references per (peer, vertex), a writer
 *        elected by try-lock, and a stall strike.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * **One frame per send.** A send that has to wait builds its frame ONCE, in one block drawn
 * from the link's own egress source (@ref tr::net::egress_frame_t): the link's framing and the
 * caller's head spans are copied into it, and a payload that arrived as a published value is
 * kept by reference (`value_ref_t::keep`), never copied. Every peer the send reaches holds a
 * reference to that one block, so a fan-out to N peers costs one gather into one frame, not
 * N copies. A send that does not have to wait builds nothing: it writes straight from the
 * caller's memory.
 *
 * **One ring per (peer, vertex), one mechanism for SAMPLE and STREAM** (ADR-0087 §2, Q5).
 * A send names the lane it rides (@ref tr::graph::delivery_lane_t): the subscription edge it
 * comes from, and the shape the local vertex declared when the edge was admitted — its
 * retention depth and its §4.4 arm. Each peer (@ref tr::net::egress_peer_t) holds the frame on
 * the wire and, behind it, one ring per lane with frames waiting. Nothing else picks the shape,
 * and the `delivery_class` bits are never read here.
 *
 * - **Depth 1 is a SAMPLE.** A newer frame overwrites the one waiting, so the newest frame
 *   wins and a slow peer holds one reference per vertex, not a queue. It is the degenerate
 *   ring, not a separate slot type.
 * - **Depth N is a STREAM.** Frames leave in order. A full ring follows the vertex's §4.4
 *   arm (RFC-0025): best-effort sheds the OLDEST waiting frame; reliable refuses the newest.
 *   Both are counted, never silent.
 * - **Round-robin across lanes, FIFO inside one.** A vertex that publishes fast cannot push
 *   another vertex's frames out of the peer's queue: each has its own ring.
 * - **Frames from no vertex** (a reply, a control frame) ride the peer's control lane: depth
 *   @ref tr::net::kEgressControlDepth, best-effort.
 *
 * A lane's ring is drawn from the link's source when its first frame has to wait and goes back
 * when its last one leaves, so a peer that keeps up holds no ring at all.
 *
 * **The producer writes, elected by try-lock** (ADR-0087 Q6). A sender that finds the peer
 * idle — no writer, nothing waiting — takes the writer role and writes its own record
 * directly. A sender that finds a writer in flight puts its reference in the ring and
 * returns; it never waits on another sender's write. The holder drains the ring before it
 * lets the role go, and re-checks it after (the store-buffering pattern: a pusher draws its
 * lane, then claims; the writer lets the role go, then reads the round, all seq_cst), so
 * nothing is stranded. A sender to an idle peer takes no lock at all: the claim is a
 * try-lock and "nothing waits" is one atomic read (#2145); the peer's own lane lock (#2310)
 * is taken only to put a frame in a lane or take one out, so no two peers share a lock. A write
 * that the socket cuts short keeps its frame on the wire with its offset: the rest goes on the
 * peer's next turn, so a part write never desyncs the stream.
 *
 * **The stall strike.** A strike is an offer the peer could not take: its STREAM lane was
 * full, so the offer cost a frame (the ring's arm shed one), and the turn it took moved no
 * byte of the held frame. Any byte moved clears the count. A SAMPLE lane (depth 1) never
 * strikes: its overwrite is its contract and costs one slot by construction, so a fast
 * publisher cannot condemn a peer whose socket stalls briefly. A peer that is truly stuck
 * behind SAMPLE lanes alone is left to the link's own down detection (`POLLHUP`, `POLLERR`,
 * the liveness window). A refused lane draw is the source's verdict and never strikes. The verdict
 * is in whole rings: a peer is condemned when its strikes in a row reach the link's cap times the
 * depth of the lane being offered, so a peer that let that many rings of a vertex go by without
 * taking a byte. A peer whose lane still has room is inside the depth its vertex declared, so a
 * burst that only fills rings strikes nothing, and a deeper ring tolerates a longer burst. Turns
 * run only on events — a new frame offered to the peer (its sender resumes the held frame first) or
 * the socket reporting writability (@ref tr::net::egress_t::flush) — so the count is of offers the
 * peer refused, never of time.
 * At the link's cap the peer is condemned: the call that reached the verdict says so, and the
 * link closes the session. Anything else — a peer that is gone — is the socket's own event
 * (`POLLHUP`, `POLLERR`) or teardown. No timer and no clock read here.
 *
 * **Which thread frees** (ADR-0072, ADR-0080). A frame reference may be dropped by whichever
 * thread finishes or sheds it, so its block comes from a source that is safe to free on any
 * thread, and it is never released under the lane lock (a lane's own block is: it holds
 * nothing but references).
 *
 * Receiver pays: the frames and the rings come from the link's own source, and a ring's
 * depth is its vertex's declaration. No library-internal buffer, no timer.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <utility>

#include "libtracer/function_ref.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/value.hpp"

namespace tr::net {

/**
 * @brief The depth of a peer's control lane: frames that come from no vertex (a reply, a
 *        control frame, a forwarded record). Compile-time, and best-effort, as every link's
 *        one queue was before #2144: a full control lane sheds its oldest frame, counted.
 */
inline constexpr std::uint16_t kEgressControlDepth = 8;

/**
 * @brief Whole rings that condemn a peer whose writes move nothing: each strike is one offer
 *        that found the peer's STREAM lane full and whose turn moved no byte of the held
 *        frame (a SAMPLE lane never strikes), and
 *        the verdict is this many times the lane's depth in a row. Turns run only on events —
 *        a new frame offered to the peer, or the socket reporting writability — never on a
 *        clock. The #2130 figure.
 */
inline constexpr std::uint8_t kEgressStallStrikes = 64;

/**
 * @brief One outbound frame, built once and shared by every peer it reaches.
 *
 * One block from the link's egress source: this header, then the copied head bytes. A frame
 * that ends with a published value keeps it by reference and its bytes follow the head on
 * the wire. The last @ref release returns the block to the source it came from, on whatever
 * thread drops it.
 */
class egress_frame_t {
   public:
    egress_frame_t(const egress_frame_t&) = delete;
    egress_frame_t& operator=(const egress_frame_t&) = delete;

    /**
     * @brief Draw a frame of @p head_bytes copied bytes and let @p fill write them.
     *
     * @param src        The link's egress source.
     * @param head_bytes Room for the copied part of the frame.
     * @param fill       Writes the copied part into the span it is given and returns the
     *                   byte count it wrote; `0` refuses (nothing could be encoded).
     * @param value      A payload to keep by reference after the copied part, or null.
     * @return The frame with one reference (the caller's), or null when @p src refused the
     *         block, @p fill refused, or @p value could not be kept.
     */
    [[nodiscard]] static egress_frame_t* make(
        mem::block_source_t& src, std::size_t head_bytes,
        function_ref_t<std::size_t(std::span<std::byte>)> fill,
        const graph::value_t* value = nullptr) noexcept {
        const std::size_t bytes = sizeof(egress_frame_t) + head_bytes;
        void* const mem = src.try_alloc(bytes, alignof(egress_frame_t));
        if (mem == nullptr) return nullptr;
        auto* const f = new (mem) egress_frame_t(src, bytes);
        f->head_len_ = fill(std::span<std::byte>(f->head_data(), head_bytes));
        if (value != nullptr) f->value_ = graph::value_ref_t::keep(*value);
        if ((f->head_len_ == 0 && value == nullptr) || (value != nullptr && !f->value_)) {
            release(f);
            return nullptr;
        }
        return f;
    }

    /** @brief Draw a frame whose copied part is @p spans, in order, then @p value if any. */
    [[nodiscard]] static egress_frame_t* make(mem::block_source_t& src,
                                              std::span<const std::span<const std::byte>> spans,
                                              const graph::value_t* value = nullptr) noexcept {
        std::size_t total = 0;
        for (const std::span<const std::byte>& s : spans) total += s.size();
        return make(
            src, total,
            [&](std::span<std::byte> out) {
                std::size_t at = 0;
                for (const std::span<const std::byte>& s : spans) {
                    if (!s.empty()) std::memcpy(out.data() + at, s.data(), s.size());
                    at += s.size();
                }
                return at;
            },
            value);
    }

    /** @brief One more reference (a peer's ring entry). */
    void retain() noexcept { refs_.fetch_add(1, std::memory_order_relaxed); }

    /** @brief Drop one reference; the last one returns the block. Null is a no-op. */
    static void release(egress_frame_t* f) noexcept {
        if (f == nullptr || f->refs_.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        mem::block_source_t* const src = f->src_;
        const std::size_t bytes = f->block_bytes_;
        f->~egress_frame_t();
        src->release(f, bytes, alignof(egress_frame_t));
    }

    /** @brief The copied part: the link's framing and the caller's head spans. */
    [[nodiscard]] std::span<const std::byte> head() const noexcept {
        return {head_data(), head_len_};
    }
    /** @brief The payload kept by reference, or null. */
    [[nodiscard]] const graph::value_t* value() const noexcept { return value_.get(); }
    /** @brief The frame's whole length on the wire. */
    [[nodiscard]] std::size_t size() const noexcept {
        return head_len_ + (value_ ? value_->total_length() : 0);
    }
    /** @brief Spans the frame is written from: the head, then one per value link. */
    [[nodiscard]] std::size_t span_count() const noexcept {
        return 1 + (value_ ? value_->link_count() : 0);
    }

    /**
     * @brief Write the frame's spans that lie past its first @p off bytes into @p out.
     *
     * @p out must hold @ref span_count entries. Empty spans are skipped.
     * @return The number of spans written.
     */
    std::size_t spans_from(std::size_t off, std::span<const std::byte>* out) const noexcept {
        std::size_t n = 0;
        const auto put = [&](std::span<const std::byte> s) {
            if (off >= s.size()) {
                off -= s.size();
                return;
            }
            out[n++] = s.subspan(off);
            off = 0;
        };
        put(head());
        if (value_)
            for (const view::view_t& l : value_->links()) put(l.bytes());
        return n;
    }

   private:
    egress_frame_t(mem::block_source_t& src, std::size_t bytes) noexcept
        : src_(&src), block_bytes_(bytes) {}
    ~egress_frame_t() = default;

    [[nodiscard]] std::byte* head_data() noexcept { return reinterpret_cast<std::byte*>(this + 1); }
    [[nodiscard]] const std::byte* head_data() const noexcept {
        return reinterpret_cast<const std::byte*>(this + 1);
    }

    std::atomic<std::uint32_t> refs_{1}; /**< @brief Peers and senders holding the frame. */
    std::uint32_t head_len_ = 0;         /**< @brief Copied bytes after this header. */
    mem::block_source_t* src_;           /**< @brief Where the block goes back to. */
    std::size_t block_bytes_;            /**< @brief The block's size, for the sized release. */
    graph::value_ref_t value_;           /**< @brief The kept payload, or empty. */
};

/** @brief How one write turn on one frame ended — the link's write callback answers this. */
struct egress_step_t {
    std::size_t moved = 0; /**< @brief Bytes this turn put on the wire. */
    bool held = false;     /**< @brief The socket stopped taking bytes before the frame's end:
                                       the rest waits for the peer's next turn. */
    bool failed = false;   /**< @brief The socket is dead or refused the call: the frame is
                                       abandoned, and so is any byte of it still unwritten. */
};

/**
 * @brief What one @ref egress_t::send or @ref egress_t::flush cost, for the link's counters.
 */
struct egress_result_t {
    std::uint32_t shed = 0;      /**< @brief Frames this call lost toward the peer: refused
                                             (closed peer, reliable ring full, no block),
                                             displaced from a full ring, or abandoned by a
                                             failed write. The link's `dropped_tx`. */
    std::uint32_t displaced = 0; /**< @brief Of @ref shed, waiting frames a newer one pushed
                                             out (the SAMPLE overwrite, the best-effort
                                             drop-oldest). */
    std::uint32_t stalled = 0;   /**< @brief Offers that found the peer's STREAM lane full
                                             and whose turn moved nothing — each one a
                                             strike. A SAMPLE lane never strikes. */
    bool condemn = false;        /**< @brief The peer reached the strike cap, or a failed
                                             write cut a frame mid-way: the link closes it. */
};

/**
 * @brief A fan-out's one shared frame, built on the first peer that needs it and released
 *        when the send returns.
 *
 * Pass it as the `frame` argument of every @ref egress_t::send in the fan-out: the first
 * peer that has to wait builds the frame through @p build, every later one shares it, and a
 * fan-out in which every peer was idle builds nothing at all.
 *
 * @tparam Build Callable `egress_frame_t*()`, held by value (a lambda's captures stay on the
 *               caller's stack; nothing here allocates).
 */
template <class Build>
class egress_once_t {
   public:
    /** @brief A frame @p build makes on first use (null refuses, once and for all). */
    explicit egress_once_t(Build build) noexcept : build_(std::move(build)) {}
    ~egress_once_t() { egress_frame_t::release(f_); }
    egress_once_t(const egress_once_t&) = delete;
    egress_once_t& operator=(const egress_once_t&) = delete;

    /** @brief The frame, built now if this is the first call. */
    [[nodiscard]] egress_frame_t* operator()() noexcept {
        if (!tried_) {
            tried_ = true;
            f_ = build_();
        }
        return f_;
    }

   private:
    Build build_;                 /**< @brief Builds the frame. */
    egress_frame_t* f_ = nullptr; /**< @brief The built frame, or null. */
    bool tried_ = false;          /**< @brief `build_` already ran. */
};

class egress_t;

/**
 * @brief One peer's egress state: the frame on the wire, the lanes waiting behind it, the
 *        writer role and the peer's own lane lock.
 *
 * At most 24 B on a 64-bit host (pinned below; 16 B on a 32-bit MCU), and nothing else at
 * rest: a lane — one ring per (peer, vertex), plus the control lane (ADR-0087 §2) — is drawn
 * from the link's source when that vertex's first frame has to wait, and returned when its
 * last waiting frame leaves. A peer that keeps up never holds one. The lanes are guarded by
 * a bit of the peer's own role byte (#2310), so no two peers' sends share a lock.
 */
class egress_peer_t {
   public:
    egress_peer_t() = default;
    egress_peer_t(const egress_peer_t&) = delete;
    egress_peer_t& operator=(const egress_peer_t&) = delete;

    /** @brief Something waits for the socket to take bytes: a waiter should report this
     *         peer's writability and call @ref egress_t::flush when it comes. */
    [[nodiscard]] bool wants_write() const noexcept {
        return held_.load(std::memory_order_acquire);
    }
    /** @brief The peer is closed: never opened (@ref egress_t::open), or closed since
     *         (@ref egress_t::close). */
    [[nodiscard]] bool closed() const noexcept {
        return (role_.load(std::memory_order_acquire) & kClosed) != 0;
    }

   private:
    friend class egress_t;

    static constexpr std::uint8_t kWriter = 1;   /**< @brief A sender holds the writer role. */
    static constexpr std::uint8_t kClosed = 2;   /**< @brief Closed: nobody may claim the role. */
    static constexpr std::uint8_t kLanes = 4;    /**< @brief A lane step holds this peer's lanes. */
    static constexpr std::uint8_t kLaneWait = 8; /**< @brief A sender sleeps for `kLanes`. */

    /**
     * @brief One vertex's ring toward this peer: FIFO inside, one block from the link's
     *        source, the ring's `depth` entries right after this header.
     */
    struct lane_t {
        const void* key;     /**< @brief The delivery lane's key (null: the control lane). */
        lane_t* next;        /**< @brief The next lane in the round (circular). */
        std::uint16_t depth; /**< @brief Ring entries (≥ 1). */
        std::uint16_t head;  /**< @brief Oldest waiting entry. */
        std::uint16_t count; /**< @brief Waiting entries (≥ 1 while the lane exists). */
        bool reliable;       /**< @brief The STREAM arm a full ring takes. */
        /** @brief The ring, right after the header in the same block. */
        [[nodiscard]] egress_frame_t** ring() noexcept {
            return reinterpret_cast<egress_frame_t**>(this + 1);
        }
    };

    /** @brief The lane served last; `tail_->next` is served next. Its lanes are the lane
     *         lock's (`kLanes`); the pointer is atomic so a sender and the writer can see that
     *         nothing waits without taking that lock (#2145). Null: nothing waits. */
    std::atomic<lane_t*> tail_{nullptr};
    egress_frame_t* wire_ = nullptr; /**< @brief The frame being written (writer only). */
    std::uint32_t off_ = 0;          /**< @brief Bytes of `wire_` on the wire (writer only). */
    std::uint16_t strikes_ = 0;      /**< @brief Consecutive strikes: full-lane offers that
                                                 moved nothing (writer only, saturating). */
    std::atomic<std::uint8_t> role_{kClosed}; /**< @brief `kWriter` | `kClosed` | `kLanes` |
                                                           `kLaneWait`. */
    std::atomic<bool> held_{false};           /**< @brief @ref wants_write. */
};

static_assert(sizeof(void*) != 8 || sizeof(egress_peer_t) <= 24,
              "the egress slot is at most 24 B per peer (#2144)");

/**
 * @brief The link-level half of the egress module: the source and the strike cap.
 *
 * One per link, and it holds no lock: each peer's lanes are guarded by that peer's own lane
 * lock (#2310), a bit of its role byte. The lock is held only for a lane step — an index step,
 * and the lane block drawn or returned with it — never across a write, a frame build or a
 * frame release, and never by a send that finds the peer idle (#2145): it guards the lanes,
 * not the writer election and not the write. A sender that finds it held sleeps on the role
 * byte (`std::atomic::wait`, a futex on Linux, no clock) until the holder lets it go, so a
 * holder preempted inside its step gets the CPU back from a higher-priority sender (the #1618
 * hang a spinner would bring back).
 *
 * @note Thread-safe: any number of senders, plus the link's own receive or wait context.
 */
class egress_t {
   public:
    /**
     * @brief An egress whose lanes are drawn from @p src.
     *
     * @param strike_cap Whole rings of strikes (full-lane offers that move nothing) before
     *                   the peer is condemned. A link whose
     *                   write turns wait a bound passes its own small cap; a non-blocking one
     *                   takes @ref kEgressStallStrikes.
     */
    explicit egress_t(mem::block_source_t& src,
                      std::uint8_t strike_cap = kEgressStallStrikes) noexcept
        : src_(&src), strike_cap_(strike_cap != 0 ? strike_cap : 1) {}

    egress_t(const egress_t&) = delete;
    egress_t& operator=(const egress_t&) = delete;

    /**
     * @brief Put one record on the wire toward @p p, or queue it on its vertex's lane behind
     *        the write in flight.
     *
     * @param p      The destination peer.
     * @param lane   The vertex the record comes from and the ring it waits in: depth 1 is a
     *               SAMPLE (a newer frame overwrites the one waiting), depth N a STREAM whose
     *               full ring takes the vertex's arm. Null key: the control lane
     *               (@ref kEgressControlDepth, best-effort). Lanes are served round-robin, FIFO
     *               inside each.
     * @param frame  Builds the shared frame when it is needed — called at most once per call,
     *               and only when the record has to wait. Return a frame with one reference
     *               the caller keeps (this call adds its own), or null to refuse. A fan-out
     *               passes the same lazily-built frame for every peer.
     * @param direct Writes the caller's record from its own memory, from byte 0, when the
     *               peer was idle: `egress_step_t(void)`.
     * @param write  Writes a waiting frame from an offset: `egress_step_t(const
     *               egress_frame_t&, std::size_t off)`.
     * @param condemn Shuts the peer's socket down. Called while this call still holds the
     *               writer role, so the socket it names cannot have been closed and reused —
     *               only when the peer earned it (@ref egress_result_t::condemn), and
     *               perhaps more than once in one call, so it is idempotent (`shutdown(2)`
     *               is).
     */
    template <class Direct, class Write>
    egress_result_t send(egress_peer_t& p, const graph::delivery_lane_t& lane,
                         function_ref_t<egress_frame_t*()> frame, Direct&& direct, Write&& write,
                         function_ref_t<void()> condemn) {
        egress_result_t r;
        // The role first, by try-lock and no mutex (#2145): only its holder may read wire_.
        // A closed peer refuses the claim, so it is told apart before anything is built.
        const bool own = claim(p);
        if (!own && p.closed()) {
            ++r.shed;  // the peer has gone: nobody to write to
            return r;
        }
        // Nothing waits: no lane (the seq_cst read pairs with a pusher's, see drain_owned) and
        // no frame on the wire. A send to a peer that keeps up takes no mutex at all.
        if (own && p.tail_.load(std::memory_order_seq_cst) == nullptr && p.wire_ == nullptr) {
            const egress_step_t s = std::forward<Direct>(direct)();
            bool held = false;
            if (s.failed) {
                ++r.shed;
                r.condemn = s.moved != 0;  // a frame cut off by a dead socket
            } else if (s.held) {
                // Cut short: the rest waits on the wire, so the stream never desyncs.
                if (egress_frame_t* const f = frame()) {
                    f->retain();
                    p.wire_ = f;
                    p.off_ = static_cast<std::uint32_t>(s.moved);
                    held = true;
                } else {
                    ++r.shed;
                    r.condemn = s.moved != 0;  // half a record went out and the rest cannot
                }
            }
            if (s.moved != 0) p.strikes_ = 0;
            drain_owned(p, std::forward<Write>(write), condemn, r, held, 0, true);
            return r;
        }
        egress_frame_t* const f = frame();
        egress_frame_t* out = f;
        std::uint16_t lost = 0;  // the depth of the full lane this offer cost a frame of
        if (f == nullptr) {
            ++r.shed;
        } else {
            f->retain();
            const lanes_guard_t lock(p);
            if ((lock.role & egress_peer_t::kClosed) != 0)
                ++r.shed;
            else
                out = admit(p, lane, f, r, lost);
        }
        egress_frame_t::release(out);  // outside the lane lock: it may free a block
        // The store-buffering pair: the push above, then the claim. A holder that let the
        // role go after our push re-reads the round and finds it; a holder that has not let
        // go yet drains it. Either way it leaves. The claimed turn
        // resumes a held frame first: this offer is the event a peer's strike counts.
        if (own || claim(p)) drain_owned(p, std::forward<Write>(write), condemn, r, false, lost);
        return r;
    }

    /**
     * @brief The socket of @p p reported writability: resume its held frame and drain its
     *        lanes, unless a sender already holds the role (it drains them itself).
     *        Parameters as @ref send.
     */
    template <class Write>
    egress_result_t flush(egress_peer_t& p, Write&& write, function_ref_t<void()> condemn) {
        egress_result_t r;
        if (claim(p)) drain_owned(p, std::forward<Write>(write), condemn, r);
        return r;
    }

    /**
     * @brief The application's liveness check closing @p p (#2198): run @p condemn if a frame
     *        still waits on its wire, inside the writer role, so the socket it shuts down is
     *        still the peer's own (a teardown waits the role out before it closes the fd).
     *
     * The claim does not wait. A role somebody holds is a write in progress, so the peer is
     * not frozen and nothing runs. Nothing is written here: a lane frame found with the wire
     * free waits on the wire for the peer's own writability, as a full socket's would.
     * @return True iff @p condemn ran.
     */
    bool condemn_held(egress_peer_t& p, function_ref_t<void()> condemn) {
        if (!claim(p)) return false;
        egress_result_t r;
        r.condemn = p.wire_ != nullptr;
        drain_owned(
            p, [](const egress_frame_t&, std::size_t) { return egress_step_t{0, true, false}; },
            condemn, r, r.condemn);
        return r.condemn;
    }

    /**
     * @brief Close @p p: refuse every later send, wait out a write in flight, release every
     *        frame and every lane. Teardown's half of the role handoff.
     *
     * The caller makes the in-flight write return first — `shutdown(2)` on the socket — and
     * never calls this from inside a write callback on the same peer. The wait is on the
     * writer leaving the role (`std::atomic::wait`), not on a clock.
     */
    void close(egress_peer_t& p) noexcept {
        std::uint8_t r = p.role_.fetch_or(egress_peer_t::kClosed, std::memory_order_acq_rel);
        while ((r & egress_peer_t::kWriter) != 0) {
            p.role_.wait(r, std::memory_order_acquire);
            r = p.role_.load(std::memory_order_acquire);
        }
        egress_frame_t::release(std::exchange(p.wire_, nullptr));
        p.off_ = 0;
        // The lane lock once, empty: a pusher that read the peer open under it has finished
        // its push by now, and one that takes it later reads the peer closed (the lock and
        // `kClosed` share one byte). pop's lock-free "nothing waits" read alone could miss a
        // push that raced the close (#2145).
        { const lanes_guard_t lock(p); }
        while (egress_frame_t* const f = pop(p)) egress_frame_t::release(f);
        p.strikes_ = 0;
        p.held_.store(false, std::memory_order_release);
    }

    /**
     * @brief Open @p p for a session: a peer starts closed, and a closed one holds no frame.
     *
     * @param first A frame to put on the wire ahead of anything a sender offers (a WebSocket
     *              server's 101), or null. The egress takes over the caller's reference. It
     *              is held, so the next turn — a send or @ref flush — writes it first.
     */
    void open(egress_peer_t& p, egress_frame_t* first = nullptr) noexcept {
        p.strikes_ = 0;
        p.wire_ = first;
        p.off_ = 0;
        p.held_.store(first != nullptr, std::memory_order_relaxed);
        // Clears `kClosed` only: a late sender may still hold the lane lock to read the peer
        // closed, and its unlock must not land on a lock somebody took after this store.
        p.role_.fetch_and(egress_peer_t::kLanes | egress_peer_t::kLaneWait,
                          std::memory_order_release);
    }

   private:
    using lane_t = egress_peer_t::lane_t;

    /** @brief Take the writer role on a peer that is open and not being written, whoever
     *         holds its lane lock. The pusher's half of the store-buffering pair (see
     *         drain_owned): seq_cst. */
    [[nodiscard]] static bool claim(egress_peer_t& p) noexcept {
        std::uint8_t r = p.role_.load(std::memory_order_relaxed);
        while ((r & (egress_peer_t::kWriter | egress_peer_t::kClosed)) == 0)
            if (p.role_.compare_exchange_weak(r, r | egress_peer_t::kWriter,
                                              std::memory_order_seq_cst))
                return true;
        return false;
    }

    /**
     * @brief @p p's lane lock for one lane step (#2310): the `kLanes` bit of its role byte.
     *
     * A sender that finds it held marks `kLaneWait` and sleeps on the byte
     * (`std::atomic::wait`): it never spins against a holder that may be preempted, so a
     * higher-priority sender on the holder's CPU hands that CPU back (#1618, ruling (a) of
     * #2307). The unlock is the one `fetch_and` it always was, and wakes only when somebody
     * marked that it sleeps. No clock. `role` is the role byte the lock was taken over, so a
     * pusher reads `kClosed` in the same read-modify-write that orders it against
     * @ref close.
     */
    struct lanes_guard_t {
        explicit lanes_guard_t(egress_peer_t& peer) noexcept : p(peer) {
            role = p.role_.load(std::memory_order_relaxed);
            for (;;) {
                if ((role & egress_peer_t::kLanes) == 0) {
                    if (p.role_.compare_exchange_weak(role, role | egress_peer_t::kLanes,
                                                      std::memory_order_seq_cst))
                        return;
                } else if ((role & egress_peer_t::kLaneWait) != 0 ||
                           p.role_.compare_exchange_weak(role, role | egress_peer_t::kLaneWait,
                                                         std::memory_order_relaxed)) {
                    role |= egress_peer_t::kLaneWait;
                    p.role_.wait(role, std::memory_order_relaxed);  // until the unlock wakes us
                    role = p.role_.load(std::memory_order_relaxed);
                }
            }
        }
        ~lanes_guard_t() {
            constexpr auto kHeld =
                static_cast<std::uint8_t>(egress_peer_t::kLanes | egress_peer_t::kLaneWait);
            const std::uint8_t was =
                p.role_.fetch_and(static_cast<std::uint8_t>(~kHeld), std::memory_order_release);
            if ((was & egress_peer_t::kLaneWait) != 0) p.role_.notify_all();
        }
        lanes_guard_t(const lanes_guard_t&) = delete;
        lanes_guard_t& operator=(const lanes_guard_t&) = delete;
        egress_peer_t& p;  /**< @brief The peer whose lanes are held. */
        std::uint8_t role; /**< @brief The role byte the lock was taken over. */
    };

    /**
     * @brief Put @p f in its lane of @p p under the lane's shape, drawing the lane if it is
     *        the vertex's first waiting frame (the peer's lane lock held).
     * @param lost Set to the lane's depth when the lane was full, so the offer cost one of its
     *             frames; left alone otherwise. A refused draw is the source's verdict, not
     *             the peer's, and sets nothing. Its frame is shed, counted: keeping it for the
     *             next drain would need the storage the source just refused (the peer's slot
     *             has no room to name a vertex, and parking it in another lane could let a
     *             stale SAMPLE overtake a newer one), so the vertex's next publish is what
     *             reaches the peer.
     * @return The reference the call must release: a displaced frame, @p f itself when the
     *         reliable arm or the source refused it, or null.
     */
    egress_frame_t* admit(egress_peer_t& p, const graph::delivery_lane_t& lane, egress_frame_t* f,
                          egress_result_t& r, std::uint16_t& lost) noexcept {
        lane_t* const tail = p.tail_.load(std::memory_order_relaxed);
        lane_t* l = tail;
        if (l != nullptr) do
                l = l->next;  // the round, from the lane served next
            while (l->key != lane.key && l != tail);
        if (l == nullptr || l->key != lane.key) {
            l = draw(p, lane);
            if (l == nullptr) {
                ++r.shed;  // no block for the lane: refused, counted
                return f;
            }
        }
        egress_frame_t* out = nullptr;
        if (l->count == l->depth) {
            lost = l->depth;
            if (l->reliable && l->depth > 1) {
                ++r.shed;  // the reliable arm: the newest is refused, nothing waiting is lost
                return f;
            }
            out = l->ring()[l->head];  // SAMPLE overwrite / best-effort drop-oldest
            l->head = static_cast<std::uint16_t>((l->head + 1) % l->depth);
            --l->count;
            ++r.shed;
            ++r.displaced;
        }
        l->ring()[(l->head + l->count) % l->depth] = f;
        ++l->count;
        return out;
    }

    /** @brief Draw @p lane's ring and join it to the END of @p p's round (lane lock held). */
    lane_t* draw(egress_peer_t& p, const graph::delivery_lane_t& lane) noexcept {
        const bool control = lane.key == nullptr;
        const std::uint16_t depth =
            control ? kEgressControlDepth : std::max<std::uint16_t>(lane.depth, 1);
        void* const mem =
            src_->try_alloc(sizeof(lane_t) + sizeof(egress_frame_t*) * depth, alignof(lane_t));
        if (mem == nullptr) return nullptr;
        auto* const l = new (mem) lane_t{lane.key, nullptr, depth, 0, 0, !control && lane.reliable};
        lane_t* const tail = p.tail_.load(std::memory_order_relaxed);
        l->next = tail != nullptr ? tail->next : l;
        if (tail != nullptr) tail->next = l;
        p.tail_.store(l, std::memory_order_seq_cst);  // served after every lane already waiting
        return l;
    }

    /**
     * @brief Pop the oldest frame of the lane served next, and move the round on; a lane
     *        left empty goes back to the source. Null when nothing waits.
     *
     * Only the writer-role holder pops (or @ref close, once the writer has left and every
     * racing push has finished under the lane lock), so only a pop ever empties the round: a
     * null read without the lock is never stale that way, and when nothing waits the pop
     * takes no lock (#2145). A lane a pusher draws meanwhile is found by the writer's
     * re-check after it lets the role go.
     */
    egress_frame_t* pop(egress_peer_t& p) noexcept {
        if (p.tail_.load(std::memory_order_seq_cst) == nullptr) return nullptr;
        const lanes_guard_t lock(p);
        lane_t* const tail = p.tail_.load(std::memory_order_relaxed);
        lane_t* const l = tail->next;
        egress_frame_t* const f = l->ring()[l->head];
        l->head = static_cast<std::uint16_t>((l->head + 1) % l->depth);
        if (--l->count != 0) {
            p.tail_.store(l, std::memory_order_relaxed);  // round-robin: the next lane goes next
            return f;
        }
        tail->next = l->next;
        if (tail == l) p.tail_.store(nullptr, std::memory_order_seq_cst);  // the only lane
        src_->release(l, sizeof(lane_t) + sizeof(egress_frame_t*) * l->depth, alignof(lane_t));
        return f;
    }

    /**
     * @brief The writer's loop: drain the wire frame and the lanes, let the role go, re-check.
     *
     * Ends with the role released. A frame the socket would not finish stays on the wire
     * (@ref egress_peer_t::wants_write) for the next turn. A step is held or failed, never
     * both. Any byte moved clears the strikes;
     * a turn that moved nothing, taken by an offer that found its STREAM lane full, is one,
     * and the cap times that lane's depth (@p lost) condemns the peer. A SAMPLE lane
     * (@p lost 1) and an offer that cost nothing (0) never strike.
     *
     * @p drained skips the first drain pass: the caller took the role over empty lanes and
     * wrote its own record. A frame pushed since is found by the re-check after the role is
     * let go, so the pass would only take the lane lock to find nothing.
     */
    template <class Write>
    void drain_owned(egress_peer_t& p, Write&& write, function_ref_t<void()> condemn,
                     egress_result_t& r, bool held = false, std::uint16_t lost = 0,
                     bool drained = false) {
        for (;;) {
            std::size_t moved = 0;
            while (!held && !std::exchange(drained, false) &&
                   (p.role_.load(std::memory_order_relaxed) & egress_peer_t::kClosed) == 0) {
                if (p.wire_ == nullptr) {
                    p.wire_ = pop(p);
                    p.off_ = 0;
                    if (p.wire_ == nullptr) break;
                }
                const egress_step_t s = write(static_cast<const egress_frame_t&>(*p.wire_),
                                              static_cast<std::size_t>(p.off_));
                p.off_ += static_cast<std::uint32_t>(s.moved);
                moved += s.moved;
                held = s.held;  // the socket is full: the frame waits for it
                if (held) break;
                if (s.failed) {
                    ++r.shed;
                    r.condemn |= p.off_ != 0;  // cut off mid-frame
                }
                egress_frame_t::release(std::exchange(p.wire_, nullptr));
                p.off_ = 0;
            }
            // A strike: an offer that cost a frame of a full STREAM lane, on a turn that
            // moved nothing. A SAMPLE's overwrite is its contract, bounded at one slot, so
            // depth 1 never strikes. Any byte moved clears the run; the verdict is whole rings.
            const std::uint32_t depth = std::exchange(lost, 0);
            const bool strike = (moved == 0) & (depth > 1);
            r.stalled += strike;
            p.strikes_ = static_cast<std::uint16_t>(
                std::min<std::uint32_t>((p.strikes_ + strike) * (moved == 0), UINT16_MAX));
            r.condemn |=
                strike & (p.strikes_ >= std::min<std::uint32_t>(strike_cap_ * depth, UINT16_MAX));
            if (r.condemn) condemn();  // still inside the role: the socket is the peer's own
            p.held_.store(held, std::memory_order_release);
            // The writer's half of the store-buffering pair: let the role go, then look for a
            // lane. A pusher draws its lane, then claims (both seq_cst, no mutex here): either
            // this read finds the lane, or that claim finds the role free. Either way it leaves.
            const std::uint8_t was = p.role_.fetch_and(
                static_cast<std::uint8_t>(~egress_peer_t::kWriter), std::memory_order_seq_cst);
            if ((was & egress_peer_t::kClosed) != 0) {
                p.role_.notify_all();  // a teardown waits for exactly this
                return;
            }
            if (held) return;  // the waiter, or the next send, resumes it
            if (p.tail_.load(std::memory_order_seq_cst) == nullptr || !claim(p)) return;
        }
    }

    mem::block_source_t* src_; /**< @brief Where the lanes come from. */
    std::uint8_t strike_cap_;  /**< @brief Strikes before condemnation. */
};

}  // namespace tr::net
