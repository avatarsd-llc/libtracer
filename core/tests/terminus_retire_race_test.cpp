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
 *   and a READ, an AWAIT, a SUBSCRIBE and a `:children[]` creation on the handle in hand.
 *   Each must be refused, and the creation must leave no child behind.
 * - **Concurrent.** `/t` is a handler vertex. One thread retires and revives it, rewriting its
 *   `:acl` after each revival; another sends `p0`'s WRITE through the router the whole time,
 *   by PAIR and by NAME. A WRITE made entirely inside one `retire` call must never reach the
 *   handler. Run under TSan (the `tsan` CI job builds every test with `-fsanitize=thread`),
 *   it is the data-race check of the lock-free registration read.
 * - **Role flip under a write** (#2115). One thread retires `/t` (its role reverts to
 *   `STORED_VALUE`) and revives it as a handler vertex; another writes `/t` through the API the
 *   whole time. A write that read the old role and stored under the new one used to take the
 *   handler leg's empty "consumed" result for a stored value and dereference it. Every write
 *   must answer success or `NOT_FOUND`, and none may crash.
 * - **Ring arm follows the write's role** (#2115). An admission filter parks a write after it
 *   read the role. Retiring `/s` and reviving it as a STREAM under a write that read
 *   STORED_VALUE must deliver the value exactly once (it used to be queued AND delivered, so
 *   the next drain delivered it again). Retiring a STREAM `/s` under a write that read STREAM,
 *   with no revive, must push nothing and create no ring on the placeholder.
 */

#include <array>
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

/** @brief A `SPEC{ type stored_value, name @p name }`, the `:children[]` creation value. */
tr::view::view_t child_spec(std::string_view name) {
    bytes_t body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, "stored_value");
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return tr::testing::make_value(out);
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
    {
        bracket_node_t n;
        const path_t append("/t:children[]");
        tr::graph::result_t<void> r{};  // a value: it must become the refusal
        bracket(n.g, n.park, [&] { r = n.g.write(*n.h, append.field(), child_spec("c"), "p0"); });
        check(!r && r.error() == status_t::PERMISSION_DENIED, "a :children[] creation is refused");
        check(!n.g.find(path_t("/t/c").key()), "no child was created under /t");
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

/** @brief A write racing a retire-and-revive answers a defined status and never crashes. */
void write_across_role_flip() {
    std::printf("a write racing a retire-and-revive answers a defined status:\n");
    graph_t g;
    const path_t at("/t");
    auto on_write = [](const tr::graph::value_t&,
                       const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> { return {}; };
    const auto revive = [&] {
        tr::graph::handlers_t h;
        h.on_write = tr::graph::thunk(on_write);
        (void)g.register_vertex(at, role_t::HANDLER, std::move(h));
    };
    revive();
    const bytes_t frame = value_byte();
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> writes{0};
    std::atomic<std::size_t> undefined{0};
    std::thread writer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const tr::graph::result_t<void> r = g.write(at, tr::testing::make_value(frame));
            writes.fetch_add(1, std::memory_order_relaxed);
            if (!r && r.error() != status_t::NOT_FOUND)
                undefined.fetch_add(1, std::memory_order_relaxed);
        }
    });
    constexpr int kCycles = 20000;
    for (int i = 0; i < kCycles; ++i) {
        const auto h = g.find(at.key());
        if (h) (void)g.retire(*h);
        revive();
    }
    stop.store(true, std::memory_order_release);
    writer.join();
    std::printf("  (%zu writes, %zu answered an undefined status)\n", writes.load(),
                undefined.load());
    check(undefined.load() == 0, "every racing write answered success or NOT_FOUND");
}

/** @brief An admission filter that holds one write inside it, after the write read the role. */
struct admit_park_t {
    std::mutex m;               /**< @brief Guards the two flags. */
    std::condition_variable cv; /**< @brief Signals each flag change. */
    bool parked = false;        /**< @brief The write is inside the filter. */
    bool released = false;      /**< @brief The write may go on. */

    /** @brief The filter: park until released, then admit unchanged. */
    tr::graph::admission_t operator()(const tr::graph::value_t&, const tr::graph::write_ctx_t&) {
        std::unique_lock lk(m);
        parked = true;
        cv.notify_all();
        cv.wait(lk, [this] { return released; });
        return std::nullopt;
    }
};

/** @brief The single payload byte of @p v, or 0 when it is not one byte. */
std::uint8_t only_byte(const tr::graph::value_t& v) {
    const tr::view::view_t flat = v.flatten();
    const std::span<const std::byte> b = flat.bytes();
    return b.size() == 1 ? std::to_integer<std::uint8_t>(b[0]) : 0;
}

/** @brief Register @p at as @p role with a filter that parks the first write in @p park. */
std::optional<vertex_handle_t> register_parking(graph_t& g, const path_t& at, role_t role,
                                                admit_park_t& park) {
    tr::graph::handlers_t h;
    h.on_admit = tr::graph::thunk(park);
    (void)g.register_vertex(at, role, std::move(h));
    return g.find(at.key());
}

/**
 * @brief Write 0x0A to @p h on a thread, run @p meanwhile while it is parked in its filter,
 *        then let it finish.
 */
