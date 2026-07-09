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
 * :schema, and the in-process dispatch-depth cycle cap. The stress is the
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
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::delivery_mode_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::settings_t;
using tr::graph::status_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A view_t over a fresh, owned heap segment holding `bytes`. */
tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

tr::view::view_t make_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> v;
    v.reserve(bytes.size());
    for (std::uint8_t b : bytes) v.push_back(std::byte{b});
    return make_value(v);
}

void test_path_parse() {
    std::printf("path_t parse / canonicalize / field tail:\n");
    const auto p = path_t::parse("/sensor/temp");
    check(p.has_value(), "valid path parses");
    // Canonical PATH payload: 02 00 06 00 'sensor' 02 00 04 00 'temp' = 18 bytes.
    check(p && p->key().size() == 18, "canonical key is 18 bytes for /sensor/temp");
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

    const auto f = path_t::parse("/sensor/temp:settings.deadline_ns");
    check(f && f->field().steps.size() == 2 && f->field().steps[0].name == "settings" &&
              f->field().steps[1].name == "deadline_ns",
          "field tail :settings.deadline_ns parses to two steps");
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
    check(r.has_value() && r->only().bytes().size() == 3, "read returns the written value");
    check(r && std::to_integer<int>(r->only().bytes()[0]) == 0xAA, "value byte 0 == 0xAA");
    // The LKV holds one reference; read returned an independent clone => use_count 2.
    check(r && r->only().owner.use_count() == 2,
          "read is a clone (segment use_count == 2), not a copy");

    check(!g.try_register_vertex(*path, role_t::STORED_VALUE).has_value(),
          "re-register same path fails");
    check(g.try_register_vertex(*path, role_t::STORED_VALUE).error() == status_t::PATH_IN_USE,
          "duplicate registration reports PathInUse");
}

void test_stream() {
    std::printf("Stream vertex (bounded history ring):\n");
    graph_t g;
    const auto path = path_t::parse("/log/events");
    settings_t s;
    s.history_keep_last = 3;
    tr::graph::vertex_handle_t v = g.register_vertex(*path, role_t::STREAM, {}, s);

    for (std::uint8_t i = 1; i <= 5; ++i) (void)g.write(v, make_value({i}));

    auto hist = g.history(v);
    check(hist.has_value() && hist->size() == 3, "history bounded to keep_last = 3");
    check(hist && std::to_integer<int>((*hist)[0].only().bytes()[0]) == 3,
          "oldest kept is the 3rd write");
    check(hist && std::to_integer<int>((*hist)[2].only().bytes()[0]) == 5,
          "newest kept is the 5th write");
    auto latest = g.read(v);
    check(latest && std::to_integer<int>(latest->only().bytes()[0]) == 5,
          "read returns the latest (5)");
}

void test_handler() {
    std::printf("Handler vertex (user on_read / on_write seam):\n");
    graph_t g;
    const auto path = path_t::parse("/compute/answer");
    auto written = std::make_shared<std::vector<std::byte>>();
    tr::graph::handlers_t h;
    h.on_read = [] { return make_value({0x2A}); };  // always 42
    h.on_write = [written](const tr::view::rope_t& in) -> tr::graph::result_t<void> {
        const auto b = in.only().bytes();
        written->assign(b.begin(), b.end());
        return {};
    };
    tr::graph::vertex_handle_t v = g.register_vertex(*path, role_t::HANDLER, std::move(h));

    auto r = g.read(v);
    check(r && std::to_integer<int>(r->only().bytes()[0]) == 0x2A,
          "on_read supplies the value (42)");
    (void)g.write(v, make_value({0x99}));
    check(written->size() == 1 && std::to_integer<int>((*written)[0]) == 0x99,
          "on_write receives the written bytes");
}

void test_await() {
    std::printf("await (blocks until next write; times out otherwise):\n");
    graph_t g;
    tr::graph::vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    // A writer publishes shortly after we begin awaiting.
    std::thread writer([&] {
        std::this_thread::sleep_for(40ms);
        (void)g.write(v, make_value({0x7E}));
    });
    auto r = g.await(v, 2s);
    writer.join();
    check(r.has_value() && std::to_integer<int>(r->only().bytes()[0]) == 0x7E,
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

    std::atomic<bool> go{false};
    std::atomic<long> reads_done{0};
    std::vector<std::thread> threads;

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            while (!go.load(std::memory_order_acquire)) {
            }
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
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kReadsEach; ++i) {
                auto rr = g.read(v);
                // Any non-empty read must be a well-formed 8-byte value (no torn read).
                if (rr && rr->only().bytes().size() != 8)
                    return;  // leaves reads_done short => FAIL
            }
            reads_done.fetch_add(kReadsEach, std::memory_order_relaxed);
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    check(reads_done.load() == static_cast<long>(kReaders) * kReadsEach,
          "all reads completed without crash under contention");
    auto fin = g.read(v);
    check(fin.has_value() && fin->only().bytes().size() == 8,
          "final read returns a valid 8-byte value");
}

