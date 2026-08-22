/**
 * @file
 * @brief L4 graph-runtime tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * M3a: path parse/canonicalize, the three vertex roles
 * (stored-value, stream, handler), read/write/await, lock-free LKV clone-on-read,
 * and a multithreaded stress over a shared vertex. M3b: subscribe (callback +
 * spec-faithful target), field-write (:settings.*, :subscribers[]) + unsubscribe,
 * :schema, and delivery-terminates-at-target (ADR-0051: no chained relay, cycles
 * loop-free by construction). The stress is the
 * verification M2 deferred: under TSan it proves the lock-free LKV + atomic
 * segment refcounts race-free, and under ASan+UBSan leak/UB-free.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <latch>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::delivery_mode_t;
using tr::graph::delivery_policy_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;

/** @brief A subscription that REQUESTS the transient-local latch (RFC-0022 §3.A bit 5) —
 *         what used to be the producer's `settings.durability == 1`. */
constexpr delivery_policy_t kDurableSub{delivery_policy_t::kDurabilityRequest};

using tr::testing::check;
using tr::testing::make_value;

void test_path_parse() {
    std::printf("path_t parse / canonicalize / field tail:\n");
    const auto p = path_t::parse("/sensor/temp");
    check(p.has_value(), "valid path parses");
    // Canonical PATH payload (RFC-0018, packed): 06 'sensor' 04 'temp' = 12 bytes — the
    // address string's own length, one byte per '/' included.
    check(p && p->key().size() == 12, "canonical key is 12 bytes for /sensor/temp");
    check(p && p->segment_count() == 2, "two segments");

    // Trailing slash canonicalizes to the same key.
    const auto p2 = path_t::parse("/sensor/temp/");
    check(p2 && p2->key().size() == p->key().size() &&
              std::memcmp(p2->key().data(), p->key().data(), p->key().size()) == 0,
          "trailing slash canonicalizes to the same key");

    check(!path_t::parse("sensor/temp").has_value(), "unrooted path rejected");
    check(!path_t::parse("/sensor//temp").has_value(), "empty segment // rejected");
    check(path_t::parse("/").has_value() && path_t::parse("/")->segment_count() == 0,
          "root '/' is zero segments");

    const auto f = path_t::parse("/sensor/temp:settings.history_keep_last");
    check(f && f->field().steps.size() == 2 && f->field().steps[0].name == "settings" &&
              f->field().steps[1].name == "history_keep_last",
          "field tail :settings.history_keep_last parses to two steps");
    const auto app = path_t::parse("/s:subscribers[]");
    check(app && app->field().steps.size() == 1 && app->field().steps[0].append,
          "subscribers[] parses as an append step");
    const auto idx = path_t::parse("/s:subscribers[3]");
    check(idx && idx->field().steps[0].indexed && !idx->field().steps[0].append &&
              idx->field().steps[0].index == 3,
          "subscribers[3] parses with index 3");
}

void test_stored_value() {
    std::printf("Stored-value vertex (read/write, clone-on-read):\n");
    graph_t g;
    const auto path = path_t::parse("/sensor/temp");
    const auto v = g.register_vertex(*path, role_t::STORED_VALUE);  // infallible (ADR-0056)

    check(!g.read(v).has_value() && g.read(v).error() == status_t::NOT_FOUND,
          "read before any write => NotFound");

    auto w = g.write(v, make_value({0xAA, 0xBB, 0xCC}));
    check(w.has_value(), "write succeeds");

    auto r = g.read(v);
    check(r.has_value() && (*r)->only().bytes().size() == 3, "read returns the written value");
    check(r && std::to_integer<int>((*r)->only().bytes()[0]) == 0xAA, "value byte 0 == 0xAA");
    // The read hands back a REFERENCE to the published rope rather than a clone of it, so the
    // segment's refcount does not move at all: the LKV's own reference is the only one. This
    // asserted `== 2` while a read copied the rope out, cloning one segment_ptr_t per link —
    // the per-link contended RMW that returning the reference removes.
    check(r && (*r)->only().owner.use_count() == 1,
          "read shares the published segment (use_count stays 1), it does not clone it");

    check(!g.try_register_vertex(*path, role_t::STORED_VALUE).has_value(),
          "re-register same path fails");
    check(g.try_register_vertex(*path, role_t::STORED_VALUE).error() == status_t::PATH_IN_USE,
          "duplicate registration reports PathInUse");
}

/**
 * @brief The LKV publish and the seq bump that publishes nothing, through the handle (#1300).
 *
 * Restates what the bare-`vertex_t` storage suite asserted on `store()` / `note_write()`
 * directly before `vertex_t::store()` became `graph_t`'s alone: `assign` is the verb that
 * publishes a last-known-value, and a HANDLER write is the verb that moves the readiness
 * cursor without publishing one.
 */
void test_assign_lkv_and_seq_bump() {
    std::printf("assign — the LKV publish, and a seq bump that publishes nothing:\n");
    graph_t g;
    tr::graph::vertex_handle_t v = g.register_vertex(path_t("/lkv/cell"), role_t::STORED_VALUE);
    check(!g.read(v).has_value() && g.read(v).error() == status_t::NOT_FOUND,
          "a never-assigned vertex holds no LKV");
    check(g.assign(v, make_value({0x42})).has_value(), "assign is admitted");
    auto r = g.read(v);
    check(r.has_value() && std::to_integer<int>((*r)->only().bytes()[0]) == 0x42,
          "assign publishes the last-known-value");
    (void)g.assign(v, make_value({0x43}));
    r = g.read(v);
    check(r.has_value() && std::to_integer<int>((*r)->only().bytes()[0]) == 0x43,
          "a later assign wins (last-writer-wins)");

    // The "bump without storing" half: a HANDLER whose on_write consumes and whose on_read is
    // unset. The write bumps the await cursor — a blocked awaiter WAKES — and nothing lands,
    // so the wake answers NOT_FOUND rather than TIMEOUT. That is `note_write` observed through
    // the only public seam that can see it.
    tr::graph::handlers_t h;
    h.on_write = [](const tr::view::rope_t&,
                    const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> { return {}; };
    tr::graph::vertex_handle_t hv =
        g.register_vertex(path_t("/lkv/sink"), role_t::HANDLER, std::move(h));
    // The #1418 edge-trigger again, on the HANDLER arm: `note_write` bumps the same sequence
    // `await` snapshots on entry, so a write that beats this thread into `await` is missed and
    // the await times out — which would report TIMEOUT where the assertion demands NOT_FOUND.
    // Re-arm from the writer under a deadline, exactly as `test_await` below does.
    std::atomic<bool> awaited{false};
    std::thread writer([&] {
        // A pure BACKSTOP against an `await` that never returns, not a synchronization window:
        // in every reachable case the latch is set within (scheduling delay + the 2 s await
        // timeout), because a broken wake path still makes `await` return TIMEOUT and release
        // this loop. So the deadline never fires in a real failure — the assertion below does,
        // at ~2 s — and making it generous costs nothing while removing the one way this fix
        // could reintroduce the very defect it removes: giving up re-arming while the awaiting
        // thread is merely starved (observed reachable under 16-way oversubscription).
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!awaited.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)g.write(hv, make_value({0x99}));
            std::this_thread::sleep_for(2ms);
        }
    });
    const auto woke = g.await(hv, 2s);
    awaited.store(true, std::memory_order_release);
    writer.join();
    check(!woke.has_value() && woke.error() == status_t::NOT_FOUND,
          "a handler write bumps the await cursor without storing (a wake, not a timeout)");
}

void test_stream() {
    std::printf("Stream vertex (bounded history ring):\n");
    graph_t g;
    const auto path = path_t::parse("/log/events");
    tr::graph::vertex_handle_t v = g.register_vertex(*path, role_t::STREAM);
    g.set_history_depth(v, 3);

    for (std::uint8_t i = 1; i <= 5; ++i) (void)g.write(v, make_value({i}));

    auto hist = g.history(v);
    check(hist.has_value() && hist->size() == 3, "history bounded to keep_last = 3");
    check(hist && std::to_integer<int>((*hist)[0].only().bytes()[0]) == 3,
          "oldest kept is the 3rd write");
    check(hist && std::to_integer<int>((*hist)[2].only().bytes()[0]) == 5,
          "newest kept is the 5th write");
    auto latest = g.read(v);
    check(latest && std::to_integer<int>((*latest)->only().bytes()[0]) == 5,
          "read returns the latest (5)");
}

