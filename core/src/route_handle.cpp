/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/route_handle.hpp"

#include <array>
#include <iterator>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::net {

using wire::opt_t;
using wire::type_t;

void route_handle_t::bind_ingress(std::string_view in_link, std::uint16_t label,
                                  handle_binding_t binding) {
    const std::lock_guard lock(m_);
    ingress_[key_t{std::string(in_link), label}] = std::move(binding);
}

std::optional<handle_binding_t> route_handle_t::lookup_ingress(std::string_view in_link,
                                                               std::uint16_t label) const {
    const std::lock_guard lock(m_);
    const auto it = ingress_.find(key_t{std::string(in_link), label});
    if (it == ingress_.end()) return std::nullopt;
    return it->second;
}

void route_handle_t::record_egress(std::string_view out_link, std::uint16_t label,
                                   std::vector<std::byte> route) {
    const std::lock_guard lock(m_);
    egress_[key_t{std::string(out_link), label}] = std::move(route);
}

std::optional<std::vector<std::byte>> route_handle_t::egress_route(std::string_view out_link,
                                                                   std::uint16_t label) const {
    const std::lock_guard lock(m_);
    const auto it = egress_.find(key_t{std::string(out_link), label});
    if (it == egress_.end()) return std::nullopt;
    return it->second;
}

std::uint16_t route_handle_t::alloc_label(std::string_view link) {
    const std::lock_guard lock(m_);
    // Monotonic per link; 0 is reserved as "none" so a fresh link starts at 1.
    std::uint16_t& next = next_label_[std::string(link)];
    if (next == 0) next = 1;
    return next++;
}

void route_handle_t::clear_link(std::string_view link) {
    const std::lock_guard lock(m_);
    const std::string l(link);
    for (auto it = ingress_.begin(); it != ingress_.end();)
        it = (it->first.first == l) ? ingress_.erase(it) : std::next(it);
    for (auto it = egress_.begin(); it != egress_.end();)
        it = (it->first.first == l) ? egress_.erase(it) : std::next(it);
    next_label_.erase(l);
}

std::size_t route_handle_t::ingress_count() const {
    const std::lock_guard lock(m_);
    return ingress_.size();
}

std::size_t route_handle_t::egress_count() const {
    const std::lock_guard lock(m_);
    return egress_.size();
}

// --- transport-plane frame codec ---------------------------------------------

namespace {

// A 2-byte little-endian VALUE TLV carrying a u16 label (the FIRST child of every
// route-handle frame). Opaque (opt.PL=0), 2-byte length: 6 bytes on the wire.
void emit_label(std::vector<std::byte>& out, std::uint16_t label) {
    std::array<std::byte, 2> p{};
    detail::store_le<std::uint16_t>(p, label);
    detail::emit_tlv(out, type_t::VALUE, opt_t{}, p);
}

}  // namespace

std::vector<std::byte> encode_advertise(std::uint16_t label,
                                        std::span<const std::byte> route_path) {
    std::vector<std::byte> body;
    emit_label(body, label);
    body.insert(body.end(), route_path.begin(), route_path.end());
    std::vector<std::byte> out;
    detail::emit_tlv(out, type_t::ADVERTISE, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> encode_compact(std::uint16_t label, std::span<const std::byte> payload) {
    std::vector<std::byte> body;
    emit_label(body, label);
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::byte> out;
    detail::emit_tlv(out, type_t::COMPACT, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> encode_handle_nack(std::uint16_t label) {
    std::vector<std::byte> body;
    emit_label(body, label);
    std::vector<std::byte> out;
    detail::emit_tlv(out, type_t::HANDLE_NACK, opt_t{.pl = true}, body);
    return out;
}

}  // namespace tr::net
