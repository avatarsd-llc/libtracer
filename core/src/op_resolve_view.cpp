/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The OWNING rope-tier instantiation of the ONE templated terminus resolve walk
 * (ADR-0053 §7): the `view_node` reader over `wire::tlv_view_t` (the lazy
 * rope-backed decode node, ADR-0053 §1). Its own translation unit so a span-only
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
 * (ADR-0042 §3 on the rope tier). `canonical_path()` is `false`: the view re-emits
 * its PATH key via `emit_name`, producing the identical canonical lookup bytes
 * (ADR-0041 §3) without span-aliasing a borrowed frame.
 */

#include "libtracer/op_resolve.hpp"
#include "libtracer/tlv_view.hpp"
#include "op_resolve_walk.hpp"

namespace tr::graph {
namespace {

// The owning rope-tier node-reader (ADR-0053 §7): one lazy `tlv_view_t` adapted
// to the node-reader concept the templated `resolve_node` walks. Default- and
// copy-constructible (parsed_fwd_t holds nodes by value); a copy bumps segment
// refcounts, never bytes.
class view_node {
   public:
    view_node() = default;
    explicit view_node(wire::tlv_view_t v) noexcept : v_(std::move(v)) {}

    [[nodiscard]] type_t type() const noexcept { return v_->type(); }
    [[nodiscard]] opt_t opt() const noexcept { return v_->opt(); }
    [[nodiscard]] bool structured() const noexcept { return v_->structured(); }
    [[nodiscard]] bool canonical_path() const noexcept { return false; }

    // The trailer-excluded whole-TLV bytes: header + body, dropping any CRC/TS
    // trailer exactly as the arena's `wire` span does (byte-identical to the span
    // tier for the reply builder and the ownership copy).
    [[nodiscard]] std::span<const std::byte> wire() const {
        ensure_cache();
        return cache_.bytes().first(header_size() + v_->body_size());
    }
    // The body (payload / children) region.
    [[nodiscard]] std::span<const std::byte> body() const {
        ensure_cache();
        return cache_.bytes().subspan(header_size(), v_->body_size());
    }

    // The trailer-excluded whole TLV as a fresh OWNED segment (ADR-0053 ⑤): a
    // multi-link value is flattened ONCE and adopted — not materialized into the
    // node cache and then copied a second time by the shared `own_tlv`. A
    // single-link value aliases the frame, so it is copied once into an owned
    // segment (the required ADR-0041 §2 ownership copy). The shared `own_tlv`
    // clears the trailer bits on the owned opt byte; both branches yield an
    // exclusively-owned segment safe to patch.
    [[nodiscard]] view_t own_wire() const {
        const rope_t sub = v_->wire().subrope(0, wire_size());  // trailer excluded
        if (sub.link_count() > 1) return sub.flatten();         // one flatten, adopt (no 2nd copy)
        return view::over_bytes(sub.only().bytes()).value_or(view_t{});
    }

    // The trailer-excluded whole-TLV length — from the header + body, NO materialize
    // (the ADR-0042 §3 store-size test must not flatten just to measure).
    [[nodiscard]] std::size_t wire_size() const noexcept { return header_size() + v_->body_size(); }

    // Pin this TLV as a subrope of the delivery's own scatter-gather segments (ADR-0042
    // §3 on the rope tier, ADR-0053 ⑤): the stored value refcounts the frame's links —
    // a multi-link payload is stored with ZERO copy. `frame_view` is unused (the rope IS
    // the owning delivery here). Eligibility (opt-in / size / trailer-less) is the
    // caller's; this always CAN pin.
    [[nodiscard]] std::optional<rope_t> pin_wire(const view_t*) const {
        return v_->wire().subrope(0, wire_size());
    }

    // Forward-only child cursor — the shared shape of `arena_node::children_cursor`
    // and `tlv_view_t::children_t`. A grammar error in a child ends iteration (the
    // interim slice builds well-formed, trailer-less frames; 3c-iii wires the
    // per-TLV verify-at-access, ADR-0053 §4).
    class children_cursor {
       public:
        explicit children_cursor(wire::tlv_view_t::children_t ch) noexcept : ch_(std::move(ch)) {}
        [[nodiscard]] std::optional<view_node> next() {
            std::expected<std::optional<wire::tlv_view_t>, wire::err_t> n = ch_.next();
            if (!n || !n->has_value()) return std::nullopt;
            return view_node{std::move(**n)};
        }

       private:
        wire::tlv_view_t::children_t ch_;
    };
    [[nodiscard]] children_cursor children() const { return children_cursor{v_->children()}; }

   private:
    // A TLV header is type(1) + opt(1) + length(2, or 4 when opt.LL) — the width
    // the trailer-excluded span is offset by.
    [[nodiscard]] std::size_t header_size() const noexcept { return 2u + (v_->opt().ll ? 4u : 2u); }

    // Materialize this TLV's wire rope into ONE contiguous refcounted segment the
    // spans point into: zero-copy for a single-link rope (a refcount adopt), one
    // flatten for a multi-link one (ADR-0053 §6). Cached so each node flattens at
    // most once; shared across copies via the segment refcount.
    void ensure_cache() const {
        if (cached_) return;
        cache_ = v_->wire().materialize();
        cached_ = true;
    }

    std::optional<wire::tlv_view_t> v_{};
    mutable view_t cache_{};
    mutable bool cached_ = false;
};

}  // namespace

result_t<rope_t> op_resolver_t::resolve(const wire::tlv_view_t& fwd, std::string_view inbound_link,
                                        const view_t* frame_view) {
    // The owning rope-tier instantiation: the lazy view root read through the
    // node-reader concept. Same walk as the arena tier — nothing here names a
    // decode representation (ADR-0053 §7).
    return resolve_node(graph_, view_node{fwd}, inbound_link, frame_view);
}

}  // namespace tr::graph
