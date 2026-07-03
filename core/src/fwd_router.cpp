/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/fwd_router.hpp"

#include <array>
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

// The forward dispatch decision, read by OFFSET with no allocation (ADR-0038 inv. #1,
// ADR-0039): a FWD whose first `dst` segment names a transport child is a forward hop
// that never needs the decoded tree. Returns the first-dst-segment NAME bytes iff the
// frame is a structured FWD with an op VALUE + a non-empty dst PATH; nullopt otherwise
// (malformed, non-FWD, or empty dst ⇒ fall back to the full-decode terminus path).
[[nodiscard]] std::optional<std::span<const std::byte>> peek_fwd_first_dst_seg(
    std::span<const std::byte> frame) {
    const auto fwd_h = read_header(frame, 0);
    if (!fwd_h || fwd_h->type != type_t::FWD || !fwd_h->opt.pl) return std::nullopt;
    const std::size_t body_end = fwd_h->body_off + fwd_h->body_len;
    // child[0] = op VALUE
    const auto op_h = read_header(frame, fwd_h->body_off);
    if (!op_h || op_h->type != type_t::VALUE) return std::nullopt;
    // child[1] = dst PATH
    const std::size_t dst_pos = fwd_h->body_off + op_h->total;
    if (dst_pos >= body_end) return std::nullopt;
    const auto dst_h = read_header(frame, dst_pos);
    if (!dst_h || dst_h->type != type_t::PATH || dst_h->body_len == 0) return std::nullopt;
    // dst.child[0] = first segment NAME
    const auto seg_h = read_header(frame, dst_h->body_off);
    if (!seg_h || seg_h->type != type_t::NAME) return std::nullopt;
    return frame.subspan(seg_h->body_off, seg_h->body_len);
}

// Read the FWD op discriminant (child[0], a VALUE u8) by OFFSET — the terminus
// split (REPLY → originator sink vs request → arena resolve) without a decode.
[[nodiscard]] std::optional<graph::fwd_op_t> peek_fwd_op(std::span<const std::byte> frame) {
    const auto fwd_h = read_header(frame, 0);
    if (!fwd_h || fwd_h->type != type_t::FWD || !fwd_h->opt.pl) return std::nullopt;
    const auto op_h = read_header(frame, fwd_h->body_off);
    if (!op_h || op_h->type != type_t::VALUE || op_h->body_len == 0) return std::nullopt;
    return static_cast<fwd_op_t>(u8(frame[op_h->body_off]));
}

// A fixed-capacity stack byte-writer — the zero-heap counterpart of the old
// vector-based header builder for the forward hop (ADR-0038 inv. #2: "the fresh header bytes ... a
// stack std::array, not a std::vector"). Bounded by the wire header widths + one NAME
// (kMaxSegmentBytes), so `N` is a small compile-time constant; a write past capacity
// clamps to empty (the caller treats an empty head as a drop — never a buffer overrun).
template <std::size_t N>
class stack_writer {
   public:
    void header(type_t type, std::size_t body_len) {
        opt_t opt{.pl = true};
        if (body_len > 0xFFFFu) opt.ll = true;
        const std::size_t width = opt.ll ? 4u : 2u;
        if (len_ + 2 + width > N) {
            overflow_ = true;
            return;
        }
        buf_[len_++] = static_cast<std::byte>(std::to_underlying(type));
        buf_[len_++] = static_cast<std::byte>(opt.encode());
        for (std::size_t i = 0; i < width; ++i)
            buf_[len_++] = static_cast<std::byte>((body_len >> (8 * i)) & 0xFF);
    }
    void name(std::string_view s) {  // a NAME TLV over `s` (type, opt=0, u16 len, bytes)
        if (len_ + 4 + s.size() > N || s.size() > 0xFFFFu) {
            overflow_ = true;
            return;
        }
        buf_[len_++] = static_cast<std::byte>(std::to_underlying(type_t::NAME));
        buf_[len_++] = std::byte{0};
        buf_[len_++] = static_cast<std::byte>(s.size() & 0xFF);
        buf_[len_++] = static_cast<std::byte>((s.size() >> 8) & 0xFF);
        for (char c : s) buf_[len_++] = static_cast<std::byte>(c);
    }
    void raw(std::span<const std::byte> bytes) {  // copy opaque bytes (the op TLV)
        if (len_ + bytes.size() > N) {
            overflow_ = true;
            return;
        }
        for (std::byte b : bytes) buf_[len_++] = b;
    }
    [[nodiscard]] std::span<const std::byte> span() const {
        return overflow_ ? std::span<const std::byte>{}
                         : std::span<const std::byte>(buf_.data(), len_);
    }
    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

