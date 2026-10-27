/**
 * @file
 * @brief #2144 — the shared-frame egress module (ADR-0087 §2): one refcounted frame per send,
 *        one ring of frame references per (peer, vertex) at the vertex's depth, a writer
 *        elected by try-lock, a stall strike counted on events.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Driven through scripted write callbacks (a socket that takes so many bytes now, or blocks
 * until released) and through the socket-free fake link of #2139 as the peers' wire, so every
 * case is deterministic and none needs a socket:
 *
 *  1. SAMPLE (depth 1) is latest-wins: a newer frame overwrites the waiting one.
 *  2. STREAM (depth N) keeps order up to the declared depth; a full ring takes the vertex's
 *     pressure arm — best-effort sheds the oldest, reliable refuses the newest.
 *  3. One ring per vertex: a vertex that floods its ring sheds none of another vertex's
 *     frames, and the lanes leave round-robin, FIFO inside each. A frame from no vertex rides
 *     the control lane (depth `kEgressControlDepth`, best-effort).
 *  4. A frame the socket took only part of resumes from its offset; a failed write that cut
 *     a frame mid-way condemns the peer.
 *  5. Offers past a full STREAM lane that move nothing are strikes; the cap times the lane's
 *     depth condemns the peer, inside the writer role; an offer the lane still had room for is
 *     none, a turn that moves a byte clears them, and no offer means no strike. A SAMPLE lane
 *     never strikes: a fast publisher to a briefly stalled peer condemns nothing, and the peer
 *     gets the latest value when it drains, and SAMPLE offers interleaved with STREAM ones on
 *     the same stalled peer strike nothing. The control lane strikes like a STREAM lane.
 *  6. Close waits the writer out and releases every frame and lane; a refused lane sheds the
 *     frame, counted, and leaves the peer open. A close that races senders strands nothing.
 *  7. A peer whose writer is stuck inside a write does not delay a send to another peer of the
 *     same link, and a second sender to the stuck peer returns at once.
 *  8. Many senders, each its own vertex, and a flusher on one peer: every frame arrives whole,
 *     each sender's in its own order, and nothing leaks (the TSan leg of the slot handoff).
 */

#include "libtracer/egress.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "fake_link.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::delivery_lane_t;
using tr::net::egress_frame_t;
using tr::net::egress_peer_t;
using tr::net::egress_result_t;
using tr::net::egress_step_t;
using tr::net::egress_t;
using tr::testing::check;
using tr::testing::check_quiet;
using bytes_t = std::vector<std::byte>;

/** @brief A heap-backed source that counts live blocks and can be told to refuse. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : block_source_t("egress_test") {}
    void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refuse.load(std::memory_order_relaxed)) return nullptr;
        void* const p = tr::mem::heap_source().try_alloc(bytes, align);
        if (p != nullptr) live.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        live.fetch_sub(1, std::memory_order_relaxed);
        tr::mem::heap_source().release(p, bytes, align);
    }
    std::atomic<int> live{0};        /**< @brief Blocks out. */
    std::atomic<bool> refuse{false}; /**< @brief Refuse every allocation. */
};

/** @brief Two vertices' identities: a lane key is compared, never dereferenced. */
int vertex_a = 0;
int vertex_b = 0;

/** @brief The lane of @p key at @p depth and arm. */
[[nodiscard]] delivery_lane_t lane_of(const void* key, std::uint16_t depth, bool reliable = false) {
    return delivery_lane_t{.key = key, .depth = depth, .reliable = reliable};
}

/** @brief A frame of @p n bytes, every byte @p mark. */
[[nodiscard]] egress_frame_t* frame(counting_source_t& src, std::uint8_t mark, std::size_t n = 8) {
    const bytes_t b(n, std::byte{mark});
    const std::span<const std::byte> one[1] = {b};
    return egress_frame_t::make(src, one);
}

/**
 * @brief A scripted peer socket: takes at most `room` bytes now (moving the rest to the next
 *        turn), records what it took, and fails when told to. Only the writer-role holder
 *        calls it, so its state is unsynchronized on purpose: TSan reports a second writer.
 */
struct fake_sock_t {
    std::size_t room = SIZE_MAX; /**< @brief Bytes it still takes. */
    bool fail = false;           /**< @brief Every write fails (a dead socket). */
    bytes_t got;                 /**< @brief Everything it took, in order. */
    int condemned = 0;           /**< @brief Times the egress condemned it. */

