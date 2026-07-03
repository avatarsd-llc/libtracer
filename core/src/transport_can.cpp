/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_can.hpp"

#include <cstring>
#include <utility>

#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

namespace tr::net {

namespace {

// Reassembly identity for a group is derived purely from the CAN ID: the `node`
// sub-field becomes the 16-byte origin and the group's base endpoint becomes the
// `ts`. Both peers compute it the same way from the same id, so no per-frame
// origin/ts ever rides the constrained bus (header-elided).
tr::mem::can_origin_id_t origin_of(std::uint16_t node) {
    tr::mem::can_origin_id_t o{};
    o[0] = static_cast<std::uint8_t>(node & 0xFFu);
    o[1] = static_cast<std::uint8_t>((node >> 8) & 0xFFu);
    return o;
}

tr::mem::reassembly_key_t key_of(std::uint16_t node, std::uint16_t base_endpoint) {
    return tr::mem::reassembly_key_t{origin_of(node), static_cast<std::uint64_t>(base_endpoint)};
}

// (removed) own_copy — the alloc/copy/over triplet now lives in one audited
// locus, tr::view::over_bytes (mem_heap.hpp). Call sites use it directly.

}  // namespace

// socketcan_link_t lives in its OWN translation unit (src/socketcan_link.cpp,
// Linux-only; src/socketcan_link_stub.cpp elsewhere) — platform selection is a
// build-system concern, never an in-source #ifdef. This TU stays 100% portable:
// the transport talks only to the can_link_t seam.

transport_can::transport_can(std::unique_ptr<can_link_t> link, transport_can_config_t config)
    : link_(std::move(link)), cfg_(std::move(config)) {
    link_->on_receive([this](const can_frame_data_t& f) { on_rx(f); });
}

transport_can::~transport_can() {
    // Drop the receiver first; then releasing the link stops its receive thread,
    // which can no longer re-enter a half-destroyed transport.
    {
        const std::lock_guard lock(m_);
        receiver_ = nullptr;
    }
    link_.reset();
}

void transport_can::set_receiver(receiver_t receiver) {
    const std::lock_guard lock(m_);
    receiver_ = std::move(receiver);
}

std::optional<can::advertise_t> transport_can::learned_binding(std::uint32_t base_can_id) const {
    const std::lock_guard lock(const_cast<std::mutex&>(rx_m_));
    const auto it = learned_.find(base_can_id);
    if (it == learned_.end()) return std::nullopt;
    return it->second;
}

std::uint16_t transport_can::alloc_base(std::size_t slice_count) {
    const auto span = static_cast<std::uint16_t>(slice_count == 0 ? 1 : slice_count);
    if (static_cast<std::size_t>(next_base_) + span > can::kEndpointMax) {
        next_base_ = kCanFirstDataEndpoint;  // wrap, leaving the control slot free
    }
    const std::uint16_t base = next_base_;
    next_base_ = static_cast<std::uint16_t>(next_base_ + span);
    return base;
}

void transport_can::emit_advertise(const can::advertise_t& adv) {
    // The advertise rides the control ID as an in-order byte stream, sliced into
    // CLASSIC (≤8B, exact-length) windows so no CAN-FD DLC padding can perturb the
    // stream decoder on the far side.
    const std::vector<std::byte> bytes = can::encode_advertise(adv);
    const std::uint32_t control_id =
        can::encode_can_id({cfg_.version, cfg_.node, kCanControlEndpoint});
    std::size_t off = 0;
    while (off < bytes.size()) {
        const std::size_t n =
            std::min<std::size_t>(tr::view::kCanClassicMaxData, bytes.size() - off);
        can_frame_data_t frame;
        frame.id = control_id;
        frame.fd = false;
        frame.len = static_cast<std::uint8_t>(n);
        std::memcpy(frame.data.data(), bytes.data() + off, n);
        link_->write_raw(frame);
        off += n;
    }
}

void transport_can::send(std::span<const std::byte> frame) {
    if (frame.empty()) return;

    const std::lock_guard lock(tx_m_);

    // Own the bytes so view_can_frames_t can carve zero-copy subviews out of them.
    const tr::view::view_t payload = tr::view::over_bytes(frame);
    const tr::view::view_can_frames_t frames =
        tr::view::view_can_frames_t::split(payload, cfg_.mode);
    const std::size_t count = frames.frame_count();
    if (count == 0) return;

    const std::uint16_t base_ep = alloc_base(count);
    const can::can_id_fields_t base_fields{cfg_.version, cfg_.node, base_ep};
    const std::uint32_t base_id = can::encode_can_id(base_fields);

    // The manifest carries the exact total length (so the peer trims FD padding)
    // and the slice count (totality opt-in → trailing-drop detection).
    can::advertise_t adv;
    adv.can_id = base_id;
    adv.group = count > 1;
    adv.group_total_len = static_cast<std::uint32_t>(frame.size());
    adv.slice_count = static_cast<std::uint16_t>(count);
    adv.path = cfg_.path;
    emit_advertise(adv);

    const bool fd = cfg_.mode == tr::view::can_frame_mode_t::FD;
    for (std::size_t i = 0; i < count; ++i) {
        const tr::view::view_t& window = frames.frames()[i];
        const std::span<const std::byte> wb = window.bytes();
        const auto slice_id = can::slice_can_id(base_fields, i);
        if (!slice_id) break;  // endpoint overflow — group too large for this node

        can_frame_data_t out;
        out.id = *slice_id;
        out.fd = fd;
        const std::size_t logical = wb.size();
        const std::size_t on_wire =
            fd ? tr::view::can_fd_dlc_round_up(logical) : logical;  // DLC pad (FD only)
        out.len = static_cast<std::uint8_t>(on_wire);
        std::memcpy(out.data.data(), wb.data(), logical);
        // Pad bytes already zero (data{} is value-initialized); the peer trims to
        // group_total_len so the padding never reaches the delivered frame.
        link_->write_raw(out);
    }
}

void transport_can::on_rx(const can_frame_data_t& frame) {
    const auto fields = can::decode_can_id(frame.id);
    if (!fields) return;

    const std::lock_guard lock(rx_m_);
    if (fields->endpoint == kCanControlEndpoint) {
        // Accumulate the per-node advertise byte stream and pop every complete frame.
        std::vector<std::byte>& buf = control_[fields->node];
        const std::span<const std::byte> in = frame.bytes();
        buf.insert(buf.end(), in.begin(), in.end());
        while (true) {
            const auto decoded = can::decode_advertise(buf);
            if (!decoded) break;  // need more bytes / malformed-but-incomplete
            learn_advertise(decoded->first);
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(decoded->second));
        }
        return;
    }
    process_data(frame);
}