/**
 * @brief RFC-0008 §E through the handle: the STREAM drain cursor, mirrored on `graph_t` (#1300).
 *
 * The bare-`vertex_t` twin of this suite is gone — `vertex_t::store()` is `graph_t`'s alone now
 * — so this is where §E's "a stream is a queue, not a coalesce" lives. Assertion for assertion
 * the same ground, driven through `vertex_handle_t` instead of a raw vertex.
 *
 * Load-bearing: the appends here are `assign`, never `write`. `write` on a STREAM runs
 * `deliver_current`, which DRAINS and advances the cursor even with zero subscribers, so a
 * drain test written on `write` reads 0 everywhere and looks like a broken mirror. `assign` is
 * the RFC-0008 §A state-only verb, and the only one that leaves the cursor for a test to see.
 */
void test_stream_drain_cursor() {
    std::printf("STREAM drain cursor through the handle (RFC-0008 §E):\n");
    graph_t g;
    tr::graph::vertex_handle_t v = g.register_vertex(path_t("/log/drain"), role_t::STREAM);
    g.set_history_depth(v, 3);
    for (std::uint8_t b = 1; b <= 5; ++b) (void)g.assign(v, make_value({b}));

    const auto hist = g.history(v);
    check(hist.has_value() && hist->size() == 3, "the ring keeps exactly the owner-declared depth");
    check(hist && hist->size() == 3 && std::to_integer<int>((*hist)[0].only().bytes()[0]) == 3 &&
              std::to_integer<int>((*hist)[2].only().bytes()[0]) == 5,
          "trim drops the oldest entries (ring holds 3,4,5)");

    std::vector<std::shared_ptr<const tr::view::rope_t>> batch;
    const auto d1 = g.drain_unflushed(v, batch);
    check(d1.has_value() && *d1 == 3, "5 unflushed appends drain to the 3 ring survivors");
    check(batch.size() == 3 && std::to_integer<int>(batch[0]->only().bytes()[0]) == 3 &&
              std::to_integer<int>(batch[2]->only().bytes()[0]) == 5,
          "drain is in append order");
    const auto d2 = g.drain_unflushed(v, batch);
    check(d2.has_value() && *d2 == 0, "a drained stream has nothing more to drain");

    (void)g.assign(v, make_value({6}));
    const auto d3 = g.drain_unflushed(v, batch);
    check(d3.has_value() && *d3 == 1 && std::to_integer<int>(batch[0]->only().bytes()[0]) == 6,
          "the next append drains alone (a queue, not a coalesce)");

    (void)g.assign(v, make_value({7}));
    check(g.mark_flushed(v).has_value(), "mark_flushed is accepted on a STREAM");
    const auto d4 = g.drain_unflushed(v, batch);
    check(d4.has_value() && *d4 == 0, "mark_flushed advances the cursor without draining");

    // The role gate on both verbs: a non-stream role owns no ring, so it owns no cursor —
    // the same disposition `history` already gives.
    tr::graph::vertex_handle_t sv = g.register_vertex(path_t("/log/cell"), role_t::STORED_VALUE);
    const auto nd = g.drain_unflushed(sv, batch);
    check(!nd.has_value() && nd.error() == status_t::SCHEMA_NOT_FOUND,
          "drain_unflushed on a non-stream role is SchemaNotFound");
    const auto nf = g.mark_flushed(sv);
    check(!nf.has_value() && nf.error() == status_t::SCHEMA_NOT_FOUND,
          "mark_flushed on a non-stream role is SchemaNotFound");
}

void test_handler() {
    std::printf("Handler vertex (user on_read / on_write seam):\n");
    graph_t g;
    const auto path = path_t::parse("/compute/answer");
    auto written = std::make_shared<std::vector<std::byte>>();
    tr::graph::handlers_t h;
    h.on_read = [] { return make_value({0x2A}); };  // always 42
    h.on_write = [written](const tr::view::rope_t& in,
                           const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        const auto b = in.only().bytes();
        written->assign(b.begin(), b.end());
        return {};
    };
    tr::graph::vertex_handle_t v = g.register_vertex(*path, role_t::HANDLER, std::move(h));

    auto r = g.read(v);
    check(r && std::to_integer<int>((*r)->only().bytes()[0]) == 0x2A,
          "on_read supplies the value (42)");
    (void)g.write(v, make_value({0x99}));
    check(written->size() == 1 && std::to_integer<int>((*written)[0]) == 0x99,
          "on_write receives the written bytes");
}

void test_await() {
    std::printf("await (blocks until next write; times out otherwise):\n");
    graph_t g;
    tr::graph::vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    // Same edge-trigger as #1418: `await` snapshots `write_seq_` on entry, so a write that
    // lands before THIS thread gets into `await` is never observed and the await runs to its
    // full timeout. The `sleep_for(40ms)` this replaced was a guess that the awaiter would be
    // parked by then. Re-arm from the writer instead, until the awaiter is out; the writer's
    // deadline exceeds the 2 s await timeout, so a broken wake path still fails the assertion
    // below rather than hanging the suite. Every re-arm writes the same 0x7E, so the value
    // assertion is unchanged.
    std::atomic<bool> awaited{false};
    std::thread writer([&] {
        const auto deadline = std::chrono::steady_clock::now() + 30s;  // backstop; see above
        while (!awaited.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)g.write(v, make_value({0x7E}));
            std::this_thread::sleep_for(2ms);
        }
    });
    auto r = g.await(v, 2s);
    awaited.store(true, std::memory_order_release);
    writer.join();
    check(r.has_value() && std::to_integer<int>((*r)->only().bytes()[0]) == 0x7E,
          "await wakes on a concurrent write and delivers it");

    tr::graph::vertex_handle_t idle =
        g.register_vertex(path_t("/sensor/idle"), role_t::STORED_VALUE);
    auto t = g.await(idle, 20ms);
    check(!t.has_value() && t.error() == status_t::TIMEOUT, "await times out with no write");
}

void test_concurrent_stress() {
    std::printf("Concurrent stress (lock-free LKV under contention):\n");
    graph_t g;
    tr::graph::vertex_handle_t v = g.register_vertex(path_t("/stress/v"), role_t::STORED_VALUE);

    constexpr int kWriters = 4;
    constexpr int kReaders = 4;
    constexpr int kWritesEach = 20000;
    constexpr int kReadsEach = 20000;

    // A REAL rendezvous, not a spin (#1425): what this arm wants is for all eight threads to
    // enter the loop together, and `std::latch` gives exactly that — every arrival blocks in
    // the kernel until the count-down releases them all. The `std::atomic<bool>` flag it
    // replaces was polled in a loop with no yield, so the threads spawned first burned their
    // slices fighting the very thread that still had to spawn the rest.
    std::latch go{1};
    std::atomic<long> reads_done{0};
    std::vector<std::thread> threads;

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            go.wait();
            std::array<std::byte, 8> buf{};
            buf[0] = static_cast<std::byte>(w);
            for (int i = 0; i < kWritesEach; ++i) {
                buf[1] = static_cast<std::byte>(i & 0xFF);
                (void)g.write(v, make_value(buf));
            }
        });
    }
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&] {
            go.wait();
            for (int i = 0; i < kReadsEach; ++i) {
                auto rr = g.read(v);
                // Any non-empty read must be a well-formed 8-byte value (no torn read).
                if (rr && (*rr)->only().bytes().size() != 8)
                    return;  // leaves reads_done short => FAIL
            }
            reads_done.fetch_add(kReadsEach, std::memory_order_relaxed);
        });
    }

    go.count_down();
    for (auto& t : threads) t.join();

    check(reads_done.load() == static_cast<long>(kReaders) * kReadsEach,
          "all reads completed without crash under contention");
    auto fin = g.read(v);
    check(fin.has_value() && (*fin)->only().bytes().size() == 8,
          "final read returns a valid 8-byte value");
}

