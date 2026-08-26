/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * L1 rope: an ordered chain of views forming one logical byte sequence that may
 * span multiple segments without copying (docs/reference/08 §ropes). A rope is
 * how one TLV can be assembled from, say, a static header segment + a live DMA
 * payload segment. Assembly is chaining views — never a memcpy. Walk it or
 * scatter-gather it at egress with zero copies; flatten it to one contiguous
 * segment only when a flat-buffer consumer demands it (the single
 * bridge-boundary copy). See docs/adr/0016 §1.
 *
 * A rope_t keeps its first two links in inline small-buffer storage, so the hot
 * path — a single-link value (or a two-link head+payload) — allocates nothing
 * for the chain; only a third link spills the chain to the heap. This is the
 * ADR-0053 §6 trivial-case cost guard: a rope-valued vertex slot costs exactly
 * what a view-valued slot cost, so the contention/latency benches do not move.
 */
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include "libtracer/backend.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief L1 (`tr::view`) `rope_t`: the transport-agnostic scatter-gather chain.
 */

namespace tr::view {

/**
 * @brief Why the single contiguous copy could not be taken (#917).
 *
 * The two refusals are DIFFERENT verdicts and a caller must not conflate them:
 * @ref flatten_err_t::NO_MEMORY is transient backpressure (the same rope may
 * flatten once the allocator recovers), while @ref flatten_err_t::NOT_HOST is a
 * property of the rope itself (a DEVICE link the CPU must not dereference,
 * docs/adr/0024) — no retry fixes it and the payload must go via its device
 * path. Before @ref rope_t::try_flatten both collapsed into an empty @ref view_t,
 * indistinguishable from each other AND from a legitimately empty rope, so a
 * router reading that empty as "malformed frame" reported a local OOM as a
 * PERMANENT protocol error against the peer.
 */
enum class flatten_err_t : std::uint8_t {
    NOT_HOST,  /**< @brief The rope has a DEVICE link — not CPU-flattenable, ever. */
    NO_MEMORY, /**< @brief The backend refused the segment — transient backpressure. */
};

/**
 * @brief An ordered chain of @ref view_t links — one logical byte sequence
 *        spread across segments, assembled by chaining and never by copying.
 *
 * The rope is the transport-agnostic scatter-gather representation: each
 * transport lowers it to its native DMA (`iovec`/`sendmsg`, CAN descriptors,
 * RDMA verbs) via @ref to_iovec. The single contiguous copy is @ref flatten,
 * taken only at a substrate boundary that cannot scatter-gather.
 */
class rope_t {
   public:
    rope_t() = default;
    /** @brief Implicitly adopt a single view as a one-link rope. */
    rope_t(view_t v) { append(std::move(v)); }

    /** @brief Append a link (chaining — no copy). */
    void append(view_t v) {
        if (!heap_.empty()) {
            heap_.push_back(std::move(v));  // already spilled — stays on the heap chain
            return;
        }
        if (inline_n_ < kInline) {
            inline_[inline_n_++] = std::move(v);  // fits in small-buffer storage — no alloc
            return;
        }
        // The kInline+1-th link spills the whole chain to the heap (the only allocation).
        heap_.reserve(kInline + 1);
        for (std::size_t i = 0; i < inline_n_; ++i) heap_.push_back(std::move(inline_[i]));
        heap_.push_back(std::move(v));
        inline_n_ = 0;
        for (view_t& s : inline_) s = view_t{};  // drop the moved-from links' refcounts eagerly
    }
    /**
     * @brief Chain @p other's links onto this rope (no copy).
     *
     * **Self-concat safe by construction** (`r.concat(r)`), and the safety is charged only
     * to the case that needs it. Source and destination storage can overlap in exactly one
     * way — `&other == this`; two distinct `rope_t`s own disjoint chains — so the aliasing
     * case gets its own arm and the cross-rope arm pays for none of its guards.
     *
     * On the ALIASING arm, @ref append mutates the very storage `other.links()` spans: the
     * inline→heap spill blanks every inline slot and zeroes `inline_n_` mid-walk (so a
     * naive range-for yielded `[a,b,a,{}]` instead of `[a,b,a,b]`), and a heap `push_back`
     * can reallocate the vector the walk points into (a dangling span). Two independent
     * guards close that: @ref try_reserve pins the final link count up front, so none of
     * the appends spills or reallocates; and the walk indexes the source afresh each step,
     * so link `i` is re-read from wherever the chain now lives even if the reservation
     * soft-failed. `append` only ever adds a link at the end — it never reorders or drops
     * one, and the spill migrates link `i` to heap index `i` — so index `i` names the same
     * link for the whole walk.
     *
     * The CROSS-ROPE arm walks the source span once and appends: `other`'s storage is
     * disjoint from ours, so nothing this loop does can invalidate it, and neither guard
     * buys anything. Charging them there cost path-target delivery +3.5% / +10.1%
     * (`inproc-target-*`, #1022) for the hot 1–2-link clone. A caller that wants one sized
     * growth instead of the geometric `push_back` ladder for a long cross-rope join calls
     * @ref try_reserve itself with the count it already holds — which is what the delivery
     * clone, the composed-read reply builder and the folded child listing do.
     */
    rope_t& concat(const rope_t& other) {
        if (&other != this) {
            for (const view_t& l : other.links()) append(l);  // disjoint storage — walk once
            return *this;
        }
        const std::size_t add = link_count();
        // Best effort: on soft-fail the indexed walk below is still correct, it just pays
        // the ordinary spill/growth path that concat paid before. #981 residual: that
        // fallback path is `std::vector<view_t>` growth, which under `-fno-exceptions`
        // abort()s the node on OOM — see @ref try_reserve for why the chain cannot take the
        // ADR-0065 failable seam.
        static_cast<void>(try_reserve(add));
        for (std::size_t i = 0; i < add; ++i) append(links()[i]);
        return *this;
    }

