/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/fwd_router.hpp"

#include <array>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/grammar.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/path.hpp"
#include "libtracer/rope_decode.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tlv_view.hpp"
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

// Verify a lazy rope-tier frame to the same accept/reject decision the arena
// terminus reaches (ADR-0053 §4). The arena path flattens then `decode_into` with
// `crc_check_t::VERIFY`, checking EVERY CR-bearing TLV; the lazy `tlv_view_t`
// defers CRC, so the rope terminus verifies here — verify-all-then-apply, before
// the op mutates state. A forward recursive walk: `verify()` checks THIS TLV's own
// CRC trailer (trivial when absent, whole-body when present), and recursion reaches
// every CR-bearing descendant a root-only verify would miss. A child grammar error
// surfaces via `next()` (as a failed decode would); depth is capped at the shared
// `kMaxDepth` exactly as the eager decoders reject `TLV_NESTING_TOO_DEEP` (a node at
// `kMaxDepth` ancestors is rejected — tlv_arena.cpp / frame.cpp). Allocation-free.
[[nodiscard]] bool verify_view_tree(const wire::tlv_view_t& node, std::size_t depth) {
    if (depth >= wire::kMaxDepth) return false;  // parity with the decoders' cap
    if (!node.verify()) return false;            // this TLV's CRC trailer
    auto children = node.children();
    for (;;) {
        const std::expected<std::optional<wire::tlv_view_t>, wire::err_t> child = children.next();
        if (!child) return false;              // malformed child header ⇒ reject the frame
        if (!child->has_value()) return true;  // region cleanly exhausted
        if (!verify_view_tree(**child, depth + 1)) return false;
    }
}

// One top-level TLV header read in isolation (NO descent) — the byte offsets the
// zero-copy forward rebuild needs, kept as ABSOLUTE offsets into the source so the
// rebuild can re-slice src/payload as views (no copy). It is a thin ADAPTER over the
// ONE wire grammar (`grammar::parse_header`, ADR-0048 §1 / finding #7): the length math
// is no longer mirrored here — this only turns the grammar's relative `header_t` into the
// absolute `body_off = pos + header` the forward plane reads by. CRC is DEFERRED (the
// forward hop never walks a payload; the terminus / next hop verifies), matching the old
// reader's offset-only behavior. One deliberate difference: the grammar rejects a
// `type == 0x00` or reserved-opt-bit header up front, so a malformed frame is dropped at
// this hop instead of forwarded — every caller already rejected such a header by its type
// check, so well-formed traffic is byte-identical.
struct hdr_t {
    type_t type{};
    opt_t opt{};
    std::size_t header_len = 0;  // 4 (u16 length) or 6 (u32 length)
    std::size_t body_off = 0;    // absolute offset of the body within the source
    std::size_t body_len = 0;    // body (children/payload) length, trailer excluded
    std::size_t total = 0;       // header_len + body_len + trailer
};

// Templated over the grammar `Cursor` concept (ADR-0053 ④b): the forward plane reads
// its dispatch offsets through the SAME byte-source seam the one grammar validates
// through — `span_cursor` for the contiguous path (byte-identical to before), the rope
// cursor for a scatter-gather frame, with no per-cursor offset math. `cur.region(pos, …)`
// narrows either source in O(1) before the header parse.
template <class Cursor>
[[nodiscard]] std::optional<hdr_t> read_header(const Cursor& cur, std::size_t pos) {
    if (pos > cur.size()) return std::nullopt;
    const auto h = wire::grammar::parse_header(cur.region(pos, cur.size() - pos),
                                               wire::grammar::crc_check_t::DEFER);
    if (!h) return std::nullopt;
    return hdr_t{.type = h->type,
                 .opt = h->opt,
                 .header_len = h->header,
                 .body_off = pos + h->header,
                 .body_len = h->length,
                 .total = h->total};
}

