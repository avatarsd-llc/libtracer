/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/router.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <string_view>

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::net {

using wire::decode;
using wire::encode;
using wire::error_t;
using wire::opt_t;
using wire::tlv_t;
using wire::type_t;
namespace {

std::uint8_t u8(std::byte b) { return std::to_integer<std::uint8_t>(b); }

// One parsed TLV header within a buffer.
struct Head {
    std::uint8_t type;
    std::size_t payload_off;
    std::size_t payload_len;
    std::size_t total;  // header + payload
};

std::optional<Head> read_head(std::span<const std::byte> buf, std::size_t at) {
    if (at + 4 > buf.size()) return std::nullopt;
    const std::uint8_t type = u8(buf[at]);
    const bool ll = (u8(buf[at + 1]) & 0x08) != 0;
    const std::size_t hdr = ll ? 6 : 4;
    if (at + hdr > buf.size()) return std::nullopt;
    std::size_t len = u8(buf[at + 2]) | (static_cast<std::size_t>(u8(buf[at + 3])) << 8);
    if (ll) {
        len |= static_cast<std::size_t>(u8(buf[at + 4])) << 16;
        len |= static_cast<std::size_t>(u8(buf[at + 5])) << 24;
    }
    if (at + hdr + len > buf.size()) return std::nullopt;
    return Head{type, at + hdr, len, hdr + len};
}

}  // namespace

std::vector<std::byte> router_wrap(std::span<const std::byte> data, const router_meta_t& meta) {
    std::vector<std::byte> body;

    detail::emit_name(body, "origin_peer_id");
    detail::emit_tlv(body, type_t::VALUE, opt_t{}, meta.origin);

    detail::emit_name(body, "origin_timestamp");
    std::array<std::byte, 8> ts{};
    detail::store_le(ts, meta.ts);
    detail::emit_tlv(body, type_t::TIME, opt_t{}, ts);

    detail::emit_name(body, "hop_count");
    const std::array<std::byte, 1> hop{static_cast<std::byte>(meta.hop)};
    detail::emit_tlv(body, type_t::VALUE, opt_t{}, hop);

    detail::emit_name(body, "data");
    body.insert(body.end(), data.begin(), data.end());  // wrapped data TLV, verbatim, last

    std::vector<std::byte> out;
    detail::emit_tlv(out, type_t::ROUTER, opt_t{.pl = true}, body);
    return out;
}

std::expected<unwrapped_t, error_t> router_unwrap(std::span<const std::byte> frame) {
    const auto router = read_head(frame, 0);
    if (!router || router->type != static_cast<std::uint8_t>(type_t::ROUTER) ||
        router->total != frame.size()) {
        return std::unexpected(error_t::FRAME_INVALID);
    }

    unwrapped_t out;
    const std::size_t end = router->payload_off + router->payload_len;
    std::size_t cur = router->payload_off;

    while (cur < end) {
        const auto tag = read_head(frame, cur);
        if (!tag || tag->type != static_cast<std::uint8_t>(type_t::NAME))
            return std::unexpected(error_t::FRAME_INVALID);
        const std::string_view name(reinterpret_cast<const char*>(frame.data() + tag->payload_off),
                                    tag->payload_len);
        cur += tag->total;

        if (name == "data") {  // the rest of the ROUTER payload is the wrapped TLV
            if (cur >= end) return std::unexpected(error_t::FRAME_INVALID);
            out.data = frame.subspan(cur, end - cur);
            return out;
        }

        const auto val = read_head(frame, cur);
        if (!val) return std::unexpected(error_t::FRAME_INVALID);
        const auto payload = frame.subspan(val->payload_off, val->payload_len);
        if (name == "origin_peer_id") {
            if (payload.size() != out.meta.origin.size())
                return std::unexpected(error_t::FRAME_INVALID);
            std::memcpy(out.meta.origin.data(), payload.data(), payload.size());
        } else if (name == "origin_timestamp") {
            out.meta.ts = detail::load_le(payload);
        } else if (name == "hop_count") {
            out.meta.hop = payload.empty() ? 0 : u8(payload[0]);
        }
        // Unknown metadata names are ignored (forward-compatible).
        cur += val->total;
    }

    return std::unexpected(error_t::FRAME_INVALID);  // no "data" child
}

}  // namespace tr::net