/** @brief A VALUE TLV wrapping `payload` (01 00 <len> <payload>), as an owned view_t. */
tr::view::view_t value_tlv(std::span<const std::byte> payload) {
    tr::wire::tlv_t t{.type = tr::wire::type_t::VALUE, .payload = payload};
    return make_value(tr::wire::encode(t));
}

/** @brief A SUBSCRIBER TLV naming a single-segment target path, as an owned view_t. */
tr::view::view_t subscriber_tlv(std::string_view target_segment) {
    std::vector<std::byte> packed_body;
    (void)tr::wire::emit_path_segment(packed_body, target_segment);
    tr::wire::tlv_t path{.type = tr::wire::type_t::PATH,
                         .payload = packed_body};  // packed, PL = 0 (RFC-0018)
    tr::wire::tlv_t sub{.type = tr::wire::type_t::SUBSCRIBER};
    sub.opt.pl = true;
    sub.children.push_back(path);
    return make_value(tr::wire::encode(sub));
}

void test_subscribe_callback() {
    std::printf("subscribe(src, callback) — direct in-process delivery:\n");
    graph_t g;
    auto seen = std::make_shared<int>(-1);
    tr::graph::vertex_handle_t src =
        g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    auto on_temp = [seen](const tr::view::rope_t& v) {
        *seen = std::to_integer<int>(v.only().bytes()[0]);
    };
    (void)g.subscribe(path_t("/sensor/temp"), on_temp);
    (void)g.write(src, make_value({0x42}));
    check(*seen == 0x42, "callback fires on write with the delivered value");
}

void test_subscribe_target() {
    std::printf("subscribe(src, target) — delivery runs the target vertex's on_write:\n");
    graph_t g;
    auto sink_seen = std::make_shared<int>(-1);
    tr::graph::handlers_t h;
    h.on_write = [sink_seen](const tr::view::rope_t& in,
                             const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        *sink_seen = std::to_integer<int>(in.only().bytes()[0]);
        return {};
    };
    (void)g.register_vertex(path_t("/log/temp"), role_t::HANDLER, std::move(h));
    tr::graph::vertex_handle_t src =
        g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.subscribe(path_t("/sensor/temp"), path_t("/log/temp"));
    (void)g.write(src, make_value({0x55}));
    check(*sink_seen == 0x55, "delivery terminates at the target, running its on_write");
}

void test_field_write_settings() {
    std::printf("field-write :settings.<field> — the core namespace is closed (RFC-0022):\n");
    graph_t g;
    tr::graph::vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    const auto payload = std::array<std::byte, 4>{std::byte{0x88}, std::byte{0x13}};  // 5000 LE
    // `settings_t` is deleted, so a flat knob name is exactly as unknown as `bogus`.
    check(
        !g.write(path_t("/sensor/temp:settings.history_keep_last"), value_tlv(payload)).has_value(),
        "a former knob name => SchemaNotFound");
    check(!g.write(path_t("/sensor/temp:settings.bogus"), value_tlv(payload)).has_value(),
          "unknown settings field => SchemaNotFound");
    // The owner-side declaration is where the ring depth lives now, and it takes effect.
    g.set_history_depth(v, 5000);
    check(g.write(v, make_value({0x01})).has_value(), "the vertex still takes ordinary writes");
}

void test_field_write_handle() {
    std::printf("Handle-based field-write (no strings on the hot path):\n");
    graph_t g;
    auto v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    std::vector<tr::graph::app_field_t> table;
    table.push_back(tr::graph::app_field_t{.name = "kp", .access = tr::graph::app_access_t::RW});
    g.set_app_fields(v, std::move(table));
    // Parse the field path ONCE; reuse the vertex_handle_t + field_path_t thereafter
    // (no per-call string parse, no map lookup).
    const auto fp = path_t::parse("/sensor/temp:settings.app.kp");
    const std::array<std::byte, 4> le{std::byte{0x10}, std::byte{0x27}};  // 10000 LE
    check(g.write(v, fp->field(), value_tlv(le)).has_value(),
          "write(vertex_handle_t, field_path_t, view_t) returns OK");
    check(g.read(v, fp->field()).has_value(),
          "the field read serves it back via the handle (no string parse, no map lookup)");
    check(g.write(v, tr::graph::field_path_t{}, make_value({0x55})).has_value(),
          "an empty field_path_t is an ordinary value write");
}

void test_subscribe_via_field_write_and_unsubscribe() {
    std::printf("field-write :subscribers[] (wire-faithful) + unsubscribe:\n");
    graph_t g;
    auto sink_seen = std::make_shared<int>(0);
    tr::graph::handlers_t h;
    h.on_write = [sink_seen](const tr::view::rope_t& in,
                             const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        *sink_seen += std::to_integer<int>(in.only().bytes()[0]);
        return {};
    };
    (void)g.register_vertex(path_t("/sink"), role_t::HANDLER, std::move(h));
    tr::graph::vertex_handle_t src =
        g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    auto sub = g.write(path_t("/sensor/temp:subscribers[]"), subscriber_tlv("sink"));
    check(sub.has_value(), "subscribe via field-write a SUBSCRIBER TLV");
    (void)g.write(src, make_value({0x10}));
    check(*sink_seen == 0x10, "fan-out reaches the SUBSCRIBER's target path");

    auto unsub = g.write(path_t("/sensor/temp:subscribers[0]"),
                         make_value({0x09, 0x00, 0x00, 0x00}));  // empty STATUS = the §D.1 sentinel
    check(unsub.has_value(), "unsubscribe clears the slot");
    (void)g.write(src, make_value({0x20}));
    check(*sink_seen == 0x10, "no further delivery after unsubscribe");
}

/**
 * @brief RFC-0009 §D.1: an indexed `:subscribers[N]` write is payload-DISCRIMINATING
 *        (#598), and `[*]` is not a write selector at all (#579).
 *
 * Before this, the `[N]` arm cleared the slot payload-blind: a `SUBSCRIBER`, a junk `VALUE`
 * and the empty-`STATUS` sentinel all produced the identical clear and the identical
 * `RESULT`. A peer writing a SUBSCRIBER to slot N — plainly meaning to REPLACE that edge —
 * silently destroyed it and was told it succeeded. `[*]` was worse: it sets
 * `indexed=true, wildcard=true` and never assigns `index`, so it landed on slot 0.
 */
