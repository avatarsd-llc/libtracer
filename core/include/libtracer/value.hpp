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
 * The payload is a LINK CHAIN: a `value_t` is what a `rope_t` is to its readers — `links()`,
 * `only()`, `total_length()`, `walk()`, `materialize()` — and a SHARED value's bytes stay where
 * the producer or the transport put them (zero-copy is preserved, ADR-0053 §6). `rope_t` stays
 * the EGRESS and the WRITE type: a caller that needs one clones the links with
 * @ref tr::graph::value_t::rope.
 *
 * ## The inline arm (RFC-0028 §5.1, slice 5)
 *
 * A value BELOW its vertex's share threshold is COPIED, and the copy lands in the value's own
 * block — no second allocation for the bytes, no third for a segment header:
 *
 * ```text
 * value_t, inline                (one try_alloc; value_t::make_inline)
 * ┌──────────┬──────────┬─────────┬──────────────┬──────────────────────┬──────────────┐
 * │ refs u32 │ links=1  │ source* │ view_t link  │ segment_t (embedded) │ bytes[len]   │
 * └──────────┴──────────┴─────────┴──────────────┴──────────────────────┴──────────────┘
 * ```
 *
 * The one link is an ordinary `view_t` over a `segment_t` embedded in the same block, so the
 * whole read surface — `links()`, `only()`, `materialize()`, a `rope()` clone a sink keeps —
 * works unchanged: `view_t` has no owner-less byte form, and a `segment_t` is the smallest
 * owner it has. That owner is why the block is **80 + len bytes on the host (44 + len on
 * rv32)** and not the prototype's 24 + len: 16 B header + 24 B link + 40 B segment (12 + 12 +
 * 20 on rv32). It replaces the copy arm's THREE blocks (the value's link block, the copied
 * segment's header, its bytes) of the same total size.
 *
 * Two lifetimes share the block. `refs` counts VALUE references (slots, handles); the embedded
 * segment's refcount counts the LINK and every clone of it. The last value reference drops the
 * link's segment reference and nothing else, and the block goes back to its source when the
 * segment's count reaches zero — so a `rope_t` a sink cloned out of the value keeps the bytes
 * alive past the value, exactly as it would for a shared value's segment.
 *
 * A rope that is exactly one live inline value's bytes IS that value:
 * @ref tr::graph::value_t::make adopts it (one reference) instead of drawing a block, which is
 * how a terminus hands an inline copy to `graph_t::write`'s `rope_t` surface and still publishes
 * ONE block.
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
#include <cstring>
#include <expected>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include "libtracer/backend.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

