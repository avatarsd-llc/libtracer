/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/fwd_router.hpp"

#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/view.hpp"

namespace tr::net {

using graph::fwd_op_t;
using view::segment_ptr_t;
using view::view_t;
using wire::opt_t;
using wire::tlv_t;
using wire::type_t;

namespace {

constexpr std::uint8_t u8(std::byte b) noexcept { return std::to_integer<std::uint8_t>(b); }

// One top-level TLV header read in isolation (NO descent) — the byte offsets the
// zero-copy forward rebuild needs. Mirrors frame.cpp's parse_one length math,
// including the optional trailer size, but keeps everything as offsets into the
// caller's buffer so the rebuild can re-slice src/payload as views (no copy).
struct hdr_t {
    type_t type{};
    opt_t opt{};
    std::size_t header_len = 0;  // 4 (u16 length) or 6 (u32 length)
    std::size_t body_off = 0;    // absolute offset of the body within the buffer
    std::size_t body_len = 0;    // body (children/payload) length, trailer excluded
    std::size_t total = 0;       // header_len + body_len + trailer
};

[[nodiscard]] std::optional<hdr_t> read_header(std::span<const std::byte> buf, std::size_t pos) {
    if (pos + 2 > buf.size()) return std::nullopt;
    const opt_t opt = opt_t::decode(u8(buf[pos + 1]));
    const std::size_t header_len = opt.ll ? 6u : 4u;
    if (pos + header_len > buf.size()) return std::nullopt;
    const std::size_t body_len =
        static_cast<std::size_t>(detail::load_le(buf.subspan(pos + 2, opt.ll ? 4u : 2u)));
    const std::size_t ts_size = opt.ts ? (opt.tf ? 4u : 8u) : 0u;
    const std::size_t crc_size = opt.cr ? (opt.cw ? 2u : 4u) : 0u;
    const std::size_t total = header_len + body_len + ts_size + crc_size;
    if (pos + total > buf.size()) return std::nullopt;
    return hdr_t{.type = static_cast<type_t>(u8(buf[pos])),
                 .opt = opt,
                 .header_len = header_len,
                 .body_off = pos + header_len,
                 .body_len = body_len,
                 .total = total};
}

// Append a structured-TLV header (type, opt.PL [+LL], little-endian length) for a
// body of `body_len` bytes. Identical to op_resolve.cpp's emit_struct_header.
void push_header(std::vector<std::byte>& out, type_t type, std::size_t body_len) {
    opt_t opt{.pl = true};
    if (body_len > 0xFFFFu) opt.ll = true;
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(type)));
    out.push_back(static_cast<std::byte>(opt.encode()));
    detail::append_le(out, static_cast<std::uint32_t>(body_len), opt.ll ? 4u : 2u);
}

}  // namespace

void fwd_router_t::add_child(std::string name, transport_t& link) {
    children_.push_back(child_t{name, &link});
    link.set_receiver([this, name](std::span<const std::byte> frame) { on_frame(name, frame); });
}

void fwd_router_t::on_reply(std::function<void(const tlv_t&)> cb) { reply_cb_ = std::move(cb); }

void fwd_router_t::on_inbound(std::function<void(std::string_view, const tlv_t&)> cb) {
    inbound_cb_ = std::move(cb);
}

void fwd_router_t::on_raw(std::function<void(std::string_view, std::span<const std::byte>)> cb) {
    raw_cb_ = std::move(cb);
}

void fwd_router_t::on_compact_delivery(
    std::function<void(std::span<const std::byte>, std::span<const std::byte>)> cb) {
    delivery_cb_ = std::move(cb);
}

void fwd_router_t::on_stale_label(std::function<void(std::string_view, std::uint16_t)> cb) {
    stale_cb_ = std::move(cb);
}

void fwd_router_t::clear_link(std::string_view link_name) { handles_.clear_link(link_name); }

std::uint16_t fwd_router_t::advertise(std::string_view link_name,
                                      std::span<const std::byte> route_path) {
    transport_t* const link = link_by_name(link_name);
    if (link == nullptr) return 0;
    const std::uint16_t label = handles_.alloc_label(link_name);
    handles_.record_egress(link_name, label,
                           std::vector<std::byte>(route_path.begin(), route_path.end()));
    const std::vector<std::byte> adv = encode_advertise(label, route_path);
    link->send(std::span<const std::byte>(adv));
    return label;
}

