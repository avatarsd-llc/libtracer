/**
 * @file
 * @brief Unit tests for the nothrow failable-block seam (mem_source.hpp, #551).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The seam exists because `std::pmr::memory_resource` structurally cannot report
 * exhaustion by value on the profile that ships: its `allocate` is annotated
 * `returns_nonnull`, so a caller's null check is deleted at `-Os`, and a throw
 * reaches ESP-IDF's `__cxa_throw`→`abort()` stub. What these tests pin is
 * therefore the *contract* — nothrow, `nullptr` on exhaustion, alignment honored,
 * sized reclaim — plus the two structural properties the whole argument rests on:
 * the type is NOT a `std::pmr::memory_resource`, and the default source needs no
 * dynamic initialization.
 */

#include "libtracer/mem_source.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <new>
#include <span>
#include <type_traits>

#include "libtracer/graph.hpp"
#include "test_support.hpp"

// ASan/TSan intercept `operator new` and treat a request past their allocator cap
// as a fatal error rather than returning null, so the one probe that asks for an
// unservable block has to sit out those builds (see its call site).
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define LIBTRACER_TEST_UNDER_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#ifndef LIBTRACER_TEST_UNDER_SANITIZER
#define LIBTRACER_TEST_UNDER_SANITIZER 1
#endif
#endif

namespace {

using tr::testing::check;

/**
 * @brief A counting source with a hard block budget — the bounded-node stand-in and
 *        the failure-injection seam for every later migration slice.
 */
class budget_source_t final : public tr::mem::block_source_t {
   public:
    explicit budget_source_t(int budget) noexcept
        : tr::mem::block_source_t("budget"), left_(budget) {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (left_ <= 0) {
            ++refused_;
            return nullptr;
        }
        --left_;
        ++served_;
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        ++released_;
        ::operator delete(p, bytes, std::align_val_t{align});
    }

    int served_ = 0;   /**< @brief Blocks handed out. */
    int refused_ = 0;  /**< @brief Requests answered `nullptr`. */
    int released_ = 0; /**< @brief Blocks returned. */

   private:
    int left_;
};

/** @brief The seam must NOT be a `std::pmr::memory_resource` — that is the whole ruling. */
static_assert(!std::is_base_of_v<std::pmr::memory_resource, tr::mem::block_source_t>,
              "block_source_t must not inherit memory_resource: its returns_nonnull "
              "allocate() would stay callable and its null check is deleted at -Os");
static_assert(!std::is_convertible_v<tr::mem::block_source_t*, std::pmr::memory_resource*>,
              "a block_source_t* must not silently pass where a memory_resource* is wanted");

/**
 * @brief The default source must be CONSTANT-initializable: `constinit` is a hard
 *        compile error if the object would need dynamic initialization, which is
 *        exactly the `__cxa_guard` word + acquire fence the definition avoids.
 */
constinit tr::mem::heap_source_t g_constinit_probe{};

}  // namespace

