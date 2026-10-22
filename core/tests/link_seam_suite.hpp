/**
 * @file
 * @brief The shared link case suite (#2139): seven wire situations every link kind is run
 *        through, written against the router's receive and send seams only.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The router reaches a link through two seams and nothing else: the receive seam
 * (`transport_t::set_receiver` / `set_rope_receiver`, chosen by `delivers_ropes()` exactly as
 * `fwd_router_t::add_child` chooses it) and the send seam (`transport_t::send`), plus the
 * counters and liveness a supervisor reads (`drop_stats()`, `link_up()`). Every case below
 * asserts on those and on nothing private to a link.
 *
 * What a case needs from the far end of the wire, a link kind supplies through a
 * @ref link_rig_t: it owns the link under test and plays the PEER. The rig is where the
 * kind's framing lives (`encode`, `oversize_header`, the decode inside `peer_drain`), so a case
 * never builds a header itself: "two frames in one write" is the concatenation of two
 * `encode` results, whatever a frame looks like on that wire.
 *
 * The cases (ADR-0087 §7, the test surface for the frame-ingress and shared-frame egress
 * modules):
 *
 *  1. **partial frame**: one frame written in three pieces, the first ending inside the
 *     header; nothing is delivered before the last byte, then exactly that frame.
 *  2. **several frames per read**: three frames in one write arrive as three, in order.
 *  2b. **large then small**: an 8 KiB frame and a 5 B frame in one write arrive as two,
 *     byte-identical: the small frame's bytes read past the large body are not lost (#2140).
 *  2c. **refused then small**: an 8 KiB frame the exhausted receive source refuses is
 *     counted as one drop and skipped whole; the 5 B frame written after it
 *     arrives byte-identical once the source recovers (a refused body larger than the read
 *     scratch must not be read into the next header).
 *  3. **oversize length**: a header announcing more than the link's `max_frame` closes the
 *     link, is counted as malformed, delivers nothing, and draws NOTHING from the link's
 *     receive source.
 *  4. **fragmented message**: one message sent as three fragments arrives as one message.
 *     Only for a kind whose wire has fragments (`supports_fragments`).
 *  5. **stalled peer**: a peer that stops reading cannot hold a send: the link is sent to
 *     until its first counted shed, every send returns inside a bound, and what the peer
 *     later reads is whole frames, at least one of them.
 *  6. **peer leaving mid-send**: sends reach a present peer intact; a peer that leaves while a
 *     sender is busy releases the sender, the link reports itself down, and one later send is
 *     counted as exactly one drop.
 *
 * The lane cases (ADR-0087 §2, #2238) drive the retained send, `send(head, value, lane)`, the
 * one a subscription edge's delivery takes:
 *
 *  7. **SAMPLE is latest-wins**: a SAMPLE vertex that keeps publishing to a stalled peer has
 *     ONE frame waiting, the newest; once the peer reads it gets that frame and none of the
 *     ones it replaced, and the waiting frame keeps the value by reference, not a copy.
 *  8. **one lane per vertex**: a STREAM vertex flooding a stalled peer sheds only its own
 *     frames, keeping its newest `depth` in order; another vertex's frame queued behind the
 *     same stall still arrives.
 *
 * They need a link whose window a case can fill on demand, so a rig opts in through
 * @ref link_rig_t::lane_cases; a real socket's window is the host's.
 *
 * A kind that cannot express a case says so through its rig and the case reports N/A; it
 * never weakens the assertion for that kind.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/backend.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/value.hpp"
#include "test_support.hpp"

namespace tr::testing::link_seam {

using namespace std::chrono_literals;

/** @brief One frame's bytes, as the suite compares them. */
using bytes_t = std::vector<std::byte>;

/** @brief How long a case waits for something that should happen at once (a delivery). */
inline constexpr std::chrono::milliseconds kWait = 3000ms;

/** @brief How long a case watches for something that must NOT happen (an early delivery). */
inline constexpr std::chrono::milliseconds kQuiet = 60ms;

/**
 * @brief The `max_frame` every rig configures: small enough that "oversize" is cheap to
 *        announce, large enough that the cases' inbound frames (8 KiB at most) fit.
 */
inline constexpr std::size_t kMaxFrame = 16384;

