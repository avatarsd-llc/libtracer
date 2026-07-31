/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/route_handle.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::net {

using wire::opt_t;
using wire::type_t;

std::shared_ptr<route_handle_t::link_tables_t> route_handle_t::tables(std::string_view link) {
    {
        const std::shared_lock lock(links_m_);
        if (const auto it = links_.find(link); it != links_.end()) return it->second;
    }
    const std::unique_lock lock(links_m_);
    // Re-check: another thread may have created it between the two locks.
    if (const auto it = links_.find(link); it != links_.end()) return it->second;
    // The tables node is INDEPENDENTLY owned (allocate_shared draws it from mr_), not
    // stored inline in the map node: the returned shared_ptr copy pins it so clear_link
    // can erase the registry entry while a concurrent writer still holds this table (#488).
    auto sp = std::allocate_shared<link_tables_t>(
        std::pmr::polymorphic_allocator<link_tables_t>(mr_), mr_);
    links_.emplace(std::pmr::string(link, mr_), sp);
    return sp;
}

std::shared_ptr<route_handle_t::link_tables_t> route_handle_t::find_tables(
    std::string_view link) const {
    const std::shared_lock lock(links_m_);
    const auto it = links_.find(link);
    return it == links_.end() ? nullptr : it->second;  // a pinning copy; per-link mutex inside
}

bool route_handle_t::bind_ingress(std::string_view in_link, std::uint16_t label,
                                  handle_binding_t binding) {
    const std::shared_ptr<link_tables_t> t = tables(in_link);
    const std::lock_guard lock(t->m);
    for (ingress_entry_t& e : t->ingress) {
        if (e.label == label) {
            e.binding = std::move(binding);
            return true;  // rebind in place — adds no entry, so the bound cannot refuse it
        }
    }
    // REFUSE rather than evict (#603). Evict-oldest is right for `can_reassembly_t`, whose
    // groups are short-lived so oldest ~ stalest; a label binding is the opposite -- it
    // exists precisely because a flow is long-running, so evicting the oldest preferentially
    // kills the longest-lived stream and makes it re-advertise, forever. True LRU would need
    // a write on `resolved()`, which is the per-delivery hot path, to solve a flow-setup
    // problem. Refusing keeps every established flow untouched and degrades only NEW ones,
    // down the path a full label space already takes.
    if (max_bindings_ != 0 && t->ingress.size() >= max_bindings_) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    t->ingress.push_back(ingress_entry_t{.label = label, .binding = std::move(binding)});
    return true;
}

std::optional<handle_binding_t> route_handle_t::lookup_ingress(std::string_view in_link,
                                                               std::uint16_t label) const {
    const std::shared_ptr<link_tables_t> t = find_tables(in_link);
    if (!t) return std::nullopt;
    const std::lock_guard lock(t->m);
    for (const ingress_entry_t& e : t->ingress)
        if (e.label == label) return e.binding;
    return std::nullopt;
}

resolved_binding_t route_handle_t::resolved(std::string_view in_link, std::uint16_t label) const {
    resolved_binding_t out;
    const std::shared_ptr<link_tables_t> t = find_tables(in_link);
    if (!t) return out;
    const std::lock_guard lock(t->m);
    for (const ingress_entry_t& e : t->ingress) {
        if (e.label != label) continue;
        out.found = true;
        out.terminus = e.binding.terminus;
        out.out_label = e.binding.out_label;
        out.warm = e.binding.warm;
        out.down_slot = e.binding.down_slot;
        out.target = e.binding.target;
        out.target_gen = e.binding.target_gen;
        return out;  // ~24 trivially copyable bytes — no string, no vector, no allocation
    }
    return out;
}

void route_handle_t::cache_resolution(std::string_view in_link, std::uint16_t label,
                                      const resolved_binding_t& r) {
    const std::shared_ptr<link_tables_t> t = find_tables(in_link);
    if (!t) return;
    const std::lock_guard lock(t->m);
    for (ingress_entry_t& e : t->ingress) {
        if (e.label != label) continue;
        e.binding.warm = true;
        e.binding.down_slot = r.down_slot;
        e.binding.target = r.target;
        e.binding.target_gen = r.target_gen;
        return;
    }
    // The binding went away between resolve and cache — nothing to record; the next frame
    // simply resolves again. Never an error: this is best-effort memoization, not state.
}

bool route_handle_t::record_egress(std::string_view out_link, std::uint16_t label,
                                   std::vector<std::byte> route) {
    const std::shared_ptr<link_tables_t> t = tables(out_link);
    const std::lock_guard lock(t->m);
    for (egress_entry_t& e : t->egress) {
        if (e.label == label) {
            e.route.assign(route.begin(), route.end());
            return true;  // re-record in place — adds no entry
        }
    }
    if (max_bindings_ != 0 && t->egress.size() >= max_bindings_) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    t->egress.push_back(egress_entry_t{
        .label = label, .route = std::pmr::vector<std::byte>(route.begin(), route.end(), mr_)});
    return true;
}

std::optional<std::vector<std::byte>> route_handle_t::egress_route(std::string_view out_link,
                                                                   std::uint16_t label) const {
    const std::shared_ptr<link_tables_t> t = find_tables(out_link);
    if (!t) return std::nullopt;
    const std::lock_guard lock(t->m);
    for (const egress_entry_t& e : t->egress)
        if (e.label == label) return std::vector<std::byte>(e.route.begin(), e.route.end());
    return std::nullopt;
}

std::pair<std::uint16_t, bool> route_handle_t::ensure_egress(std::string_view out_link,
                                                             std::span<const std::byte> route) {
    const std::shared_ptr<link_tables_t> t = tables(out_link);
    const std::lock_guard lock(t->m);
    // Reuse: the egress table doubles as the route -> label index (a link carries
    // few compact flows; a linear route compare beats a third keyed-by-bytes map).
    for (const egress_entry_t& e : t->egress) {
        if (e.route.size() == route.size() &&
            std::equal(e.route.begin(), e.route.end(), route.begin()))
            return {e.label, false};  // already advertised on this link - reuse the label
    }
    // Saturate, never wrap (#603): a wrapped `next_label` handed out the reserved 0 and
    // then re-issued 1, 2, ... while those labels still aliased LIVE routes — a delivery
    // on the reused label resolves the wrong route, which is a misroute, not a drop.
    // Exhaustion returns `{0, false}`, records nothing, and leaves the caller to send the
    // full-route FWD (which carries its own route and needs no label).
    if (t->next_label == 0) return {0, false};
    // Table full (#603): refuse, same answer and same caller degrade as an exhausted label
    // space. Checked AFTER the reuse scan above, so an established flow is never refused —
    // only a new one, and only into the full-route form that always works.
    if (max_bindings_ != 0 && t->egress.size() >= max_bindings_) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return {0, false};
    }
    const std::uint16_t label = t->next_label++;
    t->egress.push_back(egress_entry_t{
        .label = label, .route = std::pmr::vector<std::byte>(route.begin(), route.end(), mr_)});
    return {label, true};
}

