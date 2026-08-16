/**
 * @file
 * @brief The OWNING rope-tier instantiation of the ONE templated terminus resolve walk (ADR-0053
 *        §7): the `view_node` reader over `wire::tlv_view_t` (the lazy rope-backed decode node,
 *        ADR-0053 §1).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Its own translation unit so a span-only
 * MCU target that never links the lazy tier never instantiates this walk
 * (ADR-0048 §1 / ADR-0047 templating rule) — exactly as `op_resolve.cpp`
 * instantiates the span-tier `arena_node` reader and NOTHING else.
 *
 * The reader adapts a forward-only `tlv_view_t` to the node-reader concept
 * (op_resolve_walk.hpp): header facts delegate to the view; the small contiguous
 * `wire`/`body` spans the walk reads for parsing are materialized once per node
 * into a refcounted segment (a single-link rope is adopted zero-copy, ADR-0053 §6;
 * a multi-link one pays one flatten). The OWNERSHIP path is scatter-gather (ADR-0053
 * ⑤): `own_wire` adopts a multi-link flatten instead of copying it twice, and
 * `pin_wire` stores an opted-in payload as a zero-copy subrope of the delivery
 * (ADR-0042 §3 on the rope tier). There is no `canonical_path()` any more: under
 * RFC-0018 a `PATH` body is packed records with one spelling per address, so
 * `path_lookup_key` reads `body()` on BOTH tiers — here that is the node's own
 * materialized span, which the rope tier had to build regardless.
 */

#include <array>
#include <optional>
#include <utility>

#include "libtracer/op_resolve.hpp"
#include "libtracer/tlv_view.hpp"
#include "op_resolve_walk.hpp"

namespace tr::graph {
namespace {

/**
 * @brief The per-resolve flatten seam (#766): the injected byte backend every rope-tier
 *        flatten of ONE resolve draws from, plus the sticky "a flatten was refused" flag
 *        that makes the refusal answerable by value.
 *
 * One instance lives on `op_resolver_t::resolve`'s stack and every `view_node` of that walk
 * points at it — nodes are copied by value (`parsed_fwd_t` holds them so), and each node
 * caches its own materialize, so the failure signal cannot live in a node: a refusal seen
 * while reading the `dst` PATH's third NAME must still be visible at the walk's decision
 * point. The flag is sticky by construction — nothing clears it inside a resolve.
 *
 * This stays rope-tier-local. The SPAN tier takes its backend as a `resolve_node` ARGUMENT
 * instead (#801): it has no flag to carry (a borrowed arena span cannot be shortened by a
 * refusal), and `arena_node` is copied by value on a hot walk, so it is kept two words wide.
 * A `view_node` needs the pointer regardless, because `ensure_cache` is reached from
 * `wire()`/`body()`, which take no arguments — and it is already a heavyweight node.
 */
struct flatten_seam_t {
    mem::mem_backend_t* backend = nullptr; /**< @brief Injected byte backend; null ⇒ heap. */
    bool refused = false;                  /**< @brief A flatten was refused during this walk. */
};

/**
 * @brief The owning rope-tier node-reader (ADR-0053 §7): one lazy `tlv_view_t` adapted to the node-
 *        reader concept the templated `resolve_node` walks.
 *
 * Default- and
 * copy-constructible (parsed_fwd_t holds nodes by value); a copy bumps segment
 * refcounts, never bytes. A copy shares the walk's @ref flatten_seam_t — the backend
 * injection and the refusal flag are per-RESOLVE, not per-node.
 */
class view_node {
   public:
    view_node() = default;
    explicit view_node(wire::tlv_view_t v, flatten_seam_t* seam = nullptr) noexcept
        : v_(std::move(v)), seam_(seam) {}

    [[nodiscard]] type_t type() const noexcept { return v_->type(); }
    [[nodiscard]] opt_t opt() const noexcept { return v_->opt(); }
    [[nodiscard]] bool structured() const noexcept { return v_->structured(); }

    /** @brief The decoded trailer timestamp, if `opt.TS` — the view's own bounded stitched
     *         read (#1109; the reply echo asks this of the request's root). */
    [[nodiscard]] std::optional<wire::timestamp_t> trailer_ts() const { return v_->timestamp(); }

    /**
     * @brief The trailer-excluded whole-TLV bytes: header + body, dropping any CRC/TS trailer
     *        exactly as the arena's `wire` span does (byte-identical to the span tier for the reply
     *        builder and the ownership copy).
     */
    [[nodiscard]] std::span<const std::byte> wire() const {
        if (!ensure_cache()) return {};
        return cache_.bytes().first(header_size() + v_->body_size());
    }
    /** @brief The body (payload / children) region. */
    [[nodiscard]] std::span<const std::byte> body() const {
        if (!ensure_cache()) return {};
        return cache_.bytes().subspan(header_size(), v_->body_size());
    }

