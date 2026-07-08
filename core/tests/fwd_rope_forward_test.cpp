/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0053 ④b — the FWD forward hop over a MULTI-LINK rope, WITHOUT flattening.
 * A frame delivered as a scatter-gather rope (CAN reassembly / fragmented WS) is
 * routed by reading its dispatch offsets through the link-walking grammar cursor
 * and scatter-gathering the untouched links onward — no interim flatten copy.
 *
 * The proof is an ORACLE equality: the same canonical FWD frame is routed twice —
 * once contiguously (the span-cursor path, `on_frame`) and once as a rope split at
 * an adversarial boundary (the rope-cursor path, `on_frame` via a rope-delivering
 * link) — and the bytes the downstream child receives must be IDENTICAL for every
 * split. Splits are chosen to straddle the FWD header, a segment NAME (forcing the
 * bounded-scratch stitch), and every single byte, so header stitching and the
 * segment materialize are all exercised. A terminus (dst names no child) is
 * resolved straight off the rope through the view-tier resolver (ADR-0053 3c-iii —
 * no flatten), verifying CRC at access (§4) before the op updates the local LKV.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// --- wire builders (canonical bytes via the production emit helpers) ----------
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        const std::vector<std::byte> n = b_name(s);
        body.insert(body.end(), n.begin(), n.end());
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}
void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}
std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src,
                             const std::vector<std::byte>& payload = {}) {
    std::vector<std::byte> body;
    std::vector<std::byte> opv;
    const std::byte ob{static_cast<std::uint8_t>(op)};
    tr::wire::emit_tlv(opv, type_t::VALUE, opt_t{}, std::span<const std::byte>(&ob, 1));
    append(body, opv);
    append(body, dst);
    append(body, src);
    if (!payload.empty()) append(body, payload);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

// Build a rope over `bytes` split at the given cut points (each cut is a link
// boundary). Every link owns its own heap segment — a genuine scatter-gather
// frame the router must walk without flattening.
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

// --- fake transports ----------------------------------------------------------
// A span link: records every send()'s bytes (the downstream egress under test).
class fake_link_t : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        sent_.emplace_back(frame.begin(), frame.end());
    }
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    std::vector<std::vector<std::byte>>& sent() { return sent_; }

   private:
    std::vector<std::vector<std::byte>> sent_;
};

// A rope-delivering link (ADR-0053 §5): hands the frame up as the rope it is,
// exercising the router's no-flatten forward path.
class fake_rope_link_t : public transport_t {
   public:
    void send(std::span<const std::byte>) override {}  // unused inbound-only link
    [[nodiscard]] bool delivers_ropes() const override { return true; }
    void inject(tr::view::rope_t frame) { rx_.deliver_rope(std::move(frame)); }
};

// Route `frame` (as a rope, split at `cuts`) through a fresh forwarder and return
// what the "up" child received. An empty graph — this node only forwards.
std::vector<std::vector<std::byte>> forward_as_rope(std::span<const std::byte> frame,
                                                    std::span<const std::size_t> cuts) {
    graph_t g;
    fwd_router_t router(g);
    fake_rope_link_t cli;
    fake_link_t up;
    router.add_child("cli", cli);  // inbound (rope) link
    router.add_child("up", up);    // the dst-resolved forward child
    cli.inject(rope_split(frame, cuts));
    return std::move(up.sent());
}

// The oracle: route the identical `frame` contiguously (the span-cursor path).
std::vector<std::vector<std::byte>> forward_contiguous(std::span<const std::byte> frame) {
    graph_t g;
    fwd_router_t router(g);
    fake_link_t cli;
    fake_link_t up;
    router.add_child("cli", cli);
    router.add_child("up", up);
    cli.inject(frame);
    return std::move(up.sent());
}

}  // namespace

