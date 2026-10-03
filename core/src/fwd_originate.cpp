/**
 * @file
 * @brief `fwd_router_t::originate` — a node issues a forwarded READ or WRITE itself (#1645).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The REQUEST half of origination: frame building, arming and cancelling the caller's record.
 * The reply half (`reply_target`) stays in fwd_router.cpp beside the reply terminus it serves.
 * A TU of its own, and that is measured: added to fwd_router.cpp, this code re-partitioned
 * GCC's inline budget and grew the ratchet-pinned `route_fwd_forward<rope_cursor>` by 319 B
 * without touching it.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/path.hpp"
#include "libtracer/view.hpp"

namespace tr::net {

using graph::fwd_op_t;
using view::segment_ptr_t;
using view::view_t;

namespace {

/** @brief A TLV header's size for a @p body-byte body: the u16 form, or u32 past it. */
[[nodiscard]] constexpr std::size_t tlv_head_bytes(std::size_t body) noexcept {
    return body > 0xFFFFu ? 6u : 4u;
}

/** @brief Emit one TLV header (type, opt, LE length) at @p p; returns the advanced cursor. */
std::byte* put_head(std::byte* p, wire::type_t type, bool pl, std::size_t body) noexcept {
    const bool ll = body > 0xFFFFu;
    *p++ = static_cast<std::byte>(std::to_underlying(type));
    *p++ = static_cast<std::byte>(wire::opt_t{.pl = pl, .ll = ll}.encode());
    detail::store_le(std::span<std::byte>(p, ll ? 4u : 2u), static_cast<std::uint32_t>(body),
                     ll ? 4u : 2u);
    return p + (ll ? 4u : 2u);
}

/** @brief Emit a whole leaf TLV over @p body at @p p; returns the advanced cursor. */
std::byte* put_leaf(std::byte* p, wire::type_t type, std::span<const std::byte> body) noexcept {
    p = put_head(p, type, false, body.size());
    if (!body.empty()) std::memcpy(p, body.data(), body.size());
    return p + body.size();
}

/** @brief A little-endian @p v as a @p W-byte VALUE leaf at @p p (an index / index_mode). */
template <std::size_t W>
std::byte* put_value(std::byte* p, std::uint32_t v) noexcept {
    std::array<std::byte, W> raw{};
    detail::store_le(std::span<std::byte>(raw), v, W);
    return put_leaf(p, wire::type_t::VALUE, raw);
}

/**
 * @brief The FIELD selector body of @p field (RFC-0004 §C), the inverse of the terminus's
 *        `selector_to_field`: per step a NAME, then nothing (SCALAR), `u8 1` (`[]`, ELEMENT),
 *        `u8 2` (`[*]`, WILDCARD) or `u32 N, u8 1` (`[N]`). With @p out null it only sizes.
 */
std::size_t put_field_body(std::byte* out, const graph::field_path_t& field) noexcept {
    constexpr std::size_t kU8 = 5;   // VALUE header + 1
    constexpr std::size_t kU32 = 8;  // VALUE header + 4
    std::size_t n = 0;
    for (const graph::field_step_t& s : field.steps) {
        const std::size_t step = tlv_head_bytes(s.name.size()) + s.name.size() +
                                 (s.append || s.wildcard ? kU8
                                  : s.indexed            ? kU32 + kU8
                                                         : 0u);
        if (out != nullptr) {
            std::byte* p =
                put_leaf(out + n, wire::type_t::NAME, std::as_bytes(std::span<const char>(s.name)));
            if (s.wildcard) {
                p = put_value<1>(p, 2);
            } else if (s.append) {
                p = put_value<1>(p, 1);
            } else if (s.indexed) {
                p = put_value<4>(p, s.index);
                p = put_value<1>(p, 1);
            }
        }
        n += step;
    }
    return n;
}

}  // namespace

fwd_router_t::origin_t::~origin_t() {
    if (fwd_router_t* const r = owner_.load(std::memory_order_acquire); r != nullptr)
        (void)r->cancel(*this);
}

fwd_router_t::~fwd_router_t() {
    // A record outliving its router must not keep a dangling owner: disarm every one still
    // linked, so its destructor finds nothing to cancel.
    const std::lock_guard lock(origin_m_);
    for (origin_t* o = origins_.load(std::memory_order_relaxed); o != nullptr;) {
        origin_t* const next = o->next_;
        o->next_ = nullptr;
        o->owner_.store(nullptr, std::memory_order_release);
        o = next;
    }
    origins_.store(nullptr, std::memory_order_relaxed);
}

