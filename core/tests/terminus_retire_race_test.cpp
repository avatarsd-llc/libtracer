/**
 * @file
 * @brief The terminus access check rechecks vertex registration, so an operation whose check
 *        overlaps a retire is refused.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The terminus of a forwarded operation is found by a registration check (`deref_vertex_slot`
 * for the PAIR spelling, `find` for the NAME one) that returns before the operation's own
 * access check runs. The access check tests the vertex's registration once more after it has
 * read the ACEs, and a retire marks the vertex unregistered before it reverts the vertex's
 * state, so an operation whose check overlaps a retire is refused.
 *
 * `/t` carries an `:acl` that admits only `admin`, so `p0` is refused there (each case's
 * control). Two cases:
 *
 * - **Bracketed.** The subject resolver runs inside the access check, before any ACE is read,
 *   so it is the seam that holds an operation there: `p0`'s first resolution parks while the
 *   main thread retires `/t`, then the operation finishes its check. Every terminus operation
 *   is bracketed this way: a WRITE through the production `fwd_router_t` by PAIR and by NAME,
 *   and a READ, an AWAIT and a SUBSCRIBE on the handle in hand. Each must be refused.
 * - **Concurrent.** `/t` is a handler vertex. One thread retires and revives it, rewriting its
 *   `:acl` after each revival; another sends `p0`'s WRITE through the router the whole time,
 *   by PAIR and by NAME. A WRITE made entirely inside one `retire` call must never reach the
 *   handler. Run under TSan (the `tsan` CI job builds every test with `-fsanitize=thread`),
 *   it is the data-race check of the lock-free registration read.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_pair.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "pair_body.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::testing::check;
using tr::wire::opt_t;
using tr::wire::path_pair_t;
using tr::wire::type_t;
using bytes_t = std::vector<std::byte>;

/** @brief The test resolver (ADR-0018): the caller context IS the subject token. */
std::expected<tr::graph::subject_token_t, tr::wire::err_t> caller_is_subject(void*,
                                                                             std::string_view c) {
    const auto* p = reinterpret_cast<const std::byte*>(c.data());
    return tr::graph::subject_token_t(p, p + c.size());
}

/** @brief An `:acl` that admits `admin` to everything and nobody else. */
bytes_t admin_only_acl() {
    const auto* p = reinterpret_cast<const std::byte*>("admin");
    const tr::graph::ace_t ace{
        .type = tr::graph::ace_type_t::ALLOW,
        .flags = tr::graph::kAceInherit,
        .subject = bytes_t(p, p + 5),
        .access_mask = 0xFFFFFFFFu,
        .expires_ns = 0,
    };
    return tr::graph::encode_acl(std::span<const tr::graph::ace_t>(&ace, 1));
}

