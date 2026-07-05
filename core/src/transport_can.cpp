/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_can.hpp"

#include <charconv>
#include <cstring>
#include <optional>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

namespace tr::net {

namespace {

// The canonical bus-peer name (ADR-0044): 'n' + the node id in decimal, no
// leading zeros — deterministic, collision-safe within the bus, and small enough
// for a stack buffer (13-bit node => at most 4 digits).
struct peer_name_buf_t {
    std::array<char, 8> buf{};
    std::size_t len = 0;
    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(buf.data(), len);
    }
};

[[nodiscard]] peer_name_buf_t format_peer_name(std::uint16_t node) {
    peer_name_buf_t out;
    out.buf[0] = 'n';
    const auto [end, ec] = std::to_chars(out.buf.data() + 1, out.buf.data() + out.buf.size(), node);
    out.len = ec == std::errc{} ? static_cast<std::size_t>(end - out.buf.data()) : 0;
    return out;
}

// The exact inverse of format_peer_name: nullopt for anything non-canonical
// (wrong prefix, empty digits, leading zero, non-digit, out of the node range).
[[nodiscard]] std::optional<std::uint16_t> parse_peer_name(std::string_view name) {
    if (name.size() < 2 || name.front() != 'n') return std::nullopt;
    const std::string_view digits = name.substr(1);
    if (digits.size() > 1 && digits.front() == '0') return std::nullopt;  // non-canonical
    std::uint32_t v = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10u + static_cast<std::uint32_t>(c - '0');
        if (v > can::kNodeMax) return std::nullopt;
    }
    return static_cast<std::uint16_t>(v);
}

// Reassembly identity for a group is derived purely from the CAN ID: the `node`
// sub-field becomes the 16-byte origin and the group's base endpoint becomes the
// `ts`. Both peers compute it the same way from the same id, so no per-frame
// origin/ts ever rides the constrained bus (header-elided).
tr::net::can_origin_id_t origin_of(std::uint16_t node) {
    tr::net::can_origin_id_t o{};
    o[0] = static_cast<std::uint8_t>(node & 0xFFu);
    o[1] = static_cast<std::uint8_t>((node >> 8) & 0xFFu);
    return o;
}

tr::net::reassembly_key_t key_of(std::uint16_t node, std::uint16_t base_endpoint) {
    return tr::net::reassembly_key_t{origin_of(node), static_cast<std::uint64_t>(base_endpoint)};
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
    // Announce presence at join (ADR-0044): a hello advertise (slice_count == 0)
    // seeds every listener's last-heard table before any data flows, so a fresh
    // node is enumerable immediately. Liveness thereafter refreshes with traffic.
    emit_hello();
}

transport_can::~transport_can() {
    // Drop the receivers first; then releasing the link stops its receive thread,
    // which can no longer re-enter a half-destroyed transport.
    {
        const std::lock_guard lock(m_);
        receiver_ = nullptr;
        peer_receiver_ = nullptr;
    }
    link_.reset();
}

void transport_can::set_receiver(receiver_t receiver) {
    const std::lock_guard lock(m_);
    receiver_ = std::move(receiver);
}

void transport_can::set_peer_receiver(peer_receiver_t receiver) {
    const std::lock_guard lock(m_);
    peer_receiver_ = std::move(receiver);
}

std::optional<can::advertise_t> transport_can::learned_binding(std::uint32_t base_can_id) const {
    const std::lock_guard lock(const_cast<std::mutex&>(rx_m_));
    const auto it = learned_.find(base_can_id);
    if (it == learned_.end()) return std::nullopt;
    return it->second.adv;
}

// --- the bus capability (ADR-0044) -------------------------------------------

void transport_can::touch_peer(std::uint16_t node) {
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard lock(peers_m_);
    // Insert-only, one entry per DISTINCT node id ever heard (the learned_ map's
    // policy): growth tracks the bus population — structurally bounded by the
    // 13-bit id space, never per-frame — and an existing entry only refreshes.
    const auto [it, fresh] = peers_.try_emplace(node);
    it->second.last_heard = now;
    if (fresh) {
        it->second.endpoint.owner_ = this;
        it->second.endpoint.node_.store(node, std::memory_order_relaxed);
    }
}

void transport_can::enumerate_peers(const peer_visitor_t& visit) const {
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard lock(peers_m_);
    for (const auto& [node, e] : peers_) {
        if (now - e.last_heard > cfg_.peer_ttl) continue;  // expired = inaudible
        const peer_name_buf_t name = format_peer_name(node);
        visit(name.view());
    }
}