void test_subscribers_indexed_write_discriminates() {
    std::printf(":subscribers[N] discriminates on payload; [*] is not a write selector:\n");
    graph_t g;
    auto seen_a = std::make_shared<int>(0);
    auto seen_b = std::make_shared<int>(0);
    auto sink = [](std::shared_ptr<int> tally) {
        tr::graph::handlers_t h;
        h.on_write = [tally](const tr::view::rope_t& in,
                             const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
            *tally += std::to_integer<int>(in.only().bytes()[0]);
            return {};
        };
        return h;
    };
    (void)g.register_vertex(path_t("/sink_a"), role_t::HANDLER, sink(seen_a));
    (void)g.register_vertex(path_t("/sink_b"), role_t::HANDLER, sink(seen_b));
    tr::graph::vertex_handle_t src =
        g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    check(g.write(path_t("/sensor/temp:subscribers[]"), subscriber_tlv("sink_a")).has_value(),
          "seed: slot 0 routes to sink_a");
    (void)g.write(src, make_value({0x10}));
    check(*seen_a == 0x10 && *seen_b == 0, "seed delivers to sink_a only");

    // (1) A SUBSCRIBER to [0] REPLACES the edge — it does not destroy it.
    check(g.write(path_t("/sensor/temp:subscribers[0]"), subscriber_tlv("sink_b")).has_value(),
          "a SUBSCRIBER write to [0] is accepted");
    (void)g.write(src, make_value({0x20}));
    check(*seen_b == 0x20, "slot 0 now routes to sink_b — REPLACED, per §D.1");
    check(*seen_a == 0x10, "... and no longer to sink_a");

    // (2) Anything that is neither the sentinel nor a SUBSCRIBER is a TYPE_MISMATCH,
    //     where it used to be an accepted, silent destruction.
    const auto junk = g.write(path_t("/sensor/temp:subscribers[0]"), make_value({0x01, 0x02}));
    check(!junk && junk.error() == status_t::TYPE_MISMATCH, "a junk payload is TYPE_MISMATCH");
    (void)g.write(src, make_value({0x30}));
    check(*seen_b == 0x50, "... and the edge SURVIVED the rejected write");

    // (3) `[*]` is INVALID_PATH — and, decisively, slot 0 is untouched.
    //
    // The wildcard is NOT text-path syntax: RFC-0004 §C defines it as a FIELD TLV with
    // index_mode=WILDCARD, so `path_t::parse("…[*]")` rejects it and only a decoded wire
    // FIELD produces this shape. Build it the way the decoder does — `indexed` set,
    // `wildcard` set, `index` left at 0 — which is exactly #579's defect.
    auto star_fp = path_t::parse("/sensor/temp:subscribers[0]")->field();
    star_fp.steps[0].wildcard = true;
    const auto star = g.write(src, star_fp, subscriber_tlv("sink_a"));
    check(!star && star.error() == status_t::INVALID_PATH, "[*] on a WRITE is INVALID_PATH");
    (void)g.write(src, make_value({0x02}));
    check(*seen_b == 0x52, "... and slot 0 SURVIVED the wildcard write (#579's data loss)");

    // (4) An index no slot answers to is refused, not back-filled — a wire-supplied index
    //     must not be able to grow the slot vector.
    const auto oor_fp = path_t::parse("/sensor/temp:subscribers[900]");
    check(oor_fp.has_value(), "the out-of-range field-path parses");
    const auto oor = g.write(src, oor_fp->field(), subscriber_tlv("sink_a"));
    check(!oor && oor.error() == status_t::INVALID_PATH, "out-of-range [N] is INVALID_PATH");

    // (5) The sentinel still clears, and a cleared slot can be refilled by index.
    check(g.write(path_t("/sensor/temp:subscribers[0]"), make_value({0x09, 0x00, 0x00, 0x00}))
              .has_value(),
          "the empty-STATUS sentinel still clears");
    (void)g.write(src, make_value({0x04}));
    check(*seen_b == 0x52, "... and delivery stops");
    check(g.write(path_t("/sensor/temp:subscribers[0]"), subscriber_tlv("sink_a")).has_value(),
          "a cleared slot accepts a SUBSCRIBER by index");
    (void)g.write(src, make_value({0x05}));
    check(*seen_a == 0x15, "... and routes again");
}

/**
 * @brief `:subscribers` is addressed WHOLE — a trailing step names nothing and MUST NOT
 *        reach the slot clear (#580).
 *
 * The `[N]` arm is an unconditional `clear_edge`, so before the bound a caller aiming at a
 * member — `:subscribers[0].liveness.last_seen_ns`, or any typo'd tail — unbound a live
 * subscriber and got `kind=RESULT`, byte-identical to a legitimate `[0]` clear. The
 * decisive assertion here is therefore not the status code but the DELIVERY: the edge must
 * still route after the rejected write.
 */
void test_subscribers_addressed_whole() {
    std::printf(":subscribers is addressed whole — a trailing step must not clear the slot:\n");
    for (const char* p : {"/sensor/temp:subscribers[0].liveness.last_seen_ns",
                          "/sensor/temp:subscribers[0].totally.made.up",
                          "/sensor/temp:subscribers[].tail", "/sensor/temp:subscribers.bogus"}) {
        graph_t g;
        auto sink_seen = std::make_shared<int>(0);
        tr::graph::handlers_t h;
        h.on_write = [sink_seen](const tr::view::rope_t& in,
                                 const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
            *sink_seen += std::to_integer<int>(in.only().bytes()[0]);
            return {};
        };
        (void)g.register_vertex(path_t("/sink"), role_t::HANDLER, std::move(h));
        tr::graph::vertex_handle_t src =
            g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
        check(g.write(path_t("/sensor/temp:subscribers[]"), subscriber_tlv("sink")).has_value(),
              "seed: one live subscriber edge");

        const auto fp = path_t::parse(p);
        check(fp.has_value(), "the multi-step :subscribers path parses (the branch is reachable)");
        if (!fp) continue;
        const auto w = g.write(src, fp->field(), make_value({0x01}));
        check(!w.has_value() && w.error() == tr::graph::status_t::SCHEMA_NOT_FOUND,
              "a non-whole :subscribers write names nothing: SCHEMA_NOT_FOUND");

        // The assertion that actually catches the defect: the edge still delivers.
        (void)g.write(src, make_value({0x20}));
        check(*sink_seen == 0x20, "... and the subscriber edge SURVIVED (it still routes)");
    }

    // The two legal shapes are untouched — this gate must not use `plain_step`.
    graph_t g;
    tr::graph::vertex_handle_t src =
        g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/sink"), role_t::STORED_VALUE);
    check(g.write(path_t("/sensor/temp:subscribers[]"), subscriber_tlv("sink")).has_value(),
          ":subscribers[] (append) still subscribes");
    check(g.write(src, path_t::parse("/sensor/temp:subscribers[0]")->field(),
                  make_value({0x09, 0x00, 0x00, 0x00}))
              .has_value(),
          ":subscribers[0] (clear) still unsubscribes");
}

