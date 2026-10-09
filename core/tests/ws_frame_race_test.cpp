/**
 * @file
 * @brief RFC-0028 §6.9 / §9 item 6 — two publishes racing onto ONE WebSocket peer arrive as
 *        two whole frames, in order: no frame is torn by another frame's bytes.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The guard slice 9 must land behind. A link that queues a frame and writes it later, from
 * whichever thread holds the writer role, shares its socket with every other frame that link
 * emits. Slice 9 changes what a queued record IS — today a copy of the whole frame in a queue
 * slot (`tr::net::tx_handoff_t`), after slice 9 a retained `value_t` plus a header — and a
 * retained-value send is one partial write away from a frame whose tail is another frame's
 * bytes. Nothing in the bench set or the host suite raced two frames onto one socket, so a
 * torn frame would have shipped green (§9 item 6). This suite is that race.
 *
 * Every frame carries its own identity (the sending thread, a per-thread sequence number,
 * its own length) and a body whose every byte is a function of that identity and its
 * offset, so the receiver can tell a whole frame from one spliced out of two. Two arms, one
 * per direction of one connection:
 *
 *  1. **client → server** through `ws_client_transport_t::send` — the enqueue-then-write
 *     queue (`stream_endpoint_t::handoff_send`, RFC-0028 §4.7). Two threads publish frames of
 *     mixed sizes, the large ones well past both socket buffers so a write is still in
 *     flight when the other thread's frame arrives and has to QUEUE behind it. The server's
 *     sink reads slowly now and then, so the queue actually fills.
 *  2. **server → client** through the server's broadcast `send` and, where the build has the
 *     bus module, a DIRECTED `peer_link(...)->send` — two doors onto the same peer socket.
 *
 * Per arm the asserts are the same: every frame that arrived is whole (length and every body
 * byte), each thread's frames arrive in the order it sent them, and every frame is accounted
 * for — received, or counted in `dropped_tx` (a full queue drops and counts by contract; it
 * never waits and never tears). The client arm also asserts the race it exists for really
 * happened: some send of one thread began AND returned inside the other thread's send, which
 * on the enqueue-then-write queue means it was queued (or refused) behind a write in flight.
 *
 * Run under TSan (`setarch $(uname -m) -R`) and ASan+UBSan as well as plain: the queue's
 * slots and the writer role cross threads.
 */

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/transport_ws.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;
using namespace std::chrono_literals;
using clock_t_ = std::chrono::steady_clock;

/** @brief Bytes of the identity header every test frame opens with. */
constexpr std::size_t kHeaderBytes = 12;
/** @brief The first header byte, so a frame that starts mid-body is recognisable at once. */
constexpr std::uint8_t kMagic = 0x5A;
/** @brief Frame sizes, cycled by sequence number: small records and ones far past both
 *         socket buffers, so small frames queue behind large writes and vice versa. */
constexpr std::array<std::size_t, 6> kSizes = {48, 1500, 70000, 300000, 17 + kHeaderBytes, 131072};
/** @brief Frames each thread sends per round. */
constexpr std::uint32_t kFramesPerThread = 96;
/** @brief Rounds per arm: more if the race has not been observed yet, never fewer. */
constexpr int kMinRounds = 3;
/** @brief Upper bound on rounds while waiting to observe the race (client arm). */
constexpr int kMaxRounds = 12;

/** @brief The body byte at @p off of frame (@p thread, @p seq) — its identity, spread out. */
[[nodiscard]] std::byte body_byte(std::uint8_t thread, std::uint32_t seq, std::size_t off) {
    return static_cast<std::byte>((thread * 131u + seq * 7u + off * 13u + (off >> 8)) & 0xFFu);
}

/** @brief Store @p v little-endian at @p p. */
void put_u32(std::byte* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}

/** @brief Read a little-endian u32 at @p p. */
[[nodiscard]] std::uint32_t get_u32(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::to_integer<std::uint32_t>(p[i]) << (8 * i);
    return v;
}