    /**
     * @brief Nothrow-reserve room for @p links more @ref append / @ref concat links —
     *        the soft-fail growth the composed-reply builder needs.
     *
     * The chain's spill to `heap_` is a `std::vector` growth that throws `std::bad_alloc`
     * on OOM, which under `-fno-exceptions` is an `abort()` — a node reboot when a large
     * (e.g. composed-root) reply is assembled on a fragmented heap. A caller that knows
     * its final link count reserves it here up front: on success the next @p links
     * @ref append calls are guaranteed **non-reallocating hence nothrow**.
     *
     * While the whole chain still fits INLINE (`have + links <= kInline`) this is a no-op
     * that touches neither `heap_` nor the allocator — every `append` is then a pure
     * `inline_[]` array write that cannot throw, so the hot small-reply delivery path
     * (`assemble`) keeps its zero-alloc small-buffer fast path (ADR-0053 §6). Only once
     * the chain WILL spill does it reserve `heap_` to `have + links` and migrate the
     * inline links there, so no later `append` re-enters the inline→heap spill `reserve`
     * (an empty rope that still spills keeps the reserved capacity, making even that one
     * spill `reserve` a no-op). On failure the rope is unchanged and the caller drops the
     * reply (BACKPRESSURE) instead of aborting.
     *
     * @note #981 residual — the ONE thing this promise does not cover. "Instead of aborting"
     *       is exact on every profile whose growth THROWS, and exact on ANY profile while
     *       the chain still fits inline (that arm reaches no allocator). Once the chain
     *       spills under `-fno-exceptions`, the `heap_` growth runs through
     *       `%tr::detail::try_reserve`, which there can only PROBE the global heap, free the
     *       probe block, and then run the throwing `reserve` — and a FreeRTOS context switch
     *       in that window lets another task take the block, so the `reserve` hits
     *       exhaustion inside a `noexcept` and abort()s the node (#850). The chain cannot
     *       move to the ADR-0065 seam (`%tr::mem::block_array_t`) as it stands: `view_t` is
     *       refcounted, and that container relocates by `memcpy` and never runs a
     *       destructor. Closing it needs a failable array that relocates by MOVE (#873).
     *
     * The no-op arm is the ONLY thing this function body holds, so it stays cheap enough for
     * the compiler to inline (#1065). The spilling arm — the `max_size` guard, the allocator
     * call and the inline→heap migration — lives in a separate out-of-line member. Charging
     * the delivery clone a real `call` for a check that folds to one compare at a fresh
     * 1-link rope is a fixed per-dispatch cost on the hottest leg the rope has.
     * @retval false Reservation failed (OOM / impossible count) — the rope is untouched.
     */
    [[nodiscard]] bool try_reserve(std::size_t links) noexcept {
        // `heap_.empty()` is exactly "the chain is still inline", so `link_count()` is
        // `inline_n_` here and `kInline - inline_n_` cannot underflow (`inline_n_ <= kInline`
        // is the small-buffer invariant). The condition is therefore the same
        // `have + links <= kInline` no-op test as before, spelled without the addition so no
        // overflow guard is needed to reach it — and a caller holding a fresh empty rope
        // constant-folds `inline_n_` to 0.
        if (heap_.empty() && links <= kInline - inline_n_) return true;
        return reserve_spilled(links);
    }