transport_t* transport_can::peer_link(std::string_view peer) {
    const std::optional<std::uint16_t> node = parse_peer_name(peer);
    if (!node) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard lock(peers_m_);
    const auto it = peers_.find(*node);
    if (it == peers_.end() || now - it->second.last_heard > cfg_.peer_ttl) return nullptr;
    return &it->second.endpoint;
}

void transport_can::peer_endpoint_t::send(std::span<const std::byte> frame) {
    owner_->send_impl(frame, node_.load(std::memory_order_relaxed));
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

void transport_can::emit_hello() {
    // The presence form (ADR-0044): slice_count == 0 binds nothing and precedes no
    // data — it only says "this node is on the bus" and carries its identity path.
    can::advertise_t hello;
    hello.can_id = can::encode_can_id({cfg_.version, cfg_.node, kCanControlEndpoint});
    hello.group = false;
    hello.group_total_len = 0;
    hello.slice_count = 0;
    hello.path = cfg_.path;
    const std::lock_guard lock(tx_m_);
    emit_advertise(hello);
}

void transport_can::send(std::span<const std::byte> frame) {
    send_impl(frame, can::kCanBroadcastNode);
}

void transport_can::send_impl(std::span<const std::byte> frame, std::uint16_t target) {
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

    // The manifest carries the exact total length (so the peer trims FD padding),
    // the slice count (totality opt-in → trailing-drop detection), and — for a
    // per-peer directed send (ADR-0044) — the target node id.
    can::advertise_t adv;
    adv.can_id = base_id;
    adv.group = count > 1;
    adv.group_total_len = static_cast<std::uint32_t>(frame.size());
    adv.slice_count = static_cast<std::uint16_t>(count);
    adv.target = target;
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
    // Discovery-layer versioning (ADR-0030): a distinct version prefix is a
    // disjoint protocol band — frames outside ours are not ours to interpret.
    if (fields->version != cfg_.version) return;
    // Self-echo guard (e.g. CAN_RAW_RECV_OWN_MSGS, or a second local socket):
    // our own frames neither feed the map nor make us our own peer.
    if (fields->node == cfg_.node) return;
    // ANY valid same-version frame from another node is a liveness signal —
    // the last-heard table (ADR-0044) is refreshed before the payload is parsed.
    touch_peer(fields->node);

    const std::lock_guard lock(rx_m_);
    if (fields->endpoint == kCanControlEndpoint) {
        // Accumulate the per-node advertise byte stream and pop every complete frame.
        std::vector<std::byte>& buf = control_[fields->node];
        const std::span<const std::byte> in = frame.bytes();
        buf.insert(buf.end(), in.begin(), in.end());
        while (!buf.empty()) {
            const auto decoded = can::decode_advertise(buf);
            if (decoded) {
                learn_advertise(decoded->first);
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(decoded->second));
                continue;
            }
            // Not decodable from the front. A plausible prefix just needs more
            // bytes; anything else is a fragment (a mid-stream join saw the tail
            // of an in-flight advertise, or a lost control frame tore one) —
            // resynchronize by dropping bytes up to the next plausible boundary,
            // or the stream wedges permanently on the garbage prefix.
            if (can::advertise_prefix_plausible(buf)) break;
            std::size_t skip = 1;
            while (skip < buf.size() &&
                   std::to_integer<std::uint8_t>(buf[skip]) != can::kAdvertiseMagic) {
                ++skip;
            }
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(skip));
        }
        return;
    }
    process_data(frame);
}

void transport_can::learn_advertise(const can::advertise_t& adv) {
    // The hello/presence form (slice_count == 0) binds nothing — its liveness
    // effect already landed in touch_peer on the frame that carried it.
    if (adv.slice_count == 0) return;
    // A directed group addressed to another node (ADR-0044) is learned so its
    // data slices are recognized and CONSUMED, but never reassembled/delivered.
    const bool deliver = adv.target == can::kCanBroadcastNode || adv.target == cfg_.node;
    learned_[adv.can_id] = binding_t{adv, deliver};
    const auto base = can::decode_can_id(adv.can_id);
    if (!base) return;
    if (deliver) reasm_.set_expected_count(key_of(base->node, base->endpoint), adv.slice_count);

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
    const binding_t* binding = nullptr;
    std::uint16_t base_ep = 0;
    for (const auto& [base_id, b] : learned_) {
        const auto base = can::decode_can_id(base_id);
        if (!base || base->node != fields->node) continue;
        if (fields->endpoint >= base->endpoint &&
            fields->endpoint < base->endpoint + b.adv.slice_count) {
            binding = &b;
            base_ep = base->endpoint;
            break;
        }
    }
    if (!binding) {
        pending_.push_back(frame);  // hold until its advertise lands
        return;
    }
    // A directed group addressed to another node (ADR-0044): recognized, consumed,
    // dropped — no reassembly buffer is spent on a neighbour's traffic.
    if (!binding->deliver) return;

    const tr::net::reassembly_key_t key = key_of(fields->node, base_ep);
    const std::uint32_t index = static_cast<std::uint32_t>(fields->endpoint - base_ep);
    reasm_.add_slice(key, index, tr::view::over_bytes(frame.bytes()));

    if (!reasm_.is_complete(key)) return;
    const auto rope = reasm_.assemble(key);
    const std::uint32_t total = binding->adv.group_total_len;
    reasm_.erase(key);
    // The learned binding is kept (the identity↔path map persists and self-heals by
    // overwrite when the node re-advertises); only the per-group slice buffer is freed.
    if (!rope) return;

    // Flatten the reassembled slices, then trim back to the advertised total —
    // this is where CAN-FD DLC padding on the tail slice is removed.
    const tr::view::view_t flat = rope->flatten();
    const std::span<const std::byte> all = flat.bytes();
    const std::size_t n = std::min<std::size_t>(total, all.size());
    deliver(fields->node, std::span<const std::byte>(all.data(), n));
}

