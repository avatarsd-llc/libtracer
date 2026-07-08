/**
 * @file
 * @brief fwd_frame_view unit test — drives the FWD offset-dispatch cluster directly (no router, no
 *        transports), the point of extracting it from fwd_router.cpp (the length_prefix_framer
 *        precedent): first-dst-seg / op / control peeks over BOTH cursors (contiguous span +
 *        adversarially split rope), the shrunk-dst / grown-src head rebuild proved BYTE-EXACT
 *        against a reference re-encode, stack_writer clamp-to-empty overflow, and malformed
 *        rejects.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#include "libtracer/fwd_frame_view.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/rope_decode.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/view.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::wire::opt_t;
using tr::wire::type_t;
using tr::wire::grammar::rope_cursor;
using tr::wire::grammar::span_cursor;

int g_failures = 0;

/** @brief Print + tally one check. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

using bytes_t = std::vector<std::byte>;

/** @brief One NAME TLV over @p s (canonical bytes via the production emitter). */
bytes_t b_name(std::string_view s) {
    bytes_t out;
    tr::wire::emit_name(out, s);
    return out;
}

/** @brief One structured PATH TLV whose children are the given segment NAMEs. */
bytes_t b_path(std::initializer_list<std::string_view> segs) {
    bytes_t body;
    for (std::string_view s : segs) {
        const bytes_t n = b_name(s);
        body.insert(body.end(), n.begin(), n.end());
    }
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief One opaque VALUE TLV holding a LE u32. */
bytes_t b_value_u32(std::uint32_t v) {
    bytes_t p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

/** @brief Append @p src to @p dst. */
void append(bytes_t& dst, const bytes_t& src) { dst.insert(dst.end(), src.begin(), src.end()); }

/** @brief The 5-byte op VALUE TLV. */
bytes_t b_op(fwd_op_t op) {
    bytes_t out;
    const std::byte ob{static_cast<std::uint8_t>(op)};
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&ob, 1));
    return out;
}

/** @brief A canonical FWD frame: `FWD{ op, dst, sel?, src, payload? }`. */
bytes_t b_fwd(fwd_op_t op, const bytes_t& dst, const bytes_t& src, const bytes_t& payload = {},
              const bytes_t& sel = {}) {
    bytes_t body = b_op(op);
    append(body, dst);
    if (!sel.empty()) append(body, sel);
    append(body, src);
    if (!payload.empty()) append(body, payload);
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/** @brief An owned single-link view holding a copy of @p bytes. */
tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief A rope over @p bytes split at the given cut points (each cut a link boundary). */
tr::view::rope_t rope_split(std::span<const std::byte> bytes, std::span<const std::size_t> cuts) {
    tr::view::rope_t r;
    std::size_t prev = 0;
    const auto add = [&](std::size_t from, std::size_t to) {
        if (to > from) r.append(make_value(bytes.subspan(from, to - from)));
    };
    for (const std::size_t c : cuts) {
        const std::size_t cut = c > bytes.size() ? bytes.size() : c;
        add(prev, cut);
        prev = cut;
    }
    add(prev, bytes.size());
    return r;
}

/** @brief The rebuilt forward-hop frame gathered into one contiguous byte vector. */
template <class Cursor>
std::optional<bytes_t> forward_bytes(const Cursor& cur, std::string_view inbound_name) {
    const auto r = tr::net::rebuild_fwd_forward(cur, inbound_name);
    if (!r || !r->ok()) return std::nullopt;
    bytes_t out;
    r->gather(cur,
              [&](std::span<const std::byte> s) { out.insert(out.end(), s.begin(), s.end()); });
    return out;
}

}  // namespace