namespace tr::graph {

using tr::view::rope_t;
using tr::view::view_t;

/**
 * @brief The reclaimer of an inline value's embedded segment (RFC-0028 §5.1, slice 5).
 *
 * One process-wide instance, @ref inline_value_backend. It allocates nothing: its only job is
 * the last segment reference's `destroy`, which hands the WHOLE block — header, link, segment
 * and bytes — back to the `block_source_t` the value was drawn from. It is also the identity
 * that marks a segment as embedded: a segment whose backend is this one lives inside a
 * `value_t`'s block (`value_t::inline_owner`).
 */
class inline_value_backend_t final : public mem::mem_backend_t {
   public:
    /** @brief The singleton's name, as census and diagnostics print it. */
    inline_value_backend_t() noexcept : mem::mem_backend_t("value_inline") {}
    /** @brief Return the enclosing value block to its source. Defined after `value_t`. */
    void destroy(view::segment_t* seg) noexcept override;
    /** @brief The inline bytes follow the segment header, so they are aligned to it. */
    [[nodiscard]] std::size_t alignment() const noexcept override {
        return alignof(view::segment_t);
    }
};

/**
 * @brief The one @ref inline_value_backend_t.
 *
 * Constructed on first use into static storage and NEVER destroyed: a value released during
 * static destruction (a graph with static storage duration) still reaches a live reclaimer.
 */
[[nodiscard]] inline inline_value_backend_t& inline_value_backend() noexcept {
    alignas(inline_value_backend_t) static std::byte storage[sizeof(inline_value_backend_t)];
    static inline_value_backend_t* const b = new (storage) inline_value_backend_t();
    return *b;
}

/**
 * @brief The "source" of a value whose header lives in a loaned receive block (RFC-0028 §6.9,
 *        #1626) — it serves nothing and reclaims nothing.
 *
 * A loaned value's header sits in the receive block's reserve (`view::kRxLoanBytes`), and the
 * block goes back to ITS backend when the last segment reference drops, exactly as it did
 * before the value existed. So there is nothing for a value source to hand out or take back:
 * `try_alloc` refuses and `release` is never asked. It exists as an IDENTITY — non-null, so a
 * loaned value is a published block to every seam that tells a stack value from a shared one
 * (`value_ref_t::keep`, the target adopt), and unique, so the last release knows which arm it
 * is on.
 */
class rx_loan_source_t final : public mem::block_source_t {
   public:
    /** @brief The singleton's name, as census and diagnostics print it. */
    rx_loan_source_t() noexcept : mem::block_source_t("rx_loan") {}
    /** @brief Refuses: a loaned header is never drawn, it is placed. */
    [[nodiscard]] void* try_alloc(std::size_t, std::size_t) noexcept override { return nullptr; }
    /** @brief Never reached: the loaned arm of the last release returns before it. */
    void release(void*, std::size_t, std::size_t) noexcept override {}
};

/**
 * @brief The one @ref rx_loan_source_t. Static storage, never destroyed, for the reason
 *        @ref inline_value_backend gives.
 */
[[nodiscard]] inline rx_loan_source_t& rx_loan_source() noexcept {
    alignas(rx_loan_source_t) static std::byte storage[sizeof(rx_loan_source_t)];
    static rx_loan_source_t* const s = new (storage) rx_loan_source_t();
    return *s;
}

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
        // A rope that is exactly one live inline value's bytes IS that value: adopt it (one
        // reference) rather than drawing a second block that links to the first.
        if (links.link_count() == 1) {
            value_t* const inl = inline_owner(links.links()[0]);
            if (inl != nullptr && inl->try_retain()) {
                links = rope_t{};
                return inl;
            }
            // A loaned receive block (RFC-0028 §6.9): the record goes IN the block. One byte
            // tested inline; the claim and the placement are out of line and cold.
            const view::segment_t* const seg = links.links()[0].owner.get();
            if (seg != nullptr && seg->rx_loan != 0) [[unlikely]] {
                if (value_t* const loaned = make_loaned(links)) return loaned;
            }
        }
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

    /**
     * @brief The block size an INLINE value of @p len bytes occupies: header, its one link,
     *        the embedded segment, and the bytes.
     */
    [[nodiscard]] static constexpr std::size_t inline_bytes_for(std::size_t len) noexcept {
        return bytes_for(1) + sizeof(view::segment_t) + len;
    }

