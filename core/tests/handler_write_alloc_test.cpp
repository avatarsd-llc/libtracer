/**
 * @file
 * @brief #1505 — a HANDLER write allocates exactly ONE block fewer per write than a retaining
 *        write, at EVERY link count. The non-retaining role is never the more expensive one.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A `role_t::HANDLER` publishes no LKV, so its write must cost one allocation less than a
 * `role_t::STORED_VALUE` write of the same value: the `make_shared` publish, and nothing else.
 * Until #1505 that held only while the value fit the rope's inline link capacity. Past it the
 * handler leg's notify clone (`try_clone_rope` into a `notify` rope, taken before `store_value`
 * so the caller's rope could be moved from) grew a `std::vector<view_t>` on **every write** and
 * gave the ordering back: at 3 links and beyond a HANDLER allocated exactly as much as the
 * retaining role it was supposed to undercut, and was ~40 ns/write slower.
 *
 * The fix deletes the clone — `store_value`'s HANDLER leg only READS `value` and returns the
 * null "consumed" sentinel, so the caller's rope is still live and is delivered directly.
 *
 * @section instrument What makes this non-vacuous
 *
 * The instrument is a global `operator new` counter, on the `edge_cold_half_share_test`
 * precedent (every allocating and deallocating form replaced — a hole would make the very
 * allocation under test invisible, and a missing delete form is an ASan alloc-dealloc-mismatch).
 * The assertion is a **difference**, not a budget: both arms build the same borrowed-view rope
 * inside the armed window and both dispatch the same fan-out, so every term except the LKV
 * publish and the vanished clone cancels. What is left is exactly 1 per write, and the old code
 * reddens the ladder's top three rungs by producing 0.
 *
 * Three positive controls, because each failure mode has a way of passing quietly:
 *
 *  - **The arms are proven live before they are counted** — the HANDLER's `on_write` must have
 *    fired and the subscriber must have been delivered to, or the point reports the cost of not
 *    delivering.
 *  - **The counter must be live** — an armed retaining write allocates SOMETHING (its LKV).
 *  - **The ladder must straddle the inline knee.** If every rung stayed inline, the old code
 *    would satisfy the difference too and the test would guard nothing. So the retaining arm's
 *    per-write count at the top of the ladder must exceed its count at one link — that is the
 *    spill, measured rather than asserted against `rope_t`'s private capacity constant.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <vector>

#include "libtracer/rope.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

/** @brief Global-new call counter, live only while @ref g_arm is set. */
std::size_t g_allocs = 0;
bool g_arm = false;

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm) ++g_allocs;
    return std::malloc(n == 0 ? 1 : n);
}

/** @brief The aligned counted allocation — `aligned_alloc` only for a genuinely OVER-aligned
 *         request (which `free` accepts), `malloc` for a fundamental one. */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm) ++g_allocs;
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}

}  // namespace