/** @brief Build frame (@p thread, @p seq): the identity header, then the patterned body. */
[[nodiscard]] std::vector<std::byte> make_frame(std::uint8_t thread, std::uint32_t seq) {
    const std::size_t len = kSizes[seq % kSizes.size()];
    std::vector<std::byte> f(len);
    f[0] = std::byte{kMagic};
    f[1] = std::byte{thread};
    f[2] = std::byte{0};
    f[3] = std::byte{0};
    put_u32(f.data() + 4, seq);
    put_u32(f.data() + 8, static_cast<std::uint32_t>(len));
    for (std::size_t i = kHeaderBytes; i < len; ++i) f[i] = body_byte(thread, seq, i);
    return f;
}

/**
 * @brief The receiving end of one arm: validates each frame as it lands, keeps only the
 *        verdict, and reads slowly now and then so the sender's queue fills.
 */
struct race_sink_t {
    std::mutex m;               /**< @brief Guards every field below. */
    std::condition_variable cv; /**< @brief Signalled on every frame. */
    std::size_t frames = 0;     /**< @brief Frames received this round. */
    std::size_t torn = 0;       /**< @brief Frames whose header or body was not their own. */
    std::size_t reordered = 0;  /**< @brief Frames that arrived behind a later one of the
                                            same thread. */
    std::array<std::int64_t, 2> last_seq{-1, -1}; /**< @brief Last seq seen, per thread. */
    std::array<std::size_t, 2> per_thread{0, 0};  /**< @brief Frames received, per thread. */

    /** @brief Reset for a new round. */
    void reset() {
        const std::lock_guard lock(m);
        frames = torn = reordered = 0;
        last_seq = {-1, -1};
        per_thread = {0, 0};
    }

    /** @brief The receiver callable (bound by address via set_receiver(F&)). */
    void operator()(std::span<const std::byte> f) {
        bool whole = f.size() >= kHeaderBytes && f[0] == std::byte{kMagic} &&
                     std::to_integer<std::uint8_t>(f[1]) < 2 && get_u32(f.data() + 8) == f.size();
        const std::uint8_t thread = whole ? std::to_integer<std::uint8_t>(f[1]) : 0;
        const std::uint32_t seq = whole ? get_u32(f.data() + 4) : 0;
        whole = whole && f.size() == kSizes[seq % kSizes.size()];
        for (std::size_t i = kHeaderBytes; whole && i < f.size(); ++i)
            whole = f[i] == body_byte(thread, seq, i);
        std::size_t n = 0;
        {
            const std::lock_guard lock(m);
            n = ++frames;
            if (!whole) {
                ++torn;
            } else {
                if (static_cast<std::int64_t>(seq) <= last_seq[thread]) ++reordered;
                last_seq[thread] = seq;
                ++per_thread[thread];
            }
        }
        cv.notify_all();
        // A slow reader every few frames: the sender's socket buffer fills, its write stays in
        // flight longer, and the other thread's frames have to queue behind it.
        if (n % 8 == 0) std::this_thread::sleep_for(300us);
    }

    /** @brief Wait until frames received + @p dropped() reach @p total, or @p timeout. */
    template <class Dropped>
    bool wait_accounted(std::size_t total, Dropped&& dropped, std::chrono::milliseconds timeout) {
        const auto deadline = clock_t_::now() + timeout;
        std::unique_lock lock(m);
        while (frames + dropped() < total) {
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout)
                return frames + dropped() >= total;
        }
        return true;
    }
};

/** @brief One send call's wall interval, ns since an arbitrary epoch. */
struct span_t {
    std::int64_t begin = 0; /**< @brief When the call was entered. */
    std::int64_t end = 0;   /**< @brief When it returned. */
};

/** @brief Now, ns since the steady clock's epoch. */
[[nodiscard]] std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now().time_since_epoch())
        .count();
}

/**
 * @brief Count the calls of one thread that began AND returned inside a call of the other.
 *
 * On the enqueue-then-write queue a call nested inside another thread's call cannot have been
 * the writer (the other thread held the role for the whole outer interval, or was itself
 * queued behind a writer that — with two threads — is the nested call's own thread, which
 * would have started first). So a nested call is a record that was QUEUED or REFUSED while a
 * write was in flight: the race this suite exists for, observed rather than assumed.
 */
