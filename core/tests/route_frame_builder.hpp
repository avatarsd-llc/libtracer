/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#pragma once

/**
 * @file
 * @brief HOST TEST SUPPORT: the owning mount-run encoder the library no longer ships (#1779).
 *
 * `encode_mount_tlv` returned an owning `std::vector` built on the throwing global heap. No
 * production path calls it — a child's mount run is its registry slot's own text — so under
 * ADR-0083 (one allocation seam, no owning std type across the core API) it left the library.
 * Tests and host benches still want a mount run as a VALUE, so it lives on here, unchanged in
 * name and bytes. Nothing under core/src or core/include includes this header. The label-plane
 * frame builders that lived here went with the label plane (#1951).
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/packed_path.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::net {

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