int main() {
    std::printf("mem_source — the nothrow failable-block seam (#551):\n");

    tr::mem::block_source_t& heap = tr::mem::heap_source();
    check(std::strcmp(heap.name(), "heap") == 0, "the default source names itself \"heap\"");
    check(&heap == &tr::mem::heap_source(), "heap_source() is a stable singleton");

    // Alignment is honored and the block is writable for its full length.
    for (const std::size_t align : {alignof(std::max_align_t), std::size_t{64}}) {
        void* p = heap.try_alloc(96, align);
        check(p != nullptr, "heap try_alloc succeeds");
        check(reinterpret_cast<std::uintptr_t>(p) % align == 0, "block honors the alignment");
        std::memset(p, 0xA5, 96);
        heap.release(p, 96, align);
    }

    // THE contract: exhaustion is a value, not a throw and not an abort. An
    // allocation this size cannot be served, and `::operator new(nothrow)` must
    // answer nullptr rather than reaching __cxa_throw.
    static_assert(noexcept(heap.try_alloc(1, 1)), "try_alloc is noexcept");
    static_assert(noexcept(heap.release(nullptr, 1, 1)), "release is noexcept");
#if defined(LIBTRACER_TEST_UNDER_SANITIZER)
    // Skipped, not weakened: a sanitizer's allocator treats an over-cap request as
    // a hard error and exits before `operator new` can answer, so the probe would
    // fail the RUN rather than the assertion. The property still has coverage —
    // every non-sanitizer job runs the line below, and `budget_source_t` exercises
    // the same nullptr-is-BACKPRESSURE path unconditionally just underneath.
    std::printf("  [SKIP] unservable-request probe (sanitizer allocator aborts first)\n");
#else
    void* huge = heap.try_alloc(static_cast<std::size_t>(-1) / 2, alignof(std::max_align_t));
    check(huge == nullptr, "an unservable request returns nullptr (never throws)");
#endif

    // A bounded source: served until the budget runs out, then a clean refusal.
    budget_source_t budget(2);
    void* a = budget.try_alloc(32, alignof(std::max_align_t));
    void* b = budget.try_alloc(32, alignof(std::max_align_t));
    void* c = budget.try_alloc(32, alignof(std::max_align_t));
    check(a != nullptr && b != nullptr, "a bounded source serves within its budget");
    check(c == nullptr, "past the budget it returns nullptr — the BACKPRESSURE signal");
    check(budget.served_ == 2 && budget.refused_ == 1, "the source observed both outcomes");
    budget.release(a, 32, alignof(std::max_align_t));
    budget.release(b, 32, alignof(std::max_align_t));
    check(budget.released_ == 2, "sized reclaim reaches the source");

    // A source that counts nothing answers all-zero rather than a fabricated number —
    // the optional-virtual default (#932's mould, #1503).
    check(budget.stats().capacity == 0 && budget.stats().refused == 0,
          "a source that does not override stats() reports nothing, not a guess");

    // bump_source_t's census (#1503): the caller's buffer is the ceiling, occupancy is
    // used-polarity, peak survives a reset, and a refusal is counted only when the
    // request dies at BOTH this source and its upstream.
    {
        alignas(std::max_align_t) std::byte buf[128]{};
        tr::mem::bump_source_t bump{std::span<std::byte>(buf), tr::mem::null_source()};
        check(bump.stats().capacity == 128, "capacity is the span the caller handed over");
        check(bump.stats().in_use == 0 && bump.stats().peak == 0, "and nothing is carved yet");

        void* const p = bump.try_alloc(96, 8);
        check(p != nullptr, "the buffer serves the request");
        check(bump.stats().in_use == 96, "in_use follows the cursor (used-polarity)");
        check(bump.stats().peak == 96, "peak reads through to the live cursor");

        check(bump.try_alloc(64, 8) == nullptr,
              "a request the buffer cannot fit reaches the null upstream and is refused");
        check(bump.stats().refused == 1, "which IS counted");
        check(bump.stats().largest_refused == 64, "with the refused size recorded");

        bump.reset();
        check(bump.stats().in_use == 0, "reset rewinds the cursor");
        check(bump.stats().peak == 96, "but peak REMEMBERS the cycle that ended");
        check(bump.stats().refused == 1, "and the refusal counter is monotonic across cycles");

        // Against an unbounded upstream the same request is served, so nothing is
        // refused — the spill is the upstream's census, not this one's.
        alignas(std::max_align_t) std::byte small[16]{};
        tr::mem::bump_source_t spilling{std::span<std::byte>(small), tr::mem::heap_source()};
        void* const q = spilling.try_alloc(64, 8);
        check(q != nullptr, "the upstream serves what the buffer cannot");
        check(spilling.stats().refused == 0, "so nobody was refused");
        check(spilling.stats().in_use == 0, "and the spilled bytes are NOT this source's in_use");
        spilling.release(q, 64, 8);
    }

    // Wiring: the graph defaults to the heap source, and an injected source is the
    // one the graph reports. The parameter is APPENDED, so the two shorter forms
    // must still compile unchanged — that is the zero-diff migration promise.
    {
        tr::graph::graph_t g_default;
        check(&g_default.control_source() == &tr::mem::heap_source(),
              "graph_t{} defaults its control seam to heap_source()");

        tr::graph::graph_t g_mr{std::pmr::get_default_resource()};
        check(&g_mr.control_source() == &tr::mem::heap_source(),
              "graph_t{&mr} (the shipping host's spelling) still compiles and defaults");

        tr::graph::graph_t g_two{std::pmr::get_default_resource(), &tr::mem::heap_backend()};
        check(&g_two.control_source() == &tr::mem::heap_source(),
              "graph_t{&mr, &backend} still compiles and defaults");

        budget_source_t injected(8);
        tr::graph::graph_t g_ctl{std::pmr::get_default_resource(), &tr::mem::heap_backend(),
                                 &injected};
        check(&g_ctl.control_source() == &injected,
              "an injected source is the one the graph holds");
        check(std::strcmp(g_ctl.control_source().name(), "budget") == 0,
              "the injected source reports its own name");
        // Constructing a graph draws nothing from the seam — the first consumer is the
        // branch-write decode (graph.cpp), which only reaches the seam past its 4 KiB
        // stack buffer. Registration is still to migrate.
        check(injected.served_ == 0, "constructing a graph draws no control blocks");
    }

    return tr::testing::summary("mem_source");
}