void transport_can::learn_advertise(const can::advertise_t& adv) {
    learned_[adv.can_id] = adv;
    const auto base = can::decode_can_id(adv.can_id);
    if (!base) return;
    reasm_.set_expected_count(key_of(base->node, base->endpoint), adv.slice_count);

    // A data frame may have arrived ahead of its manifest (cross-ID arbitration);
    // re-drive any now-matchable pending slices.
    std::vector<can_frame_data_t> still_pending;
    std::vector<can_frame_data_t> ready;
    still_pending.reserve(pending_.size());
    for (auto& f : pending_) {
        const auto ff = can::decode_can_id(f.id);
        if (ff && ff->node == base->node && ff->endpoint >= base->endpoint &&
            ff->endpoint < base->endpoint + adv.slice_count) {
            ready.push_back(f);
        } else {
            still_pending.push_back(f);
        }
    }
    pending_ = std::move(still_pending);
    for (const auto& f : ready) process_data(f);
}

void transport_can::process_data(const can_frame_data_t& frame) {
    const auto fields = can::decode_can_id(frame.id);
    if (!fields) return;

    // Find the binding whose [base, base+slice_count) endpoint range owns this id.
    const can::advertise_t* binding = nullptr;
    std::uint16_t base_ep = 0;
    for (const auto& [base_id, adv] : learned_) {
        const auto base = can::decode_can_id(base_id);
        if (!base || base->node != fields->node) continue;
        if (fields->endpoint >= base->endpoint &&
            fields->endpoint < base->endpoint + adv.slice_count) {
            binding = &adv;
            base_ep = base->endpoint;
            break;
        }
    }
    if (!binding) {
        pending_.push_back(frame);  // hold until its advertise lands
        return;
    }

    const tr::mem::reassembly_key_t key = key_of(fields->node, base_ep);
    const std::uint32_t index = static_cast<std::uint32_t>(fields->endpoint - base_ep);
    reasm_.add_slice(key, index, tr::view::over_bytes(frame.bytes()));

    if (!reasm_.is_complete(key)) return;
    const auto rope = reasm_.assemble(key);
    const std::uint32_t total = binding->group_total_len;
    reasm_.erase(key);
    // The learned binding is kept (the identity↔path map persists and self-heals by
    // overwrite when the node re-advertises); only the per-group slice buffer is freed.
    if (!rope) return;

    // Flatten the reassembled slices, then trim back to the advertised total —
    // this is where CAN-FD DLC padding on the tail slice is removed.
    const tr::view::view_t flat = rope->flatten();
    const std::span<const std::byte> all = flat.bytes();
    const std::size_t n = std::min<std::size_t>(total, all.size());
    deliver(std::span<const std::byte>(all.data(), n));
}

void transport_can::deliver(std::span<const std::byte> frame) {
    receiver_t receiver;
    {
        const std::lock_guard lock(m_);
        receiver = receiver_;
    }
    if (receiver) receiver(frame);
}

}  // namespace tr::net
