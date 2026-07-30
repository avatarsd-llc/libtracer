/**
 * @file
 * @brief The leaf/branch fork of `graph_t::read`, and the state machine that now answers it
 *        without the map lock (#652).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `read` forks on "does this vertex have at least one REGISTERED direct child": a branch
 * serves the composed POINT tree of its descendants, a leaf serves its own last-known value.
 * That predicate used to be computed by walking the child list under `map_mutex_` shared —
 * one process-wide reader-writer lock on **every** read, which capped the whole process at
 * roughly 20 M reads/s no matter how disjoint the vertices were. It is now a bit on the
 * vertex, set when a child is filled and recomputed when one is retired.
 *
 * Setting is trivial and was already covered by `subtree_read_test`. **Clearing is not**, and
 * nothing in the suite exercised it: no existing test retires a vertex and then reads its
 * parent. That transition is the whole risk of this change — a stuck bit sends a leaf read
 * down the composed path (wrong shape, wrong bytes) or a branch read down the leaf path
 * (silently dropping every descendant). Each case below is one edge of that machine.
 *
 * The last test races readers against registration and retirement on the same vertex, which
 * is what makes the lock-free read carry its weight under ThreadSanitizer.
 */

#include <atomic>
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

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A one-byte value living in its own heap segment. */
[[nodiscard]] tr::view::view_t val_u8(std::uint8_t b) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(1);
    seg->bytes[0] = std::byte{b};
    return tr::view::view_t::over(std::move(seg));
}

/**
 * @brief Whether a read of @p p came back as the vertex's own scalar rather than a composed
 *        POINT tree.
 *
 * The leaf read returns exactly the bytes that were stored — one byte here. The composed
 * branch read returns a POINT TLV, which is never one byte long. Length alone separates them,
 * which keeps this test independent of the composed encoding `subtree_read_test` owns.
 */
[[nodiscard]] bool reads_as_leaf_scalar(graph_t& g, const char* p, std::uint8_t expect) {
    const auto r = g.read(path_t(p));
    if (!r.has_value()) return false;
    std::vector<std::byte> flat;
    for (const tr::view::view_t& l : (*r)->links()) {
        const auto s = l.bytes();
        flat.insert(flat.end(), s.begin(), s.end());
    }
    return flat.size() == 1 && flat[0] == std::byte{expect};
}

/** @brief Whether a read of @p p came back as something bigger than a bare scalar. */
[[nodiscard]] bool reads_as_composed(graph_t& g, const char* p) {
    const auto r = g.read(path_t(p));
    if (!r.has_value()) return false;
    std::size_t n = 0;
    for (const tr::view::view_t& l : (*r)->links()) n += l.bytes().size();
    return n > 1;
}

/** @brief The fork follows registration, in both directions. */
void test_fork_tracks_registration() {
    std::printf("the fork follows registration and retirement:\n");
    graph_t g;
    const auto dev = g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    (void)g.write(dev, val_u8(0x11));
    check(reads_as_leaf_scalar(g, "/dev", 0x11), "a vertex with no children reads as a leaf");

    const auto a = g.register_vertex(path_t("/dev/a"), role_t::STORED_VALUE);
    (void)g.write(a, val_u8(0x22));
    check(reads_as_composed(g, "/dev"), "registering a child turns the parent into a branch");

    const auto b = g.register_vertex(path_t("/dev/b"), role_t::STORED_VALUE);
    (void)g.write(b, val_u8(0x33));
    check(reads_as_composed(g, "/dev"), "a second child keeps it a branch");

    check(g.retire(a).has_value(), "retire the first child");
    check(reads_as_composed(g, "/dev"),
          "with one child left the parent is STILL a branch (the recompute must see the "
          "surviving sibling, not just the departing one)");

    check(g.retire(b).has_value(), "retire the last child");
    check(reads_as_leaf_scalar(g, "/dev", 0x11),
          "the parent is a LEAF again and serves its own value byte-identically");

    const auto c = g.register_vertex(path_t("/dev/c"), role_t::STORED_VALUE);
    (void)g.write(c, val_u8(0x44));
    check(reads_as_composed(g, "/dev"), "re-registering under it makes it a branch once more");
}

/** @brief Only DIRECT children count — a placeholder between does not promote a leaf. */
void test_placeholders_do_not_count() {
    std::printf("placeholders are not registered children:\n");
    graph_t g;
    const auto top = g.register_vertex(path_t("/top"), role_t::STORED_VALUE);
    (void)g.write(top, val_u8(0x55));
    // `/top/mid` is created as an unregistered PLACEHOLDER on the way to `/top/mid/leaf`.
    const auto leaf = g.register_vertex(path_t("/top/mid/leaf"), role_t::STORED_VALUE);
    (void)g.write(leaf, val_u8(0x66));
    check(reads_as_leaf_scalar(g, "/top", 0x55),
          "a registered GRANDchild behind a placeholder leaves the vertex a leaf");
    // The placeholder is invisible to `find`, so it never reaches the fork at all — the
    // bit it carries for its registered child is simply unobservable through `read`.
    check(!g.read(path_t("/top/mid")).has_value(),
          "the placeholder itself stays invisible to read, fork bit or no fork bit");
    check(reads_as_leaf_scalar(g, "/top/mid/leaf", 0x66),
          "and the registered grandchild reads as its own leaf");
}