    /**
     * @brief Has every contiguous span this walk produced been materialized successfully
     *        (#766)?
     *
     * `false` once ANY node of this resolve had its flatten refused by the injected backend:
     * that node's `wire()`/`body()` answered an EMPTY span rather than reading a short one,
     * so every value the walk derived from it downstream (the op discriminant, a lookup key,
     * a field name) is unsound. The walk checks this at its two decision points and answers
     * `BACKPRESSURE` — it never sends a reply built on a refused flatten. The span tier's
     * `arena_node` answers a constant `true` and the checks fold away: it draws from the
     * same seam for its ownership copy (#801), but its SPANS are borrowed from the frame and
     * a refusal cannot shorten one, so there is nothing here for it to condemn.
     */
    [[nodiscard]] bool spans_intact() const noexcept { return seam_ == nullptr || !seam_->refused; }

    /**
     * @brief The trailer-excluded whole TLV as a fresh OWNED segment (ADR-0053 ⑤): a multi-link
     *        value is flattened ONCE and adopted — not materialized into the node cache and then
     *        copied a second time by the shared `own_tlv`.
     *
     * A
     * single-link value aliases the frame, so it is copied once into an owned
     * segment (the required ADR-0041 §2 ownership copy). The shared `own_tlv`
     * clears the trailer bits on the owned opt byte; both branches yield an
     * exclusively-owned segment safe to patch.
     *
     * BOTH branches draw from the injected seam. Until #793 only the multi-link one did
     * (#766) and the single-link one copied through `view::over_bytes`'s global heap — so
     * one function allocated from two different allocators depending on how the PEER
     * happened to fragment the frame, and the fragmentation that took the *cheaper* branch
     * was the one that escaped the node's memory bound. A whole terminus WRITE whose payload
     * TLV lands inside one RX segment is exactly that case, and it is the common one.
     */
    [[nodiscard]] view_t own_wire(mem::mem_backend_t& flat) const {
        const rope_t sub = v_->wire().subrope(0, wire_size());  // trailer excluded
        if (sub.link_count() > 1) {                             // one flatten, adopt (no 2nd copy)
            // Through the injected seam (#766): the ADR-0053 ⑤ ownership flatten of a
            // fragmented WRITE payload is peer-provoked and must stay inside the node's
            // memory bound. An empty result is already the walk's BACKPRESSURE signal
            // (`own_tlv` → the empty-value guards in `resolve_node`), so the refusal needs
            // no second channel here — but it is recorded so a LATER span read on the same
            // walk cannot be believed either.
            view_t owned = sub.flatten(flat);
            if (owned.empty()) note_refusal();
            return owned;
        }
        // Single link: the ADR-0041 §2 ownership COPY, through the same seam (#793). A wire
        // TLV is never zero bytes, so `over_bytes` cannot answer its engaged-empty
        // "legitimately-empty input" here — `nullopt` is exactly a refusal, and it maps to
        // the empty view the walk's empty-value guards already read as BACKPRESSURE. It is
        // recorded for the same reason the flatten branch records its own: a LATER span read
        // on this walk must not be believed either.
        std::optional<view_t> owned = view::over_bytes(sub.only().bytes(), flat);
        if (!owned) {
            note_refusal();
            return view_t{};
        }
        return std::move(*owned);
    }

    /**
     * @brief The trailer-excluded whole-TLV length — from the header + body, NO materialize (the
     *        ADR-0042 §3 store-size test must not flatten just to measure).
     */
    [[nodiscard]] std::size_t wire_size() const noexcept { return header_size() + v_->body_size(); }

    /**
     * @brief Pin this TLV as a subrope of the delivery's own scatter-gather segments (ADR-0042 §3
     *        on the rope tier, ADR-0053 ⑤): the stored value refcounts the frame's links — a multi-
     *        link payload is stored with ZERO copy.
     *
     * `frame_view` is unused (the rope IS
     * the owning delivery here). Eligibility (opt-in / size / trailer-less) is the
     * caller's; this always CAN pin.
     */
    [[nodiscard]] std::optional<rope_t> pin_wire(const view_t*) const {
        return v_->wire().subrope(0, wire_size());
    }

    /**
     * @brief The bytes a pin would KEEP ALIVE (RFC-0022 §3.D's `segment_bytes`): the sum of the
     *        ALLOCATED sizes of the segments the pinned subrope's links belong to.
     *
     * The rope tier is the multi-link case, so the held quantity is a sum, not one segment: a
     * payload straddling three RX segments pins all three in full, and pricing only the first
     * would under-report the RAM the ratio exists to bound. Distinct owners are counted once —
     * two links into the same segment hold it once, and double-counting would make the
     * predicate decline a pin that costs nothing extra.
     */
    [[nodiscard]] std::size_t segment_bytes(const view_t*) const {
        const rope_t sub = v_->wire().subrope(0, wire_size());
        std::size_t total = 0;
        const std::span<const view_t> links = sub.links();
        for (std::size_t i = 0; i < links.size(); ++i) {
            if (!links[i].owner) continue;
            bool seen = false;
            for (std::size_t j = 0; j < i && !seen; ++j)
                seen = links[j].owner.get() == links[i].owner.get();
            if (!seen) total += links[i].owner->bytes.size();
        }
        return total;
    }

