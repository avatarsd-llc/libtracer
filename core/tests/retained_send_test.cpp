/**
 * @file
 * @brief RFC-0028 §6.9 / §8.2 (#1620): the retained send. A queued link keeps the value (one
 *        reference) and copies only the frame's head.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Pinned here:
 * 1. `tx_handoff_t` with a retained record: the slot holds one reference to the value while
 *    the record is queued, hands it to the writer, and drops it at the writer's next step.
 * 2. `transport_t::send(head, value)`'s default lowering is byte-exact: `head ++ links` as
 *    one gathered frame, for a link that overrides nothing but the gather.
 * 3. The TCP dial link under two concurrent senders and a slow reader. Every frame arrives
 *    whole, in per-thread order, and accounted for. The retained path is proven taken (a send
 *    returned with the queue still holding its value). Once the link drains, every value is
 *    back to its caller's one reference.
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/tracer.hpp"
#include "libtracer/tx_handoff.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::value_ref_t;
using tr::graph::value_t;
using tr::net::tcp_transport_t;
using tr::net::tx_handoff_t;
using tr::testing::check;
using span_t = std::span<const std::byte>;

/** @brief A published (sourced) value over a copy of @p bytes. */
[[nodiscard]] value_ref_t published(std::span<const std::byte> bytes) {
    return value_ref_t::adopt(value_t::make_copy(bytes, tr::mem::heap_source()));
}

/** @brief @p n bytes of a pattern seeded by @p seed. */
[[nodiscard]] std::vector<std::byte> pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = std::byte(static_cast<std::uint8_t>(seed + i * 13u));
    return v;
}

/** @brief Vector 1: the handoff queue holds a retained record's value exactly while queued. */
void test_handoff_retains() {
    std::printf("tx_handoff_t keeps a retained record's value while it is queued:\n");
    tx_handoff_t q(4, tr::mem::heap_source(), 64);
    const std::vector<std::byte> body = pattern(9000, 3);
    const value_ref_t v = published(body);
    const std::array<std::byte, 3> head{std::byte{0xA1}, std::byte{0xA2}, std::byte{0xA3}};
    const auto fill = [&head](tr::mem::block_array_t<std::byte>& slot) -> std::size_t {
        if (!slot.reserve(head.size())) return 0;
        std::memcpy(slot.data(), head.data(), head.size());
        return head.size();
    };

    check(q.admit(fill, v.get()) == tx_handoff_t::admit_t::WRITE,
          "no writer in flight: the caller becomes the writer (nothing kept)");
    check(v->use_count() == 1, "the writer's own record is not retained");
    check(q.admit(fill, v.get()) == tx_handoff_t::admit_t::QUEUED, "a second record queues");
    check(v->use_count() == 2, "the queued record holds one reference");

    const tx_handoff_t::record_t rec = q.next();
    check(static_cast<bool>(rec) && rec.value == v.get(), "the writer gets the value back");
    check(rec.bytes.size() == head.size() && rec.size() == head.size() + body.size(),
          "the record is the copied head plus the retained value's length");
    check(v->use_count() == 2, "the value stays held while the writer is on it");
    check(!q.next(), "the drain ends");
    check(v->use_count() == 1, "the writer's next step released it");

    // A record whose head fill refuses is refused whole, and keeps nothing.
    check(q.admit(fill, v.get()) == tx_handoff_t::admit_t::WRITE, "writer again");
    const auto refuse = [](tr::mem::block_array_t<std::byte>&) -> std::size_t { return 0; };
    check(q.admit(refuse, v.get()) == tx_handoff_t::admit_t::REFUSED && v->use_count() == 1,
          "a refused fill keeps no reference");
    check(!q.next(), "and nothing was queued");
}

/** @brief A link that overrides only the gather: records what the default lowering hands it. */
class gather_probe_t final : public tr::net::transport_t {
   public:
    std::vector<std::byte> last; /**< @brief The last gathered frame, concatenated. */
    std::size_t parts = 0;       /**< @brief How many spans it came as. */

