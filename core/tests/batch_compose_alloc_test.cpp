/**
 * @file
 * @brief #1468 — `compose_batch` is ZERO-COPY: the bytes it allocates do not grow with the
 *        sample payload, at any sample size (RFC-0025 §4.1.3, Amendment 4, clause 4).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * User-orchestrated batching only pays for itself if composing a batch is a **rope append**
 * rather than a serialization. `compose_batch` allocates exactly one small owned segment — the
 * record header, the `TIME` base child and, on a non-uniform stream, the packed offset child —
 * and ropes the application's existing sample segments on behind it as refcounted links. The
 * claim under test is that the *payload never touches an allocator*: a 64 KiB sample costs the
 * composition the same bytes a 64 B sample does.
 *
 * @section instrument What makes this non-vacuous
 *
 * The instrument is a global `operator new` counter, on the `handler_write_alloc_test`
 * precedent — every allocating and deallocating form replaced, because a hole would make the
 * very allocation under test invisible and a missing delete form is an ASan
 * alloc-dealloc-mismatch. It counts **calls and bytes**, because calls alone would not catch a
 * composer that copied the payload into one big block.
 *
 * Two positive controls, because a "nothing grew" assertion passes loudest when nothing ran:
 *
 *  - **The counter must be live, and the ladder must be able to show growth.** The contiguous
 *    emitter `emit_batch` — the same layout, into a `std::vector` — is measured on the identical
 *    ladder, and its allocated bytes MUST grow with the payload. That is the copy this test
 *    exists to prove `compose_batch` does not take. Without it, a counter that had silently
 *    stopped counting would satisfy every flatness assertion below.
 *  - **The composition must be real.** Every rung asserts the rope's link count and its total
 *    byte length, so a composer that failed (empty rope on a refused segment) cannot pass by
 *    allocating nothing at all.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <vector>

#include "libtracer/batch.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tlv_emit.hpp"
#include "test_support.hpp"

namespace {

/** @brief Global-new call counter, live only while @ref g_arm is set. */
std::size_t g_allocs = 0;
/** @brief Global-new BYTE counter, live only while @ref g_arm is set. */
std::size_t g_bytes = 0;
bool g_arm = false;

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm) {
        ++g_allocs;
        g_bytes += n;
    }
    return std::malloc(n == 0 ? 1 : n);
}

/** @brief The aligned counted allocation — `aligned_alloc` only for a genuinely OVER-aligned
 *         request (which `free` accepts), `malloc` for a fundamental one. */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm) {
        ++g_allocs;
        g_bytes += n;
    }
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

using tr::testing::check;
using tr::view::rope_t;
using tr::view::view_t;

/** @brief How many sample frames every rung of the ladder folds. */
constexpr std::size_t kSamples = 4;

/** @brief The batch base — the app's number; the graph has no clock and never supplies one. */
constexpr std::int64_t kBase = 1'700'000'000'000'000'000;

/**
 * @brief A fixed ceiling on what one composition may allocate, at ANY payload size.
 *
 * Generous by design — the point is that it is a CONSTANT, not that it is tight. It covers the
 * owned head segment (≤ 34 B here), its control block, and the rope's link storage once five
 * links spill the inline small-buffer. Measured at ~198 B; a composer that copied even one
 * 512-byte sample would blow through it.
 */
constexpr std::size_t kCompositionCeiling = 512;

/** @brief One measured point: the allocations a composition or an emission cost. */
struct cost_t {
    std::size_t allocs = 0;
    std::size_t bytes = 0;
};

/**
 * @brief The app's own sample buffers at @p payload bytes each, plus views over them.
 *
 * Built OUTSIDE the armed window on purpose: these are the acquisition buffers an application
 * already holds when it decides to batch. Charging their allocation to the composition would
 * measure the app's own storage rather than what batching adds to it.
 */
struct fixture_t {
    std::vector<std::vector<std::byte>> frames;
    std::vector<view_t> views;
    std::vector<std::span<const std::byte>> spans;

    explicit fixture_t(std::size_t payload) {
        for (std::size_t i = 0; i < kSamples; ++i) {
            std::vector<std::byte> body(payload, std::byte{0xAB});
            std::vector<std::byte> tlv;
            tr::wire::emit_tlv(tlv, tr::wire::type_t::VALUE, tr::wire::opt_t{}, body);
            frames.push_back(std::move(tlv));
        }
        for (const std::vector<std::byte>& f : frames) {
            views.push_back(view_t::over(tr::view::borrow_const(std::span<const std::byte>(f))));
            spans.emplace_back(f);
        }
    }

    /** @brief Total bytes of the sample frames' own encodings. */
    [[nodiscard]] std::size_t samples_bytes() const {
        std::size_t n = 0;
        for (const std::vector<std::byte>& f : frames) n += f.size();
        return n;
    }
};

