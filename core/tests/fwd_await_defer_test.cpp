/**
 * @file
 * @brief ADR-0084: a remote AWAIT is answered later, from a one-shot receiver-side waiter,
 *        and never holds the receive context of the link it arrived on.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every case drives the router's real ingress (the receiver `add_child` installs) through a fake
 * link that records what the router sends back, so what is asserted is what a peer would see:
 *
 * - **the receive context returns.** The receive call for an AWAIT returns at once, sends nothing,
 *   and a READ sent after it on the SAME link is answered while the AWAIT is still pending;
 * - **on change.** A write to the awaited vertex sends the AWAIT's RESULT, carrying the value;
 * - **no receiver deadline.** The requester owns the timeout (RFC-0004 Amendment 3): with no
 *   write, the waiter stays pending past its `await_timeout` and answers nothing, until its link
 *   goes down or the router is destroyed;
 * - **the receiver pays.** The waiter is a block of the receiving link's own rx source, held
 *   while the AWAIT is pending and given back once it is answered, cancelled or torn down.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;
using tr::testing::make_value;

using namespace std::chrono_literals;

/** @brief A fake link: records every frame the router sends back over it. */
class rec_link_t final : public transport_t {
   public:
    /** @brief Record a contiguous frame. */
    void send(std::span<const std::byte> frame) override {
        const std::lock_guard lock(m_);
        sent_.emplace_back(frame.begin(), frame.end());
    }
    /** @brief Record a scatter-gather frame, flattened. */
    void send(std::span<const std::span<const std::byte>> iov) override {
        const std::lock_guard lock(m_);
        std::vector<std::byte> flat;
        for (const auto& s : iov) flat.insert(flat.end(), s.begin(), s.end());
        sent_.push_back(std::move(flat));
    }
    /** @brief Deliver @p frame inbound, as this link's receive thread would: through the
     *         receiver `add_child` installed, so the router sees its per-link context. */
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    /** @brief A copy of every frame recorded so far, in send order. */
    std::vector<std::vector<std::byte>> frames() {
        const std::lock_guard lock(m_);
        return sent_;
    }
    /** @brief How many frames were recorded. */
    std::size_t count() {
        const std::lock_guard lock(m_);
        return sent_.size();
    }
    /** @brief Poll until at least @p n frames were recorded or @p limit passes. */
    bool wait_for(std::size_t n, std::chrono::milliseconds limit) {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (count() < n) {
            if (std::chrono::steady_clock::now() > end) return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

   private:
    std::mutex m_;
    std::vector<std::vector<std::byte>> sent_;
};

/** @brief A heap-backed rx source that counts the blocks it has lent out. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : block_source_t("test_counting_rx") {}
    /** @brief Serve from the heap and count the loan. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        void* const p = tr::mem::heap_source().try_alloc(bytes, align);
        if (p != nullptr) {
            const std::lock_guard lock(m_);
            ++live_;
            largest_ = std::max(largest_, bytes);
        }
        return p;
    }
    /** @brief Give the block back and count the return. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
        const std::lock_guard lock(m_);
        --live_;
    }
    /** @brief Blocks currently lent out. */
    [[nodiscard]] long live() const {
        const std::lock_guard lock(m_);
        return live_;
    }

   private:
    mutable std::mutex m_;
    long live_ = 0;
    std::size_t largest_ = 0;
};

/** @brief An opaque `VALUE` TLV holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> raw(4);
    tr::detail::store_le<std::uint32_t>(raw, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

/** @brief The AWAIT's `await_timeout` child: a `VALUE` u64 LE of nanoseconds. */
std::vector<std::byte> b_timeout(std::chrono::nanoseconds t) {
    std::vector<std::byte> raw(8);
    tr::detail::store_le<std::uint64_t>(raw, static_cast<std::uint64_t>(t.count()));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

/** @brief `FWD{AWAIT, dst=/<target>, src=/cli, await_timeout=t}`. */
std::vector<std::byte> b_await(std::string_view target, std::chrono::nanoseconds t) {
    return b_fwd(fwd_op_t::AWAIT, b_path({target}), b_path({"cli"}), {}, b_timeout(t));
}

/** @brief What the test reads off one recorded reply. */
struct reply_t {
    std::vector<std::byte> src;         /**< @brief Reply `src` (the request's `dst`). */
    reply_kind_t kind{};                /**< @brief RESULT or ERROR. */
    std::optional<std::uint32_t> value; /**< @brief A RESULT's u32 payload. */
    std::optional<std::uint16_t> code;  /**< @brief An ERROR's registered code. */
};

/** @brief Decode a recorded `FWD{REPLY}`; nullopt when it is not one. */
std::optional<reply_t> parse_reply(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->type != type_t::FWD || dec->children.size() < 5) return std::nullopt;
    const auto& c = dec->children;
    if (c[0].payload.size() != 1 ||
        std::to_integer<std::uint8_t>(c[0].payload[0]) != std::to_underlying(fwd_op_t::REPLY))
        return std::nullopt;
    reply_t r;
    r.src = tr::wire::encode(c[2]);
    if (c[3].payload.size() != 1) return std::nullopt;
    r.kind = static_cast<reply_kind_t>(std::to_integer<std::uint8_t>(c[3].payload[0]));
    if (r.kind == reply_kind_t::RESULT) {
        if (c[4].type == type_t::VALUE && c[4].payload.size() == 4)
            r.value = tr::detail::load_le<std::uint32_t>(c[4].payload);
    } else if (c[4].type == type_t::STATUS && c[4].children.size() == 1) {
        const auto& err = c[4].children[0];
        if (err.children.size() == 1 && err.children[0].payload.size() == 2)
            r.code = tr::detail::load_le<std::uint16_t>(err.children[0].payload);
    }
    return r;
}

constexpr std::uint32_t kSeed = 0x11111111u;
constexpr std::uint32_t kOther = 0x22222222u;
constexpr std::uint32_t kChanged = 0x33333333u;

/** @brief One node: `/sink` and `/other`, one fake link `cli` on its own counting source. */
struct node_t {
    counting_source_t rx;
    graph_t graph;
    std::optional<fwd_router_t> router;
    rec_link_t link;
    vertex_handle_t sink;
    vertex_handle_t other;

