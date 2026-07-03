/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_can (#55) — the PURE, host-testable CAN framing layer: the
 * structured 29-bit extended-CAN-ID codec and the in-band `advertise` frame
 * codec. No SocketCAN, no kernel `vcan`, no real socket — this header knows
 * nothing about `socket(PF_CAN…)`; the `transport_can : transport_t` SocketCAN
 * binding is a deferred increment (see docs/reference/14-can-transport.md).
 *
 * Header-elided framing (ADR-0022): the CAN ID *is* the path, so the TLV header
 * never hits the constrained bus. A dynamic identity↔path map held inside the
 * transport (ADR-0030) self-establishes via the `advertise` frames defined here;
 * the lean, id-matched data frames that follow carry only payload bytes. The map
 * machinery is decentralized and self-healing — no gateway/orchestrator role
 * (docs/reference/13-network-formation.md §self-healing).
 *
 * Mirrors the style of ws.hpp (the transport_ws protocol layer): header-only,
 * inline, pure functions with explicit byte vectors.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

/**
 * @file
 * @brief CAN transport-plane (`tr::net::can`) ID codec + `advertise` frame codec.
 */

namespace tr::net::can {

/**
 * @brief Width, in bits, of the protocol-version prefix in the 29-bit CAN ID.
 *
 * The most-significant field. It *is* discovery-layer versioning on CAN — a
 * distinct ID prefix per protocol version (CONTEXT.md *Discovery-layer
 * versioning*), used instead of a per-frame version field. Because it is the
 * top field, distinct protocol versions occupy disjoint arbitration bands.
 */
inline constexpr unsigned kVersionBits = 4;

/** @brief Width, in bits, of the node sub-field (the originating node's id). */
inline constexpr unsigned kNodeBits = 13;

/** @brief Width, in bits, of the endpoint sub-field (the per-node path slot). */
inline constexpr unsigned kEndpointBits = 12;

/** @brief Total width of a CAN *extended* identifier (CAN 2.0B), in bits. */
inline constexpr unsigned kIdBits = 29;

static_assert(kVersionBits + kNodeBits + kEndpointBits == kIdBits,
              "the version|node|endpoint split must exactly fill the 29-bit extended ID");

/** @brief Largest legal protocol-version prefix (`2^kVersionBits - 1`). */
inline constexpr std::uint32_t kVersionMax = (1u << kVersionBits) - 1u;
/** @brief Largest legal node id (`2^kNodeBits - 1`). */
inline constexpr std::uint32_t kNodeMax = (1u << kNodeBits) - 1u;
/** @brief Largest legal endpoint slot (`2^kEndpointBits - 1`). */
inline constexpr std::uint32_t kEndpointMax = (1u << kEndpointBits) - 1u;
/** @brief Largest legal 29-bit extended CAN ID value (`2^29 - 1`). */
inline constexpr std::uint32_t kIdMax = (1u << kIdBits) - 1u;

/**
 * @brief The three structured fields of a header-elided CAN identifier.
 *
 * Wire layout, most-significant first: `[version:4 | node:13 | endpoint:12]`
 * (ADR-0030). Encoded into the 29-bit extended CAN ID by @ref encode_can_id.
 *
 * @note **Lower numeric ID = higher bus arbitration priority** (CAN dominant-bit
 *       arbitration). Because @ref node is more significant than @ref endpoint,
 *       a lower node id wins the bus over a higher one; assigning the
 *       version/node/endpoint values therefore *also* assigns real-time
 *       priority — a CAN-specific knob the identity↔path map exposes.
 */
struct can_id_fields_t {
    std::uint8_t version = 0;   /**< @brief Protocol-version prefix (`0..kVersionMax`). */
    std::uint16_t node = 0;     /**< @brief Originating node id (`0..kNodeMax`). */
    std::uint16_t endpoint = 0; /**< @brief Per-node endpoint slot (`0..kEndpointMax`). */

    /** @brief Field-wise equality (value type). */
    [[nodiscard]] bool operator==(const can_id_fields_t&) const = default;
};

/**
 * @brief Pack structured fields into a 29-bit extended CAN ID.
 *
 * Each field is masked to its width, so an over-range input cannot corrupt a
 * neighbouring field; @ref decode_can_id is the exact inverse for any in-range
 * value.
 *
 * @param f The version/node/endpoint fields to pack.
 * @return The 29-bit identifier (`0..kIdMax`).
 */
[[nodiscard]] constexpr std::uint32_t encode_can_id(const can_id_fields_t& f) noexcept {
    const std::uint32_t v = static_cast<std::uint32_t>(f.version) & kVersionMax;
    const std::uint32_t n = static_cast<std::uint32_t>(f.node) & kNodeMax;
    const std::uint32_t e = static_cast<std::uint32_t>(f.endpoint) & kEndpointMax;
    return (v << (kNodeBits + kEndpointBits)) | (n << kEndpointBits) | e;
}

/**
 * @brief Unpack a 29-bit extended CAN ID into its structured fields.
 *
 * @param id A candidate identifier.
 * @return The fields, or `std::nullopt` if @p id does not fit in 29 bits
 *         (a value an extended CAN frame could never carry).
 */
[[nodiscard]] constexpr std::optional<can_id_fields_t> decode_can_id(std::uint32_t id) noexcept {
    if (id > kIdMax) return std::nullopt;
    can_id_fields_t f;
    f.endpoint = static_cast<std::uint16_t>(id & kEndpointMax);
    f.node = static_cast<std::uint16_t>((id >> kEndpointBits) & kNodeMax);
    f.version = static_cast<std::uint8_t>((id >> (kNodeBits + kEndpointBits)) & kVersionMax);
    return f;
}

/**
 * @brief Derive the CAN ID of slice @p index of an address-shift group.
 *
 * Address-shift slicing (CONTEXT.md): a logically large payload is spread across
 * consecutive endpoint slots `endpoint[0..N]` of the same node, so slice @p
 * index simply *shifts the endpoint sub-field*. The id stays in the same
 * version/node band, so the whole group keeps one arbitration priority class.
 *
 * @param base  The fields of slice 0 (its @ref can_id_fields_t::endpoint is the base slot).
 * @param index The zero-based slice index.
 * @return The packed 29-bit ID for the slice, or `std::nullopt` if
 *         `base.endpoint + index` would overflow the endpoint field.
 */
[[nodiscard]] constexpr std::optional<std::uint32_t> slice_can_id(const can_id_fields_t& base,
                                                                  std::size_t index) noexcept {
    const std::size_t slot = static_cast<std::size_t>(base.endpoint) + index;
    if (slot > kEndpointMax) return std::nullopt;
    can_id_fields_t f = base;
    f.endpoint = static_cast<std::uint16_t>(slot);
    return encode_can_id(f);
}

/** @brief Leading magic byte of an in-band @ref advertise_t frame. */
inline constexpr std::uint8_t kAdvertiseMagic = 0xAD;
/**
 * @brief On-wire format version of the @ref advertise_t frame layout.
 *
 * Version `0x02` (ADR-0044) widened the header from 16 to 18 bytes with the
 * explicit `target_node` field (directed groups + the hello/presence form).
 * The advertise family is transport-internal framing (ADR-0030 — not the L2 TLV
 * spec), so the bump is a module-local change; all nodes of one bus deployment
 * run one binding version.
 */
inline constexpr std::uint8_t kAdvertiseFormatVersion = 0x02;
/** @brief Fixed size, in bytes, of the @ref advertise_t header that precedes the path. */
inline constexpr std::size_t kAdvertiseHeaderSize = 18;
/**
 * @brief The `target_node` value meaning "undirected — every node on the bus".
 *
 * Deliberately outside the 13-bit node range (`> kNodeMax`), so no real node id
 * can alias it. Any other value makes the group DIRECTED: a node whose own id
 * differs consumes the group's data slices without reassembling or delivering
 * them (ADR-0044 transparent per-peer forwarding on a broadcast medium).
 */
inline constexpr std::uint16_t kCanBroadcastNode = 0xFFFF;

/**
 * @brief `flags` bit: the binding is a multi-frame **rope group**, not a single value.
 *
 * Set when the advertise is the manifest for an address-shift group
 * (`group-id ↔ (path, slice structure)`); clear for a single `id ↔ path` value
 * binding (CONTEXT.md *Advertise + id-match*).
 */
inline constexpr std::uint8_t kAdvertiseFlagGroup = 0x01;

/**
 * @brief A decoded in-band `advertise` frame — the identity↔path manifest.
 *
 * An advertise frame is a full-TLV control frame that *establishes* a
 * header-elided binding at runtime: it maps a CAN @ref advertise_t::can_id to a
 * libtracer @ref advertise_t::path, after which lean, id-matched data frames
 * carry only payload. For a rope group it additionally carries the slice
 * manifest (@ref advertise_t::slice_count, @ref advertise_t::group_total_len) —
 * ADR-0011 address-shift slicing made dynamic. A rejoining node re-announces its
 * own advertises, which is what makes the map self-healing (docs/reference/13
 * §self-healing).
 *
 * Two special forms (ADR-0044, both transport-internal per ADR-0030):
 *  - **hello / presence** — `slice_count == 0`: no binding is established and no
 *    data frames follow; the frame only announces "this node is on the bus" (and
 *    its identity path). Emitted at join; any advertise also refreshes liveness.
 *  - **directed** — `target_node != kCanBroadcastNode`: the group is addressed to
 *    ONE peer; every other node consumes its data slices without delivery. This
 *    is how a FWD forwarded onto the broadcast bus reaches exactly the peer its
 *    stripped `dst` segment named.
 *
 * On-wire layout (little-endian; a fixed @ref kAdvertiseHeaderSize byte header,
 * then the path):
 * | offset | size | field |
 * | ------ | ---- | ----- |
 * | 0      | 1    | magic = @ref kAdvertiseMagic |
 * | 1      | 1    | format version = @ref kAdvertiseFormatVersion |
 * | 2      | 1    | flags (@ref kAdvertiseFlagGroup) |
 * | 3      | 1    | reserved, must be zero |
 * | 4      | 4    | can_id (u32 LE; a 29-bit value) |
 * | 8      | 4    | group_total_len (u32 LE; 0 for a single value) |
 * | 12     | 2    | slice_count (u16 LE; 1 for a single value, 0 for hello) |
 * | 14     | 2    | target_node (u16 LE; @ref kCanBroadcastNode = undirected) |
 * | 16     | 2    | path_len (u16 LE) |
 * | 18     | path_len | path bytes (UTF-8 libtracer path) |
 */
struct advertise_t {
    std::uint32_t can_id = 0;          /**< @brief The 29-bit ID this advertise binds. */
    bool group = false;                /**< @brief True ⇒ a multi-frame rope group binding. */
    std::uint32_t group_total_len = 0; /**< @brief Total group payload bytes (0 if single-value). */
    std::uint16_t slice_count = 1;     /**< @brief Slice count (1 = single value, 0 = hello). */
    std::uint16_t target = kCanBroadcastNode; /**< @brief Directed target node id, or
                                                   @ref kCanBroadcastNode for every node. */
    std::string path;                         /**< @brief The libtracer path the id maps to. */

    /** @brief Field-wise equality (value type). */
    [[nodiscard]] bool operator==(const advertise_t&) const = default;
};

/**
 * @brief Serialize an @ref advertise_t frame to its on-wire bytes.
 *
 * @param a The advertise to encode (its @ref advertise_t::path may be empty).
 * @return The fully serialized frame bytes (@ref kAdvertiseHeaderSize + path length).
 */
[[nodiscard]] inline std::vector<std::byte> encode_advertise(const advertise_t& a) {
    const std::size_t path_len = a.path.size();
    std::vector<std::byte> out;
    out.reserve(kAdvertiseHeaderSize + path_len);

    const auto put_u8 = [&](std::uint8_t v) { out.push_back(static_cast<std::byte>(v)); };
    const auto put_u16 = [&](std::uint16_t v) {
        put_u8(static_cast<std::uint8_t>(v & 0xFFu));
        put_u8(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    };
    const auto put_u32 = [&](std::uint32_t v) {
        put_u8(static_cast<std::uint8_t>(v & 0xFFu));
        put_u8(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
        put_u8(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
        put_u8(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    };

    put_u8(kAdvertiseMagic);
    put_u8(kAdvertiseFormatVersion);
    put_u8(a.group ? kAdvertiseFlagGroup : 0u);
    put_u8(0u);  // reserved, MBZ
    put_u32(a.can_id);
    put_u32(a.group_total_len);
    put_u16(a.slice_count);
    put_u16(a.target);
    put_u16(static_cast<std::uint16_t>(path_len));
    for (char c : a.path) put_u8(static_cast<std::uint8_t>(c));
    return out;
}

/**
 * @brief Decode exactly one @ref advertise_t frame from the front of @p buf.
 *
 * Rejects a wrong magic, an unknown format version, or a non-zero reserved byte
 * (all `std::nullopt`). The length check is overflow-safe (a bogus `path_len`
 * cannot drive an out-of-bounds read).
 *
 * @param buf A byte stream that may hold a partial or whole advertise frame,
 *            possibly followed by more bytes.
 * @return `std::nullopt` if @p buf does not yet hold a complete, valid frame
 *         (need more bytes, or malformed); otherwise the decoded advertise paired
 *         with the number of bytes consumed from the front of @p buf.
 */
[[nodiscard]] inline std::optional<std::pair<advertise_t, std::size_t>> decode_advertise(
    std::span<const std::byte> buf) {
    if (buf.size() < kAdvertiseHeaderSize) return std::nullopt;

    const auto u8 = [&](std::size_t i) { return std::to_integer<std::uint8_t>(buf[i]); };
    const auto u16 = [&](std::size_t i) {
        return static_cast<std::uint16_t>(u8(i) | (static_cast<std::uint16_t>(u8(i + 1)) << 8));
    };
    const auto u32 = [&](std::size_t i) {
        return static_cast<std::uint32_t>(u8(i)) | (static_cast<std::uint32_t>(u8(i + 1)) << 8) |
               (static_cast<std::uint32_t>(u8(i + 2)) << 16) |
               (static_cast<std::uint32_t>(u8(i + 3)) << 24);
    };

    if (u8(0) != kAdvertiseMagic) return std::nullopt;
    if (u8(1) != kAdvertiseFormatVersion) return std::nullopt;
    if (u8(3) != 0u) return std::nullopt;  // reserved MBZ

    advertise_t a;
    a.group = (u8(2) & kAdvertiseFlagGroup) != 0;
    a.can_id = u32(4);
    a.group_total_len = u32(8);
    a.slice_count = u16(12);
    a.target = u16(14);
    const std::uint16_t path_len = u16(16);

    // Overflow-safe bound: kAdvertiseHeaderSize <= buf.size() is guaranteed above,
    // so buf.size() - kAdvertiseHeaderSize cannot underflow.
    if (path_len > buf.size() - kAdvertiseHeaderSize) return std::nullopt;

    a.path.reserve(path_len);
    for (std::size_t i = 0; i < path_len; ++i) {
        a.path.push_back(static_cast<char>(u8(kAdvertiseHeaderSize + i)));
    }
    return std::make_pair(std::move(a), kAdvertiseHeaderSize + static_cast<std::size_t>(path_len));
}

}  // namespace tr::net::can