std::uint16_t route_handle_t::alloc_label(std::string_view link) {
    const std::shared_ptr<link_tables_t> t = tables(link);
    const std::lock_guard lock(t->m);
    // Saturating, monotonic, never 0 (see ensure_egress): 1..65535 then permanently
    // exhausted. Sticky by construction -- handing out 65535 leaves `next_label` at 0, and
    // this returns before incrementing, so the state cannot walk back into live labels.
    if (t->next_label == 0) return 0;
    return t->next_label++;
}

void route_handle_t::clear_link(std::string_view link) {
    // Self-heal: forget the link's ingress/egress bindings and its label allocator so a
    // post-reconnect delivery re-advertises from a clean slate.
    //
    // The registry entry is ERASED, not emptied-in-place. The tables node is owned by a
    // shared_ptr (#488): tables()/find_tables hand out a PINNING copy, so erasing the
    // entry here cannot free a link_tables_t a concurrent ensure_egress/bind_ingress is
    // mid-write on — the node is destroyed only when the last outstanding reference drops,
    // and its route bytes are freed with it. A writer that raced the erase keeps writing to
    // a now-detached table; those edits are correctly discarded, since the next tables()
    // mints a fresh entry — exactly the clean slate the self-heal wants. Erasing bounds
    // `links_` to LIVE link names instead of retaining an empty shell per departed name.
    //
    // We touch only the registry (map) structure under links_m_, never the link_tables_t
    // object itself, so this never contends with a writer holding the per-link mutex.
    const std::unique_lock lock(links_m_);
    if (const auto it = links_.find(link); it != links_.end()) links_.erase(it);
}

std::size_t route_handle_t::ingress_count() const {
    const std::shared_lock lock(links_m_);
    std::size_t n = 0;
    for (const auto& [name, t] : links_) {
        const std::lock_guard tl(t->m);
        n += t->ingress.size();
    }
    return n;
}

