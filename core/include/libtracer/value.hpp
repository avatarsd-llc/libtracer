/**
 * @file
 * @brief `value_t` — the one block a publish costs (RFC-0028 §5.1, slice 3), and
 *        `value_ref_t`, the owning handle over it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Before this header a vertex held its last-known value as a
 * `std::shared_ptr<const rope_t>`: a 16 B control block plus an 80 B `rope_t`, of which 48 B
 * were two inline `view_t`s that a single-link value leaves half empty — 104 B per publish on
 * the host, drawn through a throwing pmr channel that ADR-0079 Decision 3 had already ruled off
 * the hot path. ADR-0060 §1 named the end-state: one block packing the refcount and the value.
 * This is that block.
 *
 * ## Layout
 *
 * ```text
 * value_t                       (one try_alloc from the vertex's block_source_t per publish)
 * ┌──────────────┬────────────┬───────────┬──────────────────────────────────────────────┐
 * │ refs u32     │ links u32  │ source*   │ view_t links[n]                              │
 * └──────────────┴────────────┴───────────┴──────────────────────────────────────────────┘
 * ```
 *
 * The header is 16 B on a 64-bit host (12 B on rv32) and the payload is the value's link
 * chain, laid out in the same block: a single-link value is **40 B** on the host, where the
 * wrapper it replaces was 104 B. `refs` is a 32-bit atomic because a 32-bit AMO is native on
 * every target the library builds for, rv32 included. `source` is where the block came from,
 * so the last `release` reclaims it through the seam that served it (sized reclaim: the link
 * count is on the block, so no pool needs a header) — and a value therefore outlives nothing it
 * should not: an outstanding `value_ref_t` pins the block, exactly as the reader's own
 * `shared_ptr` did before (ADR-0069, deferred reclamation).
 *
 * The payload is a LINK CHAIN, not bytes: a `value_t` is what a `rope_t` is to its readers —
 * `links()`, `only()`, `total_length()`, `walk()`, `materialize()` — and the bytes stay where
 * the producer or the transport put them (zero-copy is preserved, ADR-0053 §6). RFC-0028 §5.1's
 * inline-bytes arm ("below the threshold the bytes follow the header") is the copy-or-share
 * policy of slice 5 and is not here. `rope_t` stays the EGRESS and the WRITE type: a caller
 * that needs one clones the links with @ref tr::graph::value_t::rope.
 *
 * ## Ownership
 *
 * A fresh block holds ONE reference, the maker's. `retain` / `release` move it around;
 * @ref tr::graph::value_ref_t is the RAII spelling and the only one application code should need.
 * The LKV slot policies (`%lkv_slot.hpp`) hold one reference per published value and hand one to
 * every reader; a STREAM ring holds one per retained entry; a subscribe-time latch holds one
 * until it has delivered.
 *
 * A value delivered to a subscriber callback (`const value_t&`) is valid **for the call**. A
 * sink that wants to keep it clones its links (@ref tr::graph::value_t::rope) — the delivered
 * object may be a stack-resident @ref tr::graph::value_storage_t that a branch write built over a
 * slice it never stored.
 */
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include "libtracer/backend.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"

namespace tr::graph {

using tr::view::rope_t;
using tr::view::view_t;

/**
 * @brief One published value: an intrusive refcount, its source, and its link chain, in one
 *        block (RFC-0028 §5.1).
 *
 * Never constructed directly — @ref make draws the block from a `block_source_t` and places the
 * header and the links in it; the last @ref release destroys the links and hands the block
 * back to that source. The read surface is the read-only half of `rope_t`'s, so a consumer
 * that used to receive `const rope_t&` reads a `const value_t&` the same way.
 */
class value_t {
   public:
    /** @brief Alignment every block is drawn and released at — the links' own. */
    static constexpr std::size_t kAlign = alignof(view_t) > alignof(void*) ? alignof(view_t)
                                                                           : alignof(void*);

    value_t(const value_t&) = delete;
    value_t& operator=(const value_t&) = delete;