    void send(std::span<const std::byte> frame) override {
        last.assign(frame.begin(), frame.end());
    }
    void send(std::span<const span_t> iov) override {
        last.clear();
        parts = iov.size();
        for (const span_t& s : iov) last.insert(last.end(), s.begin(), s.end());
    }
    using tr::net::transport_t::send;
};

/** @brief Vector 2: the base lowering of the retained send is `head ++ links`, byte-exact. */
void test_default_lowering() {
    std::printf("transport_t::send(head, value) lowers to one gathered frame:\n");
    gather_probe_t probe;
    const std::vector<std::byte> a = pattern(40, 1);
    const std::vector<std::byte> b = pattern(7000, 2);
    tr::view::rope_t r;
    r.append(tr::view::view_t::over(tr::view::heap_alloc(a.size())));
    r.append(tr::view::view_t::over(tr::view::heap_alloc(b.size())));
    std::memcpy(const_cast<std::byte*>(r.links()[0].bytes().data()), a.data(), a.size());
    std::memcpy(const_cast<std::byte*>(r.links()[1].bytes().data()), b.data(), b.size());
    const value_ref_t v = value_ref_t::composed(std::move(r));

    const std::array<std::byte, 2> h0{std::byte{0x0F}, std::byte{0x10}};
    const std::array<std::byte, 1> h1{std::byte{0x99}};
    const std::array<span_t, 3> head{span_t(h0), span_t(h1), span_t{}};
    probe.send(std::span<const span_t>(head), *v);

    std::vector<std::byte> want(h0.begin(), h0.end());
    want.insert(want.end(), h1.begin(), h1.end());
    want.insert(want.end(), a.begin(), a.end());
    want.insert(want.end(), b.begin(), b.end());
    check(probe.parts == 5, "three head spans plus two links, gathered as five parts");
    check(probe.last == want, "the frame is head ++ links, byte-exact");
}

/** @brief The small head each race frame carries: magic, thread, seq, body length. */
constexpr std::size_t kHeadBytes = 10;

/** @brief Encode a race frame's head. */
[[nodiscard]] std::array<std::byte, kHeadBytes> race_head(std::uint8_t thread, std::uint32_t seq,
                                                          std::uint32_t len) {
    std::array<std::byte, kHeadBytes> h{};
    h[0] = std::byte{0x5A};
    h[1] = std::byte{thread};
    for (int i = 0; i < 4; ++i) h[2 + i] = std::byte(static_cast<std::uint8_t>(seq >> (8 * i)));
    for (int i = 0; i < 4; ++i) h[6 + i] = std::byte(static_cast<std::uint8_t>(len >> (8 * i)));
    return h;
}

/** @brief The receiving side of the race: checks every frame whole and in order. */
struct race_sink_t {
    std::mutex m;                                 /**< @brief Guards every field below. */
    std::size_t frames = 0;                       /**< @brief Frames received. */
    std::size_t torn = 0;                         /**< @brief Frames that did not parse whole. */
    std::size_t reordered = 0;                    /**< @brief Per-thread sequence regressions. */
    std::array<std::int64_t, 2> last_seq{-1, -1}; /**< @brief Last seq seen per thread. */
    std::atomic<std::size_t> slow{0};             /**< @brief Arrivals, to pace the reader. */