    /** @brief Number of links in the chain. */
    [[nodiscard]] std::size_t link_count() const noexcept {
        return heap_.empty() ? inline_n_ : heap_.size();
    }
    /** @brief The links, in order (inline small-buffer storage or the spilled chain). */
    [[nodiscard]] std::span<const view_t> links() const noexcept {
        if (heap_.empty()) return std::span<const view_t>(inline_.data(), inline_n_);
        return std::span<const view_t>(heap_);
    }

    /**
     * @brief The single contiguous link — the consumer's explicit "this value is
     *        one segment" (ADR-0053 §6), zero copy.
     * @note Precondition: `link_count() == 1` (debug-asserted). A consumer that
     *       cannot promise contiguity calls @ref materialize instead.
     */
    [[nodiscard]] const view_t& only() const noexcept {
        assert(link_count() == 1);
        return links()[0];
    }

    /**
     * @brief The rope as one contiguous @ref view_t — zero copy when single-link,
     *        one @ref flatten copy otherwise.
     *
     * The visible choice a contiguous-bytes consumer makes (ADR-0053 §6): a
     * single-link rope is returned as its link (a refcount bump, no byte copy); a
     * multi-link rope pays the single flatten copy from @p backend. Distinct from
     * @ref flatten, which always copies — this keeps the trivial case free.
     * @note Lossy convenience (#917): a refused flatten comes back as an empty view,
     *       indistinguishable from a legitimately empty rope AND from the other
     *       refusal cause. A caller that must classify the failure
     *       (drop-vs-reply, transient-vs-permanent) calls @ref try_materialize.
     */
    [[nodiscard]] view_t materialize(mem::mem_backend_t& backend = mem::heap_backend()) const {
        if (link_count() == 1) return links()[0];
        return flatten(backend);
    }

    /**
     * @brief @ref materialize with the failure cause kept distinct (#917).
     *
     * Same tiering as @ref materialize — a single-link rope IS its link, zero copy
     * (handed back as-is even for a DEVICE link, exactly as @ref materialize does:
     * no CPU dereference happens here); a multi-link rope pays one @ref try_flatten
     * copy. The difference is the error channel: a success carrying an empty view
     * means the rope really is zero bytes, while a refusal names its cause, so an
     * OOM stays TRANSIENT backpressure and a DEVICE payload is never misfiled as a
     * malformed frame.
     */
    [[nodiscard]] std::expected<view_t, flatten_err_t> try_materialize(
        mem::mem_backend_t& backend = mem::heap_backend()) const {
        if (link_count() == 1) return links()[0];
        return try_flatten(backend);
    }

    /** @brief Total logical length across all links. */
    [[nodiscard]] std::size_t total_length() const noexcept {
        std::size_t n = 0;
        for (const view_t& l : links()) n += l.length;
        return n;
    }

    /** @brief True when every link is CPU-addressable (HOST).
     *
     * A `false` rope is **heterogeneous** — it has a DEVICE link (e.g. a GPU
     * payload, docs/adr/0024) that the CPU must not dereference, so host-side
     * operations (`flatten`, CRC) cannot touch it.
     */
    [[nodiscard]] bool all_host() const noexcept {
        for (const view_t& l : links()) {
            if (l.is_device()) return false;
        }
        return true;
    }