/** @brief What one `compose_batch` of @p payload-byte samples costs, and that it really composed.
 */
[[nodiscard]] cost_t compose_cost(std::size_t payload) {
    const fixture_t fx{payload};
    constexpr std::array<std::int32_t, kSamples> kOffsets{0, 1000, 2000, 3000};

    g_allocs = 0;
    g_bytes = 0;
    g_arm = true;
    rope_t r = tr::wire::compose_batch(
        tr::mem::heap_backend(), tr::wire::batch_carriage_t::STANDALONE, kBase, fx.views, kOffsets);
    g_arm = false;

    // The composition must be REAL — an empty rope (refused segment) would allocate nothing and
    // sail through every flatness assertion below.
    check(r.link_count() == 1 + kSamples, "the composition is head + one link per sample");
    std::size_t total = 0;
    r.walk([&total](std::span<const std::byte> s) { total += s.size(); });
    check(total == tr::wire::batch_wire_bytes(fx.samples_bytes(), kSamples),
          "... and its bytes are the whole record, payload included");
    return {g_allocs, g_bytes};
}

/** @brief What the contiguous emitter costs on the same ladder — the positive control. */
[[nodiscard]] cost_t emit_cost(std::size_t payload) {
    const fixture_t fx{payload};
    constexpr std::array<std::int32_t, kSamples> kOffsets{0, 1000, 2000, 3000};

    std::vector<std::byte> out;
    g_allocs = 0;
    g_bytes = 0;
    g_arm = true;
    tr::wire::emit_batch(out, kBase, fx.spans, kOffsets);
    g_arm = false;

    check(out.size() == tr::wire::batch_wire_bytes(fx.samples_bytes(), kSamples),
          "the contiguous emitter produced the same whole record");
    return {g_allocs, g_bytes};
}

/**
 * @brief The ladder: composition cost is FLAT in the payload; the contiguous emitter's is not.
 */
void test_composition_does_not_pay_for_the_payload() {
    std::printf("§4.1.3 compose_batch — the payload never reaches an allocator:\n");
    constexpr std::size_t kLadder[] = {64, 512, 4096, 65536};

    cost_t compose_at_bottom{};
    cost_t compose_at_top{};
    cost_t emit_at_bottom{};
    cost_t emit_at_top{};

    for (const std::size_t payload : kLadder) {
        const cost_t c = compose_cost(payload);
        const cost_t e = emit_cost(payload);
        std::printf("    %6zu B/sample: compose %zu alloc / %zu B, emit %zu alloc / %zu B\n",
                    payload, c.allocs, c.bytes, e.allocs, e.bytes);
        if (payload == kLadder[0]) {
            compose_at_bottom = c;
            emit_at_bottom = e;
        }
        compose_at_top = c;
        emit_at_top = e;

        // The head, the segment's own control block and the rope's link storage are the only
        // things the composition owns; the payload is refcounted. A FIXED ceiling — not a
        // fraction of the payload — is what says "independent of the bytes being batched".
        check(c.bytes < kCompositionCeiling,
              "the composition stays under a fixed byte ceiling, whatever the payload");
    }

    // Flatness. The allocation COUNT is exactly equal across a 1024x payload increase. The byte
    // count is equal to within TWO — and those two bytes are not a copy: past 0xFFFF of body the
    // record header's length field widens from u16 to u32 (`opt.ll`), so the owned head segment
    // is 6 bytes of header instead of 4. That is the whole of what a 1024x payload costs the
    // composition, and it is asserted as a bound rather than hidden behind a tolerance.
    check(compose_at_top.allocs == compose_at_bottom.allocs,
          "compose_batch takes the SAME number of allocations at 64 B/sample and at 64 KiB");
    check(compose_at_top.bytes >= compose_at_bottom.bytes &&
              compose_at_top.bytes - compose_at_bottom.bytes <= 2,
          "... and the same bytes to within the header's 2-byte u32-length widening");

    // The positive control. Without growth here, the counter could be dead and the flatness
    // assertion above would be vacuous.
    check(emit_at_bottom.bytes > 0, "the counter is live (the contiguous emitter allocates)");
    check(emit_at_top.bytes > emit_at_bottom.bytes + 1000,
          "... and the contiguous emitter's bytes GROW with the payload — that is the copy");
    check(emit_at_top.bytes > compose_at_top.bytes * 10,
          "at 64 KiB/sample the copy costs orders of magnitude more than the composition");
}

}  // namespace

int main() {
    std::printf("#1468 compose_batch zero-copy allocation accounting\n\n");
    test_composition_does_not_pay_for_the_payload();
    return tr::testing::summary("batch_compose_alloc");
}
