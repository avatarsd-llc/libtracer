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
 * segment materialize are all exercised. A terminus (dst names no child) still
 * takes the documented flatten fallback and updates the local LKV.
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
    void set_receiver(receiver_t receiver) override { receiver_ = std::move(receiver); }
    void inject(std::span<const std::byte> frame) {
        if (receiver_) receiver_(frame);
    }
    std::vector<std::vector<std::byte>>& sent() { return sent_; }

   private:
    std::vector<std::vector<std::byte>> sent_;
    receiver_t receiver_;
};

// A rope-delivering link (ADR-0053 §5): hands the frame up as the rope it is,
// exercising the router's no-flatten forward path.
class fake_rope_link_t : public transport_t {
   public:
    void send(std::span<const std::byte>) override {}  // unused inbound-only link
    void set_receiver(receiver_t) override {}          // never installed (delivers ropes)
    void set_rope_receiver(rope_receiver_t receiver) override { rope_rx_ = std::move(receiver); }
    [[nodiscard]] bool delivers_ropes() const override { return true; }
    void inject(tr::view::rope_t frame) {
        if (rope_rx_) rope_rx_(std::move(frame));
    }

   private:
    rope_receiver_t rope_rx_;
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

    // Terminus fallback: a multi-link rope whose dst names NO child (local /sensor)
    // still decodes via the interim flatten and applies the WRITE to the LKV.
    {
        std::printf(
            "Terminus over a multi-link rope (flatten fallback still applies the write):\n");
        graph_t g;
        const auto sensor = path_t::parse("/sensor");
        tr::graph::vertex_t* v = *g.register_vertex(*sensor, role_t::STORED_VALUE);
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
                  "LKV updated to the forwarded value (flatten fallback decoded correctly)");
        }
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