// The forward dispatch decision, read by OFFSET with no allocation (ADR-0038 inv. #1,
// ADR-0039): a FWD whose first `dst` segment names a transport child is a forward hop
// that never needs the decoded tree. Returns the `[body_off, body_len)` of the first
// dst-segment NAME iff the frame is a structured FWD with an op VALUE + a non-empty dst
// PATH; nullopt otherwise (malformed, non-FWD, or empty dst ⇒ fall back to the
// full-decode terminus path). Offsets, not a span, so the result is source-agnostic —
// the caller re-slices the segment bytes from its own cursor (contiguous or rope).
template <class Cursor>
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> peek_fwd_first_dst_seg(
    const Cursor& cur) {
    const auto fwd_h = read_header(cur, 0);
    if (!fwd_h || fwd_h->type != type_t::FWD || !fwd_h->opt.pl) return std::nullopt;
    const std::size_t body_end = fwd_h->body_off + fwd_h->body_len;
    // child[0] = op VALUE
    const auto op_h = read_header(cur, fwd_h->body_off);
    if (!op_h || op_h->type != type_t::VALUE) return std::nullopt;
    // child[1] = dst PATH
    const std::size_t dst_pos = fwd_h->body_off + op_h->total;
    if (dst_pos >= body_end) return std::nullopt;
    const auto dst_h = read_header(cur, dst_pos);
    if (!dst_h || dst_h->type != type_t::PATH || dst_h->body_len == 0) return std::nullopt;
    // dst.child[0] = first segment NAME
    const auto seg_h = read_header(cur, dst_h->body_off);
    if (!seg_h || seg_h->type != type_t::NAME) return std::nullopt;
    return std::pair{seg_h->body_off, seg_h->body_len};
}