void test_schema_read() {
    std::printf(":schema read (POINT descriptor):\n");
    graph_t g;
    (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    auto schema = g.read(path_t("/sensor/temp:schema"));
    check(schema.has_value(), ":schema read returns a value");
    auto point = tr::wire::decode((*schema)->only());
    check(point && point->type == tr::wire::type_t::POINT, ":schema decodes to a POINT");
    check(point && point->children.size() == 2, "POINT has a NAME and a SETTINGS child");
    check(point && point->children[0].type == tr::wire::type_t::NAME &&
              std::memcmp(point->children[0].payload.data(), "temp", 4) == 0,
          "POINT's NAME child is the vertex name 'temp'");
}

void test_admission_door_uniformity() {
    std::printf("ADR-0049 single admission door (uniform read-back + latch):\n");

    // Pin: the subscribe(src, target) SUGAR and an equivalent :subscribers[] FIELD-WRITE
    // produce byte-identical :subscribers[] read-back — both edges enter the same door
    // and retain the same encoded SUBSCRIBER{PATH} view.
    {
        graph_t g;
        (void)g.register_vertex(path_t("/sink"), role_t::STORED_VALUE);
        tr::graph::vertex_handle_t a = g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
        tr::graph::vertex_handle_t b = g.register_vertex(path_t("/b"), role_t::STORED_VALUE);
        check(g.subscribe(path_t("/a"), path_t("/sink")).has_value(), "sugar door subscribes");
        check(g.write(path_t("/b:subscribers[]"), subscriber_tlv("sink")).has_value(),
              "field-write door subscribes");
        const auto ra = g.read_subscribers(a);
        const auto rb = g.read_subscribers(b);
        check(ra && ra->size() == 1, "sugar edge reads back from :subscribers[]");
        check(rb && rb->size() == 1, "field-write edge reads back from :subscribers[]");
        check(ra && rb && ra->size() == 1 && rb->size() == 1 &&
                  std::ranges::equal((*ra)[0].bytes(), (*rb)[0].bytes()),
              "both doors store the byte-identical SUBSCRIBER view");
    }

    // Behavior alignment (ADR-0049) as amended by RFC-0022 §3.A: the durability latch is
    // the SUBSCRIBER's request, and it fires for LOCAL doors too — a callback subscriber
    // and a target subscriber that ask for it each receive the LKV immediately at
    // subscribe, exactly as a remote one does (was remote-only, and was a vertex flag).
    {
        graph_t g;
        tr::graph::vertex_handle_t src = g.register_vertex(path_t("/tl"), role_t::STORED_VALUE);
        (void)g.write(src, make_value({0x5A}));  // seed the LKV BEFORE subscribing

        auto seen = std::make_shared<int>(-1);
        auto on_latch = [seen](const tr::view::rope_t& v) {
            *seen = std::to_integer<int>(v.only().bytes()[0]);
        };
        (void)g.subscribe(path_t("/tl"), on_latch, kDurableSub);
        check(*seen == 0x5A, "callback door: a durability_request latches the LKV at subscribe");

        tr::graph::vertex_handle_t tgt = g.register_vertex(path_t("/tgt"), role_t::STORED_VALUE);
        (void)g.subscribe(path_t("/tl"), path_t("/tgt"), kDurableSub);
        const auto latched = g.read(tgt);
        check(latched.has_value() && std::to_integer<int>((*latched)->only().bytes()[0]) == 0x5A,
              "target door: a durability_request latches the LKV at subscribe (as a write)");
    }

    // The ablation, and the RFC-0022 behaviour change: on the SAME producer, a subscriber
    // that requested nothing gets NO latch. Before RFC-0022 one vertex flag decided this
    // for every subscriber; there was no way to express "not for me".
    {
        graph_t g;
        tr::graph::vertex_handle_t src = g.register_vertex(path_t("/vol"), role_t::STORED_VALUE);
        (void)g.write(src, make_value({0x77}));
        auto fired = std::make_shared<int>(0);
        auto on_vol = [fired](const tr::view::rope_t&) { ++*fired; };
        (void)g.subscribe(path_t("/vol"), on_vol);
        check(*fired == 0, "default subscription: no latch");
        auto latched = std::make_shared<int>(0);
        auto on_dur = [latched](const tr::view::rope_t&) { ++*latched; };
        (void)g.subscribe(path_t("/vol"), on_dur, kDurableSub);
        check(*latched == 1 && *fired == 0,
              "... and a durability_request on the SAME vertex does latch (per-subscription)");
    }
}

/**
 * @brief Wait for `slot` to reach `round` — a BOUNDED hot spin, then a real park (#1425).
 *
 * The two race guards below hand off between exactly two threads once per round, and the
 * window they aim at is tens of nanoseconds wide, so the wait genuinely has to start as a
 * spin: parking costs microseconds and swamps the window, which is the measurement each
 * guard's own doc block records. That is the reason the spin exists, and `kHotBudget` is its
 * bound.
 *
 * **Why the bound is 200 us, measured.** The bound has to exceed the real handoff latency or
 * the guard goes vacuous, and that latency is not the window — it is one `subscribe` plus the
 * writer's offset loop, single-digit microseconds. Calibrated against the #635 defect itself
 * (the `note_subscriber_added` bump moved back to trailing the append) on an idle host, one
 * process pinned to two CPUs, 8 runs per budget per guard: at **20 us and 50 us both guards
 * MISS the reintroduced bug outright** on some runs — the fall-through fires on ordinary
 * rounds and the two threads stop racing at all. At **200 us both guards catch it 8/8**, at
 * 189-1039 lost rounds out of 20000, matching the unbounded spin's own 55-500. A first draft
 * bounded the spin by ITERATION count instead (512 acquire loads, ~0.5 us): it detected
 * nothing, 0/6, while still printing PASS — a bound too small does not weaken this guard, it
 * silently deletes it.
 *
 * **Past the bound, park — do not yield.** Once 200 us of spinning has not produced the
 * partner, the partner is not merely late, it is off-CPU waiting for the core this thread is
 * holding; spinning on starves the very thread being waited for. `yield` fixes the burn but
 * not the latency, because a yield returns to the back of a runqueue that (under the recipe
 * this issue is about) is 32 threads deep. `std::atomic::wait` + `notify_one` is the real
 * rendezvous: the waiter sleeps in a futex and the store wakes it directly.
 *
 * What the parked path gives up is the CALIBRATION — an oversubscribed run no longer reliably
 * lands the writer inside the window — never the ASSERTION: `missed == 0` holds at every
 * interleaving, so the guard stays sound there and is merely less sensitive. The sensitive run
 * is the ordinary one, which is where CI and every developer execute it. Measured against the
 * same reintroduced defect with competing spinners pinned to the same two CPUs: at 2x
 * oversubscription this still detects 10/10, and at 4x it detects 5/5 where the UNBOUNDED spin
 * it replaces itself missed 2 of 8. Only a runqueue far past the recipe's own (a 30-way build
 * sharing the pair) degrades it, to ~6/10 — and there the fall-through is the point.
 *
 * **And the budget itself adapts, because paying it 40000 times is the whole remaining bill.**
 * A fixed 200 us bound still burns 8 CPU-seconds per process over the two guards, which under
 * the 16-way recipe is 16 of those on two cores: measured, that alone is a **~120 s batch**,
 * against **1.8 s** for the same binary with the budget set to zero. So `misses_` tracks
 * CONSECUTIVE fall-throughs and stops paying after `kColdAfter` of them, re-probing every
 * `kReprobe` rounds in case the machine frees up; any hot hit resets it to fully hot. The
 * counter is consecutive, not cumulative, precisely so that the isolated fall-through an idle
 * host does produce — the very first round, where the writer thread is still being created —
 * cannot latch the whole run cold.
 */
class handoff_t {
   public:
    /** @brief Wait until `slot` reads `round`, adapting the hot budget as described above. */
    void wait(std::atomic<int>& slot, int round) {
        constexpr auto kHotBudget = 200us;
        constexpr int kColdAfter = 8;   // consecutive fall-throughs that stop the spin
        constexpr int kReprobe = 1024;  // cold rounds between spin re-probes
        if (misses_ < kColdAfter || ++since_probe_ >= kReprobe) {
            since_probe_ = 0;
            const auto deadline = std::chrono::steady_clock::now() + kHotBudget;
            do {
                // The clock read is hoisted out of the inner spin: it costs ~20 ns against a
                // ~1 ns acquire load, and paying it every iteration would blunt the very
                // reaction time the hot half exists to buy.
                for (int i = 0; i < 64; ++i)
                    if (slot.load(std::memory_order_acquire) == round) {
                        misses_ = 0;
                        return;
                    }
            } while (std::chrono::steady_clock::now() < deadline);
            ++misses_;
        }
        // No lost wake: `wait` re-compares under the futex, so a `post_handoff` landing
        // between this load and the wait returns immediately rather than sleeping on a stale
        // value.
        for (int cur = slot.load(std::memory_order_acquire); cur != round;
             cur = slot.load(std::memory_order_acquire))
            slot.wait(cur, std::memory_order_acquire);
    }

   private:
    int misses_ = 0;      /**< consecutive rounds whose hot budget bought nothing */
    int since_probe_ = 0; /**< rounds parked without spinning since the last re-probe */
};

/** @brief Release a `handoff_t::wait`ing partner: publish `round`, then wake a parked waiter.
 *
 *  The `notify_one` is free on the hot path — libstdc++ skips the syscall when its waiter
 *  count is zero, which is every handoff that stayed inside the budget. */
void post_handoff(std::atomic<int>& slot, int round) {
    slot.store(round, std::memory_order_release);
    slot.notify_one();
}

/**
 * @brief #635: the fan-out gate must not open a hole in ADR-0049's latch.
 *
 * `fan_out` now SKIPS `snapshot_edges` entirely when the vertex's own-subscriber count
 * reads zero, which is what stops unrelated vertices from serialising on a shared lock
 * stripe. That gate is only sound if the count rises BEFORE the slot it stands for, and
 * this is the assertion that says so.
 *
 * A transient-local (`durability == 1`) vertex has exactly two legs a value can reach a
 * brand-new subscriber by: the latch taken inside the edge verb (when the write got there
 * first) and the fan-out (when the subscribe did). The racing write must arrive by ONE of
 * them, always — never neither. Move the bump in `graph_t::admit_subscriber` back to
 * trailing the append and this fails: a write landing in the gap stores its value, reads a
 * zero count, skips the snapshot, and the latch one line earlier already holds the OLD one.
 *
 * **Why it is shaped like this.** The gap is one stripe unlock plus one shared-lock
 * acquire — tens of nanoseconds — because `note_subscriber_added` bumps the count as its
 * first act. Two earlier drafts of this test could not see it. Spawning a thread per round
 * failed outright (creation alone is ~20 us, so the subscribe always finished first, and
 * the test passed against the bug it targets); parking that thread first found it once in
 * ~8000 rounds, because the spawn jitter still swamped the window. Both threads therefore
 * stay HOT here and hand off through one atomic, which drops the per-round noise to the
 * order of the window itself and turns a stochastic near-miss into a reliable failure.
 * `handoff_t::wait` is that hot spin, BOUNDED (#1425): the bound is never reached when the
 * partner has a CPU, and past it the spin would be starving the partner rather than racing it.
 */
void test_subscribe_never_misses_a_racing_write() {
    std::printf("#635: a write racing subscribe arrives by the latch or the fan-out:\n");
    constexpr int kRounds = 20000;
    constexpr int kOffsets = 64;  // interleaving positions the writer's arrival is swept over

    graph_t g;
    // The latch leg is armed by the SUBSCRIBER's request now (RFC-0022 §3.A), so the
    // producers are plain vertices and every subscribe below asks for durability.
    std::vector<tr::graph::vertex_handle_t> verts;
    std::vector<path_t> paths;
    verts.reserve(kRounds);
    paths.reserve(kRounds);
    for (int i = 0; i < kRounds; ++i) {
        // A FRESH vertex per round: the gate only fires from a zero count, so a reused one
        // would test nothing after the first subscribe.
        const std::string name = "/race/v" + std::to_string(i);
        paths.emplace_back(name);
        verts.push_back(g.register_vertex(paths.back(), role_t::STORED_VALUE));
        (void)g.write(verts.back(), make_value({0x01}));  // the OLD value the latch may hold
    }

    std::atomic<unsigned> seen{0};
    std::atomic<int> gate{-1};  // the round the writer may start
    std::atomic<int> done{-1};  // the round the writer has finished
    auto on_value = [&seen](const tr::view::rope_t& v) {
        seen.fetch_or(1U << std::to_integer<unsigned>(v.only().bytes()[0]),
                      std::memory_order_relaxed);
    };

    handoff_t gate_wait;  // the writer's own adaptive state; `done_wait` below is the main
    handoff_t done_wait;  // thread's — one per waiting side, never shared
    std::thread w([&] {
        for (int i = 0; i < kRounds; ++i) {
            gate_wait.wait(gate, i);                           // hot handoff, bounded
            std::atomic<int> step{0};                          // a REAL RMW — a signal fence is
            for (int k = 0; k < i % kOffsets; ++k)             // compiler-only, and an empty loop
                step.fetch_add(1, std::memory_order_relaxed);  // optimizes to nothing
            (void)g.write(verts[i], make_value({0x02}));
            post_handoff(done, i);
        }
    });

    int missed = 0;
    for (int i = 0; i < kRounds; ++i) {
        seen.store(0, std::memory_order_relaxed);
        post_handoff(gate, i);
        (void)g.subscribe(paths[i], on_value, kDurableSub);
        done_wait.wait(done, i);  // both legs have run
        if ((seen.load(std::memory_order_relaxed) & (1U << 2)) == 0) ++missed;
    }
    w.join();

    check(missed == 0, "no write racing a subscribe is lost by the zero-subscriber gate");
    if (missed != 0) std::printf("    (%d of %d rounds lost the racing write)\n", missed, kRounds);
}

/**
 * @brief #1140: the DEFERRED half of the same gate — a skipped `mark_pending` must not open
 *        a hole in ADR-0049's latch either.
 *
 * `graph_t::mark_pending` is the assign path's counterpart to #635's `fan_out` gate, and it
 * is a SKIP, not a deferral: a vertex that misses its mark enters no sweep set, so the next
 * covering `propagate` delivers it nowhere and only a LATER write can re-mark it. The two
 * legs a value can reach a brand-new subscriber by are therefore the latch taken inside the
 * edge verb (the assign got there first) and the ancestor sweep (the subscribe did). The
 * racing assign must arrive by ONE of them, always — never neither.
 *
 * **What this pins, and what it does not.** Two independent things make the gate sound, and
 * this guard covers exactly one of them:
 *
 * - **Program order — COVERED.** The count must rise BEFORE the slot the latch is taken
 *   from. Move the `note_subscriber_added` bump in `graph_t::admit_subscriber` back to
 *   trailing `add_edge` and this fails: an assign landing in the gap stores its value, reads
 *   a zero count, skips the mark, and the latch one line earlier already holds the OLD one —
 *   so the sweep below finds nothing and the new value is delivered nowhere.
 * - **Memory order — NOT COVERED HERE, by construction.** That the publisher's count read is
 *   `own_subs_ordered()` (seq_cst) rather than `own_subs()` (relaxed) cannot be observed on
 *   x86-64: the seq_cst LKV store lowers to a locked `xchg`, a full hardware barrier, so a
 *   later RELAXED read of zero still forces the subscriber's latch to see the new value.
 *   Swap the ordered read for the relaxed one and this test stays green on this host. The
 *   coverage for that half is the `ubuntu-24.04-arm` CI leg (core-ci `build-test-arm64`),
 *   where the store does not order the later load and the excluded interleaving is
 *   architecturally reachable — the same reason it is reachable on the shipped rv32 targets.
 *   Say which half a guard covers; #635's own guard did not, and that is how the deferred
 *   leg kept a relaxed read for as long as it did.
 *
 * Shaped like `test_subscribe_never_misses_a_racing_write` and for the same reason: the
 * window is tens of nanoseconds, so both threads stay HOT and hand off through one atomic
 * (`handoff_t::wait`, bounded — see there).
 * Differences are `assign` instead of `write` (the STATE half — it marks, it never fans out),
 * one shared parent whose `propagate` is the sweep that would deliver the mark, and the check
 * running AFTER that sweep.
 */
void test_assign_never_misses_a_racing_subscribe() {
    std::printf("#1140: an assign racing subscribe arrives by the latch or the sweep:\n");
    constexpr int kRounds = 20000;
    constexpr int kOffsets = 64;  // interleaving positions the assign's arrival is swept over

    graph_t g;
    // One shared parent per round set: `propagate(parent)` is the covering sweep that drains
    // the pending marks its strict descendants took.
    const path_t parent_path("/mrace");
    const tr::graph::vertex_handle_t parent = g.register_vertex(parent_path, role_t::STORED_VALUE);
    std::vector<tr::graph::vertex_handle_t> verts;
    std::vector<path_t> paths;
    verts.reserve(kRounds);
    paths.reserve(kRounds);
    for (int i = 0; i < kRounds; ++i) {
        // A FRESH vertex per round: the gate only fires from a zero count, so a reused one
        // would test nothing after the first subscribe. IF_NEWER is the default mode, which
        // is the one mark_pending marks for.
        const std::string name = "/mrace/v" + std::to_string(i);
        paths.emplace_back(name);
        verts.push_back(g.register_vertex(paths.back(), role_t::STORED_VALUE));
        (void)g.write(verts.back(), make_value({0x01}));  // the OLD value the latch may hold
    }

    std::atomic<unsigned> seen{0};
    std::atomic<int> gate{-1};  // the round the assigner may start
    std::atomic<int> done{-1};  // the round the assigner has finished
    auto on_value = [&seen](const tr::view::rope_t& v) {
        seen.fetch_or(1U << std::to_integer<unsigned>(v.only().bytes()[0]),
                      std::memory_order_relaxed);
    };

    handoff_t gate_wait;  // the writer's own adaptive state; `done_wait` below is the main
    handoff_t done_wait;  // thread's — one per waiting side, never shared
    std::thread w([&] {
        for (int i = 0; i < kRounds; ++i) {
            gate_wait.wait(gate, i);                           // hot handoff, bounded
            std::atomic<int> step{0};                          // a REAL RMW — a signal fence is
            for (int k = 0; k < i % kOffsets; ++k)             // compiler-only, and an empty loop
                step.fetch_add(1, std::memory_order_relaxed);  // optimizes to nothing
            (void)g.assign(verts[i], make_value({0x02}));
            post_handoff(done, i);
        }
    });

    int missed = 0;
    for (int i = 0; i < kRounds; ++i) {
        seen.store(0, std::memory_order_relaxed);
        post_handoff(gate, i);
        (void)g.subscribe(paths[i], on_value, kDurableSub);
        done_wait.wait(done, i);    // both legs have run
        (void)g.propagate(parent);  // the covering sweep: it drains the mark, or there is none
        if ((seen.load(std::memory_order_relaxed) & (1U << 2)) == 0) ++missed;
    }
    w.join();

    check(missed == 0, "no assign racing a subscribe is lost by mark_pending's skip gate");
    if (missed != 0) std::printf("    (%d of %d rounds lost the racing assign)\n", missed, kRounds);
}

void test_delivery_terminates_at_target() {
    std::printf("delivery terminates at the target (ADR-0051): no chained relay, no cycle:\n");
    graph_t g;

    // A -> B chained plain vertices: a write to A is DELIVERED (stored) at B, but B's own
    // subscribers are NOT notified — delivery terminates at B (RFC-0007). Pure relay is
    // wired as a direct subscription to the source, never a chain of plain vertices.
    tr::graph::vertex_handle_t a = g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/b"), role_t::STORED_VALUE);
    auto b_relayed = std::make_shared<int>(0);
    auto on_b = [b_relayed](const tr::view::rope_t&) { ++*b_relayed; };
    (void)g.subscribe(path_t("/a"), path_t("/b"));  // A -> B target edge
    (void)g.subscribe(path_t("/b"), on_b);          // an observer on B's own subscribers
    (void)g.write(a, make_value({0x55}));
    const auto rb = g.find(path_t("/b").key());
    const auto b_val = rb.has_value() ? g.read(*rb) : tr::graph::result_t<tr::graph::value_ref_t>{};
    check(b_val.has_value() && std::to_integer<int>((*b_val)->only().bytes()[0]) == 0x55,
          "the delivery stored A's write AT B (delivery IS a write to the target)");
    check(*b_relayed == 0, "but B does NOT relay to its own subscribers (terminates at B)");

    // A mutual X <-> Y cycle cannot loop: each delivery is store-only, so no re-dispatch —
    // the write fires exactly one hop (X's own subscriber) and stops, by construction.
    tr::graph::vertex_handle_t x = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/y"), role_t::STORED_VALUE);
    auto hops = std::make_shared<int>(0);
    auto on_hop = [hops](const tr::view::rope_t&) { ++*hops; };
    (void)g.subscribe(path_t("/x"), path_t("/y"));
    (void)g.subscribe(path_t("/y"), path_t("/x"));
    (void)g.subscribe(path_t("/x"), on_hop);
    (void)g.subscribe(path_t("/y"), on_hop);
    (void)g.write(x, make_value({0x01}));  // must NOT loop / stack-overflow
    check(*hops == 1, "the cycle fires exactly one hop (X's own subscriber); no transitive relay");

    // A controller re-emits by its OWN logic: a HANDLER whose on_write writes an output
    // port DOES reach that port's subscribers — propagation past a target is the target's
    // logic, not the runtime's.
    (void)g.register_vertex(path_t("/out"), role_t::STORED_VALUE);
    auto sink_seen = std::make_shared<int>(-1);
    auto on_out = [sink_seen](const tr::view::rope_t& in) {
        *sink_seen = std::to_integer<int>(in.only().bytes()[0]);
    };
    (void)g.subscribe(path_t("/out"), on_out);
    tr::graph::handlers_t hc;
    hc.on_write = [&g](const tr::view::rope_t& in,
                       const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        return g.write(path_t("/out"), in);  // re-emit on the controller's own execution
    };
    (void)g.register_vertex(path_t("/ctrl"), role_t::HANDLER, std::move(hc));
    tr::graph::vertex_handle_t ctrl_src =
        g.register_vertex(path_t("/ctrl_src"), role_t::STORED_VALUE);
    (void)g.subscribe(path_t("/ctrl_src"), path_t("/ctrl"));
    (void)g.write(ctrl_src, make_value({0x63}));
    check(*sink_seen == 0x63, "a controller handler re-emits to its output port's subscribers");
}

}  // namespace

