// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// The ROUTER envelope (docs/reference/05 §0x0D ROUTER). A bridge wraps a data TLV
// with routing metadata on egress and sheds it on ingress. A ROUTER is a
// structured TLV whose NAME-tagged children carry origin_peer_id / origin_timestamp
// / hop_count, followed by `NAME "data"` and the wrapped data TLV as the LAST
// child. `(origin_peer_id, origin_timestamp)` is the cycle-dedup key; hop_count is
// the termination guarantee
// ([ADR-0014](../../docs/adr/0014-router-cycle-termination-hop-count.md)).
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "libtracer/frame.hpp"      // Error
#include "libtracer/transport.hpp"  // PeerId

namespace tr {

struct RouterMeta {
    PeerId origin{};       // origin_peer_id (16 bytes)
    std::uint64_t ts = 0;  // origin_timestamp (u64 ns) — dedup key together with origin
    std::uint8_t hop = 0;  // hop_count
    constexpr bool operator==(const RouterMeta&) const noexcept = default;
};

struct Unwrapped {
    RouterMeta meta;
    std::span<const std::byte> data;  // the wrapped data TLV (borrows the input frame)
};

// Build a ROUTER frame wrapping `data` (a complete data TLV) with `meta`.
[[nodiscard]] std::vector<std::byte> router_wrap(std::span<const std::byte> data,
                                                 const RouterMeta& meta);

// Parse a ROUTER frame → metadata + a span over the wrapped data TLV (borrowing
// `frame`). FrameInvalid if it is not a well-formed ROUTER envelope.
[[nodiscard]] std::expected<Unwrapped, Error> router_unwrap(std::span<const std::byte> frame);

}  // namespace tr
