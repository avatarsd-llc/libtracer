/**
 * @file
 * @brief The `malloc` interposer behind `malloc_probe.hpp`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every entry point forwards to glibc's own `__libc_*` allocator, so allocation and release
 * stay one allocator and `malloc_usable_size` stays valid for every block.
 *
 * @section window Only the window's own blocks move the balance (#1420)
 *
 * A block allocated while armed is recorded in a charge table with the window's number and the
 * bytes it added; a free subtracts that recorded charge and nothing else. A block allocated
 * BEFORE the window and freed inside it was never charged to the window, so its free costs the
 * window nothing. Without that, a window that frees older blocks — a runtime's deferred
 * teardown, a library's cache — cancels bytes the measured code really kept, which is the
 * defect `bench_forward_heap`'s `canary_prewindow_free` exists for, and `canary()` below is
 * the same check. A block the full table cannot record is counted in `untracked`; its bytes
 * stay charged, so the balance can only over-state, never hide, what the window kept.
 */
#include "malloc_probe.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(__GLIBC__)
#include <malloc.h>

extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void __libc_free(void*);
void* __libc_memalign(std::size_t, std::size_t);
}

namespace {

std::atomic<bool> g_armed{false};
std::atomic<unsigned> g_window{0};
std::atomic<long long> g_live{0};
std::atomic<long long> g_peak{0};
std::atomic<long long> g_allocs{0};
std::atomic<long long> g_frees{0};
std::atomic<long long> g_untracked{0};

/** @brief One charged block: its address, the window that charged it, the bytes charged. */
struct charge_t {
    void* p;
    unsigned window;
    std::size_t bytes;
};

/** @brief Charge-table slots: a power of two, static so the table never calls malloc. */
constexpr std::size_t kCharges = std::size_t{1} << 18;
/** @brief Longest probe sequence before a block is counted as untracked. */
constexpr std::size_t kProbeCap = 64;
charge_t g_charges[kCharges];
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

/** @brief The table slot @p p hashes to. */
std::size_t slot_of(const void* p) {
    return static_cast<std::size_t>(
               (reinterpret_cast<std::uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 46) &
           (kCharges - 1);
}

/** @brief Record window block @p p; return the bytes it adds to the balance. */
long long charge(void* p) {
    const auto bytes = static_cast<long long>(malloc_usable_size(p));
    const unsigned w = g_window.load(std::memory_order_relaxed);
    while (g_lock.test_and_set(std::memory_order_acquire)) {
    }
    std::size_t i = slot_of(p);
    std::size_t n = 0;
    while (n < kProbeCap && g_charges[i].window == w && g_charges[i].p != p) {
        i = (i + 1) & (kCharges - 1);
        ++n;
    }
    if (n < kProbeCap)
        g_charges[i] = {p, w, static_cast<std::size_t>(bytes)};
    else
        g_untracked.fetch_add(1, std::memory_order_relaxed);
    g_lock.clear(std::memory_order_release);
    return bytes;
}

/** @brief What freeing @p p takes off the balance: its charge in this window, else 0. */
long long discharge(void* p) {
    const unsigned w = g_window.load(std::memory_order_relaxed);
    long long out = 0;
    while (g_lock.test_and_set(std::memory_order_acquire)) {
    }
    std::size_t i = slot_of(p);
    for (std::size_t n = 0; n < kProbeCap; ++n) {
        if (g_charges[i].p == p && g_charges[i].window == w) {
            out = static_cast<long long>(g_charges[i].bytes);
            g_charges[i].window = 0;  // dead: lookups walk past it, inserts reuse it
            break;
        }
        i = (i + 1) & (kCharges - 1);
    }
    g_lock.clear(std::memory_order_release);
    return out;
}

/** @brief Count a fresh block @p p. */
void note_alloc(void* p) {
    if (p == nullptr || !g_armed.load(std::memory_order_relaxed)) return;
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    const long long sz = charge(p);
    const long long now = g_live.fetch_add(sz, std::memory_order_relaxed) + sz;
    long long seen = g_peak.load(std::memory_order_relaxed);
    while (now > seen && !g_peak.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
    }
}

/** @brief Count the release of block @p p (before it is freed). */
void note_free(void* p) {
    if (p == nullptr || !g_armed.load(std::memory_order_relaxed)) return;
    g_frees.fetch_add(1, std::memory_order_relaxed);
    g_live.fetch_sub(discharge(p), std::memory_order_relaxed);
}

}  // namespace

extern "C" {

void* malloc(std::size_t n) {
    void* p = __libc_malloc(n);
    note_alloc(p);
    return p;
}

void* calloc(std::size_t k, std::size_t n) {
    void* p = __libc_calloc(k, n);
    note_alloc(p);
    return p;
}

void* realloc(void* old, std::size_t n) {
    if (old == nullptr) return malloc(n);
    // Read the old block's charge before glibc may hand its address back; a failed realloc
    // leaves the old block live, so its charge is restored by re-charging it.
    note_free(old);
    void* p = __libc_realloc(old, n);
    note_alloc(p != nullptr ? p : (n != 0 ? old : nullptr));
    return p;
}

void free(void* p) {
    note_free(p);
    __libc_free(p);
}

void* memalign(std::size_t a, std::size_t n) {
    void* p = __libc_memalign(a, n);
    note_alloc(p);
    return p;
}

void* aligned_alloc(std::size_t a, std::size_t n) { return memalign(a, n); }

int posix_memalign(void** out, std::size_t a, std::size_t n) {
    void* p = __libc_memalign(a, n);
    if (p == nullptr) return 12;  // ENOMEM
    note_alloc(p);
    *out = p;
    return 0;
}

}  // extern "C"

namespace bench::malloc_probe {

const bool kAvailable = true;

void arm() {
    g_live.store(0, std::memory_order_relaxed);
    g_peak.store(0, std::memory_order_relaxed);
    g_allocs.store(0, std::memory_order_relaxed);
    g_frees.store(0, std::memory_order_relaxed);
    g_untracked.store(0, std::memory_order_relaxed);
    // A fresh window number retires every earlier charge at once; 0 marks a dead slot.
    unsigned w = g_window.load(std::memory_order_relaxed) + 1;
    if (w == 0) w = 1;
    g_window.store(w, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_seq_cst);
}

reading_t disarm() {
    g_armed.store(false, std::memory_order_seq_cst);
    return {g_live.load(std::memory_order_relaxed), g_peak.load(std::memory_order_relaxed),
            g_allocs.load(std::memory_order_relaxed), g_frees.load(std::memory_order_relaxed),
            g_untracked.load(std::memory_order_relaxed)};
}

bool canary() {
    // A block from BEFORE the window, freed inside it, must cost the window nothing.
    void* volatile early = malloc(64);
    arm();
    free(early);
    const reading_t a = disarm();
    // A block allocated inside the window and kept must be seen.
    arm();
    void* volatile kept = malloc(64);
    const reading_t b = disarm();
    free(kept);
    return a.live == 0 && b.live >= 64 && b.allocs == 1;
}

}  // namespace bench::malloc_probe

#else  // !__GLIBC__

namespace bench::malloc_probe {
const bool kAvailable = false;
void arm() {}
reading_t disarm() { return {}; }
bool canary() { return true; }
}  // namespace bench::malloc_probe

#endif