    /**
     * @brief Mint an INLINE value of @p len bytes — the copy arm of the copy-or-share policy
     *        (RFC-0028 §5.1 / §5.3): ONE `try_alloc` of `inline_bytes_for(len)` on @p source.
     *
     * The bytes are UNINITIALISED: the maker fills them through @ref inline_bytes before it
     * publishes the value or hands out a reference (the only window in which a value's bytes
     * may be written). Nothrow (#477): exhaustion is `nullptr`.
     *
     * @return The value, holding ONE reference (the caller's), or `nullptr` when @p source
     *         refused the block or @p len does not fit the header's count.
     */
    [[nodiscard]] static value_t* make_inline(std::size_t len,
                                              mem::block_source_t& source) noexcept {
        if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
            if (len > UINT32_MAX) return nullptr;
        }
        void* p = source.try_alloc(inline_bytes_for(len), kAlign);
        if (p == nullptr) return nullptr;
        auto* v = new (p) value_t(1, &source);
        auto* raw = static_cast<std::byte*>(p);
        auto* seg = new (raw + bytes_for(1)) view::segment_t(
            &inline_value_backend(),
            std::span<std::byte>(raw + bytes_for(1) + sizeof(view::segment_t), len));
        new (v->slots()) view_t{view::segment_ptr_t::adopt(seg), 0, len};
        return v;
    }

    /** @brief @ref make_inline over a copy of @p bytes — one block, one `memcpy`. */
    [[nodiscard]] static value_t* make_copy(std::span<const std::byte> bytes,
                                            mem::block_source_t& source) noexcept {
        value_t* v = make_inline(bytes.size(), source);
        if (v != nullptr && !bytes.empty())
            std::memcpy(v->inline_bytes().data(), bytes.data(), bytes.size());
        return v;
    }

    /**
     * @brief The inline value whose embedded segment @p link is the WHOLE of, or `nullptr`.
     *
     * The backend identity says the segment lives inside a value block; the full-window test
     * says the link is the value's bytes and not a subview of them (a subview is a different
     * value, and publishes as a link to this one).
     */
    [[nodiscard]] static value_t* inline_owner(const view_t& link) noexcept {
        view::segment_t* const seg = link.owner.get();
        if (seg == nullptr || seg->backend != &inline_value_backend()) return nullptr;
        if (link.offset != 0 || link.length != seg->bytes.size()) return nullptr;
        return reinterpret_cast<value_t*>(reinterpret_cast<std::byte*>(seg) - bytes_for(1));
    }

    /** @brief Whether this value's header lives in a loaned receive block (RFC-0028 §6.9). */
    [[nodiscard]] bool is_loaned() const noexcept { return source_ == &rx_loan_source(); }

    /** @brief Whether this value's bytes live in its own block (the copy arm). */
    [[nodiscard]] bool is_inline() const noexcept {
        return n_ == 1 && slots()[0].owner.get() == embedded_segment() &&
               embedded_segment()->backend == &inline_value_backend();
    }

    /**
     * @brief The inline bytes, WRITABLE — for the maker to fill between @ref make_inline and
     *        publication. Precondition: @ref is_inline.
     */
    [[nodiscard]] std::span<std::byte> inline_bytes() noexcept {
        assert(is_inline());
        return embedded_segment()->bytes;
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

    /** @brief The block's size in bytes: @ref inline_bytes_for its length for an inline value,
     *         else @ref bytes_for its link count. */
    [[nodiscard]] std::size_t block_bytes() const noexcept {
        return is_inline() ? inline_bytes_for(slots()[0].length) : bytes_for(n_);
    }

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
     * Out of line: until RFC-0028 slice 4 this was the delivery clone `dispatch_edge_target`
     * took once per bound edge, and inlining the reserve-then-append loop there cost that
     * pinned symbol ~100 B on the symbol ratchet. A target now ADOPTS the published block, and
     * the adopting `store_value` takes this clone only where it cannot adopt (a HANDLER
     * target, caller-owned storage) or to show an admission filter the links. One call keeps
     * each of those bodies' shape; the loop itself is the same either way.
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
    friend class inline_value_backend_t;

    /** @brief The segment an inline value embeds, right after its one link (meaningful only
     *         when @ref is_inline). */
    [[nodiscard]] view::segment_t* embedded_segment() const noexcept {
        return reinterpret_cast<view::segment_t*>(
            const_cast<std::byte*>(reinterpret_cast<const std::byte*>(this)) + bytes_for(1));
    }

    /**
     * @brief Take a reference only while one is still held — the weak-to-strong upgrade a
     *        rope over an inline value's bytes needs, since the rope pins the BLOCK (through
     *        the segment) but not the VALUE: once the last value reference is gone the value
     *        is dead, and resurrecting it would publish a link that was already torn down.
     */
    [[nodiscard]] bool try_retain() const noexcept {
        std::uint32_t n = refs_.load(std::memory_order_relaxed);
        while (n != 0) {
            if (refs_.compare_exchange_weak(n, n + 1, std::memory_order_acquire,
                                            std::memory_order_relaxed))
                return true;
        }
        return false;
    }

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
        if (self->is_loaned()) {
            // The header lives INSIDE the block its one link references, so the link is the
            // last thing touched: moved out first, the view torn down, and the block returned
            // to its backend as the moved reference drops — possibly freeing this header.
            view::segment_ptr_t link = std::move(self->slots()[0].owner);
            self->slots()[0].~view_t();
            return;
        }
        if (self->is_inline()) {
            // The link's segment reference is the block's; move it OUT first, so the view's
            // own teardown writes nothing into a block its last reference may free. The block
            // goes back to its source when the segment's count reaches zero — now, or when the
            // last rope a sink cloned out of this value lets go.
            view::segment_ptr_t link = std::move(self->slots()[0].owner);
            self->slots()[0].~view_t();
            return;  // `link` drops here: inline_value_backend_t::destroy reclaims the block
        }
        mem::block_source_t* const source = self->source_;
        const std::size_t bytes = self->block_bytes();
        self->~value_t();
        if (source != nullptr) source->release(self, bytes, kAlign);
    }

    /**
     * @brief The ingress loan (RFC-0028 §6.9, #1626): place the value over @p links' one link
     *        IN the reserve of the receive block that link views, and move the link in.
     *
     * The block's `rx_loan` bit says the reserve exists (`view::alloc_rx` set it); the claim
     * word says nobody placed a value in it yet. A link that reaches into the reserve itself
     * is refused (no transport hands one out, and the header would overlay its bytes). Every
     * refusal returns `nullptr` and leaves @p links intact, so the caller draws its record
     * from its source exactly as it would for an ordinary block.
     *
     * The value holds one reference to the block it lives in, through its own link, so the
     * block outlives the header by construction; the last value reference's teardown
     * (@ref destroy) moves that link out before dropping it.
     */
    [[gnu::noinline, gnu::cold]] static value_t* make_loaned(rope_t& links) noexcept {
        view_t& link = links.links()[0];
        view::segment_t* const seg = link.owner.get();
        if (link.offset < view::kRxLoanBytes || !view::claim_rx_loan(seg)) return nullptr;
        auto* const v =
            new (seg->bytes.data() + view::kRxLoanValueOffset) value_t(1, &rx_loan_source());
        new (v->slots()) view_t(std::move(link));
        links = rope_t{};
        return v;
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
static_assert(value_t::bytes_for(1) % alignof(view::segment_t) == 0,
              "an inline value's segment follows its one link in the same block");
static_assert(view::kRxLoanValueOffset % value_t::kAlign == 0 &&
                  view::kRxLoanValueOffset >= sizeof(view::rx_loan_word_t) &&
                  view::kRxLoanValueOffset + value_t::bytes_for(1) <= view::kRxLoanBytes,
              "a loaned receive block's reserve must hold the claim word and a one-link value "
              "header, aligned (RFC-0028 §6.9)");

/**
 * @brief The last segment reference of an inline value: hand the whole block back.
 *
 * The header outlives the value on purpose — `refs`, the count and `source` are trivially
 * destructible and nothing tore them down — so the reclaimer reads `source` here.
 */
inline void inline_value_backend_t::destroy(view::segment_t* seg) noexcept {
    auto* const block = reinterpret_cast<std::byte*>(seg) - value_t::bytes_for(1);
    const auto* const v = reinterpret_cast<const value_t*>(block);
    mem::block_source_t* const source = v->source_;
    const std::size_t bytes = value_t::inline_bytes_for(seg->bytes.size());
    seg->~segment_t();
    if (source != nullptr) source->release(block, bytes, value_t::kAlign);
}

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

    /**
     * @brief Keep a BORROWED value past the call that lent it — the one way a seam that was
     *        handed `const value_t&` (a subscriber callback, `handlers_t::on_write`,
     *        `handlers_t::on_admit`) retains it (RFC-0028 D10).
     *
     * A value drawn from a source (a published block — what a subscription edge delivers) is
     * SHARED: one refcount bump, no allocation. A value with no source is caller-owned storage
     * on the writer's stack (`value_storage_t`, the relay and local-handler shape), which dies
     * when the call returns, so its links are cloned into ONE fresh block from @p source — the
     * payload bytes are never copied, only the links' references.
     *
     * Never keep the reference itself, or its address: a stack value's storage asserts, on the
     * way out, that nothing did.
     *
     * @return The reference, or an EMPTY one when @p source refused the block (#477).
     */
    [[nodiscard]] static value_ref_t keep(
        const value_t& v, mem::block_source_t& source = mem::heap_source()) noexcept {
        if (v.source() != nullptr) return share(&v);
        return value_ref_t{value_t::make(v.links(), source)};
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
    /** @brief Build over a rope's chain by MOVING its links in — no refcount traffic; @p r is
     *         left empty. Precondition: `r.link_count() <= N`. */
    explicit value_storage_t(rope_t&& r) noexcept {
        const std::span<view_t> in = r.links();
        assert(in.size() <= N);
        auto* v = new (buf_) value_t(static_cast<std::uint32_t>(in.size()), nullptr);
        view_t* out = v->slots();
        for (std::size_t i = 0; i < in.size(); ++i) new (out + i) view_t(std::move(in[i]));
        r = rope_t{};
    }

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