    /**
     * @brief Forward-only child cursor — the shared shape of `arena_node::children_cursor` and
     *        `tlv_view_t::children_t`.
     *
     * A grammar error in a child ends iteration (the
     * interim slice builds well-formed, trailer-less frames; 3c-iii wires the
     * per-TLV verify-at-access, ADR-0053 §4).
     */
    class children_cursor {
       public:
        explicit children_cursor(wire::tlv_view_t::children_t ch, flatten_seam_t* seam) noexcept
            : ch_(std::move(ch)), seam_(seam) {}
        [[nodiscard]] std::optional<view_node> next() {
            std::expected<std::optional<wire::tlv_view_t>, wire::err_t> n = ch_.next();
            if (!n || !n->has_value()) return std::nullopt;
            return view_node{std::move(**n), seam_};
        }

       private:
        wire::tlv_view_t::children_t ch_;
        flatten_seam_t* seam_;
    };
    [[nodiscard]] children_cursor children() const {
        return children_cursor{v_->children(), seam_};
    }

    /**
     * @brief The walk's injected byte backend, or the global heap when none was injected —
     *        public because `op_resolver_t::resolve` hands the SAME backend to `resolve_node`
     *        as its `flat` argument (#801), so the two tiers' walks take one shape.
     */
    [[nodiscard]] mem::mem_backend_t& backend() const noexcept {
        return (seam_ != nullptr && seam_->backend != nullptr) ? *seam_->backend
                                                               : mem::heap_backend();
    }

   private:
    /**
     * @brief A TLV header is type(1) + opt(1) + length(2, or 4 when opt.LL) — the width the
     *        trailer-excluded span is offset by.
     */
    [[nodiscard]] std::size_t header_size() const noexcept { return 2u + (v_->opt().ll ? 4u : 2u); }

    /**
     * @brief Materialize this TLV's wire rope into ONE contiguous refcounted segment the spans
     *        point into: zero-copy for a single-link rope (a refcount adopt), one flatten for a
     *        multi-link one (ADR-0053 §6).
     *
     * Cached so each node flattens at
     * most once; shared across copies via the segment refcount.
     */
    [[nodiscard]] bool ensure_cache() const {
        if (!cached_) {
            // Through the injected seam (#766): a multi-link materialize is the flatten a
            // FRAGMENTED terminus request provokes, so it is bounded by the same backend the
            // router's own four sites draw from. Single-link is a refcount adopt and never
            // reaches the backend at all.
            cache_ = v_->wire().materialize(backend());
            cached_ = true;
            // A wire TLV is never zero bytes, so an empty cache is exactly a refused
            // flatten. Record it: the spans this node would hand out are SHORT, and a short
            // span read as an op byte / a lookup key / a name silently changes the answer.
            if (cache_.empty()) note_refusal();
        }
        return cache_.length >= header_size() + v_->body_size();
    }

    /** @brief Mark this resolve's spans untrustworthy (sticky; see @ref spans_intact). */
    void note_refusal() const noexcept {
        if (seam_ != nullptr) seam_->refused = true;
    }

    std::optional<wire::tlv_view_t> v_{};
    flatten_seam_t* seam_ = nullptr;
    mutable view_t cache_{};
    mutable bool cached_ = false;
};

}  // namespace

result_t<rope_t> op_resolver_t::resolve(const wire::tlv_view_t& fwd, const inbound_ref_t& inbound,
                                        const view_t* frame_view,
                                        const wire::path_ref_element_t* dst_label_target) {
    // The terminus subject derivation, identical to the arena tier's — one helper, so the
    // two tiers cannot answer one logical request under two different principals.
    std::array<char, net::kPeerNameChars> subject_scratch{};
    const std::string_view subject = subject_for(inbound, subject_scratch);
    // The owning rope-tier instantiation: the lazy view root read through the
    // node-reader concept. Same walk as the arena tier — nothing here names a
    // decode representation (ADR-0053 §7).
    //
    // The seam is per-CALL and lives on this stack frame (#766): every node of the walk
    // points at it, so the injected backend reaches the flattens one call below the router
    // AND a refusal anywhere in the walk is visible at the walk's decision points. Its
    // lifetime covers the nodes', which never outlive `resolve_node`.
    flatten_seam_t seam{.backend = flat_, .refused = false};
    view_node root{fwd, &seam};
    // `egress` is the reply head + mint seam (#795, ADR-0074), separate from the flatten seam
    // the walk's nodes carry: it is passed straight to `resolve_node` because only the reply
    // builders draw from it, never a node's `wire()`/`body()`. Default heap when un-injected.
    return resolve_node(graph_, root, inbound.link, subject, frame_view, root.backend(),
                        egress_ != nullptr ? *egress_ : mem::heap_backend(), reverse_ref_fn_,
                        reverse_ref_ctx_, path_label_fn_, path_label_ctx_, dst_label_target);
}

}  // namespace tr::graph