// Read the FWD op discriminant (child[0], a VALUE u8) by OFFSET — the terminus
// split (REPLY → originator sink vs request → arena resolve) without a decode.
template <class Cursor>
[[nodiscard]] std::optional<graph::fwd_op_t> peek_fwd_op(const Cursor& cur) {
    const auto fwd_h = read_header(cur, 0);
    if (!fwd_h || fwd_h->type != type_t::FWD || !fwd_h->opt.pl) return std::nullopt;
    const auto op_h = read_header(cur, fwd_h->body_off);
    if (!op_h || op_h->type != type_t::VALUE || op_h->body_len == 0) return std::nullopt;
    return static_cast<fwd_op_t>(cur.byte_at(op_h->body_off));
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
    // Capability-matched receiver (ADR-0042 §1 / ADR-0044): a BUS link delivers
    // frames tagged with the SENDING peer's name, which becomes the hop's inbound
    // NAME — so the `src` grown on a forward (and the link a terminus reply goes
    // back over) names the bus PEER, and the registry's peer fallback turns that
    // name into a directed send. An owning-delivery link funnels through the same
    // routing with the frame view alongside; a span link keeps the borrowed-span
    // path. No adapter wraps a span into a lying view.
    if (bus_link_t* const bus = link.bus()) {
        // A reassembling bus that delivers ropes (ADR-0053 §5) hands its group up
        // as-is — zero-copy; a span-only bus keeps the borrowed peer-named path.
        if (bus->delivers_ropes()) {
            bus->set_peer_rope_receiver([this](std::string_view peer, view::rope_t frame) {
                on_frame_rope(peer, std::move(frame));
            });
        } else {
            bus->set_peer_receiver([this](std::string_view peer, std::span<const std::byte> frame) {
                on_frame(peer, frame);
            });
        }
    } else if (link.delivers_ropes()) {
        link.set_rope_receiver(
            [this, name](view::rope_t frame) { on_frame_rope(name, std::move(frame)); });
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

void fwd_router_t::on_frame_rope(std::string_view inbound_name, view::rope_t frame) {
    // Single-link (every current producer): the link's bytes span feeds the SAME
    // routing as the borrowed path — the forward hop below never touches the
    // refcount (zero-heap, ADR-0038); only the terminus sees the owner, for the
    // ADR-0042 §3 referenced store. This is the pre-ADR-0053 view path, unchanged.
    if (frame.link_count() == 1) {
        const view_t& v = frame.links()[0];
        if (v.is_device()) return;  // CPU routing cannot read a DEVICE frame
        on_frame_impl(inbound_name, v.bytes(), &v);
        return;
    }
    // Multi-link: route a FORWARD hop directly over the rope — NO flatten
    // (ADR-0053 ④b). The forward-vs-terminus split is read through the link-walking
    // grammar cursor and the egress scatter-gathers the untouched links; a header or
    // trailer straddling a link boundary is stitched by the cursor (grammar.hpp). Only
    // a terminus / reply / control frame — which still needs a contiguous decode (the
    // rope-aware sink is the migration ⑤/⑥ follow-on) — pays the one flatten fallback.
    // A device link cannot be CPU-read, so a non-all-host rope skips straight to the
    // fallback (flatten drops it, as before).
    if (frame.total_length() >= 4 && frame.all_host()) {
        const wire::grammar::rope_cursor cur{frame};
        if (const auto seg = peek_fwd_first_dst_seg(cur)) {
            // Resolve the first dst segment NAME. A routable segment is bounded
            // (≤ kMaxSegmentBytes), so it is stitched into a small stack scratch —
            // contiguous when it lies in one link, a byte-at-a-time copy across a link
            // boundary; an over-long "segment" is not routable ⇒ fall to the terminus.
            const auto [seg_off, seg_len] = *seg;
            if (seg_len > 0 && seg_len <= graph::kMaxSegmentBytes) {
                std::array<std::byte, graph::kMaxSegmentBytes> scratch;
                std::size_t w = 0;
                cur.for_each_span(seg_off, seg_len, [&](std::span<const std::byte> s) {
                    for (std::byte b : s) scratch[w++] = b;
                });
                if (transport_t* const child =
                        registry_.by_segment(std::span<const std::byte>(scratch.data(), seg_len))) {
                    route_fwd_forward(inbound_name, cur, *child);
                    return;
                }
            }
            // No child (or over-long segment) ⇒ this node is the terminus for the frame.
            // A request FWD is resolved straight off the rope (ADR-0053 3c-iii — NO
            // flatten, verify-at-access §4). A REPLY that reaches its originator here
            // still takes the flatten fallback: the reply sink (`reply_cb_`) is handed
            // the decoded tree, and that rope-aware sink is the ⑤/⑥ follow-on.
            if (peek_fwd_op(cur) != fwd_op_t::REPLY) {
                resolve_terminus_rope(inbound_name, std::move(frame));
                return;
            }
        }
    }
    // Terminus REPLY / control (or a device/short rope): the contiguous decode path
    // still needs a flat frame — the documented ADR-0053 interim flatten, deleted when
    // the rope-aware reply/control sinks land (migration ⑤/⑥). An empty flat ⇒ dropped.
    const view_t flat = frame.flatten();
    if (flat.empty()) return;
    on_frame_impl(inbound_name, flat.bytes(), &flat);
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
        const wire::grammar::span_cursor cur{frame};
        if (const auto seg = peek_fwd_first_dst_seg(cur)) {
            if (transport_t* const child =
                    registry_.by_segment(frame.subspan(seg->first, seg->second))) {
                route_fwd_forward(inbound_name, cur, *child);
                return;
            }
            // First dst segment names no child ⇒ this node is the terminus.
        }
        if (peek_fwd_op(cur) == fwd_op_t::REPLY) {
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

template <class Cursor>
void fwd_router_t::route_fwd_forward(std::string_view inbound_name, const Cursor& cur_src,
                                     transport_t& child) {
    // All offsets, no decoded tree. Layout: FWD{ op VALUE, dst PATH, FIELD? sel, src
    // PATH, tail } — strip dst's leading segment, grow src by the inbound NAME (unless
    // REPLY), scatter-gather the fresh heads + the untouched inbound regions onward.
    // Reads AND the egress go through the grammar `Cursor` seam (ADR-0053 ④b): the same
    // code serves a contiguous `span_cursor` (each region is one sub-span) and a
    // link-walking `rope_cursor` (a region yields one sub-span per link it crosses).
    const auto fwd_h = read_header(cur_src, 0);
    if (!fwd_h || fwd_h->type != type_t::FWD) return;
    const std::size_t body_end = fwd_h->body_off + fwd_h->body_len;

    std::size_t cur = fwd_h->body_off;
    const auto op_h = read_header(cur_src, cur);
    if (!op_h || op_h->type != type_t::VALUE || op_h->body_len == 0) return;
    const std::size_t op_pos = cur;
    const bool is_reply = static_cast<fwd_op_t>(cur_src.byte_at(op_h->body_off)) == fwd_op_t::REPLY;
    cur += op_h->total;

    const auto dst_h = read_header(cur_src, cur);
    if (!dst_h || dst_h->type != type_t::PATH) return;
    cur += dst_h->total;

    std::size_t sel_pos = 0;
    std::size_t sel_total = 0;
    if (cur < body_end) {
        const auto peek = read_header(cur_src, cur);
        if (peek && peek->type == type_t::FIELD) {
            sel_pos = cur;
            sel_total = peek->total;
            cur += peek->total;
        }
    }

    const auto src_h = read_header(cur_src, cur);
    if (!src_h || src_h->type != type_t::PATH) return;
    cur += src_h->total;

    const std::size_t tail_off = cur;
    const std::size_t tail_len = body_end > cur ? body_end - cur : 0;

    // The leading dst segment (a NAME) to strip.
    const auto seg_h = read_header(cur_src, dst_h->body_off);
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
    cur_src.for_each_span(op_pos, op_h->total, [&](std::span<const std::byte> s) { head1.raw(s); });
    head1.header(type_t::PATH, new_dst_body);

    stack_writer<96> head2;
    head2.header(type_t::PATH, new_src_body);
    if (!is_reply) head2.name(inbound_name);

    if (!head1.ok() || !head2.ok()) return;  // malformed oversized op ⇒ drop, no overrun

    // Scatter-gather egress: the small stack heads interleaved with the untouched inbound
    // regions (remaining dst, selector, original src bytes, payload) — no payload copy. The
    // gather is written ONCE over the cursor seam; each region is emitted via
    // `for_each_span`, which yields exactly one sub-span for a contiguous source and one per
    // straddled link for a rope. So the container is the only thing that varies: a stack
    // `std::array` for the span path (ZERO heap, ADR-0038 inv. #2) vs a `std::pmr::vector`
    // over the injected @ref mr_ for the rope path (a link count is only known at run time).
    const auto gather = [&](auto&& push) {
        push(head1.span());
        if (rem_dst_len > 0) cur_src.for_each_span(rem_dst_off, rem_dst_len, push);
        if (sel_total > 0) cur_src.for_each_span(sel_pos, sel_total, push);
        push(head2.span());
        if (src_h->body_len > 0) cur_src.for_each_span(src_h->body_off, src_h->body_len, push);
        if (tail_len > 0) cur_src.for_each_span(tail_off, tail_len, push);
    };

    if constexpr (std::is_same_v<Cursor, wire::grammar::span_cursor>) {
        // Contiguous source: at most 6 regions, each a single sub-span — a stack array.
        std::array<std::span<const std::byte>, 6> iov;
        std::size_t n = 0;
        gather([&](std::span<const std::byte> s) { iov[n++] = s; });
        child.send(std::span<const std::span<const std::byte>>(iov.data(), n));
    } else {
        // Rope source: a region may cross several links — gather into a pmr vector drawn
        // from the terminus arena's resource (the forward hop still copies no payload).
        std::pmr::vector<std::span<const std::byte>> iov{mr_};
        gather([&](std::span<const std::byte> s) { iov.push_back(s); });
        child.send(std::span<const std::span<const std::byte>>(iov.data(), iov.size()));
    }
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

void fwd_router_t::resolve_terminus_rope(std::string_view inbound_name, view::rope_t frame) {
    // ADR-0053 3c-iii: the multi-link request terminus, resolved straight off the
    // rope — the interim flatten is gone. Adopt the frame as a lazy view (bounds
    // anchored, CRC deferred), then verify integrity to the arena terminus's
    // accept/reject (verify-all-then-apply, §4) before the op mutates state.
    const auto view = wire::tlv_view_t::over(std::move(frame));
    if (!view) return;                        // malformed root ⇒ drop (as a decode error)
    if (!verify_view_tree(*view, 0)) return;  // CRC / grammar / depth failure ⇒ drop
    // The rope tier stores its one ownership copy (own_tlv) — the ADR-0042 §3
    // referenced store needs a contiguous frame view, absent for a scatter-gather
    // rope, so no frame_view is threaded. Reply routes back over the inbound link,
    // its dst the request's accumulated src, exactly as the arena terminus.
    auto reply = resolver_.resolve(*view, inbound_name, nullptr);
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
    // `payload` is a wire-encoded TLV (never empty); `nullopt` is exactly an alloc
    // failure → drop the delivery (one audited alloc/copy/over locus).
    const auto payload_view = view::over_bytes(payload);
    if (!payload_view) return false;
    return graph_.write(v, *payload_view).has_value();
}

void fwd_router_t::deliver_remote(const graph::remote_delivery_t& sub, const view::rope_t& value) {
    transport_t* const link = registry_.by_name(sub.link);
    if (link == nullptr) return;  // link torn down between subscribe and this write
    const std::span<const std::byte> route = sub.return_route.bytes();  // the stored PATH TLV

    if (sub.delivery_compact) {
        // Auto-promote (Q5 / RFC-0004 §E.1): advertise the label once per flow, then stream
        // lean COMPACT. ensure_egress is idempotent per (link,route); clear_link on a
        // reconnect drops the binding so the next delivery re-advertises (self-heal).
        // A COMPACT wraps a CONTIGUOUS payload (encode_compact), so a multi-link value pays
        // one flatten here — single-link, the common case, is a zero-copy adopt. The
        // scatter-gather win is the default full-route path below (the hot fan-out leg).
        const view_t flat = value.materialize();
        const auto [label, fresh] = handles_.ensure_egress(sub.link, route);
        if (fresh) {
            const std::vector<std::byte> adv = encode_advertise(label, route);
            link->send(std::span<const std::byte>(adv));
        }
        const std::vector<std::byte> out = encode_compact(label, flat.bytes());
        link->send(std::span<const std::byte>(out));
        return;
    }
    // Default: full-route `FWD{ op=WRITE, dst=<return route>, src=<empty PATH>,
    // payload=<VALUE> }` (delivery-is-a-write, RFC-0004 §D / #136), scatter-gathered over
    // the stored value's rope links (ADR-0053 ⑤): a fresh stack head + the ROPED stored
    // route + each value segment, NO flatten. The route bytes were copied ONCE at subscribe
    // (ADR-0041 §2); a delivery copies nothing — a multi-link value crosses as its own
    // segments. src starts empty — each forwarding hop grows it (the way back). The
    // refcounted route view (`sub.return_route`) stays alive for this call even if the slot
    // is concurrently unsubscribed.
    constexpr std::array<std::byte, 5> op_tlv{std::byte{0x01}, std::byte{0x00}, std::byte{0x01},
                                              std::byte{0x00},
                                              std::byte{std::to_underlying(fwd_op_t::WRITE)}};
    constexpr std::array<std::byte, 4> empty_src{std::byte{0x06}, std::byte{0x40}, std::byte{0x00},
                                                 std::byte{0x00}};
    const std::size_t body_len =
        op_tlv.size() + route.size() + empty_src.size() + value.total_length();
    stack_writer<16> head;  // FWD header (≤6) + the 5-byte op TLV
    head.header(type_t::FWD, body_len);
    head.raw(op_tlv);
    if (!head.ok()) return;

    // iov = head + route + empty_src + one span per value link (sized to the rope, no
    // synthetic cap — the same per-send iov vector the rope terminus reply builds).
    std::vector<std::span<const std::byte>> iov;
    iov.reserve(3 + value.link_count());
    iov.push_back(head.span());
    iov.push_back(route);
    iov.push_back(std::span<const std::byte>(empty_src));
    for (const view_t& l : value.links()) iov.push_back(l.bytes());
    link->send(std::span<const std::span<const std::byte>>(iov));
}

}  // namespace tr::net