   private:
    std::array<std::byte, N> buf_{};
    std::size_t len_ = 0;
    bool overflow_ = false;
};

}  // namespace

void fwd_router_t::add_child(std::string name, transport_t& link) {
    // Populate the registry BEFORE wiring the receiver: an async transport (UDP/ws) may
    // already have a live recv thread, so `set_receiver` is the publish point — once the
    // callback is installed, on_frame can read the registry on that thread. Adding the
    // child first ensures the entry is visible before any inbound frame can resolve it
    // (the set_receiver mutex provides the release/acquire fence). Registry is otherwise
    // immutable after setup — no lock on the read hot path.
    registry_.add(name, link);
    // Capability-matched receiver (ADR-0042 §1): an owning-delivery link funnels
    // through the same routing with the frame view alongside; a span link keeps the
    // borrowed-span path. No adapter wraps a span into a lying view.
    if (link.delivers_views()) {
        link.set_view_receiver(
            [this, name](view_t frame) { on_frame_view(name, std::move(frame)); });
    } else {
        link.set_receiver(
            [this, name](std::span<const std::byte> frame) { on_frame(name, frame); });
    }
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
    transport_t* const link = registry_.by_name(link_name);
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
    if (transport_t* const link = registry_.by_name(link_name)) {
        const std::vector<std::byte> out = encode_compact(label, payload);
        link->send(std::span<const std::byte>(out));
    }
}

void fwd_router_t::on_frame(std::string_view inbound_name, std::span<const std::byte> frame) {
    on_frame_impl(inbound_name, frame, nullptr);
}

void fwd_router_t::on_frame_view(std::string_view inbound_name, view_t frame) {
    // The owning view's bytes span feeds the SAME routing as the borrowed path —
    // the forward hop below never touches the refcount (zero-heap, ADR-0038); only
    // the terminus sees the owner, for the ADR-0042 §3 referenced store.
    on_frame_impl(inbound_name, frame.bytes(), &frame);
}

