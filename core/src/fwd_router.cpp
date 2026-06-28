/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/fwd_router.hpp"

#include <cstring>
#include <optional>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::net {

using graph::fwd_op_t;
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
    // Decode for the routing decision, the observer, and (at a terminus) the
    // op_resolver / reply sink. The zero-copy forward rebuild re-walks the same
    // bytes for offsets (read_header) rather than re-encoding the decoded tree.
    const auto dec = wire::decode(frame);
    if (!dec || dec->type != type_t::FWD || !dec->opt.pl) return;  // drop malformed / non-FWD
    if (inbound_cb_) inbound_cb_(inbound_name, *dec);

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

}  // namespace tr::net