    void on_frame(span_t f) {
        // A slow reader, now and then, so the socket fills and the other sender queues.
        if ((slow.fetch_add(1, std::memory_order_relaxed) & 7u) == 0)
            std::this_thread::sleep_for(300us);
        const std::lock_guard lock(m);
        ++frames;
        if (f.size() < kHeadBytes || f[0] != std::byte{0x5A} ||
            std::to_integer<std::uint8_t>(f[1]) > 1) {
            ++torn;
            return;
        }
        const std::uint8_t t = std::to_integer<std::uint8_t>(f[1]);
        std::uint32_t seq = 0;
        std::uint32_t len = 0;
        for (int i = 0; i < 4; ++i) seq |= std::to_integer<std::uint32_t>(f[2 + i]) << (8 * i);
        for (int i = 0; i < 4; ++i) len |= std::to_integer<std::uint32_t>(f[6 + i]) << (8 * i);
        if (f.size() != kHeadBytes + len) {
            ++torn;
            return;
        }
        const std::uint8_t seed = static_cast<std::uint8_t>(t * 31u + seq);
        for (std::size_t i = 0; i < len; ++i) {
            if (f[kHeadBytes + i] != std::byte(static_cast<std::uint8_t>(seed + i * 13u))) {
                ++torn;
                return;
            }
        }
        if (static_cast<std::int64_t>(seq) <= last_seq[t]) ++reordered;
        last_seq[t] = seq;
    }
};

/** @brief Vector 3: two senders race retained frames onto one TCP dial link. */
void test_tcp_retained_race() {
    std::printf("TCP dial link: two threads race retained frames onto one peer:\n");
    constexpr std::array<std::size_t, 6> kSizes{48, 1500, 70000, 300000, 29, 131072};
    constexpr std::uint32_t kPerThread = 96;
    std::size_t retained_queued = 0;
    for (int round = 0; round < 12; ++round) {
        race_sink_t sink;
        tcp_transport_t listener(std::uint16_t{0});
        auto rx = [&sink](span_t f) { sink.on_frame(f); };
        listener.set_receiver(rx);
        tcp_transport_t dialer("127.0.0.1", listener.local_port());

        // The values, minted up front and held by the test for the whole round, so their
        // reference counts at the end say whether the link let go of each one.
        std::array<std::vector<value_ref_t>, 2> values;
        for (std::uint8_t t = 0; t < 2; ++t) {
            for (std::uint32_t s = 0; s < kPerThread; ++s) {
                const std::size_t n = kSizes[(s + t) % kSizes.size()];
                values[t].push_back(published(pattern(n, static_cast<std::uint8_t>(t * 31u + s))));
            }
        }
        std::atomic<std::size_t> queued{0};
        const auto sender = [&](std::uint8_t t) {
            for (std::uint32_t s = 0; s < kPerThread; ++s) {
                const value_t& v = *values[t][s];
                const auto h = race_head(t, s, static_cast<std::uint32_t>(v.total_length()));
                const std::array<span_t, 1> head{span_t(h)};
                dialer.send(std::span<const span_t>(head), v);
                // Returned with the link still holding it: the retained record is queued.
                if (v.use_count() > 1) queued.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread a(sender, std::uint8_t{0});
        std::thread b(sender, std::uint8_t{1});
        a.join();
        b.join();

        const std::size_t sent = 2u * kPerThread;
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                const std::lock_guard lock(sink.m);
                if (sink.frames + dialer.dropped_tx() >= sent) break;
            }
            std::this_thread::sleep_for(2ms);
        }
        bool released = true;
        for (int spin = 0; spin < 1000; ++spin) {
            released = true;
            for (const auto& per : values)
                for (const value_ref_t& v : per) released = released && v->use_count() == 1;
            if (released) break;
            std::this_thread::sleep_for(2ms);
        }
        {
            const std::lock_guard lock(sink.m);
            check(sink.torn == 0, "no frame torn (each arrived whole and byte-exact)");
            check(sink.reordered == 0, "each thread's frames arrived in order");
            check(sink.frames + dialer.dropped_tx() == sent,
                  "every frame is accounted for: received, or dropped and counted");
            check(sink.last_seq[0] >= 0 && sink.last_seq[1] >= 0, "both threads delivered");
        }
        check(released, "the link let go of every value once it drained");
        retained_queued += queued.load(std::memory_order_relaxed);
        if (round >= 2 && retained_queued > 0) break;
    }
    check(retained_queued > 0, "the retained path was taken (a send returned while queued)");
}

}  // namespace

int main() {
    test_handoff_retains();
    test_default_lowering();
    test_tcp_retained_race();
    return tr::testing::summary("retained_send");
}