/**
 * @brief The peer liveness window every rig configures, in ms: how long a send may wait on a
 *        peer that takes no bytes. The stalled-peer case bounds each send by a multiple of it.
 */
inline constexpr std::uint32_t kLivenessMs = 100;

/** @brief A deterministic payload of @p n bytes, distinct per @p seed. */
[[nodiscard]] inline bytes_t payload(std::size_t n, std::uint8_t seed) {
    bytes_t out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + i * 31u));
    return out;
}

/**
 * @brief A receive backend that counts its draws, so a case can assert that a refused
 *        frame cost the link nothing (ADR-0087 §1: close before anything is drawn).
 *
 * Every rig hands this to its link as `link_memory_t::rx`, the one receive source the
 * receiver pays from. It draws from the global heap and counts each grant.
 */
class counting_backend_t final : public mem::mem_backend_t {
   public:
    counting_backend_t() noexcept : mem_backend_t("link_seam_counting") {}
    /** @brief Destroyed after the link that draws from it. */
    ~counting_backend_t() = default;
    counting_backend_t(const counting_backend_t&) = delete;
    counting_backend_t& operator=(const counting_backend_t&) = delete;

    /** @brief Grants since construction. */
    [[nodiscard]] std::size_t draws() const noexcept {
        return draws_.load(std::memory_order_relaxed);
    }

    /** @brief While @p on, every draw is refused (the source is exhausted). */
    void refuse(bool on) noexcept { refuse_.store(on, std::memory_order_relaxed); }

    /** @brief Draw @p bytes from the heap and count the grant, or refuse while asked to. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refuse_.load(std::memory_order_relaxed)) return nullptr;
        draws_.fetch_add(1, std::memory_order_relaxed);
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    /** @brief Return a block @ref try_alloc handed out. */
    void release(void* p, std::size_t /*bytes*/, std::size_t align) noexcept override {
        ::operator delete(p, std::align_val_t{align}, std::nothrow);
    }

   private:
    std::atomic<std::size_t> draws_{0}; /**< @brief Grants counted. */
    std::atomic<bool> refuse_{false};   /**< @brief Set by @ref refuse. */
};

/**
 * @brief One link under test plus the peer at the far end of its wire.
 *
 * The rig owns the link, and the link draws its inbound frames from @ref rx_backend. The
 * peer half speaks the kind's framing, which is the rig's business alone: a case only ever
 * passes a payload in and gets payloads back.
 */
class link_rig_t {
   public:
    virtual ~link_rig_t() = default;

    /** @brief The link kind, for the log. */
    [[nodiscard]] virtual std::string_view name() const = 0;
    /** @brief True once the link and its peer are connected; a case fails fast otherwise. */
    [[nodiscard]] virtual bool ready() const = 0;
    /** @brief The link under test. */
    [[nodiscard]] virtual net::transport_t& link() = 0;
    /** @brief The receive source the link was given (`link_memory_t::rx`). */
    [[nodiscard]] virtual counting_backend_t& rx_backend() = 0;

    /** @brief @p body as one whole message on this kind's wire, peer to link. */
    [[nodiscard]] virtual bytes_t encode(std::span<const std::byte> body) const = 0;
    /** @brief True when this kind's wire can split one message into fragments. */
    [[nodiscard]] virtual bool supports_fragments() const = 0;
    /** @brief @p body as @p pieces fragments of one message (call only if
     *         @ref supports_fragments). */
    [[nodiscard]] virtual bytes_t encode_fragments(std::span<const std::byte> body,
                                                   std::size_t pieces) const = 0;
    /** @brief A header announcing a body of `kMaxFrame + 1` bytes, with no body after it. */
    [[nodiscard]] virtual bytes_t oversize_header() const = 0;

