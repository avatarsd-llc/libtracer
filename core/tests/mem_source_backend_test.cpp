/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief The `tr::mem::source_backend_t` block contract (#873 phase 3).
 *
 * Phase 1 built the wrapper and drew TWO blocks per segment — the payload, then the
 * `segment_t` control block — mirroring `heap_backend_t`'s `operator new` pair. Phase 3
 * packs them into ONE, because on the deployments that construct this type (a bounded node
 * with an injected `pool_source_t`) two draws meant two size classes, two refusal
 * opportunities and the per-class rounding paid twice for one segment.
 *
 * The properties pinned here are the ones a size-classed source depends on and that a
 * reader cannot check by inspection: one draw per segment at exactly
 * `block_bytes(size)` / `kBlockAlign`, a payload aligned to the backend's declared
 * `alignment()` and disjoint from the control block, a refusal that answers a null
 * `segment_t*` and leaks nothing, and a release that returns exactly what was taken.
 */

#include "libtracer/mem_source_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/segment.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;

/** @brief One `try_alloc` / `release` pair as the recording source saw it. */
struct draw_t {
    void* p;           /**< @brief The block handed out (or returned). */
    std::size_t bytes; /**< @brief The size asked for (or returned). */
    std::size_t align; /**< @brief The alignment asked for (or returned). */
};

/**
 * @brief A pass-through `block_source_t` that RECORDS every draw and can refuse on demand.
 *
 * Serves from `tr::mem::heap_source()` so the bytes behave normally; the instrument is the
 * transcript, not the allocator.
 */
class recording_source_t final : public tr::mem::block_source_t {
   public:
    recording_source_t() noexcept : block_source_t("recording") {}

    /** @brief Refuse every subsequent draw — the exhaustion stand-in. */
    void refuse_all() noexcept { refusing_ = true; }

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refusing_) {
            ++refusals_;
            return nullptr;
        }
        void* const p = tr::mem::heap_source().try_alloc(bytes, align);
        allocs_.push_back(draw_t{.p = p, .bytes = bytes, .align = align});
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        frees_.push_back(draw_t{.p = p, .bytes = bytes, .align = align});
        tr::mem::heap_source().release(p, bytes, align);
    }

    /** @brief The `try_alloc` transcript. */
    [[nodiscard]] const std::vector<draw_t>& allocs() const noexcept { return allocs_; }
    /** @brief The `release` transcript. */
    [[nodiscard]] const std::vector<draw_t>& frees() const noexcept { return frees_; }
    /** @brief How many draws were refused. */
    [[nodiscard]] std::size_t refusals() const noexcept { return refusals_; }

   private:
    bool refusing_ = false;
    std::size_t refusals_ = 0;
    std::vector<draw_t> allocs_{};
    std::vector<draw_t> frees_{};
};

}  // namespace

/** @brief Run the phase-3 one-block-per-segment probes. */
int main() {
    std::printf("source_backend_t draws ONE block per segment (#873 phase 3):\n");

    using tr::mem::source_backend_t;

    check(source_backend_t::kHeaderBytes >= sizeof(tr::view::segment_t),
          "the header region holds a whole segment_t");
    check(source_backend_t::kHeaderBytes % source_backend_t::kBlockAlign == 0,
          "the header region is a whole number of block alignments (so the payload is aligned)");

    // ONE draw, of exactly block_bytes(size) at kBlockAlign, and the payload sits behind the
    // control block inside that one block.
    {
        recording_source_t src;
        source_backend_t be(src);
        constexpr std::size_t kSize = 100;
        tr::view::segment_t* const seg = be.alloc(kSize);
        check(seg != nullptr, "a segment is served");
        check(src.allocs().size() == 1, "EXACTLY one draw per segment (phase 1 made two)");
        check(src.allocs()[0].bytes == source_backend_t::block_bytes(kSize),
              "the draw is block_bytes(size) — header plus payload in one block");
        check(src.allocs()[0].align == source_backend_t::kBlockAlign,
              "the draw is at the block alignment");
        check(seg->bytes.size() == kSize, "the segment reports the requested payload size");
        const auto addr = reinterpret_cast<std::uintptr_t>(seg->bytes.data());
        check(addr % be.alignment() == 0, "the payload is aligned to the backend's alignment()");
        const auto base = reinterpret_cast<std::uintptr_t>(seg);
        check(addr == base + source_backend_t::kHeaderBytes,
              "the payload sits immediately behind the padded control block");
        check(addr >= base + sizeof(tr::view::segment_t),
              "the payload never overlaps the control block");
        // The bytes are writable through their whole extent (an ASan/UBSan-visible probe of
        // the arithmetic above, not a tautology: an off-by-one header would trip here).
        std::memset(seg->bytes.data(), 0x5A, seg->bytes.size());
        check(seg->bytes[kSize - 1] == std::byte{0x5A}, "the last payload byte is ours to write");

        be.destroy(seg);
        check(src.frees().size() == 1, "EXACTLY one release per segment");
        check(src.frees()[0].p == src.allocs()[0].p, "the block returned is the block taken");
        check(src.frees()[0].bytes == src.allocs()[0].bytes &&
                  src.frees()[0].align == src.allocs()[0].align,
              "sized reclaim: the release matches the originating try_alloc exactly");
    }

    // A ZERO-size segment is empty-but-valid, exactly as heap_backend_t's is: a null, empty
    // span rather than a one-past pointer into the header — and still one block, released at
    // block_bytes(0).
    {
        recording_source_t src;
        source_backend_t be(src);
        tr::view::segment_t* const seg = be.alloc(0);
        check(seg != nullptr, "a zero-size request still yields a valid segment");
        check(seg->bytes.empty() && seg->bytes.data() == nullptr,
              "its bytes are a NULL, empty span (as heap_backend_t's are)");
        check(src.allocs().size() == 1 && src.allocs()[0].bytes == source_backend_t::block_bytes(0),
              "one block of block_bytes(0)");
        be.destroy(seg);
        check(src.frees().size() == 1 && src.frees()[0].bytes == source_backend_t::block_bytes(0),
              "released at the same size");
    }

    // A refusal is a null segment_t* — the BACKPRESSURE signal mem_backend_t::alloc documents
    // — and with one draw there is nothing to unwind, so nothing is released either.
    {
        recording_source_t src;
        source_backend_t be(src);
        src.refuse_all();
        check(be.alloc(64) == nullptr, "a refused draw answers a NULL segment, never a throw");
        check(src.refusals() == 1, "the source was consulted exactly once");
        check(src.frees().empty(), "a refusal releases nothing (one draw, nothing to unwind)");
    }

    // The wrapper's identity survives into the segment: reclaim routes back to this backend
    // (tag UNKNOWN ⇒ the virtual destroy fallback, which is the decided phase-3 shape).
    {
        recording_source_t src;
        source_backend_t be(src);
        check(&be.source() == &src, "the backend publishes the source it draws from");
        check(be.tag() == tr::mem::backend_tag::UNKNOWN,
              "the tag stays UNKNOWN — no module-set SOURCE enumerator (phase 3 decision)");
        tr::view::segment_t* const seg = be.alloc(8);
        check(seg != nullptr && seg->backend == &be, "the segment reclaims through this wrapper");
        check(seg->btag == tr::mem::backend_tag::UNKNOWN, "and carries the wrapper's tag");
        tr::view::segment_ptr_t::adopt(seg).reset();  // reclaim through destroy_dispatch
        check(src.frees().size() == 1, "the dispatched reclaim returned the one block");
    }

    return tr::testing::summary("mem_source_backend");
}