void test_assign_propagate() {
    std::printf("assign/propagate + per-vertex delivery_mode (RFC-0008):\n");
    graph_t g;
    // A subtree /r with leaves /r/a /r/b /r/c, each carrying its own subscriber so we can
    // see exactly which vertices a sweep delivered.
    auto r = g.register_vertex(path_t("/r"), role_t::STORED_VALUE);
    auto a = g.register_vertex(path_t("/r/a"), role_t::STORED_VALUE);
    auto b = g.register_vertex(path_t("/r/b"), role_t::STORED_VALUE);
    auto c = g.register_vertex(path_t("/r/c"), role_t::STORED_VALUE);
    auto ca = std::make_shared<int>(0);
    auto cb = std::make_shared<int>(0);
    auto cc = std::make_shared<int>(0);
    auto on_a = [ca](const tr::view::rope_t&) { ++*ca; };
    auto on_b = [cb](const tr::view::rope_t&) { ++*cb; };
    auto on_c = [cc](const tr::view::rope_t&) { ++*cc; };
    (void)g.subscribe(path_t("/r/a"), on_a);
    (void)g.subscribe(path_t("/r/b"), on_b);
    (void)g.subscribe(path_t("/r/c"), on_c);

    // assign is state-only: it delivers nothing, and repeated assigns coalesce.
    (void)g.assign(a, make_value({0x01}));
    (void)g.assign(a, make_value({0x02}));
    check(*ca == 0, "assign delivers nothing (state plane only)");

    // propagate(root) flushes ONLY the descendant assigned since the last sweep, once.
    (void)g.propagate(r);
    check(*ca == 1, "propagate(root) flushes the assigned descendant once (coalesced)");
    check(*cb == 0 && *cc == 0, "IF_NEWER: descendants not assigned since the sweep are skipped");

    // Self-clearing: a second sweep with nothing newly assigned delivers nothing.
    (void)g.propagate(r);
    check(*ca == 1, "a clean subtree flushes nothing (write_seq did not advance)");

    // Selective subtree propagation: assign b and c, one propagate delivers exactly them.
    (void)g.assign(b, make_value({0x10}));
    (void)g.assign(c, make_value({0x20}));
    (void)g.propagate(r);
    check(*cb == 1 && *cc == 1, "propagate flushes exactly the newly-assigned descendants");
    check(*ca == 1, "an unassigned descendant is not re-sent");

    // UNCONDITIONAL rides every sweep, even with no assignment since the last one.
    g.set_delivery_mode(a, delivery_mode_t::UNCONDITIONAL);
    (void)g.propagate(r);
    check(*ca == 2, "UNCONDITIONAL is swept every time (even unassigned)");
    check(*cb == 1 && *cc == 1, "IF_NEWER siblings stay clean across an UNCONDITIONAL sweep");

    // EXPLICIT is never pulled in by an ancestor sweep; a direct propagate still delivers it
    // (the argument of propagate is always delivered — RFC-0008 §C).
    g.set_delivery_mode(b, delivery_mode_t::EXPLICIT);
    (void)g.assign(b, make_value({0x11}));
    (void)g.propagate(r);
    check(*cb == 1, "EXPLICIT is never included by an ancestor sweep");
    (void)g.propagate(b);
    check(*cb == 2, "a direct propagate on an EXPLICIT vertex delivers it");

    // write() remains the eager §D composition (assign then deliver the vertex).
    auto cw = std::make_shared<int>(0);
    auto w = g.register_vertex(path_t("/w"), role_t::STORED_VALUE);
    auto on_w = [cw](const tr::view::rope_t&) { ++*cw; };
    (void)g.subscribe(path_t("/w"), on_w);
    (void)g.write(w, make_value({0x77}));
    check(*cw == 1, "write() delivers immediately (assign + targeted propagate)");

    // #1185 (the #854-survivor locked-erase drop): an assign that lands BETWEEN the write's
    // store and its clear_pending publishes a value only the pending mark will ever deliver,
    // so that mark must survive the erase. The subscriber callback runs on the writer thread
    // inside the write's own fan_out — exactly that window — so the interleaving is
    // deterministic here instead of racy.
    auto s = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    auto x = g.register_vertex(path_t("/s/x"), role_t::STORED_VALUE);
    auto seen = std::make_shared<std::vector<int>>();
    auto armed = std::make_shared<bool>(true);
    auto on_x = [&g, x, seen, armed](const tr::view::rope_t& in) {
        seen->push_back(std::to_integer<int>(in.only().bytes()[0]));
        if (!*armed) return;
        *armed = false;  // once — the sweep below re-enters this same callback
        (void)g.assign(x, make_value({0x92}));  // races the in-flight write's clear_pending
    };
    (void)g.subscribe(path_t("/s/x"), on_x);
    (void)g.write(x, make_value({0x91}));
    check(seen->size() == 1 && seen->front() == 0x91, "the eager write delivers its own value");
    (void)g.propagate(s);  // a covering sweep: /s/x rides it only if its mark survived
    check(seen->size() == 2 && seen->back() == 0x92,
          "a write's clear_pending keeps the mark of an assign it did not deliver (#1185)");
    (void)g.propagate(s);
    check(seen->size() == 2, "the surviving mark is drained once, not re-delivered");
}