    node_t()
        : sink(graph.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE)),
          other(graph.register_vertex(*path_t::parse("/other"), role_t::STORED_VALUE)) {
        router.emplace(graph);
        (void)router->add_child("cli", link, &rx);
        (void)graph.write(sink, make_value(b_value_u32(kSeed)));
        (void)graph.write(other, make_value(b_value_u32(kOther)));
    }
};

/**
 * @brief The acceptance case: an AWAIT leaves the receive context at once, and a READ sent
 *        after it on the same link is answered before the AWAIT resolves.
 */
void read_after_pending_await_is_answered_first() {
    std::printf("a READ behind a pending AWAIT on the same link is answered first:\n");
    node_t n;
    const long base = n.rx.live();

    const auto t0 = std::chrono::steady_clock::now();
    n.link.inject(b_await("sink", 10s));
    const auto held = std::chrono::steady_clock::now() - t0;
    check(held < 500ms, "  the AWAIT's receive context returned without waiting");
    check(n.link.count() == 0, "  and sent nothing yet");
    check(n.router->pending_awaits() == 1, "  one AWAIT is pending");
    check(n.rx.live() == base + 1, "  its waiter is ONE block of the receiving link's rx source");

    n.link.inject(b_fwd(fwd_op_t::READ, b_path({"other"}), b_path({"cli"})));
    check(n.link.count() == 1, "  the READ behind it was answered on the same link");
    const auto first = parse_reply(n.link.frames().at(0));
    check(first && first->kind == reply_kind_t::RESULT && first->value == kOther &&
              first->src == b_path({"other"}),
          "  and that first reply is the READ's RESULT, not the AWAIT's");
    check(n.router->pending_awaits() == 1, "  the AWAIT is still pending");

    (void)n.graph.write(n.sink, make_value(b_value_u32(kChanged)));
    check(n.link.wait_for(2, 2s), "  a write to /sink sends the AWAIT's reply");
    const auto second = parse_reply(n.link.frames().at(1));
    check(second && second->kind == reply_kind_t::RESULT && second->value == kChanged &&
              second->src == b_path({"sink"}),
          "  the AWAIT's RESULT carries the new value, addressed from /sink");
    check(n.router->pending_awaits() == 0, "  nothing is pending afterwards");
    check(n.rx.live() == base, "  and the waiter's block went back to the link's source");
}

/**
 * @brief The receiver enforces no deadline (RFC-0004 Amendment 3): a 20 ms AWAIT with no write
 *        stays pending past its `await_timeout` and answers nothing; its link going down
 *        releases it, and its block goes back to the link's source.
 */
void receiver_ignores_await_timeout() {
    std::printf("the receiver ignores await_timeout — the requester owns the deadline:\n");
    node_t n;
    const long base = n.rx.live();
    n.link.inject(b_await("sink", 20ms));
    std::this_thread::sleep_for(100ms);
    check(n.link.count() == 0, "  past its 20 ms await_timeout, nothing was sent");
    check(n.router->pending_awaits() == 1, "  and the waiter is still pending");
    (void)n.router->remove_child("cli");
    check(n.router->pending_awaits() == 0, "  remove_child releases it");
    check(n.rx.live() == base, "  and its block went back to the link's source");
}