[[nodiscard]] std::size_t nested_calls(const std::vector<span_t>& a, const std::vector<span_t>& b) {
    std::size_t n = 0;
    std::size_t j = 0;
    for (const span_t& inner : b) {
        while (j < a.size() && a[j].end < inner.begin) ++j;
        if (j < a.size() && a[j].begin < inner.begin && inner.end < a[j].end) ++n;
    }
    return n;
}

/**
 * @brief Two threads publish into one link concurrently; returns the nested-call count.
 *
 * @param send_a Thread 0's send door.
 * @param send_b Thread 1's send door.
 */
template <class SendA, class SendB>
std::size_t publish_round(SendA&& send_a, SendB&& send_b) {
    // Frames are built before the start line, so the two threads race on the link alone.
    std::array<std::vector<std::vector<std::byte>>, 2> frames;
    for (std::uint8_t t = 0; t < 2; ++t)
        for (std::uint32_t s = 0; s < kFramesPerThread; ++s) frames[t].push_back(make_frame(t, s));
    std::array<std::vector<span_t>, 2> spans;
    for (auto& v : spans) v.reserve(kFramesPerThread);
    std::atomic<int> ready{0};
    auto body = [&](std::uint8_t t, auto& send) {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (ready.load(std::memory_order_acquire) < 2) {
        }
        for (const std::vector<std::byte>& f : frames[t]) {
            span_t s;
            s.begin = now_ns();
            send(std::span<const std::byte>(f));
            s.end = now_ns();
            spans[t].push_back(s);
        }
    };
    std::thread ta([&] { body(0, send_a); });
    std::thread tb([&] { body(1, send_b); });
    ta.join();
    tb.join();
    return nested_calls(spans[0], spans[1]) + nested_calls(spans[1], spans[0]);
}

/** @brief Assert one round's verdict: whole, ordered, and every frame accounted for. */
void check_round(race_sink_t& sink, std::uint64_t dropped, const char* arm) {
    const std::lock_guard lock(sink.m);
    const std::size_t sent = 2u * kFramesPerThread;
    check(sink.torn == 0, "every frame that arrived is whole — no frame carries another's bytes");
    check(sink.reordered == 0, "each thread's frames arrived in the order it sent them");
    check(sink.frames + dropped == sent,
          "every frame is accounted for: received, or dropped and counted");
    check(sink.per_thread[0] > 0 && sink.per_thread[1] > 0,
          "both threads' frames reached the peer");
    std::printf("  %s: received %zu/%zu (t0=%zu t1=%zu), dropped+counted %llu, torn %zu\n", arm,
                sink.frames, sent, sink.per_thread[0], sink.per_thread[1],
                static_cast<unsigned long long>(dropped), sink.torn);
}

/**
 * @brief The application's egress store for the client arm: a heap pass-through that counts
 *        the blocks it currently has out (#1661).
 *
 * The client's masked-frame scratch is ONE block from it. Every queued record is a copy in a
 * queue slot, and a slot keeps its block once it has grown, so a link whose slots draw from
 * the injected store holds more than one block after a round that queued anything.
 */
class live_count_source_t final : public tr::mem::block_source_t {
   public:
    live_count_source_t() noexcept : tr::mem::block_source_t("race-egress") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        void* const p = ::operator new(bytes, std::align_val_t{align}, std::nothrow);
        if (p != nullptr) live.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        live.fetch_sub(1, std::memory_order_relaxed);
        ::operator delete(p, bytes, std::align_val_t{align});
    }
    std::atomic<int> live{0}; /**< @brief Blocks handed out and not yet released. */
};

/**
 * @brief Arm 1 — client → server through the enqueue-then-write queue.
 */
