/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/route_handle.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::net {

using wire::opt_t;
using wire::type_t;

route_handle_t::link_tables_t& route_handle_t::tables(std::string_view link) {
    {
        const std::shared_lock lock(links_m_);
        if (const auto it = links_.find(link); it != links_.end()) return it->second;
    }
    const std::unique_lock lock(links_m_);
    // try_emplace: another thread may have created it between the two locks.
    return links_.try_emplace(std::pmr::string(link, mr_), mr_).first->second;
}

route_handle_t::link_tables_t* route_handle_t::find_tables(std::string_view link) const {
    const std::shared_lock lock(links_m_);
    const auto it = links_.find(link);
    return it == links_.end() ? nullptr
                              : const_cast<link_tables_t*>(&it->second);  // per-link mutex inside
}

void route_handle_t::bind_ingress(std::string_view in_link, std::uint16_t label,
                                  handle_binding_t binding) {
    link_tables_t& t = tables(in_link);
    const std::lock_guard lock(t.m);
    for (ingress_entry_t& e : t.ingress) {
        if (e.label == label) {
            e.binding = std::move(binding);
            return;
        }
    }
    t.ingress.push_back(ingress_entry_t{.label = label, .binding = std::move(binding)});
}

std::optional<handle_binding_t> route_handle_t::lookup_ingress(std::string_view in_link,
                                                               std::uint16_t label) const {
    link_tables_t* const t = find_tables(in_link);
    if (t == nullptr) return std::nullopt;
    const std::lock_guard lock(t->m);
    for (const ingress_entry_t& e : t->ingress)
        if (e.label == label) return e.binding;
    return std::nullopt;
}

void route_handle_t::record_egress(std::string_view out_link, std::uint16_t label,
                                   std::vector<std::byte> route) {
    link_tables_t& t = tables(out_link);
    const std::lock_guard lock(t.m);
    for (egress_entry_t& e : t.egress) {
        if (e.label == label) {
            e.route.assign(route.begin(), route.end());
            return;
        }
    }
    t.egress.push_back(egress_entry_t{
        .label = label, .route = std::pmr::vector<std::byte>(route.begin(), route.end(), mr_)});
}

std::optional<std::vector<std::byte>> route_handle_t::egress_route(std::string_view out_link,
                                                                   std::uint16_t label) const {
    link_tables_t* const t = find_tables(out_link);
    if (t == nullptr) return std::nullopt;
    const std::lock_guard lock(t->m);
    for (const egress_entry_t& e : t->egress)
        if (e.label == label) return std::vector<std::byte>(e.route.begin(), e.route.end());
    return std::nullopt;
}

std::pair<std::uint16_t, bool> route_handle_t::ensure_egress(std::string_view out_link,
                                                             std::span<const std::byte> route) {
    link_tables_t& t = tables(out_link);
    const std::lock_guard lock(t.m);
    // Reuse: the egress table doubles as the route -> label index (a link carries
    // few compact flows; a linear route compare beats a third keyed-by-bytes map).
    for (const egress_entry_t& e : t.egress) {
        if (e.route.size() == route.size() &&
            std::equal(e.route.begin(), e.route.end(), route.begin()))
            return {e.label, false};  // already advertised on this link - reuse the label
    }
    const std::uint16_t label = t.next_label++;
    t.egress.push_back(egress_entry_t{
        .label = label, .route = std::pmr::vector<std::byte>(route.begin(), route.end(), mr_)});
    return {label, true};
}

std::uint16_t route_handle_t::alloc_label(std::string_view link) {
    link_tables_t& t = tables(link);
    const std::lock_guard lock(t.m);
    return t.next_label++;
}

void route_handle_t::clear_link(std::string_view link) {
    // Self-heal: forget the link's ingress/egress bindings and restart its label
    // allocator, so a post-reconnect delivery re-advertises from a clean slate.
    //
    // The entry is EMPTIED in place, NOT erased. tables()/find_tables release
    // links_m_ before the caller locks the per-link mutex, so a reference they
    // handed out must stay valid while it is used; erasing the entry here (as an
    // earlier version did) could destroy a link_tables_t a concurrent
    // ensure_egress/bind_ingress was mid-write on — a use-after-free that orphaned
    // the egress buffer (a leak). Keeping `links_` insert-only makes every such
    // reference stable for this object's lifetime (std::map nodes never move), and
    // the per-link mutex serializes this clear against those writers. A cleared
    // link keeps only its small (empty) table shell, reused on the next advertise;
    // the route bytes each egress entry owned are freed with the entries.
    link_tables_t* const t = find_tables(link);
    if (t == nullptr) return;
    const std::lock_guard lock(t->m);
    t->ingress.clear();
    t->egress.clear();
    t->next_label = 1;  // 0 is reserved "none" — restart, matching a fresh table
}

std::size_t route_handle_t::ingress_count() const {
    const std::shared_lock lock(links_m_);
    std::size_t n = 0;
    for (const auto& [name, t] : links_) {
        const std::lock_guard tl(const_cast<link_tables_t&>(t).m);
        n += t.ingress.size();
    }
    return n;
}

std::size_t route_handle_t::egress_count() const {
    const std::shared_lock lock(links_m_);
    std::size_t n = 0;
    for (const auto& [name, t] : links_) {
        const std::lock_guard tl(const_cast<link_tables_t&>(t).m);
        n += t.egress.size();
    }
    return n;
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
    std::array<std::byte, 2> p{};
    detail::store_le<std::uint16_t>(p, label);
    wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
}

}  // namespace

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

}  // namespace tr::net