tr::graph::result_t<void> write_parked(graph_t& g, vertex_handle_t h, admit_park_t& park,
                                       const std::function<void()>& meanwhile) {
    tr::graph::result_t<void> r{};
    std::thread t([&] { r = g.write(h, tr::testing::make_value({0x0A})); });
    {
        std::unique_lock lk(park.m);
        check(park.cv.wait_for(lk, 5s, [&park] { return park.parked; }),
              "the write parked inside its admission filter");
    }
    meanwhile();
    {
        const std::lock_guard lk(park.m);
        park.released = true;
    }
    park.cv.notify_all();
    t.join();
    return r;
}

/** @brief A write that read STORED_VALUE, overlapped by a revive as STREAM, delivers once. */
void stored_to_stream_delivers_once() {
    std::printf("a STORED_VALUE write overlapped by a revive as STREAM delivers once:\n");
    graph_t g;
    const path_t at("/s");
    admit_park_t park;
    const auto h = register_parking(g, at, role_t::STORED_VALUE, park);
    check(h.has_value(), "/s registered as STORED_VALUE");
    if (!h) return;
    std::size_t seen_a = 0;
    std::size_t seen_b = 0;
    auto on_value = [&](const tr::graph::value_t& v) {
        const std::uint8_t b = only_byte(v);
        seen_a += b == 0x0A ? 1 : 0;
        seen_b += b == 0x0B ? 1 : 0;
    };
    std::optional<tr::graph::subscription_t> sub;
    const tr::graph::result_t<void> r = write_parked(g, *h, park, [&] {
        check(g.retire(*h).has_value(), "/s retired under the parked write");
        (void)g.register_vertex(at, role_t::STREAM);
        // Depth above one, so a stray entry would survive into the next write's drain window.
        check(g.set_policy(*h, {.retention = tr::graph::retention_t::N, .depth = 4}).has_value(),
              "the revived STREAM keeps four entries");
        auto s = g.subscribe(at, on_value);
        check(s.has_value(), "subscribed to the revived STREAM");
        if (s) sub.emplace(std::move(*s));
    });
    check(r.has_value(), "the overlapped write succeeds under the role it read");
    check(g.write(*h, tr::testing::make_value({0x0B})).has_value(), "a second write succeeds");
    check(seen_a == 1, "the overlapped value was delivered exactly once");
    check(seen_b == 1, "the second value was delivered exactly once");
}

/**
 * @brief A malloc-backed source that counts the blocks it has out — the graph's own source,
 *        so a ring the write created, and the entry it reserved, both show up as live blocks.
 */
class live_count_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Named for the census, like every other source. */
    live_count_source_t() noexcept : block_source_t("test-live-count") {}
    /** @brief Serve from `aligned_alloc`; count the block as live. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        const std::size_t a = align < alignof(std::max_align_t) ? alignof(std::max_align_t) : align;
        const std::size_t rounded = ((bytes == 0 ? 1 : bytes) + a - 1) / a * a;
        void* const p = std::aligned_alloc(a, rounded);
        if (p != nullptr) live.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    /** @brief Sized reclaim matching @ref try_alloc. */
    void release(void* p, std::size_t /*bytes*/, std::size_t /*align*/) noexcept override {
        std::free(p);
        live.fetch_sub(1, std::memory_order_relaxed);
    }
    std::atomic<std::ptrdiff_t> live{0}; /**< @brief Blocks out right now. */
};

/**
 * @brief Retire a STREAM `/s` and write it, the write either parked across the retire (it
 *        read STREAM) or made after it (it reads the placeholder's STORED_VALUE).
 * @return The source's live blocks once both are done.
 */
std::ptrdiff_t live_after_retired_write(bool overlapped) {
    live_count_source_t src;  // MUST outlive the graph: its blocks are released into it
    std::ptrdiff_t live = 0;
    {
        graph_t g(src);
        const path_t at("/s");
        admit_park_t park;
        const auto h = register_parking(g, at, role_t::STREAM, park);
        check(h.has_value(), "/s registered as STREAM");
        if (!h) return -1;
        if (overlapped) {
            const tr::graph::result_t<void> r = write_parked(g, *h, park, [&] {
                check(g.retire(*h).has_value(), "/s retired under the parked write");
            });
            check(r.has_value(), "the overlapped write succeeds under the role it read");
        } else {
            check(g.retire(*h).has_value(), "/s retired before the write");
            park.released = true;
            check(g.write(*h, tr::testing::make_value({0x0A})).has_value(),
                  "the write after the retire succeeds");
        }
        live = src.live.load();
    }
    return live;
}

/** @brief A write that read STREAM, overlapped by a retire, pushes nothing onto a ring. */
void stream_write_across_retire_pushes_nothing() {
    std::printf("a STREAM write overlapped by a retire pushes nothing:\n");
    // The control writes the placeholder after the retire, so it never reaches a ring. A write
    // that pushed onto the retired vertex would hold more: a lazily created ring and its entry.
    const std::ptrdiff_t control = live_after_retired_write(false);
    const std::ptrdiff_t raced = live_after_retired_write(true);
    std::printf("  (live blocks: %td after the control, %td after the overlapped write)\n", control,
                raced);
    check(raced == control, "no ring was created and no entry pushed on the retired vertex");
}

}  // namespace

int main() {
    stored_to_stream_delivers_once();
    stream_write_across_retire_pushes_nothing();
    write_across_role_flip();
    bracketed_terminus_ops();
    no_terminus_mid_retire(true);
    no_terminus_mid_retire(false);
    return tr::testing::summary("terminus_retire_race");
}