graph::result_t<void> fwd_router_t::originate(origin_t& slot, fwd_op_t op, const graph::path_t& dst,
                                              std::span<const std::byte> payload,
                                              const graph::path_t* reply_to) {
    // READ carries no payload and WRITE carries exactly one TLV; AWAIT is not originated here.
    if (op == fwd_op_t::READ ? !payload.empty() : op != fwd_op_t::WRITE || payload.empty())
        return std::unexpected(graph::status_t::TYPE_MISMATCH);
    if (dst.key().empty()) return std::unexpected(graph::status_t::INVALID_PATH);
    if (reply_to != nullptr) {
        // The reply must TERMINATE here: a route whose head names a mount would be forwarded
        // on by this node's own ingress instead of reaching the record.
        const graph::wire_target_split_t split = split_subscriber_target(reply_to->key());
        if (reply_to->key().empty() || !split.link.empty() || split.unroutable)
            return std::unexpected(graph::status_t::INVALID_PATH);
    }

    // Arm FIRST: the reply can arrive before the send below returns (a local terminus, a
    // synchronous link), and it must find the record.
    {
        const std::lock_guard lock(origin_m_);
        if (slot.owner_.load(std::memory_order_relaxed) != nullptr)
            return std::unexpected(graph::status_t::BACKPRESSURE);
        if (reply_to != nullptr) {
            slot.route_ = reply_to->key();
        } else {
            // One packed record, `~o` + 8 hex digits of a per-router sequence: a valid segment
            // (`path::valid_segment` reserves none of its characters) that no mount begins
            // with, distinct for 2^32 requests, and nothing that names this node's memory.
            constexpr std::string_view kHex = "0123456789abcdef";
            const std::uint32_t seq = origin_seq_.fetch_add(1, std::memory_order_relaxed);
            std::array<std::byte, 16>& t = slot.token_;
            t[0] = std::byte{10};
            t[1] = std::byte{'~'};
            t[2] = std::byte{'o'};
            for (std::size_t i = 0; i < 8; ++i)
                t[3 + i] = static_cast<std::byte>(kHex[(seq >> (28 - 4 * i)) & 0xFu]);
            slot.route_ = std::span<const std::byte>(t.data(), 11);
        }
        slot.next_ = origins_.load(std::memory_order_relaxed);
        slot.owner_.store(this, std::memory_order_release);
        origins_.store(&slot, std::memory_order_release);
    }

    // FWD{ VALUE op, PATH dst, FIELD?, PATH src, payload? } — sized exactly, emitted once.
    const std::span<const std::byte> dkey = dst.key();
    const std::size_t field_body = put_field_body(nullptr, dst.field());
    const std::size_t body = 5 + tlv_head_bytes(dkey.size()) + dkey.size() +
                             (field_body != 0 ? tlv_head_bytes(field_body) + field_body : 0u) +
                             tlv_head_bytes(slot.route_.size()) + slot.route_.size() +
                             payload.size();
    view::segment_ptr_t seg = view::segment_alloc(*egress_, tlv_head_bytes(body) + body);
    if (!seg) {
        (void)cancel(slot);
        return std::unexpected(graph::status_t::BACKPRESSURE);
    }
    std::byte* p = put_head(seg->bytes.data(), wire::type_t::FWD, true, body);
    p = put_value<1>(p, std::to_underlying(op));
    p = put_leaf(p, wire::type_t::PATH, dkey);
    if (field_body != 0) {
        p = put_head(p, wire::type_t::FIELD, true, field_body);
        p += put_field_body(p, dst.field());
    }
    p = put_leaf(p, wire::type_t::PATH, slot.route_);
    if (!payload.empty()) std::memcpy(p, payload.data(), payload.size());

    // The ONE ingress every frame takes, with no inbound link: an empty name is the node's own
    // origin, so a forward hop grows `src` by nothing and a local terminus hands its reply to
    // the record (`resolve_terminus`). The view lets a local WRITE store by reference.
    const view_t frame = view_t::over(std::move(seg));
    on_frame_impl({}, frame.bytes(), &frame, nullptr, false, peer_handle_t{});
    return {};
}

bool fwd_router_t::cancel(origin_t& slot) noexcept {
    const std::lock_guard lock(origin_m_);
    if (slot.owner_.load(std::memory_order_relaxed) != this) return false;
    for (origin_t *prev = nullptr, *o = origins_.load(std::memory_order_relaxed); o != nullptr;
         prev = o, o = o->next_) {
        if (o != &slot) continue;
        if (prev != nullptr)
            prev->next_ = o->next_;
        else
            origins_.store(o->next_, std::memory_order_release);
        break;
    }
    slot.next_ = nullptr;
    slot.owner_.store(nullptr, std::memory_order_release);
    return true;
}

}  // namespace tr::net
