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

#include <array>
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

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/rope_decode.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/view.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::wire::opt_t;
using tr::wire::type_t;
using tr::wire::grammar::rope_cursor;
using tr::wire::grammar::span_cursor;

using tr::testing::check;
using tr::testing::make_value;

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
        (void)tr::wire::emit_path_segment(body, s);
    }
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
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

using tr::testing::b_fwd;

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

/**
 * @brief The same frame stamped with a TF=0 absolute trailer: `opt.TS` patched into the
 *        header byte, the 8 LE nanosecond bytes appended (#1109).
 */
bytes_t with_ts(bytes_t frame, std::uint64_t ns) {
    opt_t o = opt_t::decode(std::to_integer<std::uint8_t>(frame[1]));
    o.ts = true;
    o.tf = false;
    frame[1] = static_cast<std::byte>(o.encode());
    tr::wire::emit_trailer_ts(frame, /*relative=*/false, static_cast<std::int64_t>(ns));
    return frame;
}

}  // namespace

int main() {
    std::printf("fwd_frame_view — the FWD offset-dispatch cluster, driven directly:\n");

    const bytes_t payload = b_value_u32(0xABCD1234u);

    // 1. peek_fwd_first_dst_seg — span cursor: offsets name the first dst segment's bytes.
    {
        const bytes_t frame =
            b_fwd(fwd_op_t::WRITE, b_path({"child", "x"}), b_path({"c"}), {}, payload);
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
        const bytes_t frame =
            b_fwd(fwd_op_t::READ, b_path({"hop", "leaf"}), b_path({}), {}, payload);
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
            b_fwd(fwd_op_t::WRITE, b_path({"child", "x"}), b_path({"c"}), {}, payload);
        const auto out = forward_bytes(span_cursor{frame}, "in");
        const bytes_t want =
            b_fwd(fwd_op_t::WRITE, b_path({"x"}), b_path({"in", "c"}), {}, payload);
        check(out.has_value(), "rebuild: a forwardable FWD rebuilds");
        check(out == want, "rebuild: shrunk-dst + grown-src bytes == the reference re-encode");
    }

    // 4b. REPLY does not grow src (a reply accumulates no return route, RFC-0004 §B).
    {
        const bytes_t frame =
            b_fwd(fwd_op_t::REPLY, b_path({"back", "home"}), b_path({}), {}, payload);
        const auto out = forward_bytes(span_cursor{frame}, "in");
        const bytes_t want = b_fwd(fwd_op_t::REPLY, b_path({"home"}), b_path({}), {}, payload);
        check(out == want, "rebuild: a REPLY shrinks dst but does NOT grow src");
    }

    // 4c. The optional FIELD selector rides through untouched, in position.
    {
        bytes_t sel;
        tr::wire::emit_tlv(sel, type_t::FIELD, opt_t{.pl = true}, b_name("mode"));
        const bytes_t frame =
            b_fwd(fwd_op_t::READ, b_path({"child", "x"}), b_path({"c"}), sel, payload);
        const auto out = forward_bytes(span_cursor{frame}, "up");
        const bytes_t want =
            b_fwd(fwd_op_t::READ, b_path({"x"}), b_path({"up", "c"}), sel, payload);
        check(out == want, "rebuild: the FIELD selector is carried byte-identically");
    }

    // 4d. A single-segment dst shrinks to an empty PATH (the next hop is the terminus).
    {
        const bytes_t frame = b_fwd(fwd_op_t::WRITE, b_path({"child"}), b_path({}), {}, payload);
        const auto out = forward_bytes(span_cursor{frame}, "in");
        const bytes_t want = b_fwd(fwd_op_t::WRITE, b_path({}), b_path({"in"}), {}, payload);
        check(out == want, "rebuild: a single-segment dst shrinks to an empty PATH");
    }

    // 4e. Rope cursor, split at EVERY byte: the gathered egress is byte-identical
    //     to the contiguous rebuild (the ADR-0053 ④b oracle, at the unit level).
    {
        bytes_t sel;
        tr::wire::emit_tlv(sel, type_t::FIELD, opt_t{.pl = true}, b_name("f"));
        const bytes_t frame =
            b_fwd(fwd_op_t::WRITE, b_path({"child", "leaf"}), b_path({"c0"}), sel, payload);
        const auto span_out = forward_bytes(span_cursor{frame}, "bus7");
        bool all_equal = span_out.has_value();
        for (std::size_t cut = 1; cut + 1 < frame.size() && all_equal; ++cut) {
            const std::size_t cuts[] = {cut};
            const tr::view::rope_t r = rope_split(frame, cuts);
            all_equal = forward_bytes(rope_cursor{r}, "bus7") == span_out;
        }
        check(all_equal, "rebuild: every rope split gathers byte-identical egress");
    }

    // 4f. #1109: an origin's TF=0 trailer stamp SURVIVES the forward hop — the TS/TF bits
    //     stay on the rebuilt head and the 8 stamp bytes are re-emitted verbatim as the
    //     outgoing frame's last bytes. (Before this fix the fresh head hardcoded
    //     `opt{.pl = true}` and gather stopped at body_end: the stamp was silently dropped
    //     at the first forwarder.)
    {
        constexpr std::uint64_t kNs = 0x1122334455667788ull;
        const bytes_t frame = with_ts(
            b_fwd(fwd_op_t::WRITE, b_path({"child", "x"}), b_path({"c"}), {}, payload), kNs);
        const auto out = forward_bytes(span_cursor{frame}, "in");
        const bytes_t want =
            with_ts(b_fwd(fwd_op_t::WRITE, b_path({"x"}), b_path({"in", "c"}), {}, payload), kNs);
        check(out == want, "rebuild: a TF=0 origin stamp survives the hop byte-exact");

        // The pre-carried path (the router's shape: peek fills the offsets, the caller sets
        // strip_at, the rebuild reuses them) must preserve the stamp identically — the peek
        // now carries the outer opt bits forward for exactly this.
        tr::net::fwd_pre_t pre;
        check(tr::net::peek_fwd_dst(span_cursor{frame}, pre), "stamped frame: dst peek accepts");
        pre.strip_at = pre.seg0_off + pre.seg0_len;  // consume the one leading segment
        const auto pre_r = tr::net::rebuild_fwd_forward(
            span_cursor{frame}, std::span<const std::byte>{}, "in", 1, &pre);
        check(pre_r.has_value() && pre_r->ok(), "stamped frame: the pre-carried rebuild succeeds");
        if (pre_r) {
            bytes_t pre_out;
            pre_r->gather(span_cursor{frame}, [&](std::span<const std::byte> s) {
                pre_out.insert(pre_out.end(), s.begin(), s.end());
            });
            check(pre_out == want, "rebuild(pre): the peek-carried opt preserves the stamp too");
        }

        // Every rope split gathers the same stamped egress (source-agnostic, as ever).
        bool all_equal = true;
        for (std::size_t cut = 1; cut + 1 < frame.size() && all_equal; ++cut) {
            const std::size_t cuts[] = {cut};
            const tr::view::rope_t r = rope_split(frame, cuts);
            all_equal = forward_bytes(rope_cursor{r}, "in") == out;
        }
        check(all_equal, "rebuild: every rope split gathers the same stamped egress");
    }

    // 4g. #1109: an inbound CRC does NOT cross the hop (the rebuilt body invalidates it),
    //     while the stamp riding beside it still does.
    {
        constexpr std::int64_t kNs = 777'000'111LL;
        const bytes_t plain =
            b_fwd(fwd_op_t::WRITE, b_path({"child", "x"}), b_path({"c"}), {}, payload);
        auto dec = tr::wire::decode(plain);
        check(dec.has_value(), "CRC+TS source frame decodes");
        tr::wire::stamp_ts(*dec, kNs);
        dec->opt.cr = true;  // CRC-32C over body ++ ts, computed by encode
        const bytes_t frame = tr::wire::encode(*dec);
        check(!frame.empty(), "CRC+TS source frame re-encodes");
        const auto out = forward_bytes(span_cursor{frame}, "in");
        const bytes_t want =
            with_ts(b_fwd(fwd_op_t::WRITE, b_path({"x"}), b_path({"in", "c"}), {}, payload),
                    static_cast<std::uint64_t>(kNs));
        check(out == want, "rebuild: TS preserved, stale CRC dropped (CR never crosses)");
    }

    // 5. Malformed rejects — each structural precondition fails to nullopt.
    {
        const bytes_t good = b_fwd(fwd_op_t::WRITE, b_path({"a", "b"}), b_path({}), {}, payload);
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
        // dst record[0] is the RFC-0018 §5.4 ESCAPE, not a literal segment — the packed
        // successor of "the first child is a VALUE, not a NAME". Nothing mints an escape,
        // and an address that LEADS with one names no transport child here, so the gate
        // must refuse it exactly as it refused a non-NAME leading child.
        bytes_t esc_body;
        const std::array<std::byte, 4> label{std::byte{1}, std::byte{0}, std::byte{2},
                                             std::byte{0}};
        tr::testing::append_path_escape(esc_body, tr::wire::kPackedEscapeKindLabel, label);
        (void)tr::wire::emit_path_segment(esc_body, "sensor");
        const bytes_t bad_dst = tr::testing::b_path_body(esc_body);
        const bytes_t frame = b_fwd(fwd_op_t::WRITE, bad_dst, b_path({}), {}, payload);
        // The refusal lives at the ROUTING GATE, which is the only tier that asks "is this an
        // address I can descend". `rebuild_fwd_forward` is frame-path context and steps over
        // an escape by design (§5.4), so a frame that reaches it has already been routed —
        // asserting a refusal THERE would put the key-context rule on the relay path.
        check(!tr::net::peek_fwd_first_dst_seg(span_cursor{frame}).has_value(),
              "reject: a dst whose FIRST record is the len==0 escape names no child");
        {
            tr::net::fwd_pre_t esc_pre;
            check(!tr::net::peek_fwd_dst(span_cursor{frame}, esc_pre),
                  "reject: the dst gate agrees, and clears the carried offsets");
            check(!esc_pre.valid, "...cleared, so no stale offsets reach a rebuild");
        }
        // But an escape BEHIND a literal leading segment is STEPPED OVER, not refused —
        // that is the forward-plane half of §5.4, and the reason the skip path exists at
        // all before anything mints.
        bytes_t skip_body;
        (void)tr::wire::emit_path_segment(skip_body, "in");
        tr::testing::append_path_escape(skip_body, tr::wire::kPackedEscapeKindLabel, label);
        (void)tr::wire::emit_path_segment(skip_body, "sensor");
        const bytes_t skip_dst = tr::testing::b_path_body(skip_body);
        const bytes_t skip_frame = b_fwd(fwd_op_t::WRITE, skip_dst, b_path({}), {}, payload);
        check(tr::net::peek_fwd_first_dst_seg(span_cursor{skip_frame}).has_value(),
              "an escape BEHIND the leading segment leaves the frame routable");
        {
            tr::net::fwd_pre_t pre;
            check(tr::net::peek_fwd_dst(span_cursor{skip_frame}, pre),
                  "...and the dst window still opens");
            tr::net::dst_seg_walk_t<span_cursor> walk(span_cursor{skip_frame}, pre);
            const auto s0 = walk.at(0);
            const auto s1 = walk.at(1);
            check(s0 && s0->second == 2, "segment 0 is the two-byte literal `in`");
            check(s1 && s1->second == 6,
                  "segment 1 is `sensor` — the escape occupied no segment index");
            check(!walk.at(2).has_value(), "and there is no third segment");
        }
        // An empty dst PATH is not forwardable either.
        const bytes_t empty_dst = b_fwd(fwd_op_t::WRITE, b_path({}), b_path({}), {}, payload);
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
        w.path_seg(overflow_name);  // 4 + 1 + 19 > 8 — clamps
        check(!w.ok() && w.span().empty(), "stack_writer: overflow clamps to an empty span");

        tr::net::stack_writer<8> wide;
        wide.header(type_t::FWD, 0x10000);  // body > 0xFFFF => the 6-byte LL header
        check(wide.ok() && wide.span().size() == 6 &&
                  std::to_integer<std::uint8_t>(wide.span()[2]) == 0x00 &&
                  std::to_integer<std::uint8_t>(wide.span()[4]) == 0x01,
              "stack_writer: an oversize body auto-widens to the u32 LL header");
    }

    return tr::testing::summary("fwd_frame_view");
}
