/**
 * @file
 * @brief The `malloc` interposer behind `malloc_probe.hpp`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every entry point forwards to glibc's own `__libc_*` allocator, so allocation and release
 * stay one allocator and `malloc_usable_size` stays valid for every block. The counters are
 * relaxed atomics: the interposer runs on every thread, and a window is read only after the
 * work it brackets has finished.
 */
#include "malloc_probe.hpp"

#include <atomic>
#include <cstddef>

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
std::atomic<long long> g_live{0};
std::atomic<long long> g_peak{0};
std::atomic<long long> g_allocs{0};
std::atomic<long long> g_frees{0};

/** @brief Count a fresh block @p p. */
void note_alloc(void* p) {
    if (p == nullptr || !g_armed.load(std::memory_order_relaxed)) return;
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    const auto sz = static_cast<long long>(malloc_usable_size(p));
    const long long now = g_live.fetch_add(sz, std::memory_order_relaxed) + sz;
    long long seen = g_peak.load(std::memory_order_relaxed);
    while (now > seen && !g_peak.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
    }
}

/** @brief Count the release of block @p p (before it is freed). */
void note_free(void* p) {
    if (p == nullptr || !g_armed.load(std::memory_order_relaxed)) return;
    g_frees.fetch_add(1, std::memory_order_relaxed);
    g_live.fetch_sub(static_cast<long long>(malloc_usable_size(p)), std::memory_order_relaxed);
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
    note_free(old);
    void* p = __libc_realloc(old, n);
    // A failed realloc leaves the old block live: count it back.
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
    g_armed.store(true, std::memory_order_seq_cst);
}

reading_t disarm() {
    g_armed.store(false, std::memory_order_seq_cst);
    return {g_live.load(std::memory_order_relaxed), g_peak.load(std::memory_order_relaxed),
            g_allocs.load(std::memory_order_relaxed), g_frees.load(std::memory_order_relaxed)};
}

}  // namespace bench::malloc_probe

#else  // !__GLIBC__

namespace bench::malloc_probe {
const bool kAvailable = false;
void arm() {}
reading_t disarm() { return {}; }
}  // namespace bench::malloc_probe

#endif