    /**
     * @brief The `[off, off + len)` sub-range as its own rope (chaining — no copy).
     *
     * Trims the covering links with @ref view_t::subview, so the result shares
     * (refcounts) exactly the segments its window touches and keeps only those
     * alive — the region primitive of the lazy decode tier (ADR-0053 §1): a
     * child TLV, a routed path suffix, or a payload handed onward is a subrope
     * of the inbound frame, never a copy of it.
     * @note Precondition: `off + len <= total_length()` (debug-asserted via the
     *       subview window invariant; a shorter tail yields a shorter rope).
     */
    [[nodiscard]] rope_t subrope(std::size_t off, std::size_t len) const {
        rope_t out;
        std::size_t skip = off;
        std::size_t remaining = len;
        for (const view_t& l : links()) {
            if (remaining == 0) break;
            if (skip >= l.length) {
                skip -= l.length;
                continue;
            }
            const std::size_t take = std::min(remaining, l.length - skip);
            if (take > 0) out.append(l.subview(skip, take));
            remaining -= take;
            skip = 0;
        }
        return out;
    }

    /** @brief Visit each link's contiguous bytes in order (parsers, serializers, CRC). */
    template <class Fn>
    void walk(Fn&& fn) const {
        for (const view_t& l : links()) fn(l.bytes());
    }

    /**
     * @brief Scatter-gather egress: spans into the original segments (no copy).
     *
     * Hand the result to `writev`/`sendmsg`-style I/O for true zero-copy transmit.
     */
    [[nodiscard]] std::vector<std::span<const std::byte>> to_iovec() const {
        std::vector<std::span<const std::byte>> iov;
        iov.reserve(link_count());
        for (const view_t& l : links()) iov.push_back(l.bytes());
        return iov;
    }

    /**
     * @brief Nothrow @ref to_iovec — fill @p out with one span per link (no copy),
     *        soft-failing instead of aborting when the span table cannot be grown.
     *
     * The `reserve` in @ref to_iovec throws on OOM (an `abort()` under
     * `-fno-exceptions`); a per-send egress table on a fragmented heap therefore aborted the
     * node. This nothrow-reserves @p out to @ref link_count first and reports the failure by
     * value instead. @p out is cleared on entry.
     *
     * @note Residual, and NO in-tree router path is on it any more. `std::span` IS trivially
     *       copyable, so unlike the link chain this table could sit on the ADR-0065 seam —
     *       but @p out is caller storage of a type this signature fixes, so the migration is
     *       an API change (a `%tr::mem::block_array_t` overload plus a source at every
     *       caller), not an edit here. What the growth keeps until then is
     *       `%tr::detail::try_reserve`'s `-fno-exceptions` probe window: a task switch
     *       between the probe's free and the `reserve` abort()s the node (#850). The
     *       router's egress iov tables took that migration in #981 and its two TERMINUS
     *       REPLY tables in #1570, each gathering `links()` into a `%block_array_t` at the
     *       call site; this helper is now used by tests, examples and the bench only.
     * @retval false The span table could not be reserved — @p out is left empty.
     */
    [[nodiscard]] bool try_to_iovec(std::vector<std::span<const std::byte>>& out) const noexcept {
        out.clear();
        if (!tr::detail::try_reserve(out, link_count())) return false;
        for (const view_t& l : links()) out.push_back(l.bytes());  // reserved — no reallocation
        return true;
    }

    /**
     * @brief Materialize the rope into one contiguous segment from @p backend (one copy).
     *
     * The single bridge-boundary copy — taken only when a flat-buffer consumer
     * demands it. The flattened view can then be cast with `decode(view_t)`
     * (`%frame.hpp`, `tr::wire`).
     * @note Lossy convenience (#917): both refusals collapse into the empty view,
     *       which a zero-length rope also returns on SUCCESS. A caller that must
     *       tell them apart calls @ref try_flatten.
     * @retval {} An empty view if the backend cannot allocate, **or if the rope
     *            is not @ref all_host** (a DEVICE link cannot be CPU-memcpy'd —
     *            docs/adr/0024; lower such a payload via its device transport).
     * @note Kept OUT OF LINE, and computed directly rather than by unwrapping
     *       @ref try_flatten's `expected` — both halves of that shape cost real time
     *       (#1250). Wrapping cost a copy: the wrapper held a `const expected`, so
     *       `std::move(*r)` yielded a `const view_t&&` that bound the COPY
     *       constructor — two extra atomic refcount RMWs per flatten. Defining it
     *       here cost the CALLER: the body's 32 B `expected` temp plus its stack
     *       canary pushed @ref materialize past this toolchain's inline threshold, so
     *       even the single-link zero-copy arm — which never flattens at all — paid an
     *       out-of-line call. Together, 25–48% on every `materialize` path.
     */
    [[nodiscard]] view_t flatten(mem::mem_backend_t& backend = mem::heap_backend()) const;