/** @brief `/a/b` spelled back from a canonical key, so the enumeration order is asserted on
 *         something a reader can check by eye rather than on raw NAME records. */
std::string spell(tr::wire::key_view_t key) {
    std::string out;
    const std::span<const std::byte> b = key.bytes();
    std::size_t i = 0;
    while (i < b.size()) {
        // Packed records (RFC-0018): `[u8 len][bytes]`, and a `len == 0` escape is not a
        // key record at all — spelling stops there, as `key_view_t` does.
        const std::size_t len = static_cast<std::uint8_t>(b[i]);
        if (len == 0 || i + 1 + len > b.size()) break;
        out.push_back('/');
        out.append(reinterpret_cast<const char*>(b.data()) + i + 1, len);
        i += 1 + len;
    }
    return out;
}

/** @brief Enumerate the graph into a `/a/b|/a/c|` joined string (visit order preserved). */
std::string enumerate(const graph_t& g) {
    std::string out;
    g.for_each_vertex([&out](tr::wire::key_view_t key, tr::graph::vertex_handle_t) {
        out += spell(key);
        out.push_back('|');
    });
    return out;
}

void test_for_each_vertex() {
    std::printf("for_each_vertex — every registered vertex, ascending canonical-key byte order:\n");
    graph_t g;
    // Registered out of order, and DEEP: /zone/b's parent /zone is a placeholder created by
    // the deep register, never declared by an owner.
    (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/zone/b"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/actuator"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/sensor"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/zone/a"), role_t::STORED_VALUE);

    // Byte order over CANONICAL keys, not over the spelled text: a NAME record is
    // `02 00 <u16 len> <text>`, so siblings order by name LENGTH first — zone(4), sensor(6),
    // actuator(8). What the order does promise is that a parent precedes its subtree and the
    // subtree is contiguous (a parent's key is a byte-prefix of every descendant's).
    const std::string seen = enumerate(g);
    check(seen == "/zone/a|/zone/b|/sensor|/sensor/temp|/actuator|",
          std::string("canonical-key byte order, placeholders skipped (got ") + seen + ")");

    check(enumerate(g) == seen, "the order is STABLE across calls while the graph is unchanged");

    // The root is a vertex once it is REGISTERED, and sorts first (the empty key).
    (void)g.register_vertex(*path_t::parse("/"), role_t::STORED_VALUE);
    check(enumerate(g) == "|" + seen, "a registered root is enumerated first (empty key)");

    // Registering the placeholder makes it appear, in place — nothing else moves.
    (void)g.register_vertex(path_t("/zone"), role_t::STORED_VALUE);
    check(enumerate(g) == "|/zone|/zone/a|/zone/b|/sensor|/sensor/temp|/actuator|",
          "a filled placeholder joins the listing at its sorted position, ahead of its subtree");

    // The handle is the real thing: write through it, then read the value back by path.
    bool found_temp = false;
    g.for_each_vertex([&](tr::wire::key_view_t key, tr::graph::vertex_handle_t vh) {
        if (spell(key) != "/sensor/temp") return;
        found_temp = true;
        (void)g.write(vh, make_value({0x2A}));
    });
    const auto vt = g.find(path_t("/sensor/temp").key());
    const bool wrote = vt && g.read(*vt).has_value();
    check(found_temp && wrote,
          "the visited handle is usable — a write through it lands on that vertex");
}