void fwd_router_t::send_compact(std::string_view link_name, std::uint16_t label,
                                std::span<const std::byte> payload) {
    if (transport_t* const link = link_by_name(link_name)) {
        const std::vector<std::byte> out = encode_compact(label, payload);
        link->send(std::span<const std::byte>(out));
    }
}

transport_t* fwd_router_t::child_by_segment(std::span<const std::byte> seg) const {
    for (const child_t& c : children_) {
        if (c.name.size() == seg.size() && std::memcmp(c.name.data(), seg.data(), seg.size()) == 0)
            return c.link;
    }
    return nullptr;
}

transport_t* fwd_router_t::link_by_name(std::string_view name) const {
    for (const child_t& c : children_) {
        if (c.name == name) return c.link;
    }
    return nullptr;
}

void fwd_router_t::on_frame(std::string_view inbound_name, std::span<const std::byte> frame) {
    if (raw_cb_) raw_cb_(inbound_name, frame);
    const auto dec = wire::decode(frame);
    if (!dec || !dec->opt.pl) return;  // drop malformed / non-structured

    // Dispatch on frame type. FWD is the slice-3 one-shot/full-route plane; the
    // route-handle control frames (ADVERTISE/COMPACT/HANDLE_NACK) are the slice-4
    // ws delivery-compaction plane that rides the same link (RFC-0004 §E.1).
    switch (dec->type) {
        case type_t::FWD:
            if (inbound_cb_) inbound_cb_(inbound_name, *dec);
            route_fwd(inbound_name, frame, *dec);
            return;
        case type_t::ADVERTISE:
            on_advertise(inbound_name, *dec);
            return;
        case type_t::COMPACT:
            on_compact(inbound_name, *dec);
            return;
        case type_t::HANDLE_NACK:
            on_nack(inbound_name, *dec);
            return;
        default:
            return;  // drop anything else
    }
}