    /**
     * @brief @ref flatten with the failure cause kept distinct (#917).
     *
     * The honest form of the bridge-boundary copy: an empty view on the value side
     * means the rope really is zero bytes (a valid, if degenerate, flatten — no
     * segment is allocated for it), and a refusal names WHICH refusal it is,
     * @ref flatten_err_t::NOT_HOST (permanent for this rope) vs
     * @ref flatten_err_t::NO_MEMORY (transient backpressure). Collapsing those into
     * one empty view is what let a local OOM be reported as a malformed frame (#917).
     */
    [[nodiscard]] std::expected<view_t, flatten_err_t> try_flatten(
        mem::mem_backend_t& backend = mem::heap_backend()) const;

   private:
    /**
     * @brief The ONE flatten body — @ref flatten and @ref try_flatten are both thin
     *        out-of-line wrappers over it, so neither duplicates the logic and
     *        neither pays for the other's error channel (#1250).
     *
     * The value comes back BY VALUE and the error channel rides beside it, so the
     * lossy wrapper drops the channel for free while the flattened view is still
     * constructed straight into the wrapper's own return slot — no copy, no move, no
     * `expected` to unwrap.
     * @param err Assigned the refusal cause, and ONLY on a refusal.
     * @param ok Set to whether this was a flatten at all. The empty view is a
     *           legitimate SUCCESS for a zero-byte rope (#917), so the returned view
     *           cannot carry the verdict itself.
     * @return The flattened view when @p ok; the empty view otherwise.
     */
    [[nodiscard]] view_t flatten_core(mem::mem_backend_t& backend, flatten_err_t& err,
                                      bool& ok) const;

    // The SPILLING arm of try_reserve, kept OUT OF LINE so the no-op arm stays inlinable:
    // this half calls the allocator and migrates the small buffer. Carrying it in the same
    // body is what kept the whole check out of line at the path-target delivery clone on
    // this toolchain — v0.8.0 inlined try_reserve into `dispatch_edge_target`, `main` emits a
    // `call`, and lifting ONLY this arm out (nothing else changed) restores the inline
    // (#1065). Reached only when the chain has already spilled or the reservation will spill
    // it, so the `call` is charged to the case that allocates. Body-identical to the
    // pre-#1065 try_reserve, including the max_size guard: try_reserve now filters out only
    // inputs for which that guard cannot fire (links <= kInline).
    [[gnu::noinline]] [[nodiscard]] bool reserve_spilled(std::size_t links) noexcept {
        const std::size_t have = link_count();
        if (links > heap_.max_size() - have) return false;  // impossible count (also guards +)
        // A chain that fits the small-buffer storage never allocates and never throws on
        // append, so no reservation is needed (the +12% delivery tax was forcing heap_ here
        // for every small reply). `have + links` cannot overflow — the max_size guard above
        // bounds it below SIZE_MAX.
        if (have + links <= kInline) return true;
        // THE spill growth, and the one site that carries the whole rope's #981 residual:
        // under `-fno-exceptions` this is probe-then-commit and abort()s the node when a
        // racer takes the freed probe block (#850). See try_reserve's @note for why
        // `vector<view_t>` cannot become a `block_array_t`.
        if (!tr::detail::try_reserve(heap_, have + links)) return false;
        // Force heap_ mode: migrate the inline links so subsequent append()s take the
        // push_back fast path and never re-enter the inline→heap spill reserve.
        for (std::size_t i = 0; i < inline_n_; ++i) heap_.push_back(std::move(inline_[i]));
        if (inline_n_ > 0) {
            inline_n_ = 0;
            for (view_t& s : inline_) s = view_t{};
        }
        return true;
    }

    // Inline small-buffer storage for the first two links (the ADR-0053 §6 cost
    // guard). `heap_` is empty iff the chain is inline; the kInline+1-th append
    // spills the whole chain there and `inline_n_` goes to 0 (see @ref append).
    static constexpr std::size_t kInline = 2;
    std::array<view_t, kInline> inline_{};
    std::size_t inline_n_ = 0;
    std::vector<view_t> heap_;
};

/** @brief Concatenate two ropes (chaining — no copy). */
[[nodiscard]] inline rope_t operator+(rope_t lhs, const rope_t& rhs) {
    lhs.concat(rhs);
    return lhs;
}

}  // namespace tr::view