/** @brief A `VALUE` TLV carrying one byte. */
bytes_t value_byte() {
    const std::byte b{0x5A};
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief A `FWD{WRITE}` of one byte to `/t`, spelled by PAIR @p e or by NAME. */
bytes_t write_to_t(bool pair, path_pair_t e) {
    bytes_t body;
    if (pair) {
        tr::testing::emit_path_pair(body, e);
    } else {
        (void)tr::wire::emit_path_segment(body, "t");
    }
    bytes_t dst;
    tr::wire::emit_tlv(dst, type_t::PATH, opt_t{}, body);
    return tr::testing::b_fwd(fwd_op_t::WRITE, dst, tr::testing::b_path({}), {}, value_byte());
}

/** @brief Holds `p0`'s first subject resolution until the test lets it go. */
struct park_t {
    std::mutex m;               /**< @brief Guards the three flags. */
    std::condition_variable cv; /**< @brief Signals each flag change. */
    bool armed = false;         /**< @brief The next resolution of `p0` parks. */
    bool parked = false;        /**< @brief A resolution is parked. */
    bool released = false;      /**< @brief The parked resolution may go on. */
};

/** @brief The subject lookup: the caller IS the subject, and `p0` may park first. */
std::expected<void, tr::wire::err_t> park_then_name(void* ctx, std::string_view caller,
                                                    tr::mem::bytes_t& out) {
    auto& p = *static_cast<park_t*>(ctx);
    {
        std::unique_lock lk(p.m);
        if (p.armed && caller == "p0") {
            p.armed = false;
            p.parked = true;
            p.cv.notify_all();
            p.cv.wait(lk, [&p] { return p.released; });
        }
    }
    if (!out.append(reinterpret_cast<const std::byte*>(caller.data()), caller.size()))
        return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
    return {};
}

/**
 * @brief Run @p op on its own thread, retire `/t` while its access check is parked, then let
 *        it finish. @p op must ask for the gate as `p0`.
 */
void bracket(graph_t& g, park_t& park, const std::function<void()>& op) {
    std::thread t(op);
    {
        std::unique_lock lk(park.m);
        check(park.cv.wait_for(lk, 5s, [&park] { return park.parked; }),
              "the operation parked inside its access check");
    }
    const auto h = g.find(path_t("/t").key());
    check(h && g.retire(*h).has_value(), "/t retired while the operation was parked");
    {
        const std::lock_guard lk(park.m);
        park.released = true;
    }
    park.cv.notify_all();
    t.join();
}

/** @brief A graph whose `/t` admits only `admin`, with the parking subject lookup. */
struct bracket_node_t {
    bracket_node_t() {
        auto hooks = g.hooks();
        hooks.subject_lookup = {park_then_name, &park};
        g.set_hooks(hooks);
        (void)g.register_vertex(path_t("/t"), role_t::STORED_VALUE);
        (void)g.write(path_t("/t:acl"), tr::testing::make_value(admin_only_acl()));
        h = g.find(path_t("/t").key());
        const auto control = h ? g.read(*h, "p0") : std::unexpected(status_t::NOT_FOUND);
        check(!control && control.error() == status_t::PERMISSION_DENIED,
              "p0 is refused at /t before the retire (the control)");
        park.armed = true;
    }
    park_t park;
    graph_t g;
    std::optional<vertex_handle_t> h; /**< @brief `/t`, held across its retire. */
};

/** @brief Each terminus operation parked inside its access check across a retire is refused. */
void bracketed_terminus_ops() {
    std::printf("a terminus op whose access check spans a retire is refused:\n");
    for (const bool pair : {true, false}) {
        bracket_node_t n;
        fwd_router_t router{n.g};
        const bytes_t frame = write_to_t(pair, n.g.vertex_slot(*n.h).value_or(path_pair_t{}));
        const std::uint64_t denied0 = n.g.delivery_drops().denied;
        bracket(n.g, n.park, [&] { router.on_frame("p0", frame); });
        check(n.g.delivery_drops().denied == denied0 + 1,
              pair ? "a PAIR WRITE is refused at the terminus"
                   : "a NAME WRITE is refused at the terminus");
    }
    {
        bracket_node_t n;
        tr::graph::result_t<tr::graph::value_ref_t> r{};  // a value: it must become the refusal
        bracket(n.g, n.park, [&] { r = n.g.read(*n.h, "p0"); });
        check(!r && r.error() == status_t::PERMISSION_DENIED, "a READ is refused");
    }
    {
        bracket_node_t n;
        tr::graph::result_t<tr::graph::value_ref_t> r{};  // a value: it must become the refusal
        bracket(n.g, n.park, [&] { r = n.g.await(*n.h, 1ms, "p0"); });
        check(!r && r.error() == status_t::PERMISSION_DENIED, "an AWAIT is refused");
    }
    {
        bracket_node_t n;
        tr::graph::result_t<void> r{};  // a value: it must become the refusal
        bracket(n.g, n.park, [&] {
            r = n.g.subscribe_wire(*n.h, tr::testing::make_value({0x04, 0x40, 0x00, 0x00}),
                                   tr::testing::make_value({0x06, 0x00, 0x00, 0x00}), "p0",
                                   tr::view::view_t{}, "p0");
        });
        check(!r && r.error() == status_t::PERMISSION_DENIED, "a SUBSCRIBE is refused");
    }
}

/** @brief A terminus WRITE made inside a retire never reaches the handler. */
void no_terminus_mid_retire(bool pair) {
    std::printf("a %s terminus WRITE inside a retire never reaches the handler:\n",
                pair ? "PAIR" : "NAME");
    graph_t g;
    auto hooks = g.hooks();
    hooks.subject_resolver = {caller_is_subject, nullptr};
    g.set_hooks(hooks);
    fwd_router_t router{g};
    const path_t at("/t");
    const path_t acl_at("/t:acl");
    const bytes_t acl = admin_only_acl();
    std::atomic<std::size_t> calls{0};
    auto on_write = [&calls](const tr::graph::value_t&,
                             const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        calls.fetch_add(1, std::memory_order_acq_rel);
        return {};
    };
    const auto revive = [&] {
        tr::graph::handlers_t h;
        h.on_write = tr::graph::thunk(on_write);
        (void)g.register_vertex(at, role_t::HANDLER, std::move(h));
        (void)g.write(acl_at, tr::testing::make_value(acl));
    };
    revive();
    const auto v0 = g.find(at.key());
    const std::uint32_t index = v0 ? g.vertex_slot(*v0)->index : 0;
    router.on_frame("p0", write_to_t(pair, g.vertex_slot_at(index).value_or(path_pair_t{})));
    check(calls.load() == 0, "p0 is refused at the terminus (the control)");
    // `seq` is odd while a `retire` call is in progress.
    std::atomic<std::uint32_t> seq{0};
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> bad{0};
    std::atomic<std::size_t> inside{0};
    std::thread sender([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto e = g.vertex_slot_at(index);
            if (!e) continue;
            const bytes_t frame = write_to_t(pair, *e);
            const std::uint32_t s0 = seq.load(std::memory_order_acquire);
            const std::size_t n0 = calls.load(std::memory_order_acquire);
            router.on_frame("p0", frame);
            const std::size_t n1 = calls.load(std::memory_order_acquire);
            const std::uint32_t s1 = seq.load(std::memory_order_acquire);
            if (s0 != s1 || (s0 & 1u) == 0) continue;
            inside.fetch_add(1, std::memory_order_relaxed);
            if (n1 != n0) bad.fetch_add(1, std::memory_order_relaxed);
        }
    });
    constexpr int kCycles = 20000;
    for (int i = 0; i < kCycles; ++i) {
        const auto h = g.find(at.key());
        if (!h) break;
        seq.fetch_add(1, std::memory_order_acq_rel);  // odd: a retire is in progress
        (void)g.retire(*h);
        seq.fetch_add(1, std::memory_order_acq_rel);  // even: it has returned
        revive();
    }
    stop.store(true, std::memory_order_release);
    sender.join();
    std::printf("  (%zu ops made inside a retire, %zu reached the handler)\n", inside.load(),
                bad.load());
    check(bad.load() == 0, "no terminus op inside a retire reached the handler");
}

}  // namespace

int main() {
    bracketed_terminus_ops();
    no_terminus_mid_retire(true);
    no_terminus_mid_retire(false);
    return tr::testing::summary("terminus_retire_race");
}
