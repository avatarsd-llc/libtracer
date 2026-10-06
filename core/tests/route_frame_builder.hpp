/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#pragma once

/**
 * @file
 * @brief HOST TEST SUPPORT: the owning label-plane frame builders and the mount-run encoder
 *        the library no longer ships (#1779).
 *
 * `encode_advertise`, `encode_compact`, `encode_handle_nack` and `encode_mount_tlv` returned
 * owning `std::vector`s built on the throwing global heap. Since #885 no production path
 * called them — the router scatter-gathers every ADVERTISE, COMPACT and HANDLE_NACK off a
 * stack head, and a child's mount run is its registry slot's own text — so under ADR-0083
 * (one allocation seam, no owning std type across the core API) they left the library. Tests
 * and host benches still want a frame as a VALUE, so they live on here, unchanged in name and
 * bytes. Nothing under core/src or core/include includes this header.
 *
 * The bytes are pinned against the router's gathered frames by `compact_cache_test`, which is
 * what makes a builder that is not the library's own still a faithful oracle.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/packed_path.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::net {

/** @brief `ROUTE_HANDLE_FRAME{ VALUE label(u16), <body> }` as one owned frame. */
[[nodiscard]] inline std::vector<std::byte> encode_label_frame(wire::type_t type,
                                                               std::uint16_t label,
                                                               std::span<const std::byte> tail) {
    const std::array<std::byte, 6> head = label_tlv(label);
    std::vector<std::byte> body(head.begin(), head.end());
    body.insert(body.end(), tail.begin(), tail.end());
    std::vector<std::byte> out;
    wire::emit_tlv(out, type, wire::opt_t{.pl = true}, body);
    return out;
}

/** @brief `ADVERTISE{ VALUE label(u16), PATH route }` (@p route_path is a whole PATH TLV). */
[[nodiscard]] inline std::vector<std::byte> encode_advertise(
    std::uint16_t label, std::span<const std::byte> route_path) {
    return encode_label_frame(wire::type_t::ADVERTISE, label, route_path);
}

/** @brief `COMPACT{ VALUE label(u16), <payload TLV> }`. */
[[nodiscard]] inline std::vector<std::byte> encode_compact(std::uint16_t label,
                                                           std::span<const std::byte> payload) {
    return encode_label_frame(wire::type_t::COMPACT, label, payload);
}

/** @brief `HANDLE_NACK{ VALUE label(u16) }` — the stale-label signal. */
[[nodiscard]] inline std::vector<std::byte> encode_handle_nack(std::uint16_t label) {
    return encode_label_frame(wire::type_t::HANDLE_NACK, label, {});
}

/**
 * @brief @p segs as a run of packed PATH segment records — a mount run, as
 *        `child_registry_t::encode_mount_name` spells one.
 * @return nullopt if a segment is empty (it would spell the §5.4 escape) or exceeds the
 *         record's `u8` length field.
 */
[[nodiscard]] inline std::optional<std::vector<std::byte>> encode_mount_tlv(
    std::span<const std::string_view> segs) {
    std::vector<std::byte> out;
    for (const std::string_view s : segs) {
        if (!wire::emit_path_segment(out, s)) return std::nullopt;
    }
    return out;
}

}  // namespace tr::net