void test_for_each_vertex_concurrent_register() {
    std::printf("for_each_vertex — a concurrent register during the walk is safe:\n");
    graph_t g;
    constexpr int kPre = 64;
    for (int i = 0; i < kPre; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "/pre/v%03d", i);
        (void)g.register_vertex(path_t(buf), role_t::STORED_VALUE);
    }
    (void)g.register_vertex(path_t("/pre"), role_t::STORED_VALUE);

    // A writer registering NEW vertices while the walk runs. The snapshot is taken under the
    // shared map hold, so a late arrival may or may not be seen — but every vertex that
    // existed BEFORE the walk started must be visited exactly once, and nothing may fault.
    // The writer is BOUNDED (kNew) so the walk's cost stays bounded too; it then idles until
    // the reader is done, keeping the two overlapped for the whole run.
    constexpr int kNew = 512;
    std::atomic<bool> stop{false};
    std::atomic<int> written{0};
    std::thread writer([&] {
        for (int i = 0; i < kNew && !stop.load(std::memory_order_relaxed); ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "/new/v%05d", i);
            (void)g.register_vertex(path_t(buf), role_t::STORED_VALUE);
            written.store(i + 1, std::memory_order_relaxed);
        }
    });

    // Hold the walk phase until the writer has landed its first registration. Without this
    // gate, 200 fast walk rounds can complete before the OS schedules the writer thread at
    // all, and the "really did register concurrently" self-check below races the scheduler
    // instead of pinning the interleaving it exists to prove (#1343).
    while (written.load(std::memory_order_relaxed) == 0) std::this_thread::yield();

    int pre_seen = 0;
    bool ordered = true;
    for (int round = 0; round < 200; ++round) {
        std::string prev;
        bool first = true;
        pre_seen = 0;
        g.for_each_vertex([&](tr::wire::key_view_t key, tr::graph::vertex_handle_t) {
            // Compared on the KEY BYTES — the order for_each_vertex actually promises.
            const std::string raw(reinterpret_cast<const char*>(key.bytes().data()),
                                  key.bytes().size());
            if (!first && !(prev < raw)) ordered = false;  // strictly ascending, no dupes
            first = false;
            prev = raw;
            if (spell(key).rfind("/pre/v", 0) == 0) ++pre_seen;
        });
        if (pre_seen != kPre || !ordered) break;
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    check(written.load() > 0, "the writer really did register concurrently with the walks");

    check(pre_seen == kPre, "every vertex registered BEFORE the walk is visited, exactly once");
    check(ordered, "the visit order stays strictly ascending under concurrent registration");
}

int main() {
    test_path_parse();
    test_stored_value();
    test_assign_lkv_and_seq_bump();
    test_stream();
    test_stream_drain_cursor();
    test_handler();
    test_await();
    test_subscribe_callback();
    test_subscribe_target();
    test_assign_propagate();
    test_field_write_settings();
    test_field_write_handle();
    test_subscribe_via_field_write_and_unsubscribe();
    test_subscribers_indexed_write_discriminates();
    test_subscribers_addressed_whole();
    test_admission_door_uniformity();
    test_subscribe_never_misses_a_racing_write();
    test_assign_never_misses_a_racing_subscribe();
    test_schema_read();
    test_delivery_terminates_at_target();
    test_for_each_vertex();
    test_for_each_vertex_concurrent_register();
    test_concurrent_stress();

    return tr::testing::summary("graph");
}