/** @brief Many AWAITs on one link each answer exactly once, from the write to their vertex. */
void many_awaits_each_answer_once() {
    std::printf("many pending AWAITs each answer exactly once:\n");
    node_t n;
    const long base = n.rx.live();
    constexpr int kSinkWaiters = 8;
    constexpr int kOtherWaiters = 8;
    for (int i = 0; i < kSinkWaiters; ++i) n.link.inject(b_await("sink", 10s));
    for (int i = 0; i < kOtherWaiters; ++i) n.link.inject(b_await("other", 10s));
    check(n.router->pending_awaits() == kSinkWaiters + kOtherWaiters, "  all are pending");
    (void)n.graph.write(n.sink, make_value(b_value_u32(kChanged)));
    check(n.link.count() == kSinkWaiters, "  a write to /sink answers exactly the /sink waiters");
    (void)n.graph.write(n.other, make_value(b_value_u32(kOther + 1)));
    check(n.link.count() == kSinkWaiters + kOtherWaiters, "  a write to /other answers the rest");
    int sink_results = 0;
    int other_results = 0;
    for (const auto& f : n.link.frames()) {
        const auto r = parse_reply(f);
        if (r && r->kind == reply_kind_t::RESULT && r->value == kChanged) ++sink_results;
        if (r && r->kind == reply_kind_t::RESULT && r->value == kOther + 1) ++other_results;
    }
    check(sink_results == kSinkWaiters, "  one RESULT per /sink waiter, carrying its value");
    check(other_results == kOtherWaiters, "  one RESULT per /other waiter, carrying its value");
    check(n.router->pending_awaits() == 0, "  nothing is pending afterwards");
    check(n.rx.live() == base, "  every waiter's block went back to the link's source");
}

/** @brief Taking the link down releases its pending waiters without answering them. */
void link_down_releases_waiters() {
    std::printf("removing the link releases its pending AWAITs:\n");
    node_t n;
    const long base = n.rx.live();
    n.link.inject(b_await("sink", 10s));
    n.link.inject(b_await("sink", 10s));
    check(n.router->pending_awaits() == 2, "  two are pending");
    (void)n.router->remove_child("cli");
    check(n.router->pending_awaits() == 0, "  none after remove_child");
    check(n.rx.live() == base, "  and their blocks went back to the link's source");
    (void)n.graph.write(n.sink, make_value(b_value_u32(kChanged)));
    check(n.link.count() == 0, "  a later write answers nothing");
}

/** @brief Destroying the router with AWAITs pending neither hangs nor leaks. */
void teardown_with_pending_awaits() {
    std::printf("router teardown with pending AWAITs:\n");
    node_t n;
    const long base = n.rx.live();
    n.link.inject(b_await("sink", 10s));
    n.link.inject(b_await("other", 10s));
    const auto t0 = std::chrono::steady_clock::now();
    n.router.reset();
    check(std::chrono::steady_clock::now() - t0 < 2s, "  the destructor returns promptly");
    check(n.rx.live() == base, "  and every waiter block was given back");
    (void)n.graph.write(n.sink, make_value(b_value_u32(kChanged)));
    check(n.link.count() == 0, "  a write after teardown answers nothing");
}

/** @brief A writer thread races the receive thread arming waiters; each AWAIT still answers
 *         exactly once. */
void concurrent_writes_race_arming() {
    std::printf("a writer racing the arming receive thread — exactly one answer per AWAIT:\n");
    node_t n;
    constexpr std::size_t kRounds = 200;
    std::atomic<bool> armed_all{false};
    std::thread writer([&] {
        // Keep writing until every AWAIT has been answered: a waiter armed after the last
        // write would otherwise wait forever (the receiver holds no deadline).
        while (!armed_all.load() || n.link.count() < kRounds)
            (void)n.graph.write(n.sink, make_value(b_value_u32(kChanged)));
    });
    for (std::size_t i = 0; i < kRounds; ++i) n.link.inject(b_await("sink", 10s));
    armed_all.store(true);
    writer.join();
    check(n.link.count() == kRounds, "  exactly one answer per AWAIT");
    check(n.router->pending_awaits() == 0, "  nothing is left pending");
}

}  // namespace

int main() {
    read_after_pending_await_is_answered_first();
    receiver_ignores_await_timeout();
    many_awaits_each_answer_once();
    link_down_releases_waiters();
    teardown_with_pending_awaits();
    concurrent_writes_race_arming();
    return tr::testing::summary("fwd_await_defer");
}
