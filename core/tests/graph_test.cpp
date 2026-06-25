// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// L4 graph-runtime tests (M3a): path parse/canonicalize, the three vertex roles
// (stored-value, stream, handler), read/write/await semantics, lock-free LKV
// clone-on-read, and a multithreaded stress over a shared vertex. The stress is
// the verification M2 deferred: built under TSan it proves the lock-free LKV +
// atomic segment refcounts race-free, and under ASan+UBSan leak/UB-free.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tracer::graph::Graph;
using tracer::graph::Path;
using tracer::graph::Role;
using tracer::graph::Settings;
using tracer::graph::Status;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// A View over a fresh, owned heap segment holding `bytes`.
tracer::View make_value(std::span<const std::byte> bytes) {
    tracer::SegmentPtr seg = tracer::mem::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tracer::View::over(std::move(seg));
}

tracer::View make_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> v;
    v.reserve(bytes.size());
    for (std::uint8_t b : bytes) v.push_back(std::byte{b});
    return make_value(v);
}

void test_path_parse() {
    std::printf("Path parse / canonicalize / field tail:\n");
    const auto p = Path::parse("/sensor/temp");
    check(p.has_value(), "valid path parses");
    // Canonical PATH payload: 02 00 06 00 'sensor' 02 00 04 00 'temp' = 18 bytes.
    check(p && p->key().size() == 18, "canonical key is 18 bytes for /sensor/temp");
    check(p && p->segment_count() == 2, "two segments");

    // Trailing slash canonicalizes to the same key.
    const auto p2 = Path::parse("/sensor/temp/");
    check(p2 && p2->key().size() == p->key().size() &&
              std::memcmp(p2->key().data(), p->key().data(), p->key().size()) == 0,
          "trailing slash canonicalizes to the same key");

    check(!Path::parse("sensor/temp").has_value(), "unrooted path rejected");
    check(!Path::parse("/sensor//temp").has_value(), "empty segment // rejected");
    check(Path::parse("/").has_value() && Path::parse("/")->segment_count() == 0,
          "root '/' is zero segments");

    const auto f = Path::parse("/sensor/temp:settings.deadline_ns");
    check(f && f->field().steps.size() == 2 && f->field().steps[0].name == "settings" &&
              f->field().steps[1].name == "deadline_ns",
          "field tail :settings.deadline_ns parses to two steps");
    const auto app = Path::parse("/s:subscribers[]");
    check(app && app->field().steps.size() == 1 && app->field().steps[0].append,
          "subscribers[] parses as an append step");
    const auto idx = Path::parse("/s:subscribers[3]");
    check(idx && idx->field().steps[0].indexed && !idx->field().steps[0].append &&
              idx->field().steps[0].index == 3,
          "subscribers[3] parses with index 3");
}

void test_stored_value() {
    std::printf("Stored-value vertex (read/write, clone-on-read):\n");
    Graph g;
    const auto path = Path::parse("/sensor/temp");
    auto reg = g.register_vertex(*path, Role::StoredValue);
    check(reg.has_value(), "register stored-value vertex");
    tracer::graph::Vertex* v = *reg;

    check(!g.read(v).has_value() && g.read(v).error() == Status::NotFound,
          "read before any write => NotFound");

    auto w = g.write(v, make_value({0xAA, 0xBB, 0xCC}));
    check(w.has_value(), "write succeeds");

    auto r = g.read(v);
    check(r.has_value() && r->bytes().size() == 3, "read returns the written value");
    check(r && std::to_integer<int>(r->bytes()[0]) == 0xAA, "value byte 0 == 0xAA");
    // The LKV holds one reference; read returned an independent clone => use_count 2.
    check(r && r->owner.use_count() == 2, "read is a clone (segment use_count == 2), not a copy");

    check(!g.register_vertex(*path, Role::StoredValue).has_value(), "re-register same path fails");
    check(g.register_vertex(*path, Role::StoredValue).error() == Status::PathInUse,
          "duplicate registration reports PathInUse");
}

void test_stream() {
    std::printf("Stream vertex (bounded history ring):\n");
    Graph g;
    const auto path = Path::parse("/log/events");
    Settings s;
    s.history_keep_last = 3;
    tracer::graph::Vertex* v = *g.register_vertex(*path, Role::Stream, {}, s);

    for (std::uint8_t i = 1; i <= 5; ++i) (void)g.write(v, make_value({i}));

    auto hist = g.history(v);
    check(hist.has_value() && hist->size() == 3, "history bounded to keep_last = 3");
    check(hist && std::to_integer<int>((*hist)[0].bytes()[0]) == 3, "oldest kept is the 3rd write");
    check(hist && std::to_integer<int>((*hist)[2].bytes()[0]) == 5, "newest kept is the 5th write");
    auto latest = g.read(v);
    check(latest && std::to_integer<int>(latest->bytes()[0]) == 5, "read returns the latest (5)");
}

void test_handler() {
    std::printf("Handler vertex (user on_read / on_write seam):\n");
    Graph g;
    const auto path = Path::parse("/compute/answer");
    auto written = std::make_shared<std::vector<std::byte>>();
    tracer::graph::Handlers h;
    h.on_read = [] { return make_value({0x2A}); };  // always 42
    h.on_write = [written](const tracer::View& in) -> tracer::graph::Result<void> {
        const auto b = in.bytes();
        written->assign(b.begin(), b.end());
        return {};
    };
    tracer::graph::Vertex* v = *g.register_vertex(*path, Role::Handler, std::move(h));

    auto r = g.read(v);
    check(r && std::to_integer<int>(r->bytes()[0]) == 0x2A, "on_read supplies the value (42)");
    (void)g.write(v, make_value({0x99}));
    check(written->size() == 1 && std::to_integer<int>((*written)[0]) == 0x99,
          "on_write receives the written bytes");
}

void test_await() {
    std::printf("await (blocks until next write; times out otherwise):\n");
    Graph g;
    tracer::graph::Vertex* v = *g.register_vertex(*Path::parse("/sensor/temp"), Role::StoredValue);

    // A writer publishes shortly after we begin awaiting.
    std::thread writer([&] {
        std::this_thread::sleep_for(40ms);
        (void)g.write(v, make_value({0x7E}));
    });
    auto r = g.await(v, 2s);
    writer.join();
    check(r.has_value() && std::to_integer<int>(r->bytes()[0]) == 0x7E,
          "await wakes on a concurrent write and delivers it");

    tracer::graph::Vertex* idle =
        *g.register_vertex(*Path::parse("/sensor/idle"), Role::StoredValue);
    auto t = g.await(idle, 20ms);
    check(!t.has_value() && t.error() == Status::Timeout, "await times out with no write");
}

void test_concurrent_stress() {
    std::printf("Concurrent stress (lock-free LKV under contention):\n");
    Graph g;
    tracer::graph::Vertex* v = *g.register_vertex(*Path::parse("/stress/v"), Role::StoredValue);

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
                if (rr && rr->bytes().size() != 8) return;  // leaves reads_done short => FAIL
            }
            reads_done.fetch_add(kReadsEach, std::memory_order_relaxed);
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    check(reads_done.load() == static_cast<long>(kReaders) * kReadsEach,
          "all reads completed without crash under contention");
    auto fin = g.read(v);
    check(fin.has_value() && fin->bytes().size() == 8, "final read returns a valid 8-byte value");
}

}  // namespace

int main() {
    test_path_parse();
    test_stored_value();
    test_stream();
    test_handler();
    test_await();
    test_concurrent_stress();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