void fwd_router_t::route_fwd(std::string_view inbound_name, std::span<const std::byte> frame,
                             const tlv_t& decoded) {
    const tlv_t* const dec = &decoded;     // keep the original body's `dec->...`/`*dec` shape
    if (dec->children.size() < 2) return;  // need at least op + dst
    const tlv_t& op_tlv = dec->children[0];
    const tlv_t& dst = dec->children[1];
    if (op_tlv.type != type_t::VALUE || op_tlv.payload.empty() || dst.type != type_t::PATH) return;
    const auto op = static_cast<fwd_op_t>(u8(op_tlv.payload[0]));

    // Resolve the FIRST dst segment against this node's transport children.
    transport_t* const child =
        dst.children.empty() ? nullptr : child_by_segment(dst.children[0].payload);

    if (child != nullptr) {
        // FORWARD: strip the leading dst segment; for a request also prepend the
        // inbound-link NAME to src (the way back); send onward. A REPLY does not
        // accumulate src (RFC-0004 §B).
        const bool is_reply = (op == fwd_op_t::REPLY);

        const auto fwd_h = read_header(frame, 0);
        if (!fwd_h || fwd_h->type != type_t::FWD) return;
        const std::size_t body_end = fwd_h->body_off + fwd_h->body_len;

        std::size_t cur = fwd_h->body_off;
        const auto op_h = read_header(frame, cur);
        if (!op_h) return;
        const std::size_t op_pos = cur;
        cur += op_h->total;

        const auto dst_h = read_header(frame, cur);
        if (!dst_h || dst_h->type != type_t::PATH) return;
        cur += dst_h->total;

        std::size_t sel_pos = 0;
        std::size_t sel_total = 0;
        if (cur < body_end) {
            const auto peek = read_header(frame, cur);
            if (peek && peek->type == type_t::FIELD) {
                sel_pos = cur;
                sel_total = peek->total;
                cur += peek->total;
            }
        }

        const auto src_h = read_header(frame, cur);
        if (!src_h || src_h->type != type_t::PATH) return;
        cur += src_h->total;

        const std::size_t tail_off = cur;
        const std::size_t tail_len = body_end > cur ? body_end - cur : 0;

        // The leading dst segment (a NAME) to strip.
        const auto seg_h = read_header(frame, dst_h->body_off);
        if (!seg_h || seg_h->type != type_t::NAME) return;
        const std::size_t rem_dst_off = dst_h->body_off + seg_h->total;
        const std::size_t rem_dst_len = dst_h->body_len - seg_h->total;

        // The NAME prepended to src — this node's name for the inbound link. Empty
        // for a REPLY (no accumulation).
        std::vector<std::byte> inbound_name_tlv;
        if (!is_reply) detail::emit_name(inbound_name_tlv, inbound_name);

        const std::size_t new_dst_body = rem_dst_len;
        const std::size_t new_src_body = src_h->body_len + inbound_name_tlv.size();
        const std::size_t new_dst_total = (new_dst_body > 0xFFFFu ? 6u : 4u) + new_dst_body;
        const std::size_t new_src_total = (new_src_body > 0xFFFFu ? 6u : 4u) + new_src_body;
        const std::size_t new_fwd_body =
            op_h->total + new_dst_total + sel_total + new_src_total + tail_len;

        // head1: FWD header + op (copied) + new (shrunk) dst header.
        std::vector<std::byte> head1;
        push_header(head1, type_t::FWD, new_fwd_body);
        head1.insert(head1.end(), frame.data() + op_pos, frame.data() + op_pos + op_h->total);
        push_header(head1, type_t::PATH, new_dst_body);

        // head2: new (grown) src header + the prepended inbound NAME (zero bytes for
        // a REPLY) — the rope head-insert ahead of the untouched original src bytes.
        std::vector<std::byte> head2;
        push_header(head2, type_t::PATH, new_src_body);
        head2.insert(head2.end(), inbound_name_tlv.begin(), inbound_name_tlv.end());

        // Scatter-gather egress: small fresh heads interleaved with views straight
        // into the inbound frame (remaining dst, selector, original src bytes,
        // payload) — no copy of the accumulated route or the payload.
        std::vector<std::span<const std::byte>> iov;
        iov.reserve(6);
        iov.emplace_back(head1);
        if (rem_dst_len > 0) iov.push_back(frame.subspan(rem_dst_off, rem_dst_len));
        if (sel_total > 0) iov.push_back(frame.subspan(sel_pos, sel_total));
        iov.emplace_back(head2);
        if (src_h->body_len > 0) iov.push_back(frame.subspan(src_h->body_off, src_h->body_len));
        if (tail_len > 0) iov.push_back(frame.subspan(tail_off, tail_len));
        child->send(std::span<const std::span<const std::byte>>(iov));
        return;
    }

    // TERMINUS (the first dst segment names no transport child).
    if (op == fwd_op_t::REPLY) {
        // The accumulated return route is fully consumed — this node is the
        // originator. Deliver to the reply sink.
        if (reply_cb_) reply_cb_(*dec);
        return;
    }

    // Local request terminus: apply the op and route the FWD{REPLY} back over the
    // link the request arrived on (its dst is the request's accumulated src).
    auto reply = resolver_.resolve(*dec);
    if (!reply) return;
    if (transport_t* in = link_by_name(inbound_name)) {
        const std::vector<std::span<const std::byte>> iov = reply->to_iovec();
        in->send(std::span<const std::span<const std::byte>>(iov));
    }
}

// --- route-handle (ws delivery-compaction, RFC-0004 §E.1) --------------------