    /**
     * @brief The block size a value with @p links links occupies — header plus chain.
     *
     * This is what a publish costs in bytes and what @ref release hands back to the source.
     */
    [[nodiscard]] static constexpr std::size_t bytes_for(std::size_t links) noexcept {
        return sizeof(value_t) + links * sizeof(view_t);
    }

    /**
     * @brief Mint a value over @p links, MOVING the chain out of the rope — no refcount
     *        traffic on the links.
     *
     * One `try_alloc` on @p source, nothrow (#477): exhaustion is `nullptr` and @p links is
     * left intact, so the caller still owns the chain it offered and can soft-fail.
     *
     * @param links  The chain to publish. Emptied on success; untouched on `nullptr`.
     * @param source The block source the value's block is drawn from and released to. Must
     *               outlive every reference to the value (the "handles do not outlive the
     *               graph's memory" contract the injection seam already imposes).
     * @return The value, holding ONE reference (the caller's), or `nullptr` when @p source
     *         refused the block.
     */
    [[nodiscard]] static value_t* make(rope_t&& links, mem::block_source_t& source) noexcept {
        const std::span<view_t> in = links.links();
        value_t* v = place(in.size(), source);
        if (v == nullptr) return nullptr;
        view_t* out = v->slots();
        for (std::size_t i = 0; i < in.size(); ++i) new (out + i) view_t(std::move(in[i]));
        links = rope_t{};  // the moved-from chain: drop it so the rope is empty, not a husk
        return v;
    }

    /**
     * @brief Mint a value over a COPY of @p links (one refcount clone per link).
     *
     * The spelling for a chain the caller does not own — a subview of an inbound frame, another
     * value's links. Same one-`try_alloc`, nothrow contract as the rope form.
     */
    [[nodiscard]] static value_t* make(std::span<const view_t> links,
                                       mem::block_source_t& source) noexcept {
        value_t* v = place(links.size(), source);
        if (v == nullptr) return nullptr;
        view_t* out = v->slots();
        for (std::size_t i = 0; i < links.size(); ++i) new (out + i) view_t(links[i]);
        return v;
    }

    /** @brief Take one more reference. Relaxed: a holder retaining already owns one. */
    void retain() const noexcept { refs_.fetch_add(1, std::memory_order_relaxed); }

