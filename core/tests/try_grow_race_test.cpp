/**
 * @file
 * @brief The nothrow `tr::detail::try_*` growth helpers under a REFUSING allocator (#923,
 *        and #850 folded into it): an allocation failure must come back as `false`, never
 *        as a `bad_alloc` crossing their `noexcept` boundary into `std::terminate`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @par Why this TU owns global `operator new`
 * `tr::detail::probe_fail_hook` cannot express the defect. It gates `probe_bytes` — the
 * FIRST of the two allocations the old helpers performed — so setting it makes the probe
 * refuse and the helper soft-fail exactly as intended. The bug lives in the SECOND
 * allocation, the throwing `std::vector::reserve` behind the probe, which the hook never
 * sees. Only a real allocator that refuses a specific request can reach it, which is the
 * same reason `transport_alloc_softfail_test` owns these operators.
 *
 * The regime is deliberately narrow: only allocations of at least @ref kBigBytes are
 * subject to it, so nothing incidental (iostreams, thread stacks, the vectors' own control
 * blocks) is affected, and it is disarmed outside the measured regions.
 *
 * ## The two instruments
 *
 * 1. **Deterministic (`one_token_*`)** — arm exactly ONE permitted large allocation and run
 *    a growth. The fixed helper performs exactly one large allocation (the container's own)
 *    and succeeds. The probe-then-commit form performs TWO: the probe eats the token and
 *    the `reserve` behind it is refused, throwing out of a `noexcept` function. This is
 *    #850's measured `terminate called after throwing an instance of 'std::bad_alloc'`,
 *    reduced to a single-threaded, repeatable trigger.
 * 2. **The race (`race_leg`)** — two threads on one budget, refilled to TWO tokens per
 *    iteration. The probe-then-commit form survives only while the racer does not take a
 *    token inside the window between the probe's `operator delete` and the `reserve`; when
 *    it does, the second allocation is refused and the process dies. The fixed form has no
 *    window by construction: the one allocation whose failure is reported is the one the
 *    container performs, so every outcome is `true` or `false`.
 *
 * Instrument 2 is the one the issue asks for and the one that reflects production (segment
 * reclaim self-routes onto whatever thread drops the last ref, concurrent with a writer's
 * allocations); instrument 1 is what makes the redden deterministic rather than flaky.
 */
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;

/** @brief Allocations at least this large are subject to the refusal regime. */
constexpr std::size_t kBigBytes = 1u << 13;

/**
 * @brief Remaining permitted large allocations; negative means the regime is DISARMED.
 *
 * A token budget rather than a boolean so a test can say "exactly one large allocation is
 * available" — the shape that separates a one-allocation growth from a two-allocation
 * probe-then-commit one.
 */
std::atomic<int> g_tokens{-1};

/** @brief Arm the regime with @p n permitted large allocations. */
void arm(int n) noexcept { g_tokens.store(n, std::memory_order_release); }
/** @brief Disarm: every allocation succeeds again. */
void disarm() noexcept { g_tokens.store(-1, std::memory_order_release); }

/**
 * @brief Claim a token for a @p bytes-sized request.
 * @retval false Armed, large, and out of tokens — the allocation must be refused.
 */
bool token_ok(std::size_t bytes) noexcept {
    if (bytes < kBigBytes) return true;
    int cur = g_tokens.load(std::memory_order_acquire);
    for (;;) {
        if (cur < 0) return true;    // disarmed
        if (cur == 0) return false;  // armed and spent
        if (g_tokens.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            return true;
        }
    }
}

/** @brief `std::malloc` (or its aligned form) behind the token regime; `nullptr` on refusal. */
void* raw_alloc(std::size_t bytes, std::size_t align) noexcept {
    if (bytes == 0) bytes = 1;
    if (!token_ok(bytes)) return nullptr;
    if (align <= alignof(std::max_align_t)) return std::malloc(bytes);
    const std::size_t rounded = (bytes + align - 1) & ~(align - 1);
    return std::aligned_alloc(align, rounded);
}

}  // namespace

/** @brief Throwing global allocation, behind the refusal regime. */
void* operator new(std::size_t n) {
    void* p = raw_alloc(n, alignof(std::max_align_t));
    if (p == nullptr) throw std::bad_alloc{};
    return p;
}
/** @brief Throwing array form. */
void* operator new[](std::size_t n) { return ::operator new(n); }
/** @brief Nothrow global allocation — what `probe_bytes` calls. */
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return raw_alloc(n, alignof(std::max_align_t));
}
/** @brief Nothrow array form. */
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}
/** @brief Over-aligned throwing form (the heap backend's segment bytes). */
void* operator new(std::size_t n, std::align_val_t a) {
    void* p = raw_alloc(n, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc{};
    return p;
}
/** @brief Over-aligned nothrow form. */
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return raw_alloc(n, static_cast<std::size_t>(a));
}
/** @brief Release — every form above allocates through `malloc`/`aligned_alloc`. */
void operator delete(void* p) noexcept { std::free(p); }
/** @brief Array release. */
void operator delete[](void* p) noexcept { std::free(p); }
/** @brief Sized release. */
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
/** @brief Sized array release. */
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
/** @brief Nothrow release. */
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
/** @brief Nothrow array release. */
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
/** @brief Over-aligned release. */
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
/** @brief Over-aligned sized release. */
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
/** @brief Over-aligned nothrow release. */
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