void fwd_router_t::on_advertise(std::string_view inbound_name, const tlv_t& adv) {
    if (adv.children.size() < 2) return;
    const tlv_t& label_tlv = adv.children[0];
    const tlv_t& route = adv.children[1];
    if (label_tlv.type != type_t::VALUE || route.type != type_t::PATH) return;
    const auto label = detail::load_le<std::uint16_t>(label_tlv.payload);

    // Resolve the first route segment against this node's transport children — the
    // SAME rule the FWD forward step uses, so the label tracks the delivery route.
    transport_t* const down =
        route.children.empty() ? nullptr : child_by_segment(route.children[0].payload);

    if (down != nullptr) {
        // Forwarding hop: strip the leading segment, allocate OUR own out-label,
        // record the swap, retain the stripped egress route (for NACK re-advertise),
        // and re-advertise downstream with the new label (MPLS-style swap).
        const std::span<const std::byte> seg = route.children[0].payload;
        const std::string down_name(reinterpret_cast<const char*>(seg.data()), seg.size());
        tlv_t stripped = route;
        stripped.children.erase(stripped.children.begin());
        const std::vector<std::byte> stripped_bytes = wire::encode(stripped);

        const std::uint16_t out_label = handles_.alloc_label(down_name);
        handles_.bind_ingress(inbound_name, label,
                              handle_binding_t{.terminus = false,
                                               .down_link = down_name,
                                               .out_label = out_label,
                                               .local_route = {}});
        handles_.record_egress(down_name, out_label, stripped_bytes);
        const std::vector<std::byte> adv2 = encode_advertise(out_label, stripped_bytes);
        down->send(std::span<const std::byte>(adv2));
        return;
    }

    // Terminus: the route resolves locally here — bind the label to the local route.
    handles_.bind_ingress(
        inbound_name, label,
        handle_binding_t{
            .terminus = true, .down_link = {}, .out_label = 0, .local_route = wire::encode(route)});
}

void fwd_router_t::on_compact(std::string_view inbound_name, const tlv_t& comp) {
    if (comp.children.size() < 2 || comp.children[0].type != type_t::VALUE) return;
    const auto label = detail::load_le<std::uint16_t>(comp.children[0].payload);
    const tlv_t& payload = comp.children[1];

    const std::optional<handle_binding_t> binding = handles_.lookup_ingress(inbound_name, label);
    if (!binding) {
        // Stale/unknown label: drop, observe, and NACK back to prompt a re-advertise
        // (self-heal). Never a crash — the route is simply re-learned (RFC-0004 §E.1).
        if (stale_cb_) stale_cb_(inbound_name, label);
        if (transport_t* const up = link_by_name(inbound_name)) {
            const std::vector<std::byte> nack = encode_handle_nack(label);
            up->send(std::span<const std::byte>(nack));
        }
        return;
    }

    const std::vector<std::byte> payload_bytes = wire::encode(payload);
    if (binding->terminus) {
        // Local delivery — expand the label to the bound route and apply the write
        // (a delivery IS a write, RFC-0004 §D), then notify the delivery sink.
        if (deliver_local(binding->local_route, payload_bytes) && delivery_cb_)
            delivery_cb_(binding->local_route, payload_bytes);
        return;
    }
    // Forwarding hop: swap to our out-label and re-emit the COMPACT downstream — the
    // route still does NOT ride, only the (swapped) label.
    if (transport_t* const down = link_by_name(binding->down_link)) {
        const std::vector<std::byte> out = encode_compact(binding->out_label, payload_bytes);
        down->send(std::span<const std::byte>(out));
    }
}

void fwd_router_t::on_nack(std::string_view inbound_name, const tlv_t& nack) {
    if (nack.children.empty() || nack.children[0].type != type_t::VALUE) return;
    const auto label = detail::load_le<std::uint16_t>(nack.children[0].payload);
    // A downstream peer lost the binding for `label` on this link — re-advertise the
    // route we hold for it so the flow self-heals without a setup handshake.
    const std::optional<std::vector<std::byte>> route = handles_.egress_route(inbound_name, label);
    if (!route) return;
    if (transport_t* const link = link_by_name(inbound_name)) {
        const std::vector<std::byte> adv = encode_advertise(label, *route);
        link->send(std::span<const std::byte>(adv));
    }
}

bool fwd_router_t::deliver_local(std::span<const std::byte> route_path,
                                 std::span<const std::byte> payload) {
    const auto route = wire::decode(route_path);
    if (!route || route->type != type_t::PATH) return false;
    // The canonical PATH key (concatenated NAME encodings) — the graph vertex-map
    // key, mirroring op_resolve.cpp's path_tlv_key.
    std::vector<std::byte> key;
    for (const tlv_t& name : route->children) {
        const std::vector<std::byte> enc = wire::encode(name);
        key.insert(key.end(), enc.begin(), enc.end());
    }
    graph::vertex_t* const v = graph_.find(key);
    if (v == nullptr) return false;
    segment_ptr_t seg = view::heap_alloc(payload.size());
    if (!seg) return false;
    std::memcpy(seg->bytes.data(), payload.data(), payload.size());
    return graph_.write(v, view_t::over(std::move(seg))).has_value();
}

}  // namespace tr::net