    /**
     * @brief Drop one reference; the last one destroys the links and returns the block to
     *        its source. Null-safe.
     *
     * Acquire-release on the decrement, the canonical intrusive release: the thread that frees
     * sees every write the other holders made before they released.
     */
    static void release(const value_t* v) noexcept {
        if (v == nullptr) return;
        if (v->refs_.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        destroy(const_cast<value_t*>(v));
    }

    /** @brief The current reference count (a snapshot; for tests and accounting). */
    [[nodiscard]] std::uint32_t use_count() const noexcept {
        return refs_.load(std::memory_order_acquire);
    }

    /** @brief The source the block is released to, or `nullptr` for storage the caller owns
     *         (@ref value_storage_t). */
    [[nodiscard]] mem::block_source_t* source() const noexcept { return source_; }

    /** @brief The block's size in bytes: @ref bytes_for of its link count. */
    [[nodiscard]] std::size_t block_bytes() const noexcept { return bytes_for(n_); }

    // ---- the read surface: `rope_t`'s read-only half ------------------------------------

    /** @brief The link chain, in order. */
    [[nodiscard]] std::span<const view_t> links() const noexcept {
        return std::span<const view_t>(slots(), n_);
    }

    /** @brief Number of links in the chain. */
    [[nodiscard]] std::size_t link_count() const noexcept { return n_; }

    /** @brief The single link of a one-link value. Precondition: `link_count() == 1`. */
    [[nodiscard]] const view_t& only() const noexcept {
        assert(n_ == 1);
        return slots()[0];
    }

    /** @brief Total payload bytes across the chain. */
    [[nodiscard]] std::size_t total_length() const noexcept {
        std::size_t n = 0;
        for (const view_t& l : links()) n += l.length;
        return n;
    }

    /** @brief True iff every link is in HOST space (CPU-readable). */
    [[nodiscard]] bool all_host() const noexcept {
        for (const view_t& l : links()) {
            if (l.is_device()) return false;
        }
        return true;
    }

    /** @brief Visit each link's byte span in order. */
    template <class Fn>
    void walk(Fn&& fn) const {
        for (const view_t& l : links()) fn(l.bytes());
    }

    /**
     * @brief One contiguous view of the value: the link itself when there is one, else a
     *        flattened copy through @p backend.
     */
    [[nodiscard]] view_t materialize(mem::mem_backend_t& backend = mem::heap_backend()) const {
        if (n_ == 1) return slots()[0];
        return rope().flatten(backend);
    }

    /** @brief The nothrow twin of @ref materialize — the flatten's refusal comes back by value. */
    [[nodiscard]] std::expected<view_t, view::flatten_err_t> try_materialize(
        mem::mem_backend_t& backend = mem::heap_backend()) const {
        if (n_ == 1) return slots()[0];
        return rope().try_flatten(backend);
    }

    /** @brief One contiguous copy of the whole payload through @p backend (`rope_t::flatten`). */
    [[nodiscard]] view_t flatten(mem::mem_backend_t& backend = mem::heap_backend()) const {
        return rope().flatten(backend);
    }

    /** @brief The nothrow twin of @ref flatten — the refusal comes back by value. */
    [[nodiscard]] std::expected<view_t, view::flatten_err_t> try_flatten(
        mem::mem_backend_t& backend = mem::heap_backend()) const {
        return rope().try_flatten(backend);
    }

    /** @brief The links as byte spans, in order — the scatter-gather shape a link's `send`
     *         takes. Allocates the vector; @ref try_to_iovec is the nothrow spelling. */
    [[nodiscard]] std::vector<std::span<const std::byte>> to_iovec() const {
        std::vector<std::span<const std::byte>> iov;
        iov.reserve(n_);
        for (const view_t& l : links()) iov.push_back(l.bytes());
        return iov;
    }

    /** @brief Nothrow @ref to_iovec into @p out (cleared first).
     *  @retval false @p out could not be reserved; it is left empty. */
    [[nodiscard]] bool try_to_iovec(std::vector<std::span<const std::byte>>& out) const noexcept {
        out.clear();
        if (!tr::detail::try_reserve(out, n_)) return false;
        for (const view_t& l : links()) out.push_back(l.bytes());  // reserved — no reallocation
        return true;
    }

    /**
     * @brief A `rope_t` over this value's links — one refcount clone per link.
     *
     * The bridge to every seam that speaks `rope_t` (egress, decode, a sink that keeps the
     * value past the call). A chain longer than the rope's inline buffer allocates the rope's
     * spill; @ref try_rope is the nothrow spelling for a hot leg.
     */
    [[nodiscard]] rope_t rope() const {
        rope_t r;
        for (const view_t& l : links()) r.append(l);
        return r;
    }

    /**
     * @brief Nothrow @ref rope into @p out: reserves the chain first, so the appends cannot
     *        reallocate.
     *
     * Out of line: this is the delivery clone `dispatch_edge_target` takes once per bound
     * edge, and inlining the reserve-then-append loop there cost that pinned symbol ~100 B
     * on the symbol ratchet against the one `concat` call it replaced. One call keeps the
     * dispatch body's shape; the loop itself is the same either way.
     * @retval false The chain could not be reserved — @p out is left as it was.
     */
    [[gnu::noinline]] [[nodiscard]] bool try_rope(rope_t& out) const noexcept {
        if (!out.try_reserve(n_)) return false;
        for (const view_t& l : links()) out.append(l);
        return true;
    }

   private:
    template <std::size_t N>
    friend class value_storage_t;

    /**
     * @brief The last reference's teardown: destroy the links, hand the block back.
     *
     * Out of line and cold on purpose. @ref release is inlined at every handle destructor —
     * a slot swap, a latch reset, a delivered edge snapshot — and the common outcome there is
     * "somebody else still holds it": one decrement and a branch. Inlining the teardown too
     * (the virtual `release`, the size arithmetic, the links' destructors) at each of those
     * sites re-partitioned the graph's inline budget by ~100 B per function on the symbol
     * ratchet, for a path that runs once per value. One call, in `.text.unlikely`.
     */
    [[gnu::noinline, gnu::cold]] static void destroy(value_t* self) noexcept {
        mem::block_source_t* const source = self->source_;
        const std::size_t bytes = self->block_bytes();
        self->~value_t();
        if (source != nullptr) source->release(self, bytes, kAlign);
    }

    /** @brief Place the header; the links follow and are the caller's to construct. */
    value_t(std::uint32_t n, mem::block_source_t* source) noexcept
        : refs_(1), n_(n), source_(source) {}

    /** @brief Destroy the links. The block itself is the caller's (see @ref release). */
    ~value_t() {
        view_t* s = slots();
        for (std::size_t i = 0; i < n_; ++i) s[i].~view_t();
    }

    /** @brief Draw and place a header for @p n links; the link slots are uninitialised. */
    [[nodiscard]] static value_t* place(std::size_t n, mem::block_source_t& source) noexcept {
        if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
            if (n > UINT32_MAX) return nullptr;  // a chain the header cannot count
        }
        void* p = source.try_alloc(bytes_for(n), kAlign);
        if (p == nullptr) return nullptr;
        return new (p) value_t(static_cast<std::uint32_t>(n), &source);
    }