std::size_t route_handle_t::egress_count() const {
    const std::shared_lock lock(links_m_);
    std::size_t n = 0;
    for (const auto& [name, t] : links_) {
        const std::lock_guard tl(t->m);
        n += t->egress.size();
    }
    return n;
}

std::size_t route_handle_t::link_count() const {
    const std::shared_lock lock(links_m_);
    return links_.size();
}

// --- transport-plane frame codec ---------------------------------------------

namespace {

/**
 * @brief A 2-byte little-endian VALUE TLV carrying a u16 label (the FIRST child of every route-
 *        handle frame).
 *
 * Opaque (opt.PL=0), 2-byte length: 6 bytes on the wire.
 */
void emit_label(std::vector<std::byte>& out, std::uint16_t label) {
    const std::array<std::byte, 6> tlv = label_tlv(label);
    out.insert(out.end(), tlv.begin(), tlv.end());
}

}  // namespace

std::array<std::byte, 6> label_tlv(std::uint16_t label) noexcept {
    // The one spelling of the label child's bytes: a 2-byte opaque VALUE. Written field by
    // field rather than as a literal so the length and the payload keep the wire's
    // little-endian order by construction. `emit_label` (and therefore every throwing and
    // nothrow encoder) goes through here, so a gathered frame head and a built frame cannot
    // disagree; `compact_cache_test` pins these bytes against `wire::emit_tlv` independently,
    // since a shared locus alone would let a wrong layout pass a self-comparison.
    std::array<std::byte, 6> out{};
    out[0] = static_cast<std::byte>(std::to_underlying(type_t::VALUE));
    out[1] = static_cast<std::byte>(opt_t{}.encode());
    detail::store_le<std::uint16_t>(std::span<std::byte>(out).subspan(2, 2), 2);
    detail::store_le<std::uint16_t>(std::span<std::byte>(out).subspan(4, 2), label);
    return out;
}

std::vector<std::byte> encode_advertise(std::uint16_t label,
                                        std::span<const std::byte> route_path) {
    std::vector<std::byte> body;
    emit_label(body, label);
    body.insert(body.end(), route_path.begin(), route_path.end());
    std::vector<std::byte> out;
    wire::emit_tlv(out, type_t::ADVERTISE, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> encode_compact(std::uint16_t label, std::span<const std::byte> payload) {
    std::vector<std::byte> body;
    emit_label(body, label);
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::byte> out;
    wire::emit_tlv(out, type_t::COMPACT, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> encode_handle_nack(std::uint16_t label) {
    std::vector<std::byte> body;
    emit_label(body, label);
    std::vector<std::byte> out;
    wire::emit_tlv(out, type_t::HANDLE_NACK, opt_t{.pl = true}, body);
    return out;
}

namespace {

/**
 * @brief The shared NOTHROW `<frame>{ VALUE label(u16), tail }` builder behind
 *        @ref try_encode_advertise / @ref try_encode_compact (#477): reserve body and
 *        frame exactly (≤ 6-byte headers, even LL-widened), then the emits below cannot
 *        reallocate — so nothing here can throw. One non-template locus (the footprint-
 *        sentinel discipline).
 * @retval false A buffer could not be reserved — @p out is left empty.
 */
[[nodiscard]] bool try_encode_labeled(std::vector<std::byte>& out, type_t frame,
                                      std::uint16_t label,
                                      std::span<const std::byte> tail) noexcept {
    out.clear();
    constexpr std::size_t kLabelTlv = 6;  // 4-byte VALUE header + u16 label
    std::vector<std::byte> body;
    if (!detail::try_reserve(body, kLabelTlv + tail.size())) return false;
    emit_label(body, label);
    body.insert(body.end(), tail.begin(), tail.end());  // within capacity
    if (!detail::try_reserve(out, 6 + body.size())) return false;
    wire::emit_tlv(out, frame, opt_t{.pl = true}, body);  // within capacity
    return true;
}

}  // namespace

bool try_encode_advertise(std::vector<std::byte>& out, std::uint16_t label,
                          std::span<const std::byte> route_path) noexcept {
    return try_encode_labeled(out, type_t::ADVERTISE, label, route_path);
}

bool try_encode_compact(std::vector<std::byte>& out, std::uint16_t label,
                        std::span<const std::byte> payload) noexcept {
    return try_encode_labeled(out, type_t::COMPACT, label, payload);
}

}  // namespace tr::net