void* operator new(std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    void* const p = counted_aligned(n, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {

using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::result_t;
using tr::graph::role_t;
using tr::graph::write_ctx_t;
using tr::testing::check;
using tr::view::rope_t;
using tr::view::view_t;

/** @brief Writes per counted point; the settle pass takes the one-offs out of the window. */
constexpr std::size_t kWrites = 200;
/** @brief Bytes per link — irrelevant to the count, but a realistic payload rather than 1 B. */
constexpr std::size_t kBytes = 64;

/**
 * @brief Stable per-link buffers, so building the written value copies nothing.
 *
 * The rope is a borrowed view over these — the only allocation `make()` can make is the link
 * vector itself once the count passes the rope's inline capacity, which is the spill this
 * ladder is built to cross. It is charged to BOTH arms identically, so it cancels in the
 * difference the assertion takes.
 */
struct fixture_t {
    std::vector<std::vector<std::byte>> buffers;

    explicit fixture_t(std::size_t links) {
        for (std::size_t i = 0; i < links; ++i) buffers.emplace_back(kBytes, std::byte{0xAB});
    }

    /** @brief A fresh borrowed-view rope of `buffers.size()` links. */
    [[nodiscard]] rope_t make() const {
        rope_t r;
        (void)r.try_reserve(buffers.size());
        for (const std::vector<std::byte>& b : buffers)
            r.append(view_t::over(tr::view::borrow_const(std::span<const std::byte>(b))));
        return r;
    }
};

/**
 * @brief Allocations per write at one (role, link-count) point, or 0 if the arm is not live.
 *
 * Fan-out 1 — a real subscriber, because the delivery the clone used to feed has to exist for
 * the comparison to be about the write path an embedder actually runs. The count does not
 * depend on the width: the clone was taken before the width was ever consulted.
 */
[[nodiscard]] double per_write_allocs(bool handler, std::size_t links) {
    graph_t g;
    const path_t src = *path_t::parse("/t/alloc/src");
    std::atomic<std::uint64_t> hits{0};
    auto v = [&] {
        if (!handler) return g.register_vertex(src, role_t::STORED_VALUE);
        handlers_t h;
        h.on_write = [&hits](const rope_t&, const write_ctx_t&) -> result_t<void> {
            hits.fetch_add(1, std::memory_order_relaxed);
            return {};
        };
        return g.register_vertex(src, role_t::HANDLER, std::move(h));
    }();
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const rope_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
    (void)g.subscribe(src, cb);

    const fixture_t fx{links};
    for (std::size_t i = 0; i < 50; ++i) (void)g.write(v, fx.make());  // settle the one-offs
    check(recv.load() > 0, "the subscriber is delivered to (the arm is wired)");
    check(!handler || hits.load() > 0, "the handler's on_write actually ran (the arm is wired)");
    if (recv.load() == 0 || (handler && hits.load() == 0)) return 0.0;

    g_allocs = 0;
    g_arm = true;
    for (std::size_t i = 0; i < kWrites; ++i) (void)g.write(v, fx.make());
    g_arm = false;
    return static_cast<double>(g_allocs) / kWrites;
}

/**
 * @brief The ladder: HANDLER must be exactly one allocation per write cheaper, at every rung.
 */
void test_handler_write_is_one_allocation_cheaper() {
    std::printf("a HANDLER write allocates exactly one block fewer than a retaining write:\n");
    constexpr std::size_t kLadder[] = {1, 2, 3, 4, 8};
    double stored_at_one = 0.0;
    double stored_at_top = 0.0;

    for (const std::size_t links : kLadder) {
        const double stored = per_write_allocs(/*handler=*/false, links);
        const double handler = per_write_allocs(/*handler=*/true, links);
        std::printf("    %zu link(s): stored %.3f alloc/write, handler %.3f alloc/write\n", links,
                    stored, handler);
        if (links == kLadder[0]) stored_at_one = stored;
        stored_at_top = stored;
        check(stored > 0.0, "the counter is live (an armed retaining write allocates its LKV)");
        // Exact, not a bound: every other term of the two arms is identical by construction,
        // so the difference IS the LKV publish the handler skips. Before #1505 the notify
        // clone gave that one back at 3 links and beyond, making this difference 0.
        check(stored - handler == 1.0,
              "the handler skips the LKV publish and pays nothing back for it");
    }

    // The knee, measured. Without a rung past the rope's inline capacity the old clone would
    // never have allocated either, and the assertions above would guard nothing.
    std::printf("    retaining arm: %.3f alloc/write at 1 link, %.3f at %zu links\n", stored_at_one,
                stored_at_top, kLadder[std::size(kLadder) - 1]);
    check(stored_at_top > stored_at_one,
          "the ladder straddles the rope's inline link capacity (the spill is real)");
}

}  // namespace

int main() {
    std::printf("#1505 handler write allocation ordering\n\n");
    test_handler_write_is_one_allocation_cheaper();
    return tr::testing::summary("handler_write_alloc");
}