/** @brief Retiring a subtree in one call must leave every surviving parent's bit right. */
void test_subtree_retire() {
    std::printf("a subtree retire clears the bit at every level it emptied:\n");
    graph_t g;
    const auto root = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    (void)g.write(root, val_u8(0x77));
    const auto mid = g.register_vertex(path_t("/s/mid"), role_t::STORED_VALUE);
    (void)g.write(mid, val_u8(0x88));
    const auto deep = g.register_vertex(path_t("/s/mid/deep"), role_t::STORED_VALUE);
    (void)g.write(deep, val_u8(0x99));
    check(reads_as_composed(g, "/s"), "before: /s is a branch");

    check(g.retire(mid).has_value(), "retire /s/mid, which takes /s/mid/deep with it");
    check(reads_as_leaf_scalar(g, "/s", 0x77),
          "/s is a leaf again — the subtree walk cleared its bit, not just the leaf's");
}

/** @brief Retiring an already-retired vertex must not corrupt the parent's bit. */
void test_idempotent_retire() {
    std::printf("a repeated retire does not disturb a surviving sibling:\n");
    graph_t g;
    const auto p = g.register_vertex(path_t("/i"), role_t::STORED_VALUE);
    (void)g.write(p, val_u8(0xAA));
    const auto x = g.register_vertex(path_t("/i/x"), role_t::STORED_VALUE);
    const auto y = g.register_vertex(path_t("/i/y"), role_t::STORED_VALUE);
    (void)g.write(x, val_u8(0xBB));
    (void)g.write(y, val_u8(0xCC));

    check(g.retire(x).has_value(), "retire /i/x");
    check(g.retire(x).has_value(), "retire /i/x again (idempotent per RFC-0009 §B.4)");
    check(reads_as_composed(g, "/i"),
          "/i is still a branch — a second retire of an already-retired child must not clear "
          "the bit that /i/y is holding up");
}

/**
 * @brief Readers racing registration and retirement on the vertex they are forking on.
 *
 * The fork read is lock-free while `fill` and `mark_unregistered` mutate the bit under the
 * graph's unique lock. Which side of a concurrent registration a reader lands on was never
 * ordered by anything a caller can observe, so the assertion is that every read is
 * WELL-FORMED — never that it saw a particular side.
 *
 * The overlap is asserted, not hoped for. A first version of this test ran 300 rounds against
 * four readers, finished in 5 ms, and asserted only `reads > 0` — which four freshly-spawned
 * threads satisfy before the writer has finished starting them. It passed while racing almost
 * nothing. `kMinReadsPerRound` is the fix: if the readers do not keep up with the writer by a
 * wide margin, the shape under test did not happen and the test says so instead of passing.
 */
void test_concurrent_fork() {
    constexpr std::size_t kRounds = 4000;
    constexpr std::size_t kReaders = 4;
    // Each round is a register + write + retire, all three taking the graph's UNIQUE lock; a
    // lock-free read is orders of magnitude cheaper, so anything below this means the readers
    // were starved or never scheduled and the interleaving was not exercised.
    constexpr std::size_t kMinReadsPerRound = 2;

    std::printf("readers forking while a writer registers and retires:\n");
    graph_t g;
    const auto p = g.register_vertex(path_t("/r"), role_t::STORED_VALUE);
    (void)g.write(p, val_u8(0xDD));

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> bad{0};
    std::atomic<std::size_t> reads{0};
    std::atomic<std::size_t> ready{0};
    std::vector<std::thread> pool;
    for (std::size_t i = 0; i < kReaders; ++i) {
        pool.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            std::size_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (!g.read(path_t("/r")).has_value()) bad.fetch_add(1, std::memory_order_relaxed);
                ++n;
            }
            reads.fetch_add(n, std::memory_order_relaxed);
        });
    }
    // Do not start the writer until every reader is in its loop, or the early rounds race
    // nothing but thread start-up.
    while (ready.load(std::memory_order_acquire) < kReaders) { /* spin to a common start */
    }
    for (std::size_t round = 0; round < kRounds; ++round) {
        const auto kid = g.register_vertex(path_t("/r/kid"), role_t::STORED_VALUE);
        (void)g.write(kid, val_u8(0xEE));
        (void)g.retire(kid);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : pool) t.join();

    std::printf("    %zu rounds x %zu readers -> %zu reads, %zu malformed\n", kRounds, kReaders,
                reads.load(), bad.load());
    check(bad.load() == 0, "every concurrent read of the forking vertex was well-formed");
    check(reads.load() >= kRounds * kMinReadsPerRound,
          "the readers kept up with the writer, so the interleaving actually happened");
    check(reads_as_leaf_scalar(g, "/r", 0xDD), "and the vertex settles back to a leaf");
}

}  // namespace

/** @brief Run the fork state-machine probes. */
int main() {
    std::printf("graph_t::read leaf/branch fork (#652):\n");
    test_fork_tracks_registration();
    test_placeholders_do_not_count();
    test_subtree_retire();
    test_idempotent_retire();
    test_concurrent_fork();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
