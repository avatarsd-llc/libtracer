/**
 * @file
 * @brief A PAIR hop takes no graph lock, and stays correct while vertices are being added.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #1939 (RFC-0029 §13.2 S6): resolving `(index, generation)` is a bounds check, a slot load
 * and a generation compare over an append-only vertex index whose size is published
 * atomically (the ADR-0063 pattern), so the PAIR arm of a hop never waits on the graph's map
 * lock. Two cases, both driven at the wire seam through the production `fwd_router_t`:
 *
 * - **No lock.** The graph draws its tables from a source that can be told to BLOCK inside
 *   `try_alloc`. A registration is started on a second thread with the source armed, so it
 *   stops inside an allocation it makes under the unique map lock. While it is stopped, a
 *   PAIR-spelled WRITE through a point-to-point connection vertex must still forward. A hop
 *   that took the map lock, even shared, would wait for the registration and the case fails
 *   on its deadline.
 * - **Concurrent growth.** One thread registers vertices across many index chunks and
 *   directory regrowths while another drives PAIR hops; every hop forwards. Run under TSan
 *   (the `tsan` CI job builds every test with `-fsanitize=thread`), this is the data-race
 *   check of the lock-free index.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_pair.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"
#include "pair_body.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::testing::check;
using tr::wire::opt_t;
using tr::wire::path_pair_t;
using tr::wire::type_t;
using bytes_t = std::vector<std::byte>;

/** @brief A heap source that, once armed, parks the next `try_alloc` until released. */
class parking_source_t final : public tr::mem::block_source_t {
   public:
    parking_source_t() noexcept : tr::mem::block_source_t("parking") {}
    /** @brief Serve from the heap; when armed, park first (once). */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (armed_.exchange(false)) {
            std::unique_lock lk(m_);
            parked_ = true;
            cv_.notify_all();
            cv_.wait(lk, [this] { return released_; });
        }
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    /** @brief Return to the heap. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }
    /** @brief Park the next allocation. */
    void arm() { armed_.store(true); }
    /** @brief Wait up to @p d for an allocation to be parked. */
    bool wait_parked(std::chrono::milliseconds d) {
        std::unique_lock lk(m_);
        return cv_.wait_for(lk, d, [this] { return parked_; });
    }
    /** @brief Let the parked allocation (and every later one) through. */
    void release_all() {
        const std::lock_guard lk(m_);
        released_ = true;
        cv_.notify_all();
    }

   private:
    std::atomic<bool> armed_{false};
    std::mutex m_;
    std::condition_variable cv_;
    bool parked_ = false;
    bool released_ = false;
};

/** @brief A counting point-to-point link; the inbound one also injects frames. */
struct link_t : tr::net::transport_t {
    std::atomic<std::size_t> n{0}; /**< @brief Frames sent through this link. */
    void send(std::span<const std::byte>) override { n.fetch_add(1); }
    /** @brief Deliver @p frame as one this link received. */
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
};

/** @brief A WRITE whose `dst` is the PAIR @p head followed by one NAME. */
bytes_t pair_write(path_pair_t head) {
    bytes_t body;
    tr::testing::emit_path_pair(body, head);
    (void)tr::wire::emit_path_segment(body, "leaf");
    bytes_t dst;
    tr::wire::emit_tlv(dst, type_t::PATH, opt_t{}, body);
    const std::byte b{0x5A};
    bytes_t value;
    tr::wire::emit_tlv(value, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return tr::testing::b_fwd(fwd_op_t::WRITE, dst, tr::testing::b_path({}), {}, value);
}

/** @brief A node: the out link's connection vertex, the out and in children, and the PAIR. */
struct node_t {
    explicit node_t(tr::mem::block_source_t& src) : g(src) {
        (void)g.register_vertex(path_t("/net/tcp/out"), role_t::STORED_VALUE);
        check(router.add_child("net/tcp/out", out), "out mounted");
        check(router.add_child("net/tcp/in", in), "in mounted");
        const auto v = g.find(path_t("/net/tcp/out").key());
        const auto slot = v ? g.vertex_slot(*v) : std::nullopt;
        check(slot.has_value(), "the out connection vertex has a slot");
        frame = pair_write(slot.value_or(path_pair_t{}));
    }
    ~node_t() {
        (void)router.remove_child("net/tcp/out");
        (void)router.remove_child("net/tcp/in");
    }
    graph_t g;
    fwd_router_t router{g};
    link_t out;
    link_t in;
    bytes_t frame;
};

/** @brief A PAIR hop completes while a registration holds the unique map lock. */
void hop_takes_no_lock() {
    std::printf("a PAIR hop forwards while a registration holds the map lock:\n");
    parking_source_t src;
    node_t n(src);
    // Park the next table allocation, and start a registration that will make one under the
    // unique map lock. A fresh vertex is one allocation at least, so the first one parks.
    src.arm();
    std::thread writer([&] {
        for (int i = 0; i < 64; ++i)
            (void)n.g.register_vertex(path_t("/grow/v" + std::to_string(i)), role_t::STORED_VALUE);
    });
    check(src.wait_parked(5s), "the registration parked inside an allocation");
    std::atomic<bool> done{false};
    std::thread hop([&] {
        n.in.inject(n.frame);
        done.store(true);
    });
    bool forwarded = false;
    for (int i = 0; i < 400 && !forwarded; ++i) {
        forwarded = done.load() && n.out.n.load() == 1;
        if (!forwarded) std::this_thread::sleep_for(5ms);
    }
    check(forwarded, "the PAIR hop forwarded without waiting for the registration");
    src.release_all();
    hop.join();
    writer.join();
}

/** @brief PAIR hops stay correct while the vertex index grows underneath them. */
void hops_during_growth() {
    std::printf("PAIR hops forward while vertices are added concurrently:\n");
    parking_source_t src;  // never armed: a plain heap source
    node_t n(src);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        // 2000 vertices: 31 chunks of 64 slots and four directory regrowths.
        for (int i = 0; i < 2000; ++i)
            (void)n.g.register_vertex(path_t("/grow/v" + std::to_string(i)), role_t::STORED_VALUE);
        stop.store(true);
    });
    std::size_t sent = 0;
    while (!stop.load() || sent < 1000) {
        n.in.inject(n.frame);
        ++sent;
    }
    writer.join();
    check(n.out.n.load() == sent, "every PAIR hop forwarded during the growth");
    check(n.g.vertex_slot_count() > 2000, "the index grew past 2000 slots");
}

}  // namespace

int main() {
    hop_takes_no_lock();
    hops_during_growth();
    return tr::testing::summary("pair_hop_lock_free");
}
