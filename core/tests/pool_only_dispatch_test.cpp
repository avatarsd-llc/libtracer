/**
 * @file
 * @brief The SINGLE-MEMBER (`LIBTRACER_BACKEND_SET_POOL_ONLY`) module-set destroy
 *        dispatch (#922, ADR-0047 §2): the folded fast path must still be a fast
 *        path, not a correctness dependency.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Nothing else in the suite compiles `backend_set.cpp` with the POOL_ONLY fold, so the
 * variant shipped with an unconditional `static_cast<pool_t*>(seg->backend)` and nobody
 * noticed: a POOL_ONLY target still meets segments whose backend is NOT a `pool_t`.
 * `synchronized_pool_t` re-points every segment it hands out to ITSELF with an `UNKNOWN`
 * tag (`%mem_pool.hpp`) exactly so reclaim takes the locked virtual `destroy`, and it is a
 * `mem_backend_t` holding a `pool_t` **member** at a nonzero offset — reinterpreting it
 * reads `slab_`/`stride_` from the wrong offsets and skips the critical section. A
 * `tr::view::borrow()`ed segment and any user backend are the same story.
 *
 * Four checks, in escalating blast radius so the ablated run dies on the SAFE one first:
 *   1. FAST PATH INTACT — a plain `pool_t` segment still reclaims through the devirtualized
 *      POOL leg and its slot returns to the free list.
 *   2. LOCKED VIRTUAL DESTROY — a counting sync policy proves the synchronized pool's
 *      critical section is entered on reclaim (2 acquisitions per alloc/destroy pair), and
 *      the inner pool's own state (`capacity()`) survives. Under the unconditional cast the
 *      reinterpret writes the wild slot index over `inner_.slot_count_`, so BOTH halves of
 *      this check are deterministic — no sanitizer needed.
 *   3. USER BACKEND — an `UNKNOWN`-tagged backend's virtual `destroy` runs.
 *   4. BORROWED — `tr::view::borrow()`'s control block is reclaimed and the caller's bytes
 *      are never touched (the leak half is what a `-fsanitize=address` run adds).
 *
 * The target links `backend_set.cpp` + `mem_pool.cpp` directly rather than `libtracer`,
 * because the library's own `backend_set.o` carries the multi-member dispatch — the same
 * shape `substrate_test_no_atomic` uses for its ABI-changing define.
 */
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/backend.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/segment.hpp"
#include "test_support.hpp"

namespace {

using tr::mem::alloc_hint_t;
using tr::mem::backend_tag;
using tr::mem::mem_backend_t;
using tr::mem::pool_t;
using tr::mem::spin_sync_t;
using tr::mem::synchronized_pool_t;
using tr::view::segment_ptr_t;
using tr::view::segment_t;

using tr::testing::check_quiet;

/**
 * @brief A sync policy that COUNTS acquisitions of the real host critical section.
 *
 * The instrument for check 2: the count is how we see that reclaim went through
 * `synchronized_pool_t::destroy` (virtual, locked) and not the reinterpreted
 * `pool_t::destroy` beside it.
 */
struct counting_sync_t {
    static constexpr bool is_isr_safe = false;   /**< @brief Spin => not ISR. */
    static constexpr bool is_nonblocking = true; /**< @brief Spin => no syscall, no OS wait. */
    static constexpr const char* name = "pool_only_count_sync"; /**< @brief Backend name. */
    static inline std::atomic<std::size_t> acquisitions{0};     /**< @brief Lock count. */

    /** @brief Enter the counted critical section. */
    void lock() noexcept {
        acquisitions.fetch_add(1, std::memory_order_relaxed);
        inner_.lock();
    }
    /** @brief Leave it. */
    void unlock() noexcept { inner_.unlock(); }

   private:
    spin_sync_t inner_{};
};

static_assert(tr::mem::pool_sync_policy<counting_sync_t>, "the counting policy models the seam");

/**
 * @brief A user backend outside the fast set: `UNKNOWN`-tagged, counts its reclaims.
 *
 * The generic half of the same defect — a POOL_ONLY target that injects ANY backend of its
 * own (the whole point of the L0 seam) had its `destroy` reinterpreted as `pool_t`'s.
 */
class counting_backend_t final : public mem_backend_t {
   public:
    counting_backend_t() noexcept : mem_backend_t("pool_only_test_backend") {}

    /** @brief Reclaim the control block, counting the call. */
    void destroy(segment_t* seg) noexcept override {
        ++destroys;
        delete seg;
    }
    /** @brief Deliberately outside the fast set, so dispatch must take the virtual leg. */
    [[nodiscard]] backend_tag tag() const noexcept override { return backend_tag::UNKNOWN; }