    egress_step_t write(std::span<const std::byte> bytes) {
        if (fail) return {0, false, true};
        const std::size_t n = bytes.size() < room ? bytes.size() : room;
        got.insert(got.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        room -= n;
        return {n, n < bytes.size(), false};
    }
    egress_step_t write(const egress_frame_t& f, std::size_t off) {
        std::array<std::span<const std::byte>, 4> spans;
        const std::size_t k = f.spans_from(off, spans.data());
        egress_step_t total;
        for (std::size_t i = 0; i < k; ++i) {
            const egress_step_t s = write(spans[i]);
            total.moved += s.moved;
            if (s.held || s.failed) {
                total.held = s.held;
                total.failed = s.failed;
                return total;
            }
        }
        return total;
    }
};

/** @brief One send of a @p mark frame to @p p over @p sock in @p lane. */
egress_result_t send(egress_t& eg, egress_peer_t& p, fake_sock_t& sock, counting_source_t& src,
                     std::uint8_t mark, const delivery_lane_t& lane) {
    const bytes_t own(8, std::byte{mark});
    tr::net::egress_once_t once([&] { return frame(src, mark); });
    return eg.send(
        p, lane, once, [&] { return sock.write(own); },
        [&](const egress_frame_t& f, std::size_t off) { return sock.write(f, off); },
        [&] { ++sock.condemned; });
}

egress_result_t flush(egress_t& eg, egress_peer_t& p, fake_sock_t& sock) {
    return eg.flush(
        p, [&](const egress_frame_t& f, std::size_t off) { return sock.write(f, off); },
        [&] { ++sock.condemned; });
}

/** @brief The marks of the 8-byte frames in @p got, in order (0 for a torn frame). */
[[nodiscard]] std::vector<std::uint8_t> marks(const bytes_t& got) {
    std::vector<std::uint8_t> out;
    for (std::size_t at = 0; at + 8 <= got.size(); at += 8) {
        bool same = true;
        for (std::size_t i = 1; i < 8; ++i) same = same && got[at + i] == got[at];
        out.push_back(same ? std::to_integer<std::uint8_t>(got[at]) : 0);
    }
    return out;
}

void test_sample_is_latest_wins() {
    std::printf("SAMPLE (depth 1): a newer frame overwrites the waiting one:\n");
    counting_source_t src;
    {
        egress_t eg(src);
        egress_peer_t p;
        check(p.closed(), "a fresh peer starts closed");
        eg.open(p);
        const delivery_lane_t a = lane_of(&vertex_a, 1);
        fake_sock_t sock;
        sock.room = 0;  // the peer is not reading
        egress_result_t r = send(eg, p, sock, src, 1, a);
        check(r.shed == 0 && r.stalled == 0 && p.wants_write(),
              "the first frame stays on the wire, no strike, and the peer wants writability");
        r = send(eg, p, sock, src, 2, a);
        check(r.shed == 0, "the second waits in the vertex's one slot");
        r = send(eg, p, sock, src, 3, a);
        check(r.shed == 1 && r.displaced == 1, "the third overwrites it: one displaced, counted");
        sock.room = SIZE_MAX;
        r = flush(eg, p, sock);
        check(marks(sock.got) == std::vector<std::uint8_t>{1, 3},
              "the peer gets the frame on the wire, then only the LATEST");
        check(!p.wants_write(), "and is idle again");
        check(src.live.load() == 0, "a drained lane goes back to the link's store");
        eg.close(p);
    }
    check(src.live.load() == 0, "every frame went back to the link's store");
}

void stream_case(bool reliable) {
    counting_source_t src;
    {
        egress_t eg(src);
        egress_peer_t p;
        eg.open(p);
        fake_sock_t sock;
        sock.room = 0;
        std::uint32_t shed = 0;
        for (std::uint8_t m = 1; m <= 6; ++m)
            shed += send(eg, p, sock, src, m, lane_of(&vertex_a, 4, reliable)).shed;
        check(shed == 1, "one record past the depth is lost, and counted");
        sock.room = SIZE_MAX;
        (void)flush(eg, p, sock);
        const std::vector<std::uint8_t> want = reliable ? std::vector<std::uint8_t>{1, 2, 3, 4, 5}
                                                        : std::vector<std::uint8_t>{1, 3, 4, 5, 6};
        check(marks(sock.got) == want, reliable ? "reliable: order kept, the newest refused"
                                                : "best-effort: order kept, the oldest shed");
        eg.close(p);
    }
    check(src.live.load() == 0, "and nothing leaked");
}

void test_stream_order_and_arms() {
    std::printf("STREAM (depth 4), best-effort:\n");
    stream_case(false);
    std::printf("STREAM (depth 4), reliable:\n");
    stream_case(true);
}

void test_one_ring_per_vertex() {
    std::printf("one ring per (peer, vertex): isolation, round-robin, the control lane:\n");
    counting_source_t src;
    {
        egress_t eg(src);
        egress_peer_t p;
        eg.open(p);
        fake_sock_t sock;
        sock.room = 0;
        const delivery_lane_t a = lane_of(&vertex_a, 4);
        const delivery_lane_t b = lane_of(&vertex_b, 4);
        (void)send(eg, p, sock, src, 1, a);  // on the wire, held
        (void)send(eg, p, sock, src, 2, a);
        (void)send(eg, p, sock, src, 3, a);
        (void)send(eg, p, sock, src, 11, b);
        (void)send(eg, p, sock, src, 12, b);
        (void)send(eg, p, sock, src, 4, a);
        sock.room = SIZE_MAX;
        (void)flush(eg, p, sock);
        check(marks(sock.got) == std::vector<std::uint8_t>{1, 2, 11, 3, 12, 4},
              "the lanes leave round-robin, each in its own order");
        sock.got.clear();

        // A SAMPLE vertex that floods its one slot sheds only its own frames.
        sock.room = 0;
        const delivery_lane_t fast = lane_of(&vertex_a, 1);
        (void)send(eg, p, sock, src, 21, b);  // on the wire, held
        (void)send(eg, p, sock, src, 22, b);
        std::uint32_t shed = 0;
        for (std::uint8_t m = 31; m <= 40; ++m) shed += send(eg, p, sock, src, m, fast).shed;
        check(shed == 9, "the flooding vertex overwrote only its own slot, nine times");
        sock.room = SIZE_MAX;
        (void)flush(eg, p, sock);
        check(marks(sock.got) == std::vector<std::uint8_t>{21, 22, 40},
              "the other vertex's frame survived; the flooder's latest went after it");
        sock.got.clear();

        // A frame from no vertex rides the control lane: kEgressControlDepth, best-effort.
        sock.room = 0;
        std::uint32_t displaced = 0;
        for (std::uint8_t m = 51; m <= 60; ++m)
            displaced += send(eg, p, sock, src, m, {}).displaced;
        check(displaced == 1,
              "the control lane holds kEgressControlDepth behind the wire, then "
              "sheds its oldest");
        sock.room = SIZE_MAX;
        (void)flush(eg, p, sock);
        check(marks(sock.got) == std::vector<std::uint8_t>{51, 53, 54, 55, 56, 57, 58, 59, 60},
              "and keeps the order of what it holds");
        static_assert(tr::net::kEgressControlDepth == 8);
        eg.close(p);
    }
    check(src.live.load() == 0, "every frame and lane went back to the store");
}

void test_part_write_resumes() {
    std::printf("a frame the socket took part of resumes from its offset:\n");
    counting_source_t src;
    egress_t eg(src);
    egress_peer_t p;
    eg.open(p);
    const delivery_lane_t a = lane_of(&vertex_a, 4);
    fake_sock_t sock;
    sock.room = 5;  // three bytes short of the first frame
    egress_result_t r = send(eg, p, sock, src, 7, a);
    check(r.stalled == 0 && r.shed == 0, "a turn that moved bytes is no strike, and sheds nothing");
    check(p.wants_write(), "the rest waits for writability");
    sock.room = SIZE_MAX;
    r = flush(eg, p, sock);
    check(marks(sock.got) == std::vector<std::uint8_t>{7},
          "writability alone finished it from byte 5: no second send needed");
    r = send(eg, p, sock, src, 8, a);
    check(marks(sock.got) == std::vector<std::uint8_t>{7, 8}, "the framing never desynced");
    // A dead socket that cuts a frame mid-way condemns the peer.
    sock.room = 3;
    r = send(eg, p, sock, src, 9, a);
    sock.fail = true;
    r = flush(eg, p, sock);
    check(r.condemn && r.shed == 1 && sock.condemned == 1,
          "a failed write mid-frame is shed and condemns the peer, inside the role");
    eg.close(p);
    check(src.live.load() == 0, "nothing leaked");
}

void test_strikes_count_offers() {
    std::printf(
        "offers past a full STREAM lane that move nothing are strikes; whole rings "
        "condemn:\n");
    counting_source_t src;
    egress_t eg(src, /*strike_cap=*/3);
    egress_peer_t p;
    eg.open(p);
    const delivery_lane_t a = lane_of(&vertex_a, 2);
    fake_sock_t sock;
    sock.room = 0;
    std::uint32_t stalled = 0;
    for (std::uint8_t m = 1; m <= 3; ++m) stalled += send(eg, p, sock, src, m, a).stalled;
    check(stalled == 0, "one held and the lane's depth waiting: the lane absorbs it, no strike");
    egress_result_t r = send(eg, p, sock, src, 4, a);
    check(r.stalled == 1 && r.displaced == 1,
          "an offer past the full lane that moved nothing: one");
    r = send(eg, p, sock, src, 5, a);
    check(r.stalled == 1 && !r.condemn, "the next: two");
    sock.room = 1;  // one byte moves: the strikes clear
    r = send(eg, p, sock, src, 6, a);
    check(r.stalled == 0, "a turn that moved a byte is no strike");
    sock.room = 0;
    stalled = 0;
    for (std::uint8_t m = 7; m <= 11; ++m) stalled += send(eg, p, sock, src, m, a).stalled;
    check(stalled == 5 && sock.condemned == 0, "five strikes after a moved byte: still open");
    r = send(eg, p, sock, src, 12, a);
    check(r.condemn && sock.condemned == 1,
          "the sixth, the cap's three whole rings of a depth-2 lane, condemns");
    eg.close(p);
    r = flush(eg, p, sock);
    check(r.stalled == 0, "a closed peer is no strike");
    check(src.live.load() == 0, "closing released every waiting frame and lane");
}

/**
 * @brief A SAMPLE lane never strikes (maintainer ruling on #2144): its overwrite is its
 *        contract, bounded at one slot, so a fast publisher to a peer whose socket stalls
 *        for a while condemns nothing, and the peer reads the latest value once it drains.
 */
void test_sample_never_strikes() {
    std::printf("a fast SAMPLE publisher never condemns a briefly stalled peer:\n");
    counting_source_t src;
    egress_t eg(src, /*strike_cap=*/3);
    egress_peer_t p;
    eg.open(p);
    const delivery_lane_t telemetry = lane_of(&vertex_a, 1);
    fake_sock_t sock;
    sock.room = 0;  // the stall
    egress_result_t sum;
    for (int i = 1; i <= 1000; ++i) {
        const egress_result_t r =
            send(eg, p, sock, src, static_cast<std::uint8_t>(1 + i % 250), telemetry);
        sum.stalled += r.stalled;
        sum.displaced += r.displaced;
        sum.condemn |= r.condemn;
    }
    check(sum.stalled == 0 && !sum.condemn && sock.condemned == 0 && !p.closed(),
          "a thousand overwrites, far past cap x depth: no strike, still open");
    check(sum.displaced == 998, "each newer value overwrote the one waiting, counted");
    check(src.live.load() == 3, "the cost stayed bounded: the held frame, one slot, its lane");
    sock.room = SIZE_MAX;  // the socket drains
    (void)flush(eg, p, sock);
    check(
        marks(sock.got) == std::vector<std::uint8_t>{2, static_cast<std::uint8_t>(1 + 1000 % 250)},
        "it drains the held frame, then the latest value");
    eg.close(p);
    check(src.live.load() == 0, "nothing leaked");
}

/**
 * @brief SAMPLE and STREAM offers interleaved against one stalled peer (#2174 review): the
 *        SAMPLE lane's overwrites strike nothing and clear nothing, while the STREAM lane's
 *        offers past its full ring strike and condemn at the cap times its depth.
 */
void test_sample_and_stream_interleaved() {
    std::printf("SAMPLE and STREAM interleaved on one stalled peer: only STREAM strikes:\n");
    counting_source_t src;
    egress_t eg(src, /*strike_cap=*/3);
    egress_peer_t p;
    eg.open(p);
    const delivery_lane_t telemetry = lane_of(&vertex_a, 1);
    const delivery_lane_t log = lane_of(&vertex_b, 2);
    fake_sock_t sock;
    sock.room = 0;
    (void)send(eg, p, sock, src, 1, telemetry);  // held on the wire
    std::uint32_t sample_stalled = 0;
    std::uint32_t stream_stalled = 0;
    int condemned_at = 0;
    for (int i = 1; i <= 20 && condemned_at == 0; ++i) {
        const egress_result_t s =
            send(eg, p, sock, src, static_cast<std::uint8_t>(10 + i), telemetry);
        sample_stalled += s.stalled;
        check_quiet(!s.condemn, "a SAMPLE offer never condemns");
        const egress_result_t r = send(eg, p, sock, src, static_cast<std::uint8_t>(100 + i), log);
        stream_stalled += r.stalled;
        if (r.condemn) condemned_at = i;
    }
    check(sample_stalled == 0, "no SAMPLE offer struck");
    check(stream_stalled == 6 && condemned_at == 8 && sock.condemned == 1,
          "STREAM: two absorbed, then six strikes (cap 3 x depth 2) condemn on the eighth");
    eg.close(p);
    check(src.live.load() == 0, "nothing leaked");
}

/**
 * @brief The control lane (no vertex, depth `kEgressControlDepth`, best-effort) is a STREAM
 *        lane for the strike count (#2174 review): offers past its full ring strike, and the
 *        cap times its depth condemns.
 */
void test_control_lane_strikes() {
    std::printf("the control lane strikes and condemns at cap x its depth:\n");
    counting_source_t src;
    egress_t eg(src, /*strike_cap=*/3);
    egress_peer_t p;
    eg.open(p);
    const delivery_lane_t control{};
    fake_sock_t sock;
    sock.room = 0;
    std::uint32_t stalled = 0;
    int condemned_at = 0;
    for (int i = 1; i <= 64 && condemned_at == 0; ++i) {
        const egress_result_t r = send(eg, p, sock, src, static_cast<std::uint8_t>(i), control);
        stalled += r.stalled;
        if (r.condemn) condemned_at = i;
    }
    constexpr int kDepth = tr::net::kEgressControlDepth;
    check(stalled == 3U * kDepth && condemned_at == 1 + kDepth + 3 * kDepth,
          "one held and a full ring absorbed, then cap x depth strikes condemn");
    check(sock.condemned == 1, "condemned once, inside the role");
    eg.close(p);
    check(src.live.load() == 0, "nothing leaked");
}

void test_close_and_refusal() {
    std::printf("close sheds later sends; a refused lane sheds its frame, counted:\n");
    counting_source_t src;
    egress_t eg(src);
    egress_peer_t p;
    eg.open(p);
    const delivery_lane_t a = lane_of(&vertex_a, 4);
    fake_sock_t sock;
    sock.room = 0;
    (void)send(eg, p, sock, src, 1, a);
    (void)send(eg, p, sock, src, 2, a);
    eg.close(p);
    check(p.closed() && !p.wants_write(), "closed, nothing held");
    check(src.live.load() == 0, "the frames and the lane went back");
    sock.room = SIZE_MAX;
    check(send(eg, p, sock, src, 3, a).shed == 1 && sock.got.empty(), "a send to it is shed");
    eg.open(p);
    (void)send(eg, p, sock, src, 4, a);
    check(marks(sock.got) == std::vector<std::uint8_t>{4}, "reopened, it writes again");

    // The source refuses the lane (and the frame): the record is shed, the peer stays open.
    sock.room = 0;
    (void)send(eg, p, sock, src, 5, a);  // held on the wire
    src.refuse.store(true);
    const egress_result_t refused = send(eg, p, sock, src, 6, lane_of(&vertex_b, 4));
    check(refused.shed == 1 && refused.stalled == 0,
          "a refused lane: shed, and the source's verdict is no strike");
    src.refuse.store(false);
    check(!p.closed(), "and the peer is still open");
    sock.room = SIZE_MAX;
    (void)flush(eg, p, sock);
    check(marks(sock.got) == std::vector<std::uint8_t>{4, 5}, "the held frame still finishes");
    eg.close(p);
    check(src.live.load() == 0, "nothing leaked");
}

/**
 * @brief Acceptance 1: a stalled peer does not delay other peers' sends, through the fake
 *        link of #2139 as each peer's wire.
 *
 * Peer A's writer is stuck INSIDE its write (a blocking socket that does not return until
 * released). While it is, a send to peer B of the same link — the same egress, the same queue
 * mutex — completes and B's fake link carries the frame, and a second send to A returns at
 * once with its frame queued. Before #2144 every server write held one shared write mutex, so
 * both waited for A.
 */
void test_stalled_peer_does_not_delay_others() {
    std::printf("a peer stuck in a write delays no other peer (fake link wires):\n");
    counting_source_t src;
    tr::testing::link_seam::counting_backend_t rx;
    tr::testing::link_seam::fake_link_t wire_a(rx, 4096, 1 << 20);
    tr::testing::link_seam::fake_link_t wire_b(rx, 4096, 1 << 20);
    egress_t eg(src);
    egress_peer_t a;
    egress_peer_t b;
    eg.open(a);
    eg.open(b);
    const delivery_lane_t lane = lane_of(&vertex_a, 4);

    std::mutex m;
    std::condition_variable cv;
    bool stuck = false;
    bool released = false;
    const auto onto = [](tr::testing::link_seam::fake_link_t& w, std::span<const std::byte> s) {
        w.send(s);
        return egress_step_t{s.size(), false, false};
    };
    const auto write_a = [&](const egress_frame_t& f, std::size_t off) {
        std::array<std::span<const std::byte>, 4> spans;
        (void)f.spans_from(off, spans.data());
        return onto(wire_a, spans[0]);
    };
    const auto blocking_a = [&](std::span<const std::byte> s) {
        std::unique_lock lock(m);
        stuck = true;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
        return onto(wire_a, s);
    };

    const bytes_t one(8, std::byte{0xA1});
    std::thread first([&] {
        tr::net::egress_once_t once([&] { return frame(src, 0xA1); });
        (void)eg.send(a, lane, once, [&] { return blocking_a(one); }, write_a, [] {});
    });
    {
        std::unique_lock lock(m);
        check(cv.wait_for(lock, 2s, [&] { return stuck; }), "A's writer is stuck in its write");
    }

    const auto t0 = std::chrono::steady_clock::now();
    {
        const bytes_t to_b(8, std::byte{0xB1});
        tr::net::egress_once_t once_b([&] { return frame(src, 0xB1); });
        (void)eg.send(
            b, lane, once_b, [&] { return onto(wire_b, to_b); },
            [&](const egress_frame_t& f, std::size_t off) {
                std::array<std::span<const std::byte>, 4> spans;
                (void)f.spans_from(off, spans.data());
                return onto(wire_b, spans[0]);
            },
            [] {});
        const bytes_t two(8, std::byte{0xA2});
        tr::net::egress_once_t once_a2([&] { return frame(src, 0xA2); });
        (void)eg.send(a, lane, once_a2, [&] { return onto(wire_a, two); }, write_a, [] {});
    }  // the sends' own references go here, as a link's send returns
    const auto took = std::chrono::steady_clock::now() - t0;
    check(took < 50ms, "both sends returned at once while A's write was still stuck");
    check(wire_b.wire_out().size() == 4 + 8, "B's frame is on B's wire");
    check(wire_a.wire_out().empty(), "A's second frame waits behind its stuck write");

    {
        const std::lock_guard lock(m);
        released = true;
    }
    cv.notify_all();
    first.join();
    const bytes_t got_a = wire_a.wire_out();
    check(
        got_a.size() == 2 * (4 + 8) && got_a[4] == std::byte{0xA1} && got_a[16] == std::byte{0xA2},
        "released, A's writer drained the frame queued behind it, in order");
    eg.close(a);
    eg.close(b);
    check(src.live.load() == 0, "nothing leaked");
}

/**
 * @brief The slot handoff under contention (the TSan leg; the GenMC leg is #2120): four
 *        senders, each its own vertex's lane, and a flusher on one peer whose socket takes a
 *        few bytes per turn.
 */
/**
 * @brief A close that races senders strands nothing (#2145): the "nothing waits" read is
 *        lock-free, so close takes the peer's lane lock once before it drains, and a frame pushed
 *        while the close ran is either drained by it or refused as closed.
 */
void test_close_races_senders() {
    std::printf("a close racing senders whose socket holds every write strands nothing:\n");
    counting_source_t src;
    bool leaked = false;
    for (int round = 0; round < 200 && !leaked; ++round) {
        egress_t eg(src, /*strike_cap=*/255);
        egress_peer_t p;
        eg.open(p);
        const auto none = [](const egress_frame_t&, std::size_t) {
            return egress_step_t{0, true, false};
        };
        std::atomic<bool> go{false};
        std::vector<std::thread> ts;
        for (int t = 0; t < 3; ++t)
            ts.emplace_back([&, t] {
                const delivery_lane_t lane = lane_of(t == 0 ? &vertex_a : &vertex_b, 4);
                const bytes_t own(8, std::byte{0x33});
                const std::span<const std::byte> one[1] = {own};
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 0; i < 50; ++i) {
                    tr::net::egress_once_t once([&] { return egress_frame_t::make(src, one); });
                    (void)eg.send(
                        p, lane, once, [] { return egress_step_t{0, true, false}; }, none, [] {});
                }
            });
        go.store(true, std::memory_order_release);
        std::this_thread::yield();
        eg.close(p);
        for (std::thread& t : ts) t.join();
        leaked = src.live.load() != 0;  // a push the close missed would still be held here
    }
    check(!leaked, "every frame and lane went back to the store, every round");
}