/** @brief A VALUE TLV wrapping `payload` (01 00 <len> <payload>), as an owned view_t. */
tr::view::view_t value_tlv(std::span<const std::byte> payload) {
    tr::wire::tlv_t t{.type = tr::wire::type_t::VALUE, .payload = payload};
    return make_value(tr::wire::encode(t));
}

/** @brief A SUBSCRIBER TLV naming a single-segment target path, as an owned view_t. */
tr::view::view_t subscriber_tlv(std::string_view target_segment) {
    std::vector<std::byte> name_bytes;
    for (char c : target_segment)
        name_bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    tr::wire::tlv_t name{.type = tr::wire::type_t::NAME, .payload = name_bytes};
    tr::wire::tlv_t path{.type = tr::wire::type_t::PATH};
    path.opt.pl = true;
    path.children.push_back(name);
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
    std::printf("subscribe(src, target) — spec-faithful re-dispatch to a vertex:\n");
    graph_t g;
    auto sink_seen = std::make_shared<int>(-1);
    tr::graph::handlers_t h;
    h.on_write = [sink_seen](const tr::view::rope_t& in) -> tr::graph::result_t<void> {
        *sink_seen = std::to_integer<int>(in.only().bytes()[0]);
        return {};
    };
    (void)g.register_vertex(path_t("/log/temp"), role_t::HANDLER, std::move(h));
    tr::graph::vertex_handle_t src =
        g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.subscribe(path_t("/sensor/temp"), path_t("/log/temp"));
    (void)g.write(src, make_value({0x55}));
    check(*sink_seen == 0x55, "write re-dispatches to the target vertex's on_write");
}

void test_field_write_settings() {
    std::printf("field-write :settings.<field>:\n");
    graph_t g;
    tr::graph::vertex_handle_t v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    const auto payload = std::array<std::byte, 8>{std::byte{0x88}, std::byte{0x13}};  // 5000 LE
    auto w = g.write(path_t("/sensor/temp:settings.deadline_ns"), value_tlv(payload));
    check(w.has_value(), "field-write returns OK");
    check(g.settings(v).deadline_ns == 5000, "deadline_ns updated to 5000 via field-write");
    check(!g.write(path_t("/sensor/temp:settings.bogus"), value_tlv(payload)).has_value(),
          "unknown settings field => SchemaNotFound");
}

void test_field_write_handle() {
    std::printf("Handle-based field-write (no strings on the hot path):\n");
    graph_t g;
    auto v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    // Parse the field path ONCE; reuse the vertex_t* handle + field_path_t thereafter
    // (no per-call string parse, no map lookup).
    const auto fp = path_t::parse("/sensor/temp:settings.deadline_ns");
    const std::array<std::byte, 8> le{std::byte{0x10}, std::byte{0x27}};  // 10000 LE
    check(g.write(v, fp->field(), value_tlv(le)).has_value(),
          "write(vertex_t*, field_path_t, view_t) returns OK");
    check(g.settings(v).deadline_ns == 10000,
          "deadline_ns updated via the handle (no string parse, no map lookup)");
    check(g.write(v, tr::graph::field_path_t{}, make_value({0x55})).has_value(),
          "an empty field_path_t is an ordinary value write");
}