    std::size_t destroys = 0; /**< @brief Reclaims observed. */
};

/** @brief Check 1 — the devirtualized POOL leg still works and recycles its slot. */
void plain_pool_fast_path() {
    std::vector<std::byte> slab(4096);
    pool_t pool(std::span<std::byte>(slab), 64);
    const std::size_t cap = pool.capacity();
    check_quiet(cap >= 2, "the test slab carves at least two slots");

    segment_t* raw = pool.alloc(32);
    check_quiet(raw != nullptr, "pool_t::alloc hands out a slot");
    check_quiet(raw->btag == backend_tag::POOL, "a pool segment carries the POOL tag");
    {
        segment_ptr_t held = segment_ptr_t::adopt(raw);
        check_quiet(pool.available() == cap - 1, "the slot is out while the handle lives");
    }
    check_quiet(pool.available() == cap, "POOL_ONLY reclaim returns the slot to the free list");
}

/**
 * @brief Check 2 — the synchronized pool's LOCKED virtual destroy runs on reclaim.
 *
 * The load-bearing case. Returns false when it fails so the caller can stop before the
 * wilder checks: under the ablation the reinterpreted `destroy` has already written a
 * garbage slot index over the inner pool's `slot_count_`.
 */
bool synchronized_pool_takes_the_lock() {
    std::vector<std::byte> slab(4096);
    synchronized_pool_t<counting_sync_t> pool(std::span<std::byte>(slab), 64);
    const std::size_t cap = pool.capacity();

    counting_sync_t::acquisitions.store(0, std::memory_order_relaxed);
    segment_t* raw = pool.alloc(32);
    check_quiet(raw != nullptr, "synchronized_pool_t::alloc hands out a slot");
    if (raw == nullptr) return false;
    check_quiet(raw->btag == backend_tag::UNKNOWN,
                "a synchronized-pool segment is re-tagged UNKNOWN for the virtual leg");
    check_quiet(raw->backend == static_cast<mem_backend_t*>(&pool),
                "a synchronized-pool segment is re-pointed at the synchronized pool");
    { segment_ptr_t held = segment_ptr_t::adopt(raw); }

    const std::size_t locks = counting_sync_t::acquisitions.load(std::memory_order_relaxed);
    check_quiet(locks == 2,
                "reclaim entered the critical section (alloc + destroy == 2 acquisitions)");
    check_quiet(pool.capacity() == cap, "the inner pool's slot count survived the reclaim");
    if (locks != 2 || pool.capacity() != cap) {
        std::printf("       locks=%zu (want 2), capacity=%zu (want %zu)\n", locks, pool.capacity(),
                    cap);
        return false;
    }

    // The slot really came back: the whole pool is allocatable again.
    std::vector<segment_ptr_t> held;
    for (std::size_t i = 0; i < cap; ++i) {
        segment_t* s = pool.alloc(32);
        check_quiet(s != nullptr, "every slot is re-allocatable after a synchronized reclaim");
        if (s == nullptr) return false;
        held.push_back(segment_ptr_t::adopt(s));
    }
    return true;
}

/** @brief Check 3 — a user (`UNKNOWN`-tagged) backend's virtual destroy runs. */
void user_backend_virtual_destroy() {
    counting_backend_t backend;
    std::array<std::byte, 16> bytes{};
    {
        segment_ptr_t held =
            segment_ptr_t::adopt(new segment_t(&backend, std::span<std::byte>(bytes)));
    }
    check_quiet(backend.destroys == 1, "a user backend's virtual destroy ran under POOL_ONLY");
}

/** @brief Check 4 — a borrowed segment reclaims its control block and leaves the bytes alone. */
void borrowed_segment_reclaim() {
    std::array<std::byte, 32> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>(i + 1);
    {
        segment_ptr_t held = tr::view::borrow(std::span<std::byte>(bytes));
        check_quiet(static_cast<bool>(held), "borrow() produced a segment");
        check_quiet(held->btag == backend_tag::BORROWED,
                    "a borrowed segment carries the BORROWED tag");
    }
    bool intact = true;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        intact = intact && bytes[i] == static_cast<std::byte>(i + 1);
    }
    check_quiet(intact, "borrowed bytes are untouched by the POOL_ONLY reclaim");
}

}  // namespace

int main() {
    plain_pool_fast_path();
    if (!synchronized_pool_takes_the_lock()) {
        std::printf(
            "pool_only_dispatch: FAILED (%d) — the synchronized pool's locked destroy was "
            "bypassed; skipping the checks whose ablation corrupts memory\n",
            tr::testing::failures());
        return 1;
    }
    user_backend_virtual_destroy();
    borrowed_segment_reclaim();

    return tr::testing::summary("pool_only_dispatch");
}