void fwd_router_t::on_frame_impl(std::string_view inbound_name, std::span<const std::byte> frame,
                                 const view_t* frame_view) {
    if (raw_cb_) raw_cb_(inbound_name, frame);
    if (frame.size() < 4) return;

    // The FWD plane never builds a tlv_t (ADR-0038 inv. #1 / ADR-0041 §5): the
    // forward-vs-terminus split and the op discriminant are read by OFFSET; a
    // forward hop scatter-gathers with zero heap; a terminus request decodes into
    // the pmr arena. Only the originator REPLY sink and the control frames below
    // keep the owning wire::decode (test/SDK-facing and flow-setup paths, allowed
    // to allocate per ADR-0039).
    if (static_cast<type_t>(u8(frame[0])) == type_t::FWD) {
        if (inbound_cb_) {  // read-only observer (tests/ACL seam) — wants the tree
            if (const auto dec = wire::decode(frame); dec && dec->opt.pl)
                inbound_cb_(inbound_name, *dec);
        }
        if (const auto seg = peek_fwd_first_dst_seg(frame)) {
            if (transport_t* const child = registry_.by_segment(*seg)) {
                route_fwd_forward(inbound_name, frame, *child);
                return;
            }
            // First dst segment names no child ⇒ this node is the terminus.
        }
        if (peek_fwd_op(frame) == fwd_op_t::REPLY) {
            // The accumulated return route is fully consumed — this node is the
            // originator. Deliver the decoded FWD{REPLY} to the reply sink.
            if (reply_cb_) {
                if (const auto dec = wire::decode(frame); dec && dec->opt.pl) reply_cb_(*dec);
            }
            return;
        }
        resolve_terminus(inbound_name, frame, frame_view);
        return;
    }

    // Control frames (route-handle flow setup) keep the owning decode.
    const auto dec = wire::decode(frame);
    if (!dec || !dec->opt.pl) return;  // drop malformed / non-structured
    switch (dec->type) {
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

void fwd_router_t::route_fwd_forward(std::string_view inbound_name,
                                     std::span<const std::byte> frame, transport_t& child) {
    // All offsets, no decoded tree. Layout: FWD{ op VALUE, dst PATH, FIELD? sel, src
    // PATH, tail } — strip dst's leading segment, grow src by the inbound NAME (unless
    // REPLY), scatter-gather the fresh heads + views into the untouched inbound frame.
    const auto fwd_h = read_header(frame, 0);
    if (!fwd_h || fwd_h->type != type_t::FWD) return;
    const std::size_t body_end = fwd_h->body_off + fwd_h->body_len;

    std::size_t cur = fwd_h->body_off;
    const auto op_h = read_header(frame, cur);
    if (!op_h || op_h->type != type_t::VALUE || op_h->body_len == 0) return;
    const std::size_t op_pos = cur;
    const bool is_reply = static_cast<fwd_op_t>(u8(frame[op_h->body_off])) == fwd_op_t::REPLY;
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

    // The inbound NAME appended to src (grow) — empty for a REPLY (no accumulation).
    const std::size_t inbound_name_len = is_reply ? 0u : (4u + inbound_name.size());

    const std::size_t new_dst_body = rem_dst_len;
    const std::size_t new_src_body = src_h->body_len + inbound_name_len;
    const std::size_t new_dst_total = (new_dst_body > 0xFFFFu ? 6u : 4u) + new_dst_body;
    const std::size_t new_src_total = (new_src_body > 0xFFFFu ? 6u : 4u) + new_src_body;
    const std::size_t new_fwd_body =
        op_h->total + new_dst_total + sel_total + new_src_total + tail_len;

    // head1: FWD header + op (copied) + new (shrunk) dst header. head2: new (grown) src
    // header + the prepended inbound NAME. Both fixed stack buffers — ZERO heap on the
    // forward hop (ADR-0038 inv. #2). Bounds: head1 = FWD hdr(≤6) + op TLV(small) + PATH
    // hdr(≤6); head2 = PATH hdr(≤6) + one NAME(≤4+kMaxSegmentBytes). An overflow (a
    // malformed op TLV larger than the buffer) yields an empty span ⇒ drop, never a
    // buffer overrun.
    stack_writer<64> head1;
    head1.header(type_t::FWD, new_fwd_body);
    head1.raw(frame.subspan(op_pos, op_h->total));
    head1.header(type_t::PATH, new_dst_body);

    stack_writer<96> head2;
    head2.header(type_t::PATH, new_src_body);
    if (!is_reply) head2.name(inbound_name);

    if (!head1.ok() || !head2.ok()) return;  // malformed oversized op ⇒ drop, no overrun

    // Scatter-gather egress: small stack heads interleaved with views straight into the
    // inbound frame (remaining dst, selector, original src bytes, payload) — no copy, no
    // heap. `iov` is a stack std::array, not a std::vector (ADR-0038 inv. #2).
    std::array<std::span<const std::byte>, 6> iov;
    std::size_t n = 0;
    iov[n++] = head1.span();
    if (rem_dst_len > 0) iov[n++] = frame.subspan(rem_dst_off, rem_dst_len);
    if (sel_total > 0) iov[n++] = frame.subspan(sel_pos, sel_total);
    iov[n++] = head2.span();
    if (src_h->body_len > 0) iov[n++] = frame.subspan(src_h->body_off, src_h->body_len);
    if (tail_len > 0) iov[n++] = frame.subspan(tail_off, tail_len);
    child.send(std::span<const std::span<const std::byte>>(iov.data(), n));
}

void fwd_router_t::resolve_terminus(std::string_view inbound_name, std::span<const std::byte> frame,
                                    const view_t* frame_view) {
    // Local request terminus (ADR-0041 §5): arena-decode straight from the
    // node's injected resource (ADR-0039 §1) — the library keeps no buffer of
    // its own; a bounded host injects a pool resource over its slab and the
    // terminus allocates nothing from the global heap. The arena is released
    // before this call returns. Apply the op and route the FWD{REPLY} back over
    // the link the request arrived on (its dst is the request's accumulated
    // src). The inbound link makes a `:subscribers[]` WRITE bind a REMOTE
    // subscriber whose deliveries route back over it (#136); the latch
    // (transient-local) fires inside resolve.
    const auto arena = wire::decode_into(frame, *mr_);
    if (!arena) return;  // malformed frame ⇒ drop
    auto reply = resolver_.resolve(*arena, inbound_name, frame_view);
    if (!reply) return;  // structurally non-request ⇒ drop
    if (transport_t* in = registry_.by_name(inbound_name)) {
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
        route.children.empty() ? nullptr : registry_.by_segment(route.children[0].payload);

    if (down != nullptr) {
        // Forwarding hop: strip the leading segment, allocate OUR own out-label,
        // record the swap, retain the stripped egress route (for NACK re-advertise),
        // and re-advertise downstream with the new label (MPLS-style swap).
        const std::span<const std::byte> seg = route.children[0].payload;
        const std::string down_name(detail::as_string_view(seg));
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
        if (transport_t* const up = registry_.by_name(inbound_name)) {
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
    if (transport_t* const down = registry_.by_name(binding->down_link)) {
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
    if (transport_t* const link = registry_.by_name(inbound_name)) {
        const std::vector<std::byte> adv = encode_advertise(label, *route);
        link->send(std::span<const std::byte>(adv));
    }
}

bool fwd_router_t::deliver_local(std::span<const std::byte> route_path,
                                 std::span<const std::byte> payload) {
    const auto route = wire::decode(route_path);
    if (!route || route->type != type_t::PATH) return false;
    // The canonical PATH key (concatenated NAME encodings) — the graph vertex-map key.
    graph::vertex_t* const v = graph_.find(wire::path_key(*route));
    if (v == nullptr) return false;
    // `payload` is a wire-encoded TLV (never empty); an empty result is exactly
    // an alloc failure → drop the delivery (one audited alloc/copy/over locus).
    const view_t payload_view = view::over_bytes(payload);
    if (payload_view.empty()) return false;
    return graph_.write(v, payload_view).has_value();
}

void fwd_router_t::deliver_remote(const graph::remote_delivery_t& sub, const view_t& value) {
    transport_t* const link = registry_.by_name(sub.link);
    if (link == nullptr) return;  // link torn down between subscribe and this write
    const std::span<const std::byte> payload = value.bytes();  // the stored VALUE TLV bytes
    const std::span<const std::byte> route = sub.return_route.bytes();  // the stored PATH TLV

    if (sub.delivery_compact) {
        // Auto-promote (Q5 / RFC-0004 §E.1): advertise the label once per flow, then stream
        // lean COMPACT. ensure_egress is idempotent per (link,route); clear_link on a
        // reconnect drops the binding so the next delivery re-advertises (self-heal).
        const auto [label, fresh] = handles_.ensure_egress(sub.link, route);
        if (fresh) {
            const std::vector<std::byte> adv = encode_advertise(label, route);
            link->send(std::span<const std::byte>(adv));
        }
        const std::vector<std::byte> out = encode_compact(label, payload);
        link->send(std::span<const std::byte>(out));
        return;
    }
    // Default: full-route `FWD{ op=WRITE, dst=<return route>, src=<empty PATH>,
    // payload=<VALUE> }` (delivery-is-a-write, RFC-0004 §D / #136), scatter-gathered:
    // fresh stack heads + the ROPED stored route and value views — the route bytes
    // were copied ONCE at subscribe (ADR-0041 §2); a delivery copies nothing. src
    // starts empty — each forwarding hop grows it (the way back). The refcounted
    // route view (`sub.return_route`) stays alive for this call even if the slot is
    // concurrently unsubscribed.
    constexpr std::array<std::byte, 5> op_tlv{std::byte{0x01}, std::byte{0x00}, std::byte{0x01},
                                              std::byte{0x00},
                                              std::byte{std::to_underlying(fwd_op_t::WRITE)}};
    constexpr std::array<std::byte, 4> empty_src{std::byte{0x06}, std::byte{0x40}, std::byte{0x00},
                                                 std::byte{0x00}};
    const std::size_t body_len = op_tlv.size() + route.size() + empty_src.size() + payload.size();
    stack_writer<16> head;  // FWD header (≤6) + the 5-byte op TLV
    head.header(type_t::FWD, body_len);
    head.raw(op_tlv);
    if (!head.ok()) return;

    std::array<std::span<const std::byte>, 4> iov{head.span(), route,
                                                  std::span<const std::byte>(empty_src), payload};
    link->send(std::span<const std::span<const std::byte>>(iov));
}

}  // namespace tr::net