    /** @brief The link slots, which start right after the header. */
    [[nodiscard]] view_t* slots() noexcept {
        return reinterpret_cast<view_t*>(reinterpret_cast<std::byte*>(this) + sizeof(value_t));
    }
    /** @brief The link slots, read-only. */
    [[nodiscard]] const view_t* slots() const noexcept {
        return reinterpret_cast<const view_t*>(reinterpret_cast<const std::byte*>(this) +
                                               sizeof(value_t));
    }

    mutable std::atomic<std::uint32_t> refs_; /**< @brief Outstanding references. */
    std::uint32_t n_;                         /**< @brief Link count — the block's size. */
    mem::block_source_t* source_;             /**< @brief Where the block is released to. */
};

static_assert(sizeof(value_t) % alignof(view_t) == 0,
              "the links follow the header in the same block, so the header must end on a "
              "link boundary — pad the header explicitly if a member is added");

/**
 * @brief An owning reference to a vertex's PUBLISHED value — what @ref graph_t::read and
 *        @ref graph_t::await hand back, and what every slot policy's `load()` returns.
 *
 * A read of a PUBLISHED value returns a reference to it; a read that COMPOSES a new value
 * returns the value (the rule @ref graph_t::read_children_folded and its siblings follow,
 * still answering `rope_t`). Measured when the rule was drawn, both arms alternating inside
 * one binary on a 24-thread host: median 1.37x aggregate for the reference over a rope copy,
 * and p50 improving most where it hurts most — 2,104 ns to 1,193 ns at sixteen readers on one
 * shared vertex.
 *
 * Holding one keeps the value's block alive — and under an injected `block_source_t` that is a
 * real obligation: the block was drawn from the graph's source, so an outstanding reference
 * pins it (ADR-0069, deferred reclamation). **A `value_ref_t` must not outlive the graph it
 * was read from.**
 */
class value_ref_t {
   public:
    value_ref_t() = default;

    /** @brief Adopt a reference the caller already holds (a fresh @ref value_t::make, a
     *         slot's retained load). */
    [[nodiscard]] static value_ref_t adopt(value_t* v) noexcept { return value_ref_t{v}; }

    /** @brief Take a NEW reference on @p v (one `retain`). */
    [[nodiscard]] static value_ref_t share(const value_t* v) noexcept {
        if (v != nullptr) v->retain();
        return value_ref_t{const_cast<value_t*>(v)};
    }

    /**
     * @brief Take ownership of a freshly COMPOSED value, giving it a published value's shape.
     *
     * The composed branch read builds a rope no vertex published; this is what lets it answer
     * the same signature. It draws one block from the global heap — the composed value has no
     * vertex, so no injected source — which the published path does not pay; measured neutral
     * (1.00x over 30 paired samples), because a subtree walk dominates it.
     *
     * @return The reference, or an EMPTY one when the heap refused the block (#477).
     */
    [[nodiscard]] static value_ref_t composed(rope_t&& r) noexcept {
        return value_ref_t{value_t::make(std::move(r), mem::heap_source())};
    }

