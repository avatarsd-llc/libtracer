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
 * for the chain; only a third link spills the chain, into a block drawn from a
 * `tr::mem::block_source_t` (the one the rope was built with, else
 * `tr::mem::table_source()`), never from the process heap directly (#2284). This is the
 * ADR-0053 §6 trivial-case cost guard: a rope-valued vertex slot costs exactly
 * what a view-valued slot cost, so the contention/latency benches do not move.
 */
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <limits>
#include <new>
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
    /** @brief An empty rope whose spilled chain, if it ever spills, draws from
     *         `mem::table_source()`. */
    rope_t() noexcept {}
    /**
     * @brief An empty rope whose spilled chain draws from @p src (#2284): a receive path
     *        passes the receiving link's own source, so the receiver pays for the chain.
     * @note @p src must outlive the rope and every rope copied or moved from it.
     */
    explicit rope_t(mem::block_source_t& src) noexcept : src_(&src) {}
    /** @brief Implicitly adopt a single view as a one-link rope. */
    rope_t(view_t v) noexcept : n_(1) { ::new (static_cast<void*>(inline_)) view_t(std::move(v)); }

    /** @brief Copy: the links are cloned (refcount bumps) into a chain from the same source. */
    rope_t(const rope_t& o) : src_(o.src_) {
        if (o.n_ > kInline) grow_or_die(o.n_);
        const view_t* from = o.data();
        for (std::uint32_t i = 0; i < o.n_; ++i)
            ::new (static_cast<void*>(data() + i)) view_t(from[i]);
        n_ = o.n_;
    }
    /** @brief Move: the chain changes hands; @p o is left empty, keeping its source. */
    rope_t(rope_t&& o) noexcept : src_(o.src_) { take(o); }
    /** @brief Copy-assign (copy, then move into place). */
    rope_t& operator=(const rope_t& o) {
        if (this != &o) *this = rope_t(o);
        return *this;
    }
    /** @brief Move-assign: this rope's links are dropped and its block returned first. */
    rope_t& operator=(rope_t&& o) noexcept {
        if (this != &o) {
            clear();
            src_ = o.src_;
            take(o);
        }
        return *this;
    }
    /** @brief Drops every link and returns a spilled chain's block to its source. */
    ~rope_t() { clear(); }

    /** @brief Append a link (chaining — no copy). */
    void append(view_t v) {
        // The first kInline links fill the small buffer and allocate nothing; the next one
        // spills the chain into a block from the source, and a full block grows. A refused
        // growth here has no failure channel: see `failed_growth`.
        if (n_ == cap_) grow_or_die(n_ + 1);
        ::new (static_cast<void*>(data() + n_)) view_t(std::move(v));
        ++n_;
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
     * spill moves the inline links out and blanks their slots mid-walk (a naive range-for
     * once yielded `[a,b,a,{}]` instead of `[a,b,a,b]`), and a growth moves the chain the
     * walk points into to a new block (a dangling span). Two independent
     * guards close that: @ref try_reserve pins the final link count up front, so none of
     * the appends spills or reallocates; and the walk indexes the source afresh each step,
     * so link `i` is re-read from wherever the chain now lives even if the reservation
     * soft-failed. `append` only ever adds a link at the end — it never reorders or drops
     * one, and the spill migrates link `i` to block index `i` — so index `i` names the same
     * link for the whole walk.
     *
     * The CROSS-ROPE arm walks the source span once and appends: `other`'s storage is
     * disjoint from ours, so nothing this loop does can invalidate it, and neither guard
     * buys anything. Charging them there cost path-target delivery +3.5% / +10.1%
     * (`inproc-target-*`, #1022) for the hot 1–2-link clone. A caller that wants one sized
     * growth instead of the doubling ladder for a long cross-rope join calls
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
        // the ordinary growth path of @ref append, whose refusal has no failure channel.
        static_cast<void>(try_reserve(add));
        for (std::size_t i = 0; i < add; ++i) append(links()[i]);
        return *this;
    }

    /**
     * @brief Nothrow-reserve room for @p links more @ref append / @ref concat links —
     *        the soft-fail growth the composed-reply builder needs.
     *
     * @ref append has no failure channel, so a refused growth there ends the process
     * (`failed_growth`). A caller that knows its final link count reserves it here up
     * front: on success the next @p links @ref append calls are guaranteed
     * **non-reallocating hence nothrow**, and on refusal it drops the work instead.
     *
     * While the whole chain still fits INLINE (`have + links <= kInline`) this is a no-op
     * that touches no allocator — every `append` is then a construction into the small buffer
     * that cannot fail, so the hot small-reply delivery path (`assemble`) keeps its
     * zero-alloc small-buffer fast path (ADR-0053 §6). Only once the chain WILL spill does
     * it draw a block of `have + links` links from the source and migrate the inline links
     * there. On refusal the rope is unchanged and the caller drops the reply
     * (BACKPRESSURE). The block comes from a `block_source_t`'s `try_alloc`, which answers
     * `nullptr` on exhaustion, so the refusal is exact on every profile: there is no probe
     * window in which another task can take the block (#850, #981, closed by #2284).
     *
     * The no-op arm is the ONLY thing this function body holds, so it stays cheap enough for
     * the compiler to inline (#1065). The spilling arm — the count guard, the allocator
     * call and the inline→block migration — lives in a separate out-of-line member. Charging
     * the delivery clone a real `call` for a check that folds to one compare at a fresh
     * 1-link rope is a fixed per-dispatch cost on the hottest leg the rope has.
     * @retval false Reservation failed (OOM / impossible count) — the rope is untouched.
     */
    [[nodiscard]] bool try_reserve(std::size_t links) noexcept {
        // `cap_ - n_` is the room where the links are now (the small buffer, or the block) and
        // cannot underflow. The condition is the `have + links <= cap` no-op test spelled
        // without the addition, so no overflow guard is needed to reach it — and a caller
        // holding a fresh empty rope constant-folds it to `links <= kInline`.
        if (links <= cap_ - n_) return true;
        return reserve_spilled(links);
    }

    /** @brief Number of links in the chain. */
    [[nodiscard]] std::size_t link_count() const noexcept { return n_; }
    /** @brief The links, in order (inline small-buffer storage or the spilled chain). */
    [[nodiscard]] std::span<const view_t> links() const noexcept {
        return std::span<const view_t>(data(), n_);
    }

    /**
     * @brief The link chain, MUTABLE — for a caller that owns the rope and moves its links
     *        out (`tr::graph::value_t::make`), so a publish costs no refcount traffic.
     *
     * A link moved from is an empty view still counted in `link_count()`; reset the rope
     * afterwards rather than reading it.
     */
    [[nodiscard]] std::span<view_t> links() noexcept { return std::span<view_t>(data(), n_); }

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
     * @brief Scatter-gather egress into CALLER storage (#1781): one span per link, in order,
     *        no copy and no allocation.
     *
     * Writes the first `min(out.size(), link_count())` spans and returns @ref link_count, so a
     * caller whose table was too short sees how long it must be (a stack array of the link
     * counts it expects, or a `%tr::mem::block_array_t` it sizes first). Entries past the
     * returned count are left untouched.
     */
    [[nodiscard]] std::size_t to_iovec(std::span<std::span<const std::byte>> out) const noexcept {
        const std::span<const view_t> ls = links();
        const std::size_t n = ls.size() < out.size() ? ls.size() : out.size();
        for (std::size_t i = 0; i < n; ++i) out[i] = ls[i].bytes();
        return ls.size();
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
     * demands it. The flattened view can then be cast with `tlv_node_t::over(view_t)`
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
    // body is what kept the whole check out of line at the path-target delivery clone
    // (#1065), so the `call` is charged to the case that allocates.
    [[gnu::noinline]] [[nodiscard]] bool reserve_spilled(std::size_t links) noexcept {
        if (links > kMaxLinks - n_) return false;  // impossible count (also guards the +)
        const std::size_t want = n_ + links;
        if (want <= cap_) return true;
        return grow(want);
    }

    /**
     * @brief Move the chain into a block of at least @p want links (and at least double the
     *        current one), drawn from the rope's source.
     * @retval false The source refused, or @p want is past `kMaxLinks`; the rope is
     *               untouched.
     */
    [[gnu::noinline]] [[nodiscard]] bool grow(std::size_t want) noexcept {
        if (want > kMaxLinks) return false;
        // The first spill takes kInline + 1 links, as the `std::vector` chain did; then doubles.
        std::size_t cap = spill_ != nullptr ? cap_ * std::size_t{2} : kInline + 1;
        if (cap < want) cap = want;
        if (cap > kMaxLinks) cap = kMaxLinks;
        if (src_ == nullptr) src_ = &mem::table_source();
        auto* fresh = static_cast<view_t*>(src_->try_alloc(cap * sizeof(view_t), alignof(view_t)));
        if (fresh == nullptr) return false;
        relocate(fresh, data(), n_);
        if (spill_ != nullptr) src_->release(spill_, cap_ * sizeof(view_t), alignof(view_t));
        spill_ = fresh;
        cap_ = static_cast<std::uint32_t>(cap);
        return true;
    }

    /** @brief `grow`, for the callers with no failure channel (@ref append, the copy). */
    void grow_or_die(std::size_t want) {
        if (!grow(want)) failed_growth();
    }

    /**
     * @brief The end of a growth that cannot fail by value: `std::bad_alloc` where exceptions
     *        exist (what the `std::vector` chain before #2284 threw), `std::abort()` where they
     *        do not (what that throw became there). A caller that must survive exhaustion
     *        calls @ref try_reserve first.
     */
    [[noreturn]] [[gnu::cold]] [[gnu::noinline]] static void failed_growth() {
#if defined(__cpp_exceptions)
        throw std::bad_alloc();
#else
        std::abort();
#endif
    }

    /**
     * @brief Move @p n links from @p from to the raw storage at @p to, ending them at
     *        @p from. A view is a segment pointer and two sizes and nothing points at one,
     *        so its bytes at a new address are the same view: no move constructor and no
     *        destructor runs, which keeps relocation off the segment release path.
     */
    static void relocate(view_t* to, view_t* from, std::size_t n) noexcept {
        static_assert(mem::bitwise_relocatable<view_t> && mem::bitwise_relocatable<segment_ptr_t>,
                      "rope_t relocates its links by memcpy");
        if (n != 0)
            std::memcpy(static_cast<void*>(to), static_cast<const void*>(from), n * sizeof(view_t));
    }

    /** @brief Adopt @p o's links (@p o is left empty and inline); this rope holds none. */
    void take(rope_t& o) noexcept {
        static_assert(mem::bitwise_relocatable<view_t>,
                      "rope_t hands its links over by memcpy (relocate) or by the block pointer");
        if (o.spill_ != nullptr) {
            spill_ = o.spill_;
            cap_ = o.cap_;
        } else {
            relocate(inline_, o.inline_, o.n_);
        }
        n_ = o.n_;
        o.spill_ = nullptr;
        o.n_ = 0;
        o.cap_ = kInline;
    }

    /**
     * @brief Drop every link and return a spilled chain's block; the rope is then empty.
     *
     * Always inlined (it is the destructor's body, and a function of its own would put one
     * more function on the segment release path for the no-heap census to count), so it
     * holds only the inline arm: at most `kInline` links, unrolled. A spilled chain is
     * the case that allocated, and its release is the cold `drop_spilled`.
     */
    [[gnu::always_inline]] void clear() noexcept {
        if (spill_ != nullptr) [[unlikely]] {
            drop_spilled();
            return;
        }
        for (std::size_t i = 0; i < kInline; ++i)
            if (i < n_) inline_[i].~view_t();
        n_ = 0;
    }

    /** @brief `clear` for a spilled chain: drop its links and return its block. */
    [[gnu::cold]] [[gnu::noinline]] void drop_spilled() noexcept {
        for (std::uint32_t i = 0; i < n_; ++i) spill_[i].~view_t();
        src_->release(spill_, cap_ * sizeof(view_t), alignof(view_t));
        spill_ = nullptr;
        n_ = 0;
        cap_ = kInline;
    }

    /** @brief Where the links live: the block once spilled, the small buffer before. */
    [[nodiscard]] view_t* data() noexcept { return spill_ != nullptr ? spill_ : inline_; }
    /** @brief Read-only `data`. */
    [[nodiscard]] const view_t* data() const noexcept {
        return spill_ != nullptr ? spill_ : inline_;
    }

    // Inline small-buffer storage for the first two links (the ADR-0053 §6 cost guard).
    // `spill_` is null iff the chain is inline; the links are `data()[0, n_)` and `cap_` is
    // the room there (kInline while inline); the kInline+1-th append moves the whole chain
    // into a block from `src_` (see `grow`).
    static constexpr std::size_t kInline = 2;
    /** @brief The longest chain: a 32-bit count, so the rope stays the size it was, whose
     *         block size cannot overflow `std::size_t`. */
    static constexpr std::size_t kMaxLinks =
        std::numeric_limits<std::uint32_t>::max() < SIZE_MAX / sizeof(view_t)
            ? std::numeric_limits<std::uint32_t>::max()
            : SIZE_MAX / sizeof(view_t);
    // Raw small-buffer storage: a slot holds a live view only below `n_` while inline, so
    // an empty or one-link rope constructs and destroys nothing it does not hold.
    union {
        view_t inline_[kInline];
    };
    view_t* spill_ = nullptr;
    std::uint32_t n_ = 0;
    std::uint32_t cap_ = kInline;         // the inline slots, until the chain spills
    mem::block_source_t* src_ = nullptr;  // null until the first spill: then table_source()
};

/** @brief Concatenate two ropes (chaining — no copy). */
[[nodiscard]] inline rope_t operator+(rope_t lhs, const rope_t& rhs) {
    lhs.concat(rhs);
    return lhs;
}

}  // namespace tr::view
