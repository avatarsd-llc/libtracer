/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The shared vertex lock STRIPE table (#361 §2): the mutex + waiter count a SET of
 * vertices ride instead of a per-vertex `std::mutex` + `std::condition_variable`, the
 * address-derived stripe selection, and the separately-guarded condvar table.
 *
 * Extracted from `vertex.hpp` (#868). This is process-global state, not vertex state —
 * a `constinit` table plus its lookup functions — and it was the one part of that header
 * with no dependency on any vertex type at all, which is why it comes out cleanly.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "libtracer/config.hpp"

/**
 * @file
 * @brief `tr::graph` shared vertex lock stripes (#361 §2, ADR-0068 knobs).
 */

namespace tr::graph {

// The stripe count and the padding width both live in libtracer/config.hpp (ADR-0068):
// kVertexLockStripes and kCacheLineBytes are ordinary constexprs shared by every TU
// through ONE header, so a per-target override (CMake cache variable / Kconfig) can never
// diverge across TUs the way a bare `-D` compile definition could (the std::array size
// below, and now the stripe's alignment, would be an ODR violation).

/**
 * @brief The stripe's alignment: @ref kCacheLineBytes, floored at the payload's own
 *        natural alignment.
 *
 * `alignas` may only ever STRENGTHEN alignment — [dcl.align]/5 makes a weaker request
 * ill-formed, and GCC accepts it silently rather than diagnosing it, so a target that sets
 * @ref kCacheLineBytes to 0 or 4 must not hand that number to `alignas` unchecked. Taking
 * the max of the members' own alignments keeps every value of the knob well-formed, and the
 * `static_assert` under the struct proves the request survived.
 */
inline constexpr std::size_t kStripeAlign =
    std::max({kCacheLineBytes, alignof(std::mutex), alignof(std::atomic<int>)});

struct await_waiter_t;

/**
 * @brief The fire callback of a one-shot @ref await_waiter_t, called once, on the WRITER's
 *        thread, after the stripe lock is released, with the waiter already unlinked.
 *
 * The `{fn, ctx}` shape of every graph seam (ADR-0047). The callee owns the waiter from the
 * moment it is called: the graph never touches it again, so the callee may release it.
 */
using await_fire_fn_t = void (*)(void* ctx, await_waiter_t& waiter) noexcept;

/**
 * @brief A one-shot, receiver-side change waiter (ADR-0084): the non-blocking form of
 *        `vertex_t::wait_for_change`.
 *
 * Intrusive and caller-owned: the graph links it into the stripe of the vertex it watches and
 * allocates nothing. A remote AWAIT's waiter is drawn by the router from the receiving link's
 * own rx source, so the receiver pays for the wait it serves and no library-internal buffer
 * holds it. While armed it is counted in the stripe's @ref vertex_stripe_t::waiters, so a
 * publish on a stripe with no waiter of either kind keeps the lock-free path (#555).
 *
 * Every field but @ref fire / @ref ctx is the graph's, written under the stripe mutex.
 */
struct await_waiter_t {
    await_fire_fn_t fire = nullptr; /**< @brief Called once on change; never on disarm. */
    void* ctx = nullptr;            /**< @brief Opaque to the graph; handed back to @ref fire. */
    const void* vertex = nullptr;   /**< @brief The watched vertex (its pinned address). */
    await_waiter_t* next = nullptr; /**< @brief Stripe list link (graph-owned). */
    bool armed = false;             /**< @brief Linked into a stripe list (graph-owned). */
};

/**
 * @brief One shared lock stripe: the mutex + condvar a SET of vertices ride
 *        (#361 §2), replacing a per-vertex `std::mutex` + `std::condition_variable`.
 *
 * Why: the blocking primitives were the single largest per-vertex RAM cost on
 * the MCU target — ESP-IDF pthreads lazily allocate a FreeRTOS mutex (~90 B)
 * plus condvar state PER VERTEX on first touch, and the host paid 88 B of
 * struct. The LKV read/write hot path takes no VERTEX lock (the atomic shared_ptr
 * swap), so a stripe serializes only control-plane verbs (ring trim, edge
 * mutation, ACL state, seq/notify) — cross-vertex contention is
 * wiring-frequency, not per-publish. `await` waits on the stripe's condvar
 * with a PER-VERTEX predicate (`write_seq_`), so a collision costs a spurious
 * wake + re-check, never a correctness change.
 */
struct alignas(kStripeAlign) vertex_stripe_t {  // one cache line per stripe where a second
                                                // core exists to false-share with (see
                                                // kStripeAlign); packed tight where none does
    std::mutex m; /**< @brief Serializes the stripe's vertices' verbs. */
    /** @brief Live `await` waiters on this stripe. Mutated only under @ref m, but READ
     *         without it by a publish that never takes the lock at all (#555), so it is
     *         atomic: the waiterless publish skips the mutex, not just the condvar call
     *         that #370 skipped. See `%vertex_t::store` for the ordering argument that
     *         makes the lock-free read safe against a lost wakeup. */
    std::atomic<int> waiters{0};
    /** @brief The armed one-shot waiters of this stripe's vertices (ADR-0084), guarded by
     *         @ref m. Each one is also counted in @ref waiters. */
    await_waiter_t* armed{nullptr};
};

static_assert(alignof(vertex_stripe_t) == kStripeAlign,
              "the stripe's alignas was silently dropped — kStripeAlign must never ask for "
              "less than the payload's natural alignment (see its derivation above)");

/**
 * @brief The stripe table: `constinit` where the platform's `std::mutex` is
 *        constexpr-constructible, so the per-verb lookup is a plain indexed load with
 *        NO function-local-static init-guard check on the hot path (#370). libstdc++
 *        makes the ctor constexpr only when its gthreads port supports static mutex
 *        init (`__GTHREAD_MUTEX_INIT`) — ESP-IDF's does NOT — and libc++'s always is;
 *        the fallback is a guarded function-local static (one predicted branch per
 *        verb — the MCU's constraint is RAM, not that branch). The condvars live in a
 *        separate guarded table (`vertex_stripe_cv`) because `std::condition_variable`
 *        can never be constant-initialized — only the cold await/wake paths reach it.
 */
#if defined(__GTHREAD_MUTEX_INIT) || defined(_LIBCPP_VERSION)
inline constinit std::array<vertex_stripe_t, kVertexLockStripes> vertex_stripes{};

/** @brief The stripe at table slot @p idx (guard-free constant-initialized table). */
inline vertex_stripe_t& vertex_stripe_at(std::size_t idx) noexcept { return vertex_stripes[idx]; }
#else
/** @brief The stripe at table slot @p idx (guarded-static fallback: this platform's
 *         `std::mutex` has no constexpr ctor, so the table cannot be `constinit`). */
inline vertex_stripe_t& vertex_stripe_at(std::size_t idx) noexcept {
    static std::array<vertex_stripe_t, kVertexLockStripes> stripes{};
    return stripes[idx];
}
#endif

/** @brief The stripe slot of a pinned vertex address (ADR-0056/0057 — the address is a
 *         stable identity). Same vertex ⇒ same slot, always. */
inline std::size_t vertex_stripe_index(const void* v) noexcept {
    std::uintptr_t h = reinterpret_cast<std::uintptr_t>(v);
    h ^= h >> 9;  // fold higher entropy into the allocation-aligned low bits
    return (h >> 6) % kVertexLockStripes;
}

/** @brief The stripe a vertex rides (mutex + waiter count). */
inline vertex_stripe_t& vertex_stripe_of(const void* v) noexcept {
    return vertex_stripe_at(vertex_stripe_index(v));
}

/**
 * @brief The stripe's condvar — a SEPARATE guarded-static table, reached only from
 *        `vertex_t::wait_for_change` and from a publish that saw `waiters != 0`:
 *        the waiterless publish (the hot path) never pays this table's init guard.
 */
inline std::condition_variable& vertex_stripe_cv(std::size_t idx) {
    static std::array<std::condition_variable, kVertexLockStripes> cvs;
    return cvs[idx];
}

}  // namespace tr::graph
