/**
 * @file
 * @brief Unit tests for the nothrow control-plane block seam (mem_source.hpp, #551).
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
#include <type_traits>

#include "libtracer/graph.hpp"

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

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
    std::printf("mem_source — the nothrow control-plane block seam (#551):\n");

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
    void* huge = heap.try_alloc(static_cast<std::size_t>(-1) / 2, alignof(std::max_align_t));
    check(huge == nullptr, "an unservable request returns nullptr (never throws)");
    static_assert(noexcept(heap.try_alloc(1, 1)), "try_alloc is noexcept");
    static_assert(noexcept(heap.release(nullptr, 1, 1)), "release is noexcept");

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
        // No call site draws from the seam yet (slice 1 wires it only), so a graph
        // that merely exists must not have consumed the injected budget. When the
        // first allocation migrates, THIS assertion is the one that changes.
        check(injected.served_ == 0, "slice 1: constructing a graph draws no control blocks yet");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