    /** @brief Put @p bytes on the wire toward the link, as ONE write. */
    virtual void peer_write(std::span<const std::byte> bytes) = 0;
    /** @brief The peer stops reading what the link sends. */
    virtual void peer_stall() = 0;
    /**
     * @brief The peer reads what the link sent, for up to @p budget or until the wire closes,
     *        and decodes it.
     * @param whole Receives every complete message, in order.
     * @return True when every byte read decoded into whole messages, or the wire was closed
     *         behind a trailing partial (a link may condemn a peer mid-record); false when the
     *         bytes do not frame (a torn or interleaved record on an open wire).
     */
    [[nodiscard]] virtual bool peer_drain(std::chrono::milliseconds budget,
                                          std::vector<bytes_t>& whole) = 0;
    /** @brief The peer leaves: its end of the wire closes. */
    virtual void peer_leave() = 0;
    /** @brief True once the link closed toward its peer (a server answers per peer). */
    [[nodiscard]] virtual bool link_closed() { return !link().link_up(); }
    /** @brief False for a kind whose egress the cases cannot drive point to point. */
    [[nodiscard]] virtual bool egress_cases() const { return true; }
    /** @brief True for a kind whose peer window fills deterministically (the lane cases). */
    [[nodiscard]] virtual bool lane_cases() const { return false; }
};

/** @brief A factory for a fresh rig: each case gets its own link, since several end it. */
using rig_factory_t = std::unique_ptr<link_rig_t> (*)();

/**
 * @brief The receiver a case installs: records each delivered message's bytes.
 *
 * Bound by address through the seam's callable overloads. Deliveries land on the link's own
 * receive context, so the record is guarded and a case waits on it.
 */
struct frame_sink_t {
    std::mutex m;                /**< @brief Guards @ref frames. */
    std::condition_variable cv;  /**< @brief Signalled on each delivery. */
    std::vector<bytes_t> frames; /**< @brief One entry per delivered message, in order. */

    /** @brief The borrowed-span receiver. */
    void operator()(std::span<const std::byte> f) { record(bytes_t(f.begin(), f.end())); }
    /** @brief The owning receiver: the message is flattened for comparison. */
    void operator()(view::rope_t r) {
        const view::view_t flat = r.flatten();
        const std::span<const std::byte> b = flat.bytes();
        record(bytes_t(b.begin(), b.end()));
    }
    /** @brief Store one message and wake a waiting case. */
    void record(bytes_t f) {
        {
            const std::lock_guard lock(m);
            frames.push_back(std::move(f));
        }
        cv.notify_all();
    }
    /** @brief Wait until at least @p n messages have arrived, or @p budget passes. */
    [[nodiscard]] bool wait_for(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, budget, [&] { return frames.size() >= n; });
    }
    /** @brief A copy of what has arrived so far. */
    [[nodiscard]] std::vector<bytes_t> snapshot() {
        const std::lock_guard lock(m);
        return frames;
    }
};

/**
 * @brief Install @p sink on the rig's link the way the router does, then start delivery.
 *
 * `fwd_router_t::add_child` installs the owning receiver on a link that `delivers_ropes()`
 * and the borrowed one otherwise; the two-phase bring-up then calls `start_receiving()`.
 */
inline void attach(link_rig_t& rig, frame_sink_t& sink) {
    net::transport_t& l = rig.link();
    if (l.delivers_ropes()) {
        l.set_rope_receiver(sink);
    } else {
        l.set_receiver(sink);
    }
    l.start_receiving();
}

