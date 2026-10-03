/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief The bytes `heap_backend_t` actually asks the host allocator for (#1768).
 *
 * RFC-0028 slice 10 put the segment header and the payload in one heap block. A 1024 B value
 * then asked `malloc` for 1072 B, past glibc tcache's 1032 B ceiling, and every 1 KiB heap value
 * missed the allocator's fast path: `lkv-store-heap 1024B` went 27 → 54 ns and v0.17.0 shipped
 * with it. No test could see it, because the one request-size assertion in the tree compared
 * `block_bytes(size)` with itself.
 *
 * This test counts what reaches the global `operator new` / sized `operator delete` while the
 * process-default heap backend serves one segment, for every payload size from 984 to 1088 B
 * (either side of the boundary) plus 0, 64 and 8192 B. It pins:
 * - **no draw is bigger than the small-block threshold unless the payload alone is**, so a
 *   payload the fast path can serve is served by it;
 * - **below the threshold the segment is still ONE draw** (the slice-10 win at 64 B stays);
 * - **the release mirrors the draw exactly**: the same pointers, sized, at the same sizes.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "libtracer/config.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/segment.hpp"
#include "test_support.hpp"

namespace {

/** @brief One allocator call as the counting allocator saw it. */
struct call_t {
    void* p;           /**< @brief The block handed out (or returned). */
    std::size_t bytes; /**< @brief The size asked for; 0 for an UNSIZED delete. */
};

/** @brief A fixed transcript, so recording never allocates. */
struct transcript_t {
    std::array<call_t, 8> calls{}; /**< @brief The recorded calls, in order. */
    std::size_t n = 0;             /**< @brief How many are recorded. */
    std::size_t overflow = 0;      /**< @brief Calls that did not fit (a failure in itself). */

    /** @brief Record one call. */
    void add(void* p, std::size_t bytes) noexcept {
        if (n < calls.size()) {
            calls[n++] = call_t{.p = p, .bytes = bytes};
        } else {
            ++overflow;
        }
    }
};

transcript_t g_news;  /**< @brief Every `operator new` while armed. */
transcript_t g_frees; /**< @brief Every `operator delete` while armed. */
bool g_arm = false;   /**< @brief Record only around the one alloc/destroy under test. */

/** @brief The counted allocation itself, malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) noexcept {
    void* const p = std::malloc(n == 0 ? 1 : n);
    if (g_arm) g_news.add(p, n);
    return p;
}

/** @brief The aligned counted allocation: `aligned_alloc` only for a genuinely over-aligned ask. */
void* counted_aligned(std::size_t n, std::size_t align) noexcept {
    if (align <= alignof(std::max_align_t)) return counted(n);
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    void* const p = std::aligned_alloc(align, rounded);
    if (g_arm) g_news.add(p, n);
    return p;
}

/** @brief The counted release; @p bytes is 0 for an unsized form. */
void uncounted_free(void* p, std::size_t bytes) noexcept {
    if (g_arm && p != nullptr) g_frees.add(p, bytes);
    std::free(p);
}

}  // namespace

// Every allocating and deallocating form is replaced (the `folded_read_backend_test`
// precedent): a hole in the set would make a draw INVISIBLE to the counter, and a missing
// delete form is an `alloc-dealloc-mismatch` under ASan.
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
void operator delete(void* p) noexcept { uncounted_free(p, 0); }
void operator delete[](void* p) noexcept { uncounted_free(p, 0); }
void operator delete(void* p, std::size_t n) noexcept { uncounted_free(p, n); }
void operator delete[](void* p, std::size_t n) noexcept { uncounted_free(p, n); }
void operator delete(void* p, const std::nothrow_t&) noexcept { uncounted_free(p, 0); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { uncounted_free(p, 0); }
void operator delete(void* p, std::align_val_t) noexcept { uncounted_free(p, 0); }
void operator delete[](void* p, std::align_val_t) noexcept { uncounted_free(p, 0); }
void operator delete(void* p, std::size_t n, std::align_val_t) noexcept { uncounted_free(p, n); }
void operator delete[](void* p, std::size_t n, std::align_val_t) noexcept { uncounted_free(p, n); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    uncounted_free(p, 0);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    uncounted_free(p, 0);
}

namespace {

using tr::testing::check;
using tr::testing::check_quiet;

/**
 * @brief glibc's tcache ceiling on a 64-bit host: the largest request its per-thread cache
 *        serves (64 bins of 16 B; `MAX_TCACHE_SIZE`). Stated here as a LITERAL, not read back
 *        from the library, so the test has an anchor of its own.
 */
constexpr std::size_t kTcacheMaxRequest = 1032;

/** @brief The padded header a one-block segment carries, computed independently of the library. */
constexpr std::size_t kHeader =
    (sizeof(tr::view::segment_t) + tr::mem::heap_backend_t::kBlockAlign - 1) /
    tr::mem::heap_backend_t::kBlockAlign * tr::mem::heap_backend_t::kBlockAlign;

/** @brief Serve and destroy one @p size-byte segment through the heap backend, recording both. */
void probe(std::size_t size) {
    tr::mem::mem_backend_t& be = tr::mem::heap_backend();
    g_news = {};
    g_frees = {};
    g_arm = true;
    tr::view::segment_t* const seg = be.alloc(size);
    g_arm = false;
    const transcript_t news = g_news;

    char what[96];
    std::snprintf(what, sizeof(what), "S=%zu: a segment is served", size);
    check_quiet(seg != nullptr, what);
    if (seg == nullptr) return;
    check_quiet(seg->bytes.size() == size, "the segment reports the requested payload size");
    if (size != 0) {
        const auto addr = reinterpret_cast<std::uintptr_t>(seg->bytes.data());
        check_quiet(addr % be.alignment() == 0,
                    "the payload is aligned to the backend's alignment()");
        std::memset(seg->bytes.data(), 0x5A, size);  // the whole extent is ours (ASan-visible)
    }
    check_quiet(news.overflow == 0, "the transcript held every draw");

    // The headline property: no single draw is bigger than the fast-path ceiling unless the
    // payload ALONE already is. 1024 B asked for 1072 B on v0.17.0.
    std::size_t largest = 0;
    std::size_t total = 0;
    for (std::size_t i = 0; i < news.n; ++i) {
        largest = std::max(largest, news.calls[i].bytes);
        total += news.calls[i].bytes;
    }
    std::snprintf(what, sizeof(what), "S=%zu: largest draw %zu B stays within %zu B", size, largest,
                  std::max(kTcacheMaxRequest, size));
    check_quiet(largest <= std::max(kTcacheMaxRequest, size), what);

    // Below the ceiling the segment is ONE draw of header + payload (the slice-10 layout);
    // above it, two: the payload alone and the bare header.
    if (kHeader + size <= kTcacheMaxRequest) {
        std::snprintf(what, sizeof(what), "S=%zu: one draw of header + payload (%zu B)", size,
                      kHeader + size);
        check_quiet(news.n == 1 && total == kHeader + size, what);
    } else {
        std::snprintf(what, sizeof(what), "S=%zu: two draws, payload and header (%zu draws)", size,
                      news.n);
        check_quiet(news.n == 2, what);
        check_quiet(total == size + sizeof(tr::view::segment_t),
                    "the split draws exactly the payload plus a bare segment_t");
        check_quiet(reinterpret_cast<void*>(seg) != reinterpret_cast<void*>(seg->bytes.data()),
                    "the header and the payload are separate blocks");
    }

    g_arm = true;
    be.destroy(seg);
    g_arm = false;
    const transcript_t frees = g_frees;

    // The mirror: every block drawn is returned, sized, at the size it was drawn at.
    std::snprintf(what, sizeof(what), "S=%zu: as many releases as draws", size);
    check_quiet(frees.n == news.n && frees.overflow == 0, what);
    for (std::size_t i = 0; i < news.n; ++i) {
        bool matched = false;
        for (std::size_t j = 0; j < frees.n; ++j) {
            matched = matched || (frees.calls[j].p == news.calls[i].p &&
                                  frees.calls[j].bytes == news.calls[i].bytes);
        }
        std::snprintf(what, sizeof(what), "S=%zu: draw %zu (%zu B) is released sized, exactly",
                      size, i, news.calls[i].bytes);
        check_quiet(matched, what);
    }
}

}  // namespace

/** @brief Probe the heap backend's requests either side of the small-block boundary. */
int main() {
    std::printf("heap_backend_t: the bytes asked of the host allocator (#1768):\n");

    check(tr::mem::heap_backend_t::kSmallBlockBytes == tr::graph::kHeapSmallBlockBytes,
          "the heap backend splits at this build's config_t::kHeapSmallBlockBytes");
    check(tr::mem::heap_backend_t::kHeaderBytes == kHeader,
          "its one-block header is sizeof(segment_t) padded to the block alignment");
    if constexpr (tr::graph::kHeapSmallBlockBytes != kTcacheMaxRequest) {
        // A fragment moved the threshold, so the glibc-anchored sweep below does not describe
        // this build. Say so rather than fail on a deliberate choice.
        std::printf("  [SKIP] kHeapSmallBlockBytes is %zu, not the glibc default %zu\n",
                    tr::graph::kHeapSmallBlockBytes, kTcacheMaxRequest);
        return tr::testing::summary("mem_heap_request_size");
    }
    check(tr::graph::default_config_t::kHeapSmallBlockBytes == kTcacheMaxRequest,
          "the default threshold is glibc's 64-bit tcache ceiling, 1,032 B");

    probe(0);
    probe(64);  // the slice-10 win: ONE draw
    for (std::size_t s = 984; s <= 1088; ++s) probe(s);
    probe(8192);
    check(true, "probed S = 0, 64, 984..1088 and 8192 (failures, if any, are listed above)");

    return tr::testing::summary("mem_heap_request_size");
}