void transport_can::deliver(std::uint16_t src_node, std::span<const std::byte> frame) {
    receiver_t receiver;
    peer_receiver_t peer_receiver;
    {
        const std::lock_guard lock(m_);
        receiver = receiver_;
        peer_receiver = peer_receiver_;
    }
    // The peer-named sink wins (ADR-0044): the sender's bus name becomes the FWD
    // hop's inbound NAME, so a reply routes back to exactly that peer, directed.
    if (peer_receiver) {
        const peer_name_buf_t name = format_peer_name(src_node);
        peer_receiver(name.view(), frame);
        return;
    }
    if (receiver) receiver(frame);
}

// --- the `can` catalog factory (ADR-0027 / ADR-0043 §5 / ADR-0044) -----------

transport_vertex_t::transport_factory_t can_transport_factory() {
    return [](const conn_settings_t& /*settings*/,
              const wire::tlv_t* raw_config) -> graph::result_t<std::unique_ptr<transport_t>> {
        // Every CAN-private key is parsed HERE from the raw config TLV (the
        // ADR-0043 §5 leanness ruling): nothing CAN-shaped lands in the shared
        // conn_settings_t. Positional NAME-key / value pairs, like parse_config.
        std::string ifname;
        transport_can_config_t cfg;
        bool have_node = false;
        if (raw_config != nullptr) {
            const std::vector<wire::tlv_t>& ch = raw_config->children;
            for (std::size_t i = 0; i + 1 < ch.size(); ++i) {
                if (ch[i].type != wire::type_t::NAME) continue;
                const std::string_view key = detail::as_string_view(ch[i].payload);
                const wire::tlv_t& val = ch[i + 1];
                if (key == "ifname" && val.type == wire::type_t::NAME) {
                    ifname = std::string(detail::as_string_view(val.payload));
                } else if (key == "path" && val.type == wire::type_t::NAME) {
                    cfg.path = std::string(detail::as_string_view(val.payload));
                } else if (key == "node" && val.type == wire::type_t::VALUE &&
                           !val.payload.empty()) {
                    cfg.node = detail::load_le<std::uint16_t>(val.payload);
                    have_node = true;
                } else if (key == "version" && val.type == wire::type_t::VALUE &&
                           !val.payload.empty()) {
                    cfg.version = detail::load_le<std::uint8_t>(val.payload);
                } else if (key == "fd" && val.type == wire::type_t::VALUE && !val.payload.empty()) {
                    cfg.mode = detail::load_le<std::uint8_t>(val.payload) != 0
                                   ? tr::view::can_frame_mode_t::FD
                                   : tr::view::can_frame_mode_t::CLASSIC;
                } else if (key == "peer_ttl_ms" && val.type == wire::type_t::VALUE &&
                           !val.payload.empty()) {
                    cfg.peer_ttl =
                        std::chrono::milliseconds(detail::load_le<std::uint32_t>(val.payload));
                }
            }
        }
        if (ifname.empty() || !have_node || cfg.node > can::kNodeMax ||
            cfg.version > can::kVersionMax) {
            return std::unexpected(graph::status_t::TYPE_MISMATCH);
        }
        auto link = std::make_unique<socketcan_link_t>(ifname);
        if (!link->ok()) return std::unexpected(graph::status_t::NOT_FOUND);  // no kernel CAN
        return std::make_unique<transport_can>(std::move(link), std::move(cfg));
    };
}

}  // namespace tr::net