void test_subscribe_via_field_write_and_unsubscribe() {
    std::printf("field-write :subscribers[] (wire-faithful) + unsubscribe:\n");
    graph_t g;
    auto sink_seen = std::make_shared<int>(0);
    tr::graph::handlers_t h;
    h.on_write = [sink_seen](const tr::view::rope_t& in) -> tr::graph::result_t<void> {
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

    auto unsub = g.write(path_t("/sensor/temp:subscribers[0]"), make_value({}));  // clear slot 0
    check(unsub.has_value(), "unsubscribe clears the slot");
    (void)g.write(src, make_value({0x20}));
    check(*sink_seen == 0x10, "no further delivery after unsubscribe");
}

void test_schema_read() {
    std::printf(":schema read (POINT descriptor):\n");
    graph_t g;
    (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    auto schema = g.read(path_t("/sensor/temp:schema"));
    check(schema.has_value(), ":schema read returns a value");
    auto point = tr::wire::decode(schema->only());
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

    // Behavior alignment (ADR-0049): the transient-local (durability==1) latch now fires
    // for LOCAL doors too — a callback subscriber and a target subscriber each receive
    // the LKV immediately at subscribe, exactly as a remote one does (was remote-only).
    {
        graph_t g;
        settings_t s;
        s.durability = 1;  // transient-local
        tr::graph::vertex_handle_t src =
            g.register_vertex(path_t("/tl"), role_t::STORED_VALUE, {}, s);
        (void)g.write(src, make_value({0x5A}));  // seed the LKV BEFORE subscribing

        auto seen = std::make_shared<int>(-1);
        auto on_latch = [seen](const tr::view::rope_t& v) {
            *seen = std::to_integer<int>(v.only().bytes()[0]);
        };
        (void)g.subscribe(path_t("/tl"), on_latch);
        check(*seen == 0x5A, "callback door: the LKV latches at subscribe");

        tr::graph::vertex_handle_t tgt = g.register_vertex(path_t("/tgt"), role_t::STORED_VALUE);
        (void)g.subscribe(path_t("/tl"), path_t("/tgt"));
        const auto latched = g.read(tgt);
        check(latched.has_value() && std::to_integer<int>(latched->only().bytes()[0]) == 0x5A,
              "target door: the LKV latches at subscribe (delivered as a write)");
    }

    // A volatile (durability==0, the default) vertex still does NOT latch.
    {
        graph_t g;
        tr::graph::vertex_handle_t src = g.register_vertex(path_t("/vol"), role_t::STORED_VALUE);
        (void)g.write(src, make_value({0x77}));
        auto fired = std::make_shared<int>(0);
        auto on_vol = [fired](const tr::view::rope_t&) { ++*fired; };
        (void)g.subscribe(path_t("/vol"), on_vol);
        check(*fired == 0, "no latch on a volatile vertex");
    }
}

void test_dispatch_cycle_cap() {
    std::printf("dispatch-depth cycle cap (in-process A->B->A terminates):\n");
    graph_t g;
    auto count = std::make_shared<int>(0);
    tr::graph::vertex_handle_t a = g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/b"), role_t::STORED_VALUE);
    // Mutual target subscriptions form a cycle; a counter callback on each level.
    (void)g.subscribe(path_t("/a"), path_t("/b"));
    (void)g.subscribe(path_t("/b"), path_t("/a"));
    auto on_hop = [count](const tr::view::rope_t&) { ++*count; };
    (void)g.subscribe(path_t("/a"), on_hop);
    (void)g.subscribe(path_t("/b"), on_hop);

    (void)g.write(a, make_value({0x01}));  // must terminate, not infinite-loop / stack-overflow
    check(*count > 1, "the cycle did dispatch (callbacks fired both ways)");
    check(*count <= tr::graph::kMaxDispatchDepth + 4, "re-dispatch is bounded by the depth cap");
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
    g.propagate(r);
    check(*ca == 1, "propagate(root) flushes the assigned descendant once (coalesced)");
    check(*cb == 0 && *cc == 0, "IF_NEWER: descendants not assigned since the sweep are skipped");

    // Self-clearing: a second sweep with nothing newly assigned delivers nothing.
    g.propagate(r);
    check(*ca == 1, "a clean subtree flushes nothing (write_seq did not advance)");

    // Selective subtree propagation: assign b and c, one propagate delivers exactly them.
    (void)g.assign(b, make_value({0x10}));
    (void)g.assign(c, make_value({0x20}));
    g.propagate(r);
    check(*cb == 1 && *cc == 1, "propagate flushes exactly the newly-assigned descendants");
    check(*ca == 1, "an unassigned descendant is not re-sent");

    // UNCONDITIONAL rides every sweep, even with no assignment since the last one.
    g.set_delivery_mode(a, delivery_mode_t::UNCONDITIONAL);
    g.propagate(r);
    check(*ca == 2, "UNCONDITIONAL is swept every time (even unassigned)");
    check(*cb == 1 && *cc == 1, "IF_NEWER siblings stay clean across an UNCONDITIONAL sweep");

    // EXPLICIT is never pulled in by an ancestor sweep; a direct propagate still delivers it
    // (the argument of propagate is always delivered — RFC-0008 §C).
    g.set_delivery_mode(b, delivery_mode_t::EXPLICIT);
    (void)g.assign(b, make_value({0x11}));
    g.propagate(r);
    check(*cb == 1, "EXPLICIT is never included by an ancestor sweep");
    g.propagate(b);
    check(*cb == 2, "a direct propagate on an EXPLICIT vertex delivers it");

    // write() remains the eager §D composition (assign then deliver the vertex).
    auto cw = std::make_shared<int>(0);
    auto w = g.register_vertex(path_t("/w"), role_t::STORED_VALUE);
    auto on_w = [cw](const tr::view::rope_t&) { ++*cw; };
    (void)g.subscribe(path_t("/w"), on_w);
    (void)g.write(w, make_value({0x77}));
    check(*cw == 1, "write() delivers immediately (assign + targeted propagate)");
}

int main() {
    test_path_parse();
    test_stored_value();
    test_stream();
    test_handler();
    test_await();
    test_subscribe_callback();
    test_subscribe_target();
    test_assign_propagate();
    test_field_write_settings();
    test_field_write_handle();
    test_subscribe_via_field_write_and_unsubscribe();
    test_admission_door_uniformity();
    test_schema_read();
    test_dispatch_cycle_cap();
    test_concurrent_stress();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