int main() {
    std::printf("fwd_frame_view — the FWD offset-dispatch cluster, driven directly:\n");

    const bytes_t payload = b_value_u32(0xABCD1234u);

    // 1. peek_fwd_first_dst_seg — span cursor: offsets name the first dst segment's bytes.
    {
        const bytes_t frame =
            b_fwd(fwd_op_t::WRITE, b_path({"child", "x"}), b_path({"c"}), payload);
        const span_cursor cur{frame};
        const auto seg = tr::net::peek_fwd_first_dst_seg(cur);
        check(seg.has_value(), "first-dst-seg peek: a structured FWD yields the segment window");
        const std::string_view want = "child";
        check(seg && seg->second == want.size() &&
                  std::memcmp(frame.data() + seg->first, want.data(), want.size()) == 0,
              "first-dst-seg peek: [off, len) re-slices to the segment NAME bytes");
    }

    // 1b. peek_fwd_first_dst_seg — rope cursor, split at EVERY byte boundary: the
    //     offsets must be identical to the contiguous read (they are source-agnostic).
    {
        const bytes_t frame = b_fwd(fwd_op_t::READ, b_path({"hop", "leaf"}), b_path({}), payload);
        const auto span_seg = tr::net::peek_fwd_first_dst_seg(span_cursor{frame});
        bool all_equal = span_seg.has_value();
        for (std::size_t cut = 1; cut + 1 < frame.size() && all_equal; ++cut) {
            const std::size_t cuts[] = {cut};
            const tr::view::rope_t r = rope_split(frame, cuts);
            const auto rope_seg = tr::net::peek_fwd_first_dst_seg(rope_cursor{r});
            all_equal = rope_seg == span_seg;
        }
        check(all_equal, "first-dst-seg peek: every rope split reads the same window as the span");
    }

    // 2. peek_fwd_op — op discriminant without a decode; rejects the non-FWD / opless.
    {
        const bytes_t wr = b_fwd(fwd_op_t::WRITE, b_path({"a"}), b_path({}));
        const bytes_t rp = b_fwd(fwd_op_t::REPLY, b_path({"a"}), b_path({}));
        check(tr::net::peek_fwd_op(span_cursor{wr}) == fwd_op_t::WRITE,
              "op peek: WRITE read by offset");
        check(tr::net::peek_fwd_op(span_cursor{rp}) == fwd_op_t::REPLY,
              "op peek: REPLY read by offset");
        const std::size_t cuts[] = {3, 7, 11};
        const tr::view::rope_t r = rope_split(rp, cuts);
        check(tr::net::peek_fwd_op(rope_cursor{r}) == fwd_op_t::REPLY,
              "op peek: identical over a multi-link rope");
        check(!tr::net::peek_fwd_op(span_cursor{b_path({"a"})}).has_value(),
              "op peek: a non-FWD frame yields nullopt");
        bytes_t empty_op_body;  // FWD{ VALUE(empty), ... } — an op with no payload byte
        tr::wire::emit_tlv(empty_op_body, type_t::VALUE, opt_t{}, std::span<const std::byte>{});
        bytes_t bad;
        tr::wire::emit_tlv(bad, type_t::FWD, opt_t{.pl = true}, empty_op_body);
        check(!tr::net::peek_fwd_op(span_cursor{bad}).has_value(),
              "op peek: an empty op VALUE yields nullopt");
    }

    // 3. peek_control — ADVERTISE / COMPACT / HANDLE_NACK heads, span + rope.
    {
        const bytes_t route = b_path({"unit", "temp"});
        const bytes_t adv = tr::net::encode_advertise(0xBEEF, route);
        const auto head = tr::net::peek_control(span_cursor{adv});
        check(head && head->type == type_t::ADVERTISE && head->label == 0xBEEF,
              "control peek: ADVERTISE type + u16 label");
        check(head && head->child1_total == route.size() &&
                  std::memcmp(adv.data() + head->child1_off, route.data(), route.size()) == 0,
              "control peek: child[1] window re-slices to the route TLV bytes");

        const bytes_t cmp = tr::net::encode_compact(7, payload);
        const auto chead = tr::net::peek_control(span_cursor{cmp});
        check(chead && chead->type == type_t::COMPACT && chead->label == 7 &&
                  chead->child1_total == payload.size(),
              "control peek: COMPACT type + label + payload window");

        const bytes_t nack = tr::net::encode_handle_nack(41);
        const auto nhead = tr::net::peek_control(span_cursor{nack});
        check(nhead && nhead->type == type_t::HANDLE_NACK && nhead->label == 41 &&
                  nhead->child1_off == 0 && nhead->child1_total == 0,
              "control peek: bare-label HANDLE_NACK has no child[1]");

        // The u16 label straddling a link boundary must stitch identically.
        bool all_equal = true;
        for (std::size_t cut = 1; cut + 1 < adv.size() && all_equal; ++cut) {
            const std::size_t cuts[] = {cut};
            const tr::view::rope_t r = rope_split(adv, cuts);
            const auto rh = tr::net::peek_control(rope_cursor{r});
            all_equal = rh && rh->type == head->type && rh->label == head->label &&
                        rh->child1_off == head->child1_off &&
                        rh->child1_total == head->child1_total;
        }
        check(all_equal, "control peek: every rope split reads the same head as the span");

        check(!tr::net::peek_control(span_cursor{b_fwd(fwd_op_t::READ, b_path({"a"}), b_path({}))})
                   .has_value(),
              "control peek: a FWD frame is not a control frame");
        bytes_t short_label_body;  // label VALUE with only 1 byte — malformed
        const std::byte one{0x01};
        tr::wire::emit_tlv(short_label_body, type_t::VALUE, opt_t{},
                           std::span<const std::byte>(&one, 1));
        bytes_t bad;
        tr::wire::emit_tlv(bad, type_t::HANDLE_NACK, opt_t{.pl = true}, short_label_body);
        check(!tr::net::peek_control(span_cursor{bad}).has_value(),
              "control peek: a 1-byte label VALUE is rejected");
    }

    // 4. Head rebuild — shrink dst, grow src: BYTE-EXACT vs a reference re-encode.
    {
        const bytes_t frame =
            b_fwd(fwd_op_t::WRITE, b_path({"child", "x"}), b_path({"c"}), payload);
        const auto out = forward_bytes(span_cursor{frame}, "in");
        const bytes_t want = b_fwd(fwd_op_t::WRITE, b_path({"x"}), b_path({"in", "c"}), payload);
        check(out.has_value(), "rebuild: a forwardable FWD rebuilds");
        check(out == want, "rebuild: shrunk-dst + grown-src bytes == the reference re-encode");
    }

    // 4b. REPLY does not grow src (a reply accumulates no return route, RFC-0004 §B).
    {
        const bytes_t frame = b_fwd(fwd_op_t::REPLY, b_path({"back", "home"}), b_path({}), payload);
        const auto out = forward_bytes(span_cursor{frame}, "in");
        const bytes_t want = b_fwd(fwd_op_t::REPLY, b_path({"home"}), b_path({}), payload);
        check(out == want, "rebuild: a REPLY shrinks dst but does NOT grow src");
    }

    // 4c. The optional FIELD selector rides through untouched, in position.
    {
        bytes_t sel;
        tr::wire::emit_tlv(sel, type_t::FIELD, opt_t{.pl = true}, b_name("mode"));
        const bytes_t frame =
            b_fwd(fwd_op_t::READ, b_path({"child", "x"}), b_path({"c"}), payload, sel);
        const auto out = forward_bytes(span_cursor{frame}, "up");
        const bytes_t want =
            b_fwd(fwd_op_t::READ, b_path({"x"}), b_path({"up", "c"}), payload, sel);
        check(out == want, "rebuild: the FIELD selector is carried byte-identically");
    }

    // 4d. A single-segment dst shrinks to an empty PATH (the next hop is the terminus).
    {
        const bytes_t frame = b_fwd(fwd_op_t::WRITE, b_path({"child"}), b_path({}), payload);
        const auto out = forward_bytes(span_cursor{frame}, "in");
        const bytes_t want = b_fwd(fwd_op_t::WRITE, b_path({}), b_path({"in"}), payload);
        check(out == want, "rebuild: a single-segment dst shrinks to an empty PATH");
    }

    // 4e. Rope cursor, split at EVERY byte: the gathered egress is byte-identical
    //     to the contiguous rebuild (the ADR-0053 ④b oracle, at the unit level).
    {
        bytes_t sel;
        tr::wire::emit_tlv(sel, type_t::FIELD, opt_t{.pl = true}, b_name("f"));
        const bytes_t frame =
            b_fwd(fwd_op_t::WRITE, b_path({"child", "leaf"}), b_path({"c0"}), payload, sel);
        const auto span_out = forward_bytes(span_cursor{frame}, "bus7");
        bool all_equal = span_out.has_value();
        for (std::size_t cut = 1; cut + 1 < frame.size() && all_equal; ++cut) {
            const std::size_t cuts[] = {cut};
            const tr::view::rope_t r = rope_split(frame, cuts);
            all_equal = forward_bytes(rope_cursor{r}, "bus7") == span_out;
        }
        check(all_equal, "rebuild: every rope split gathers byte-identical egress");
    }

    // 5. Malformed rejects — each structural precondition fails to nullopt.
    {
        const bytes_t good = b_fwd(fwd_op_t::WRITE, b_path({"a", "b"}), b_path({}), payload);
        const bytes_t truncated(good.begin(), good.begin() + 3);
        check(!tr::net::rebuild_fwd_forward(span_cursor{truncated}, "in").has_value(),
              "reject: a truncated frame");
        check(!tr::net::rebuild_fwd_forward(span_cursor{b_path({"a"})}, "in").has_value(),
              "reject: a non-FWD frame");
        bytes_t no_dst_body = b_op(fwd_op_t::WRITE);  // FWD{ op, VALUE } — dst is not a PATH
        append(no_dst_body, payload);
        bytes_t no_dst;
        tr::wire::emit_tlv(no_dst, type_t::FWD, opt_t{.pl = true}, no_dst_body);
        check(!tr::net::rebuild_fwd_forward(span_cursor{no_dst}, "in").has_value(),
              "reject: child[1] is not a dst PATH");
        bytes_t no_src_body = b_op(fwd_op_t::WRITE);  // FWD{ op, dst } — src PATH missing
        append(no_src_body, b_path({"a"}));
        bytes_t no_src;
        tr::wire::emit_tlv(no_src, type_t::FWD, opt_t{.pl = true}, no_src_body);
        check(!tr::net::rebuild_fwd_forward(span_cursor{no_src}, "in").has_value(),
              "reject: a missing src PATH");
        // dst.child[0] is a VALUE, not a NAME — not a routable segment.
        bytes_t dst_body;
        tr::wire::emit_tlv(dst_body, type_t::VALUE, opt_t{}, std::span<const std::byte>{});
        bytes_t bad_dst;
        tr::wire::emit_tlv(bad_dst, type_t::PATH, opt_t{.pl = true}, dst_body);
        const bytes_t frame = b_fwd(fwd_op_t::WRITE, bad_dst, b_path({}), payload);
        check(!tr::net::rebuild_fwd_forward(span_cursor{frame}, "in").has_value(),
              "reject: a dst whose first child is not a NAME");
        check(!tr::net::peek_fwd_first_dst_seg(span_cursor{frame}).has_value(),
              "reject: first-dst-seg peek agrees (no NAME, no window)");
        // An empty dst PATH is not forwardable either.
        const bytes_t empty_dst = b_fwd(fwd_op_t::WRITE, b_path({}), b_path({}), payload);
        check(!tr::net::peek_fwd_first_dst_seg(span_cursor{empty_dst}).has_value(),
              "reject: an empty dst PATH yields no segment window");
    }

    // 6. read_fwd_header — absolute offsets; out-of-range position rejected.
    {
        const bytes_t frame = b_fwd(fwd_op_t::READ, b_path({"a"}), b_path({}));
        const auto h = tr::net::read_fwd_header(span_cursor{frame}, 0);
        check(
            h && h->type == type_t::FWD && h->total == frame.size() && h->body_off == h->header_len,
            "read_fwd_header: absolute body_off + total over the whole frame");
        const auto op = tr::net::read_fwd_header(span_cursor{frame}, h->body_off);
        check(op && op->type == type_t::VALUE && op->body_off == h->body_off + op->header_len,
              "read_fwd_header: a child header reads at its absolute offset");
        check(!tr::net::read_fwd_header(span_cursor{frame}, frame.size() + 1).has_value(),
              "read_fwd_header: a past-the-end position yields nullopt");
    }

    // 7. stack_writer — clamp-to-empty overflow, never an overrun; ll auto-widening.
    {
        tr::net::stack_writer<8> w;
        w.header(type_t::FWD, 4);  // 4 bytes — fits
        check(w.ok() && w.span().size() == 4, "stack_writer: a fitting header is written");
        // A runtime-opaque length (volatile) keeps GCC from forking the guarded
        // overflow branch into a -Warray-bounds false positive.
        volatile std::size_t over_len = 19;
        const std::string overflow_name(over_len, 'x');
        w.name(overflow_name);  // 4 + 4 + 19 > 8 — clamps
        check(!w.ok() && w.span().empty(), "stack_writer: overflow clamps to an empty span");

        tr::net::stack_writer<8> wide;
        wide.header(type_t::FWD, 0x10000);  // body > 0xFFFF => the 6-byte LL header
        check(wide.ok() && wide.span().size() == 6 &&
                  std::to_integer<std::uint8_t>(wide.span()[2]) == 0x00 &&
                  std::to_integer<std::uint8_t>(wide.span()[4]) == 0x01,
              "stack_writer: an oversize body auto-widens to the u32 LL header");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