void test_concurrent_handoff() {
    std::printf("many senders and a flusher on one peer (TSan):\n");
    counting_source_t src;
    constexpr int kSenders = 4;
    constexpr int kPer = 2000;
    std::array<int, kSenders> vertices{};
    std::atomic<std::uint32_t> shed{0};
    std::atomic<int> condemned{0};
    bytes_t got;             // writer-role only
    std::size_t budget = 0;  // writer-role only: bytes the "socket" takes this turn
    {
        egress_t eg(src, /*strike_cap=*/255);
        egress_peer_t p;
        eg.open(p);
        const auto take = [&](std::span<const std::byte> s) {
            budget = (budget * 7 + 5) % 13;  // a socket that takes 0..12 bytes per turn
            const std::size_t n = s.size() < budget ? s.size() : budget;
            got.insert(got.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
            return egress_step_t{n, n < s.size(), false};
        };
        const auto write = [&](const egress_frame_t& f, std::size_t off) {
            std::array<std::span<const std::byte>, 4> spans;
            (void)f.spans_from(off, spans.data());
            return take(spans[0]);
        };
        std::atomic<bool> done{false};
        std::vector<std::thread> ts;
        for (int t = 0; t < kSenders; ++t)
            ts.emplace_back([&, t] {
                const delivery_lane_t lane = lane_of(&vertices[t], 8);
                for (int i = 0; i < kPer; ++i) {
                    // Each frame: [sender][seq hi][seq lo] then filler — 8 bytes.
                    bytes_t own(8, std::byte{0x5A});
                    own[0] = std::byte(static_cast<std::uint8_t>(t + 1));
                    own[1] = std::byte(static_cast<std::uint8_t>(i >> 8));
                    own[2] = std::byte(static_cast<std::uint8_t>(i & 0xFF));
                    const std::span<const std::byte> one[1] = {own};
                    tr::net::egress_once_t once([&] { return egress_frame_t::make(src, one); });
                    const egress_result_t r = eg.send(
                        p, lane, once, [&] { return take(own); }, write, [&] { ++condemned; });
                    shed.fetch_add(r.shed, std::memory_order_relaxed);
                }
            });
        std::thread flusher([&] {
            while (!done.load(std::memory_order_acquire))
                (void)eg.flush(p, write, [&] { ++condemned; });
        });
        for (std::thread& t : ts) t.join();
        done.store(true, std::memory_order_release);
        flusher.join();
        for (int i = 0; i < 10000 && (p.wants_write() || got.size() % 8 != 0); ++i)
            (void)eg.flush(p, write, [] {});
        // Drain whatever still waits in the lanes with a socket that takes everything.
        budget = 0;
        const auto all = [&](const egress_frame_t& f, std::size_t off) {
            std::array<std::span<const std::byte>, 4> spans;
            (void)f.spans_from(off, spans.data());
            got.insert(got.end(), spans[0].begin(), spans[0].end());
            return egress_step_t{spans[0].size(), false, false};
        };
        (void)eg.flush(p, all, [] {});
        eg.close(p);
    }
    check(got.size() % 8 == 0, "every frame arrived whole — no two writers interleaved");
    std::array<int, kSenders + 1> last{};
    last.fill(-1);
    bool ordered = true;
    std::size_t frames = 0;
    for (std::size_t at = 0; at + 8 <= got.size(); at += 8, ++frames) {
        const int t = std::to_integer<int>(got[at]);
        const int seq =
            (std::to_integer<int>(got[at + 1]) << 8) | std::to_integer<int>(got[at + 2]);
        if (t < 1 || t > kSenders || seq <= last[t]) ordered = false;
        if (t >= 1 && t <= kSenders) last[t] = seq;
    }
    check(ordered, "each sender's frames arrived in its own order");
    check(frames + shed.load() == static_cast<std::size_t>(kSenders * kPer),
          "delivered + shed = sent: no frame lost uncounted, none duplicated");
    check(condemned.load() == 0, "a slow socket that keeps moving bytes is never condemned");
    check(src.live.load() == 0, "and every frame and lane went back to the store");
    std::printf("  delivered=%zu shed=%u\n", frames, shed.load());
}

thread_local bool t_gated = false; /**< @brief This thread's draws go through the gate. */
thread_local int t_draws = 0;      /**< @brief Draws this thread made through the gate. */

/**
 * @brief #2310: the lane lock is the peer's own. A push into peer A that is held inside its
 *        lane step (its lane draw parked in the source) does not block a push into peer B.
 *
 * Before #2310 one mutex per link guarded every peer's lanes, so B's push waited for A's
 * draw to return.
 */
void test_lane_lock_is_per_peer() {
    std::printf("a push held in one peer's lane step does not block another peer's push:\n");
    // A source that parks the gated thread's SECOND draw (its first is the frame, built
    // before the lane lock; the second is the lane, drawn under it) until released.
    struct gated_source_t final : tr::mem::block_source_t {
        gated_source_t() noexcept : block_source_t("egress_test_gated") {}
        void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
            if (t_gated && ++t_draws == 2) {
                parked.store(true, std::memory_order_release);
                while (!open.load(std::memory_order_acquire)) std::this_thread::yield();
            }
            void* const p = tr::mem::heap_source().try_alloc(bytes, align);
            if (p != nullptr) live.fetch_add(1, std::memory_order_relaxed);
            return p;
        }
        void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
            live.fetch_sub(1, std::memory_order_relaxed);
            tr::mem::heap_source().release(p, bytes, align);
        }
        std::atomic<bool> parked{false};
        std::atomic<bool> open{false};
        std::atomic<int> live{0};
    };
    gated_source_t src;
    egress_t eg(src);
    egress_peer_t a;
    egress_peer_t b;
    eg.open(a);
    eg.open(b);
    const delivery_lane_t lane = lane_of(&vertex_a, 4);
    const bytes_t own(8, std::byte{0x5A});
    const std::span<const std::byte> one[1] = {own};
    const auto none = [](const egress_frame_t&, std::size_t) {
        return egress_step_t{0, true, false};
    };
    const auto push = [&](egress_peer_t& p) {
        tr::net::egress_once_t once([&] { return egress_frame_t::make(src, one); });
        (void)eg.send(p, lane, once, [] { return egress_step_t{0, true, false}; }, none, [] {});
    };
    push(a);  // each peer's socket holds its first frame on the wire, so the next send queues
    push(b);

    std::thread held([&] {
        t_gated = true;
        push(a);  // parks inside A's lane draw, A's lane lock held
    });
    while (!src.parked.load(std::memory_order_acquire)) std::this_thread::yield();
    std::atomic<bool> b_done{false};
    std::thread other([&] {
        push(b);
        b_done.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!b_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(b_done.load(), "B's push returned while A's lane step was held");
    // The positive control: a second push into A DOES wait for A's held step, so the leg
    // above shows the lock is per peer, not that pushes take no lock at all.
    std::atomic<bool> a2_done{false};
    std::thread second([&] {
        push(a);
        a2_done.store(true, std::memory_order_release);
    });
    const auto settle = std::chrono::steady_clock::now() + 200ms;
    while (!a2_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < settle)
        std::this_thread::yield();
    check(!a2_done.load(), "a second push into A waits while A's lane step is held");
    src.open.store(true, std::memory_order_release);
    held.join();
    other.join();
    second.join();
    check(a2_done.load(), "and returns once A's step lets the lane lock go");
    eg.close(a);
    eg.close(b);
    check(src.live.load() == 0, "nothing leaked");
}

void test_slot_size() {
    std::printf("the per-peer egress slot is at most 24 B (pinned by static_assert):\n");
    check(sizeof(void*) != 8 || sizeof(egress_peer_t) <= 24, "sizeof(egress_peer_t) <= 24");
    std::printf("  sizeof(egress_peer_t) = %zu B\n", sizeof(egress_peer_t));
}

}  // namespace

int main() {
    std::printf("== #2144: the shared-frame egress module ==\n");
    test_slot_size();
    test_sample_is_latest_wins();
    test_stream_order_and_arms();
    test_one_ring_per_vertex();
    test_part_write_resumes();
    test_strikes_count_offers();
    test_sample_never_strikes();
    test_sample_and_stream_interleaved();
    test_control_lane_strikes();
    test_close_and_refusal();
    test_stalled_peer_does_not_delay_others();
    test_lane_lock_is_per_peer();
    test_close_races_senders();
    test_concurrent_handoff();
    return tr::testing::summary("egress_test");
}