/** @brief Poll @p probe until it holds or @p budget passes (test-side wall clock only). */
template <typename Probe>
[[nodiscard]] bool eventually(Probe probe, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (probe()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return probe();
}

/** @brief Print a case header naming the kind. */
inline void banner(const link_rig_t& rig, const char* what) {
    std::printf("[%.*s] %s\n", static_cast<int>(rig.name().size()), rig.name().data(), what);
}

/** @brief Case 1: a frame split across three writes is held, then delivered once. */
inline void case_partial_frame(rig_factory_t make) {
    frame_sink_t sink;
    const std::unique_ptr<link_rig_t> rig = make();
    banner(*rig, "partial frame");
    check(rig->ready(), "the rig came up");
    if (!rig->ready()) return;
    attach(*rig, sink);

    const bytes_t body = payload(300, 1);
    const bytes_t wire = rig->encode(body);
    const std::span<const std::byte> all(wire);
    // The first piece ends inside the header; the last is the final body byte.
    rig->peer_write(all.first(1));
    std::this_thread::sleep_for(kQuiet / 3);
    rig->peer_write(all.subspan(1, wire.size() - 2));
    check(!sink.wait_for(1, kQuiet), "nothing is delivered while the frame is incomplete");
    rig->peer_write(all.last(1));
    check(sink.wait_for(1, kWait), "the frame is delivered once its last byte arrives");
    std::this_thread::sleep_for(kQuiet);
    const std::vector<bytes_t> got = sink.snapshot();
    check(got.size() == 1 && got[0] == body, "exactly one delivery, byte-identical");
}

/** @brief Case 2: three frames in one write arrive as three, in order. */
inline void case_several_frames_per_read(rig_factory_t make) {
    frame_sink_t sink;
    const std::unique_ptr<link_rig_t> rig = make();
    banner(*rig, "several frames per read");
    check(rig->ready(), "the rig came up");
    if (!rig->ready()) return;
    attach(*rig, sink);

    const bytes_t a = payload(5, 10), b = payload(200, 20), c = payload(1, 30);
    bytes_t wire;
    for (const bytes_t* f : {&a, &b, &c}) {
        const bytes_t one = rig->encode(*f);
        wire.insert(wire.end(), one.begin(), one.end());
    }
    rig->peer_write(wire);
    check(sink.wait_for(3, kWait), "all three frames are delivered");
    const std::vector<bytes_t> got = sink.snapshot();
    check(got.size() == 3 && got[0] == a && got[1] == b && got[2] == c,
          "as three separate frames, in order, byte-identical");
}

/**
 * @brief Case 2b: a large frame and a small one in one write arrive as two, byte-identical.
 *
 * A link that receives a large body straight into its segment and reads the next header
 * after it must keep whatever of the small frame came with that read (#2140).
 */
inline void case_large_then_small(rig_factory_t make) {
    frame_sink_t sink;
    const std::unique_ptr<link_rig_t> rig = make();
    banner(*rig, "large then small");
    check(rig->ready(), "the rig came up");
    if (!rig->ready()) return;
    attach(*rig, sink);

    const bytes_t big = payload(8192, 40), small = payload(5, 50);
    bytes_t wire = rig->encode(big);
    const bytes_t tail = rig->encode(small);
    wire.insert(wire.end(), tail.begin(), tail.end());
    rig->peer_write(wire);
    check(sink.wait_for(2, kWait), "both frames are delivered");
    std::this_thread::sleep_for(kQuiet);
    const std::vector<bytes_t> got = sink.snapshot();
    check(got.size() == 2 && got[0] == big && got[1] == small,
          "as two frames, in order, byte-identical");
}

/**
 * @brief Case 2c: a frame the receive source refuses is dropped whole, and the stream stays
 *        in step: the small frame behind it is delivered intact.
 */
inline void case_refused_then_small(rig_factory_t make) {
    frame_sink_t sink;
    const std::unique_ptr<link_rig_t> rig = make();
    banner(*rig, "refused then small");
    check(rig->ready(), "the rig came up");
    if (!rig->ready()) return;
    attach(*rig, sink);

    net::transport_t& l = rig->link();
    const std::uint64_t dropped_before = l.drop_stats().dropped_rx;
    const bytes_t big = payload(8192, 60), small = payload(5, 70);
    rig->rx_backend().refuse(true);
    rig->peer_write(rig->encode(big));
    check(eventually([&] { return l.drop_stats().dropped_rx > dropped_before; }, kWait),
          "the refused frame is counted as dropped");
    rig->rx_backend().refuse(false);
    rig->peer_write(rig->encode(small));
    check(sink.wait_for(1, kWait), "the frame behind it is delivered");
    std::this_thread::sleep_for(kQuiet);
    const std::vector<bytes_t> got = sink.snapshot();
    check(got.size() == 1 && got[0] == small, "alone and byte-identical");
    check(!rig->link_closed(), "and the link stays up");
}

/** @brief Case 3: an oversize length closes the link before anything is drawn. */
inline void case_oversize_length(rig_factory_t make) {
    frame_sink_t sink;
    const std::unique_ptr<link_rig_t> rig = make();
    banner(*rig, "oversize length");
    check(rig->ready(), "the rig came up");
    if (!rig->ready()) return;
    attach(*rig, sink);

    net::transport_t& l = rig->link();
    const std::uint64_t malformed_before = l.drop_stats().malformed_rx;
    const std::size_t draws_before = rig->rx_backend().draws();
    rig->peer_write(rig->oversize_header());
    check(eventually([&] { return l.drop_stats().malformed_rx > malformed_before; }, kWait),
          "the announced length is counted as malformed");
    check(eventually([&] { return rig->link_closed(); }, kWait), "and the link is closed");
    check(rig->rx_backend().draws() == draws_before,
          "nothing was drawn from the link's receive source for it");
    check(sink.snapshot().empty(), "and nothing was delivered");
}

/** @brief Case 4: a message sent as three fragments arrives as one. */
inline void case_fragmented_message(rig_factory_t make) {
    frame_sink_t sink;
    const std::unique_ptr<link_rig_t> rig = make();
    banner(*rig, "fragmented message");
    if (!rig->supports_fragments()) {
        check(true, "this kind's wire has no fragments: N/A");
        return;
    }
    check(rig->ready(), "the rig came up");
    if (!rig->ready()) return;
    attach(*rig, sink);

    const bytes_t body = payload(900, 40);
    const bytes_t after = payload(7, 50);
    bytes_t wire = rig->encode_fragments(body, 3);
    const bytes_t tail = rig->encode(after);
    wire.insert(wire.end(), tail.begin(), tail.end());
    rig->peer_write(wire);
    check(sink.wait_for(2, kWait), "the message and the frame after it are delivered");
    const std::vector<bytes_t> got = sink.snapshot();
    check(got.size() == 2 && got[0] == body, "the fragments arrive as ONE message, intact");
    check(got.size() == 2 && got[1] == after, "and the next frame is framed correctly after it");
}

/**
 * @brief Case 5: a peer that stops reading holds no send, the shed is counted, and what
 *        reaches the peer is whole frames.
 */
inline void case_stalled_peer(rig_factory_t make) {
    frame_sink_t sink;
    const std::unique_ptr<link_rig_t> rig = make();
    banner(*rig, "stalled peer");
    if (!rig->egress_cases()) {
        check(true, "this kind's egress is not point to point: N/A");
        return;
    }
    check(rig->ready(), "the rig came up");
    if (!rig->ready()) return;
    attach(*rig, sink);

    net::transport_t& l = rig->link();
    rig->peer_stall();
    // Send until the first counted shed, up to a cap: the window's size is the host's (a
    // socket's send buffer autotunes), so the case finds it rather than assuming it.
    constexpr std::size_t kMaxFrames = 512;
    const bytes_t big = payload(256u * 1024u, 60);
    const std::uint64_t dropped_before = l.drop_stats().dropped_tx;
    std::chrono::steady_clock::duration slowest{};
    for (std::size_t i = 0; i < kMaxFrames && l.drop_stats().dropped_tx == dropped_before; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        l.send(big);
        const auto took = std::chrono::steady_clock::now() - t0;
        if (took > slowest) slowest = took;
    }
    // The precondition, reported on its own so it is never read as a broken link.
    check(l.drop_stats().dropped_tx > dropped_before,
          "the peer's window filled on this host (a shed was counted within the cap)");
    // A send waits at most about one liveness window on this peer; 20 windows is slack for a
    // loaded sanitizer runner, and still far short of "blocked until the peer reads".
    const auto bound = std::chrono::milliseconds(kLivenessMs) * 20;
    check(slowest < bound, "no send was held by the stalled peer beyond the liveness bound");

    std::vector<bytes_t> whole;
    const bool framed = rig->peer_drain(1000ms, whole);
    check(!whole.empty(), "the frames that fit before the stall reached the peer");
    check(framed, "what the peer reads afterwards is whole frames, never a torn record");
    bool intact = true;
    for (const bytes_t& f : whole) intact = intact && f == big;
    check(intact, "and each frame it reads is byte-identical to what was sent");
}

/**
 * @brief Case 6: a peer that leaves while a sender is busy releases the sender, the link
 *        goes down, and a later send is counted.
 */
inline void case_peer_leaves_mid_send(rig_factory_t make) {
    frame_sink_t sink;
    const std::unique_ptr<link_rig_t> rig = make();
    banner(*rig, "peer leaving mid-send");
    if (!rig->egress_cases()) {
        check(true, "this kind's egress is not point to point: N/A");
        return;
    }
    check(rig->ready(), "the rig came up");
    if (!rig->ready()) return;
    attach(*rig, sink);

    net::transport_t& l = rig->link();
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> sent{0};
    std::atomic<bool> done{false};
    const bytes_t body = payload(16u * 1024u, 70);
    std::thread sender([&] {
        while (!stop.load(std::memory_order_relaxed) && sent.load() < 4096) {
            l.send(body);
            sent.fetch_add(1);
        }
        done.store(true);
    });
    check(eventually([&] { return sent.load() >= 4; }, kWait), "the sender is under way");
    std::vector<bytes_t> before;
    (void)rig->peer_drain(200ms,
                          before);  // a trailing partial is expected: the sender is mid-record
    bool intact = !before.empty();
    for (const bytes_t& f : before) intact = intact && f == body;
    check(intact, "while the peer is there, the sends reach it intact");
    rig->peer_leave();
    check(eventually([&] { return !l.link_up(); }, kWait), "the link reports the peer gone");
    stop.store(true);
    // Meaningful on a link whose send can wait on its socket; the fake's never waits.
    check(eventually([&] { return done.load(); }, kWait), "the busy sender is released");
    sender.join();

    // Empty whatever the link still holds toward the peer, so the next send would fit any
    // window and can only be shed because the peer left.
    std::vector<bytes_t> residue;
    (void)rig->peer_drain(0ms, residue);
    const std::uint64_t dropped_before = l.drop_stats().dropped_tx;
    l.send(payload(1, 71));
    check(l.drop_stats().dropped_tx == dropped_before + 1,
          "a send after the peer left is counted as exactly one drop, not lost silently");
}

/** @brief The frames a lane case sends: big enough that a few fill a rig's window. */
inline constexpr std::size_t kLaneFrame = 256u * 1024u;

/** @brief One retained send of a @p seed frame: a one-byte head naming it, then the value. */
inline void lane_send(net::transport_t& l, const graph::value_t& v, std::uint8_t seed,
                      const graph::delivery_lane_t& lane) {
    const std::byte head[1] = {std::byte{seed}};
    const std::span<const std::byte> parts[1] = {head};
    l.send(std::span<const std::span<const std::byte>>(parts), v, lane);
}

/** @brief A value of @p seed's payload, and its frame as the peer reads it (head ++ value). */
struct lane_value_t {
    graph::value_ref_t value; /**< @brief The published value. */
    bytes_t wire;             /**< @brief The frame body the peer should read. */
};

/** @brief Mint the value a @p seed frame carries. */
[[nodiscard]] inline lane_value_t lane_value(std::uint8_t seed) {
    const bytes_t body = payload(kLaneFrame, seed);
    lane_value_t out{graph::value_ref_t::copy(body), bytes_t{std::byte{seed}}};
    out.wire.insert(out.wire.end(), body.begin(), body.end());
    return out;
}

/** @brief The peer reads until a read brings nothing new; true when every read framed. */
[[nodiscard]] inline bool drain_all(link_rig_t& rig, std::vector<bytes_t>& whole) {
    bool framed = true;
    for (std::size_t before = SIZE_MAX; before != whole.size();) {
        before = whole.size();
        framed = rig.peer_drain(kQuiet, whole) && framed;
    }
    return framed;
}

/** @brief Case 7: a SAMPLE vertex's frames to a stalled peer are latest-wins. */
inline void case_sample_latest_wins(rig_factory_t make) {
    frame_sink_t sink;
    const std::unique_ptr<link_rig_t> rig = make();
    banner(*rig, "SAMPLE is latest-wins");
    if (!rig->lane_cases()) {
        check(true, "this kind's window is not filled on demand: N/A");
        return;
    }
    check(rig->ready(), "the rig came up");
    if (!rig->ready()) return;
    attach(*rig, sink);

    net::transport_t& l = rig->link();
    int vertex = 0;
    const graph::delivery_lane_t lane{.key = &vertex, .depth = 1};
    constexpr std::uint8_t kSends = 24;
    std::vector<lane_value_t> sent;
    for (std::uint8_t i = 0; i < kSends; ++i) sent.push_back(lane_value(i));
    rig->peer_stall();
    for (std::uint8_t i = 0; i < kSends; ++i) lane_send(l, *sent[i].value, i, lane);
    check(sent.back().value->use_count() > 1,
          "the newest frame waits holding the value by reference, not a copy of it");

    std::vector<bytes_t> got;
    check(drain_all(*rig, got), "the peer reads whole frames");
    // Before the stall, consecutive frames; after it, at most the one that was on the wire
    // and then only the newest: every frame in between was replaced while it waited.
    std::size_t run = 0;
    while (run < got.size() && got[run] == sent[run].wire) ++run;
    const bool newest_last = !got.empty() && got.back() == sent.back().wire;
    check(run < kSends - 1 && newest_last && got.size() <= run + 2,
          "the peer gets the newest value and none of the frames it replaced");
    check(sent.back().value->use_count() == 1, "once written, the frame lets the value go");
}

/** @brief Case 8: one STREAM vertex's flood never sheds another vertex's frame. */
inline void case_one_lane_per_vertex(rig_factory_t make) {
    frame_sink_t sink;
    const std::unique_ptr<link_rig_t> rig = make();
    banner(*rig, "one lane per vertex");
    if (!rig->lane_cases()) {
        check(true, "this kind's window is not filled on demand: N/A");
        return;
    }
    check(rig->ready(), "the rig came up");
    if (!rig->ready()) return;
    attach(*rig, sink);

    net::transport_t& l = rig->link();
    int vertex_a = 0, vertex_b = 0;
    constexpr std::uint16_t kDepth = 4;
    const graph::delivery_lane_t a{.key = &vertex_a, .depth = kDepth};
    const graph::delivery_lane_t b{.key = &vertex_b, .depth = kDepth};
    constexpr std::uint8_t kFlood = 32;
    std::vector<lane_value_t> flood;
    for (std::uint8_t i = 0; i < kFlood; ++i) flood.push_back(lane_value(i));
    const lane_value_t other = lane_value(200);
    rig->peer_stall();
    const std::uint64_t dropped_before = l.drop_stats().dropped_tx;
    // A fills the window and puts a frame on the wire; B's one frame waits behind it, then A
    // floods far past its depth.
    for (std::uint8_t i = 0; i < 8; ++i) lane_send(l, *flood[i].value, i, a);
    lane_send(l, *other.value, 200, b);
    for (std::uint8_t i = 8; i < kFlood; ++i) lane_send(l, *flood[i].value, i, a);
    check(l.drop_stats().dropped_tx > dropped_before, "A's flood past its depth was counted");

    std::vector<bytes_t> got;
    check(drain_all(*rig, got), "the peer reads whole frames");
    std::size_t from_b = 0;
    std::vector<std::uint8_t> from_a;
    for (const bytes_t& f : got) {
        if (f == other.wire) {
            ++from_b;
            continue;
        }
        const auto seed = std::to_integer<std::uint8_t>(f.empty() ? std::byte{0xFF} : f[0]);
        if (seed < kFlood && f == flood[seed].wire) from_a.push_back(seed);
    }
    check(from_b == 1, "B's frame arrives: A's flood shed none of it");
    check(from_a.size() + from_b == got.size(), "and every frame read is whole and known");
    check(std::is_sorted(from_a.begin(), from_a.end()), "A's frames arrive in order");
    const bool newest =
        from_a.size() >= kDepth &&
        std::equal(
            from_a.end() - kDepth, from_a.end(),
            std::vector<std::uint8_t>{kFlood - 4, kFlood - 3, kFlood - 2, kFlood - 1}.begin());
    // Best-effort STREAM sheds the oldest waiting: what A's lane held at the end is its newest
    // `depth`, and nothing older survived past them.
    check(newest, "A's lane keeps its newest frames, up to its depth");
    std::size_t before_newest = 0;
    for (std::uint8_t s : from_a) before_newest += s < kFlood - kDepth && s >= 8 ? 1 : 0;
    check(before_newest == 0, "and the frames it displaced never reach the peer");
}

/** @brief Run the lane cases against one link kind. */
inline void run_lane_cases(rig_factory_t make) {
    case_sample_latest_wins(make);
    case_one_lane_per_vertex(make);
}

/** @brief Run every case against one link kind. */
inline void run_suite(rig_factory_t make) {
    case_partial_frame(make);
    case_several_frames_per_read(make);
    case_large_then_small(make);
    case_refused_then_small(make);
    case_oversize_length(make);
    case_fragmented_message(make);
    case_stalled_peer(make);
    case_peer_leaves_mid_send(make);
    run_lane_cases(make);
}

}  // namespace tr::testing::link_seam