int main() {
    std::printf("FWD forward hop over a multi-link rope (ADR-0053 ④b, no flatten):\n");

    // A representative forwarded READ: dst=/up/sensor, src=/reply-ep. "up" resolves
    // to the child; the hop strips "up" and prepends "cli" to src.
    const std::vector<std::byte> frame =
        b_fwd(fwd_op_t::READ, b_path({"up", "sensor"}), b_path({"reply-ep"}));

    // The oracle egress from the contiguous path — a single scatter-gathered frame.
    const auto oracle = forward_contiguous(frame);
    check(oracle.size() == 1, "contiguous forward emits exactly one egress frame");
    check(!oracle.empty() && !oracle[0].empty(), "oracle egress is non-empty");

    // The oracle must itself be a well-formed FWD with dst shrunk + src grown, so the
    // equality below is anchored to correct bytes (not two matching wrongs).
    if (!oracle.empty()) {
        const auto dec = tr::wire::decode(oracle[0]);
        check(dec && dec->type == type_t::FWD && dec->children.size() == 3,
              "oracle egress decodes as FWD{op,dst,src}");
        if (dec && dec->children.size() == 3) {
            check(tr::wire::equal(dec->children[1], *tr::wire::decode(b_path({"sensor"}))),
                  "oracle dst shrunk to /sensor");
            check(tr::wire::equal(dec->children[2], *tr::wire::decode(b_path({"cli", "reply-ep"}))),
                  "oracle src grown to /cli/reply-ep");
        }
    }

    // Every adversarial split must reproduce the oracle egress BYTE-FOR-BYTE.
    // Single interior cuts sweep every boundary (straddling the FWD header, the op
    // TLV, the /up NAME the router materializes into scratch, and the tail).
    int mismatches = 0;
    int checked = 0;
    for (std::size_t cut = 1; cut < frame.size(); ++cut) {
        const std::array<std::size_t, 1> cuts{cut};
        const auto got = forward_as_rope(frame, cuts);
        ++checked;
        if (got != oracle) ++mismatches;
    }
    check(checked > 0, "swept every interior split boundary");
    check(mismatches == 0, "every 2-link split routes byte-identically to the contiguous path");

    // A maximally fragmented rope: one link per byte — worst case for header
    // stitching and the segment-name materialize.
    {
        std::vector<std::size_t> every_byte;
        for (std::size_t i = 1; i < frame.size(); ++i) every_byte.push_back(i);
        const auto got = forward_as_rope(frame, every_byte);
        check(got == oracle, "one-link-per-byte rope routes byte-identically (max fragmentation)");
    }

    // Two cuts straddling both the /up segment NAME and the src PATH header.
    {
        const std::array<std::size_t, 2> cuts{6, 11};
        const auto got = forward_as_rope(frame, cuts);
        check(got == oracle, "a 3-link split across segment + header routes byte-identically");
    }

    // Terminus over a multi-link rope: dst names NO child (local /sensor), so the
    // router resolves the request straight off the rope through the view-tier
    // resolver (ADR-0053 3c-iii — NO flatten) and applies the WRITE to the LKV.
    {
        std::printf("Terminus over a multi-link rope (view resolver applies the write):\n");
        graph_t g;
        const auto sensor = path_t::parse("/sensor");
        tr::graph::vertex_handle_t v = g.register_vertex(*sensor, role_t::STORED_VALUE);
        fwd_router_t router(g);
        fake_rope_link_t in;
        router.add_child("in", in);  // reply goes back over the inbound link
        const std::uint32_t kWritten = 0x0BADF00Du;
        const std::vector<std::byte> wframe =
            b_fwd(fwd_op_t::WRITE, b_path({"sensor"}), b_path({"reply-ep"}), b_value_u32(kWritten));
        const std::array<std::size_t, 1> cuts{wframe.size() / 2};
        in.inject(rope_split(wframe, cuts));
        const auto stored = g.read(v);
        check(stored.has_value(), "/sensor readable after a multi-link rope WRITE terminus");
        if (stored) {
            const auto inner = tr::wire::view_as_tlv(stored->only());
            check(inner && inner->type == type_t::VALUE && inner->payload.size() == 4 &&
                      tr::detail::load_le<std::uint32_t>(inner->payload) == kWritten,
                  "LKV updated to the forwarded value (view resolver decoded correctly)");
        }
    }

    // Verify-at-access (ADR-0053 §4): the lazy rope terminus verifies CRC before the
    // op mutates state, matching the arena terminus's decode_into(VERIFY). A
    // frame-CRC WRITE whose body is corrupt fails verify and is DROPPED (LKV never
    // written); the same frame intact applies. Proven on a FRESH vertex so "no value"
    // is unambiguous evidence the corrupt frame was dropped, not merely overwritten.
    {
        std::printf(
            "Verify-at-access over a multi-link rope (bad CRC dropped, good CRC applied):\n");
        graph_t g;
        const auto sensor = path_t::parse("/sensor");
        tr::graph::vertex_handle_t v = g.register_vertex(*sensor, role_t::STORED_VALUE);
        fwd_router_t router(g);
        fake_rope_link_t in;
        router.add_child("in", in);

        const std::uint32_t kWritten = 0x0C0FFEE0u;
        const std::vector<std::byte> plain =
            b_fwd(fwd_op_t::WRITE, b_path({"sensor"}), b_path({"reply-ep"}), b_value_u32(kWritten));
        // Re-emit the FWD carrying a whole-frame CRC-32C trailer (opt.cr covers the body).
        tr::wire::tlv_t crc_fwd = *tr::wire::decode(plain);
        crc_fwd.opt.cr = true;
        const std::vector<std::byte> crc_frame = tr::wire::encode(crc_fwd);
        check(crc_frame.size() == plain.size() + 4, "CRC frame carries a 4-byte CRC-32C trailer");

        // Corrupt the last BODY byte (payload data — grammar stays valid, CRC breaks).
        std::vector<std::byte> corrupt = crc_frame;
        corrupt[corrupt.size() - 5] ^= std::byte{0xFF};
        const std::array<std::size_t, 1> cuts{corrupt.size() / 2};
        in.inject(rope_split(corrupt, cuts));
        check(!g.read(v).has_value(), "corrupt-CRC multi-link WRITE is dropped (LKV unwritten)");

        // The intact CRC frame applies (proving the drop was the CRC, not the path).
        in.inject(rope_split(crc_frame, cuts));
        const auto stored = g.read(v);
        check(stored.has_value(), "intact-CRC multi-link WRITE applies");
        if (stored) {
            const auto inner = tr::wire::view_as_tlv(stored->only());
            check(inner && inner->type == type_t::VALUE && inner->payload.size() == 4 &&
                      tr::detail::load_le<std::uint32_t>(inner->payload) == kWritten,
                  "LKV updated to the CRC-verified value");
        }
    }

    // Control frame over a multi-link rope (ADR-0055 §2/§3): the on_frame_rope whole-frame
    // flatten is gone — ADVERTISE / COMPACT are served rope-native by on_control_rope,
    // which reads the label off the rope and materializes ONLY the child sub-rope it needs.
    {
        std::printf("ADVERTISE forward over a multi-link rope (rope-native control sink):\n");
        // route /up/sensor: "up" names a child, so this node re-advertises downstream.
        const std::vector<std::byte> adv =
            tr::net::encode_advertise(0x1234u, b_path({"up", "sensor"}));
        const auto oracle = forward_contiguous(adv);
        check(oracle.size() == 1, "contiguous ADVERTISE re-advertises exactly one frame");
        if (!oracle.empty()) {
            const auto dec = tr::wire::decode(oracle[0]);
            check(dec && dec->type == type_t::ADVERTISE, "oracle egress is an ADVERTISE");
        }
        int mismatches = 0, checked = 0;
        for (std::size_t cut = 1; cut < adv.size(); ++cut) {
            const std::array<std::size_t, 1> cuts{cut};
            if (forward_as_rope(adv, cuts) != oracle) ++mismatches;
            ++checked;
        }
        check(checked > 0 && mismatches == 0,
              "every multi-link ADVERTISE split re-advertises byte-identically to the oracle");
        std::vector<std::size_t> every_byte;
        for (std::size_t i = 1; i < adv.size(); ++i) every_byte.push_back(i);
        check(forward_as_rope(adv, every_byte) == oracle,
              "one-link-per-byte ADVERTISE rope re-advertises byte-identically");
    }

    // COMPACT terminus over a multi-link rope: advertise a LOCAL route first (binds the
    // label to this node), then deliver a label-compacted COMPACT as a scatter-gather
    // rope — on_control_rope materializes ONLY the payload sub-rope and deliver_local
    // applies the write to the LKV.
    {
        std::printf("COMPACT terminus over a multi-link rope (payload sub-rope materialize):\n");
        graph_t g;
        const auto sensor = path_t::parse("/sensor");
        tr::graph::vertex_handle_t v = g.register_vertex(*sensor, role_t::STORED_VALUE);
        fwd_router_t router(g);
        fake_rope_link_t in;
        router.add_child("in", in);
        // "sensor" names no child ⇒ a terminus binding for label 0x0042 on link "in".
        const std::uint16_t kLabel = 0x0042u;
        const std::vector<std::byte> adv = tr::net::encode_advertise(kLabel, b_path({"sensor"}));
        in.inject(rope_split(adv, std::array<std::size_t, 0>{}));  // single link: binds the label
        const std::uint32_t kVal = 0xFEEDBEEFu;
        const std::vector<std::byte> comp = tr::net::encode_compact(kLabel, b_value_u32(kVal));
        const std::array<std::size_t, 1> cuts{comp.size() / 2};
        in.inject(rope_split(comp, cuts));  // multi-link: the path under test
        const auto stored = g.read(v);
        check(stored.has_value(), "/sensor written by a multi-link COMPACT terminus");
        if (stored) {
            const auto inner = tr::wire::view_as_tlv(stored->only());
            check(inner && inner->type == type_t::VALUE && inner->payload.size() == 4 &&
                      tr::detail::load_le<std::uint32_t>(inner->payload) == kVal,
                  "LKV updated to the label-compacted value (payload sub-rope decoded)");
        }
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