    /** @brief A copy is one more reference to the same value. */
    value_ref_t(const value_ref_t& other) noexcept : p_(other.p_) {
        if (p_ != nullptr) p_->retain();
    }
    /** @brief A move takes @p other's reference and leaves it empty. */
    value_ref_t(value_ref_t&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    /** @brief Copy-and-swap: the old reference is dropped as @p other dies. */
    value_ref_t& operator=(value_ref_t other) noexcept {
        std::swap(p_, other.p_);
        return *this;
    }
    ~value_ref_t() { value_t::release(p_); }

    /** @brief Drop the reference, leaving this empty. */
    void reset() noexcept {
        value_t::release(p_);
        p_ = nullptr;
    }

    /** @brief The referenced value. Undefined if this reference is empty. */
    [[nodiscard]] const value_t& operator*() const noexcept { return *p_; }
    /** @brief Member access on the referenced value. */
    [[nodiscard]] const value_t* operator->() const noexcept { return p_; }
    /** @brief The referenced value, or null. */
    [[nodiscard]] const value_t* get() const noexcept { return p_; }
    /** @brief Whether this reference names a value. */
    [[nodiscard]] explicit operator bool() const noexcept { return p_ != nullptr; }

    /** @brief Identity: two references to the same block. */
    [[nodiscard]] friend bool operator==(const value_ref_t& a, const value_ref_t& b) noexcept {
        return a.p_ == b.p_;
    }
    /** @brief Identity against a raw value pointer (`nullptr` tests emptiness). */
    [[nodiscard]] friend bool operator==(const value_ref_t& a, const value_t* b) noexcept {
        return a.p_ == b;
    }

   private:
    explicit value_ref_t(value_t* p) noexcept : p_(p) {}

    value_t* p_ = nullptr;
};

/**
 * @brief Caller-owned storage for a value of up to @p N links that is never published —
 *        the shape a branch write delivers a slice it did not store in (RFC-0005).
 *
 * The value lives in this object, so no source is drawn from and nothing is released: the
 * header's `source` is null. It holds the ONE reference the storage owns; a consumer must not
 * keep a reference past the storage's lifetime (asserted in debug builds).
 */
template <std::size_t N>
class value_storage_t {
   public:
    /** @brief Build over a copy of @p links (one refcount clone per link). Precondition:
     *         `links.size() <= N`. */
    explicit value_storage_t(std::span<const view_t> links) noexcept {
        assert(links.size() <= N);
        auto* v = new (buf_) value_t(static_cast<std::uint32_t>(links.size()), nullptr);
        view_t* out = v->slots();
        for (std::size_t i = 0; i < links.size(); ++i) new (out + i) view_t(links[i]);
    }
    /** @brief Build over one link. */
    explicit value_storage_t(const view_t& link) noexcept
        : value_storage_t(std::span<const view_t>(&link, 1)) {}
    /** @brief Build over a rope's chain. Precondition: `r.link_count() <= N`. */
    explicit value_storage_t(const rope_t& r) noexcept : value_storage_t(r.links()) {}

    value_storage_t(const value_storage_t&) = delete;
    value_storage_t& operator=(const value_storage_t&) = delete;

    ~value_storage_t() {
        assert(get().use_count() == 1 && "a reference to a stack value outlived its storage");
        value_t::release(&get());  // the last reference: destroys the links, frees nothing
    }

    /** @brief The value. */
    [[nodiscard]] const value_t& get() const noexcept {
        return *reinterpret_cast<const value_t*>(buf_);
    }
    /** @brief The value, by conversion. */
    [[nodiscard]] operator const value_t&() const noexcept { return get(); }

   private:
    alignas(value_t::kAlign) std::byte buf_[value_t::bytes_for(N)]; /**< @brief In-place block. */
};

}  // namespace tr::graph