void test_client_queue_race() {
    std::printf("client -> server: two publishers, one queued link, frames stay whole:\n");
    live_count_source_t egress;  // outlives the client, which returns its blocks on the way out
    race_sink_t sink;  // before the transports: they join their recv threads before it dies
    tr::net::ws_server_transport_t server(0);
    check(server.ok(), "server listening");
    server.set_receiver(sink);
    tr::net::ws_client_transport_t client("127.0.0.1", server.local_port(),
                                          {.memory = {.io = &egress}});
    check(client.ok(), "client connected");
    if (!server.ok() || !client.ok()) return;

    std::size_t nested = 0;
    for (int round = 0; round < kMaxRounds && (round < kMinRounds || nested == 0); ++round) {
        sink.reset();
        const std::uint64_t dropped0 = client.dropped_tx();
        auto send = [&](std::span<const std::byte> f) { client.send(f); };
        nested += publish_round(send, send);
        auto dropped = [&] { return client.dropped_tx() - dropped0; };
        check(sink.wait_accounted(2u * kFramesPerThread, dropped, 30s),
              "the round drained: every frame arrived or was counted");
        check_round(sink, dropped(), "client->server");
    }
    std::printf("  sends queued or refused behind a write in flight: %zu\n", nested);
    check(nested > 0, "the race was exercised: a send was queued behind another's write");
    check(client.link_up(), "the connection survived every round");
    // A send nested in another's write was queued in a slot, or refused because every slot
    // was full: either way a slot holds a block. Only the scratch would be here if the
    // slots still drew from the process heap (#1661).
    check(egress.live.load(std::memory_order_relaxed) > 1,
          "the queue's slots drew from the injected egress store, beside the scratch");
}

/**
 * @brief Arm 2 — server → client: the broadcast door and the directed door onto one peer.
 */
void test_server_two_doors_race() {
    std::printf("server -> client: broadcast and directed sends onto one peer stay whole:\n");
    race_sink_t sink;
    // `max_peers = 1` (#2027): a DIRECTED send's per-record bound is the liveness window ÷ the
    // server's peer cap (#1295), and an unset cap is window ÷ kBoundedWaitMs = 100 peers, so
    // the directed door got 100 ms. A record that stalls that long mid-write closes the peer by
    // contract. The client's receiver checks every byte of frames up to 300 KB, and under TSan
    // on a loaded runner that can hold its socket full past 100 ms: the link closed, the frames
    // still in flight were lost, and the round never drained. This suite has one peer and
    // tests frame integrity, not liveness, so the cap of 1 gives each door the whole window.
    tr::net::ws_server_transport_t server(0, {.max_peers = 1, .peer_named = tr::net::kBusLinks});
    check(server.ok(), "server listening");
    tr::net::ws_client_transport_t client("127.0.0.1", server.local_port());
    check(client.ok(), "client connected");
    client.set_receiver(sink);
    if (!server.ok() || !client.ok()) return;

    // The directed door, where the build has one: the one peer's own endpoint.
    tr::net::transport_t* directed = &server;
    if constexpr (tr::net::kBusLinks) {
        std::string name;
        const auto deadline = clock_t_::now() + 2s;
        while (name.empty() && clock_t_::now() < deadline) {
            server.bus()->enumerate_peers([&](std::string_view p) { name = std::string(p); });
            if (name.empty()) std::this_thread::sleep_for(5ms);
        }
        check(!name.empty(), "the server sees its one peer");
        if (tr::net::transport_t* const l = server.bus()->peer_link(name); l != nullptr)
            directed = l;
    }

    for (int round = 0; round < kMinRounds; ++round) {
        sink.reset();
        const std::uint64_t dropped0 = server.dropped_tx();
        auto broadcast = [&](std::span<const std::byte> f) { server.send(f); };
        auto direct = [&](std::span<const std::byte> f) { directed->send(f); };
        (void)publish_round(broadcast, direct);
        auto dropped = [&] { return server.dropped_tx() - dropped0; };
        check(sink.wait_accounted(2u * kFramesPerThread, dropped, 30s),
              "the round drained: every frame arrived or was counted");
        check_round(sink, dropped(), "server->client");
    }
    check(client.link_up(), "the connection survived every round");
}

}  // namespace

int main() {
    std::printf("== RFC-0028 §6.9: two frames racing onto one WS peer arrive whole ==\n");
    test_client_queue_race();
    test_server_two_doors_race();
    return tr::testing::summary("ws_frame_race_test");
}