/** @brief Elements of a `std::vector<std::byte>` that make one growth a "large" request. */
constexpr std::size_t kBigElems = kBigBytes;

/**
 * @brief Instrument 1a — `try_reserve` with exactly ONE large allocation available.
 *
 * One allocation is all a correct growth needs. Two is what probe-then-commit spends, and
 * the second one is unguardable.
 */
void one_token_reserve() {
    std::vector<std::byte> v;
    arm(1);
    const bool ok = tr::detail::try_reserve(v, kBigElems);
    disarm();
    check(ok, "try_reserve succeeds when exactly ONE large allocation is available");
    check(v.capacity() >= kBigElems, "the capacity really landed");
}

/** @brief Instrument 1b — `try_push_back`'s capacity-doubling growth, one token. */
void one_token_push_back() {
    std::vector<std::byte> v;
    check(tr::detail::try_reserve(v, kBigElems), "pre-growth (disarmed) succeeds");
    v.resize(v.capacity());  // full: the next push_back must grow
    const std::size_t was = v.size();
    arm(1);
    std::byte x{0x5a};
    const bool ok = tr::detail::try_push_back(v, std::move(x));
    disarm();
    check(ok, "try_push_back succeeds when exactly ONE large allocation is available");
    check(v.size() == was + 1, "the element really landed");
}

/** @brief Instrument 1c — the growing `try_assign(std::string&, …)`, one token. */
void one_token_assign_string() {
    const std::string src(kBigBytes + 8, 'x');
    std::string dst;
    arm(1);
    const bool ok = tr::detail::try_assign(dst, std::string_view(src));
    disarm();
    check(ok, "try_assign(string) succeeds when exactly ONE large allocation is available");
    check(dst.size() == src.size(), "the string really landed");
}

/** @brief The soft-fail contract is unchanged: no tokens at all means `false`, not a crash. */
void zero_token_soft_fail() {
    std::vector<std::byte> v;
    arm(0);
    const bool ok = tr::detail::try_reserve(v, kBigElems);
    disarm();
    check(!ok, "try_reserve soft-fails when NO large allocation is available");
    check(v.capacity() == 0, "a refused try_reserve leaves the vector untouched");

    std::string dst;
    const std::string src(kBigBytes + 8, 'x');
    arm(0);
    const bool sok = tr::detail::try_assign(dst, std::string_view(src));
    disarm();
    check(!sok, "try_assign(string) soft-fails when NO large allocation is available");
    check(dst.empty(), "a refused try_assign leaves the string untouched");
}

/** @brief The OOM-injection seam still reaches these paths after the rework. */
void probe_hook_still_gates() {
    std::vector<std::byte> v;
    tr::detail::probe_fail_hook = [](std::size_t) noexcept { return false; };
    const bool ok = tr::detail::try_reserve(v, 1024);
    tr::detail::probe_fail_hook = nullptr;
    check(!ok, "probe_fail_hook still forces try_reserve to soft-fail");
    check(tr::detail::try_reserve(v, 1024), "and the same call succeeds once the hook is off");
}

/**
 * @brief Instrument 2 — the race the issue is about: a concurrent allocator against the
 *        growth helper, on a budget refilled to TWO tokens per iteration.
 *
 * Two is the interesting number. A one-allocation growth always has a token, so it always
 * succeeds; a two-allocation probe-then-commit growth has one to spare, and loses it exactly
 * when the racer allocates inside the window between the probe's free and the `reserve`. The
 * pass condition is not "the racer wins sometimes" (probabilistic, and a flaky green is
 * worse than none) — it is that EVERY outcome is a value, never a terminate.
 */
void race_leg() {
    constexpr int kIterations = 300000;
    std::atomic<bool> stop{false};
    std::atomic<int> refused{0};
    std::atomic<int> granted{0};

    std::thread racer([&stop] {
        while (!stop.load(std::memory_order_acquire)) {
            void* p = ::operator new(kBigBytes, std::nothrow);
            if (p != nullptr) ::operator delete(p);
        }
    });

    for (int i = 0; i < kIterations; ++i) {
        std::vector<std::byte> v;
        arm(2);
        const bool ok = tr::detail::try_reserve(v, kBigElems);
        disarm();
        (ok ? granted : refused).fetch_add(1, std::memory_order_relaxed);
        if (ok) check(v.capacity() >= kBigElems, "a granted racing try_reserve really grew");
    }

    stop.store(true, std::memory_order_release);
    racer.join();
    disarm();

    const int g = granted.load(std::memory_order_relaxed);
    const int r = refused.load(std::memory_order_relaxed);
    check(g + r == kIterations, "every racing try_reserve returned a value (no terminate)");
    std::printf("       race: %d granted, %d refused over %d iterations\n", g, r, kIterations);
}

}  // namespace

int main() {
    one_token_reserve();
    one_token_push_back();
    one_token_assign_string();
    zero_token_soft_fail();
    probe_hook_still_gates();
    race_leg();

    return tr::testing::summary("try_grow_race");
}
