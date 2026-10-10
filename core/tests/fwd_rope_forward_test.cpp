/**
 * @file
 * @brief ADR-0053 ④b — the FWD forward hop over a MULTI-LINK rope, WITHOUT flattening.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
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
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/path_pair.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "pair_body.hpp"
#include "test_support.hpp"
#include "test_values.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

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

using tr::testing::check;
using tr::testing::make_value;

// --- wire builders (canonical bytes via the production emit helpers) ----------
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        (void)tr::wire::emit_path_segment(body, s);
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}
/**
 * @brief A bound `dst` over @p elements: a `PATH` of PAIR elements, head first (RFC-0029 §4).
 */
std::vector<std::byte> b_path_pairs(std::span<const tr::wire::path_pair_t> elements) {
    std::vector<std::byte> body;
    for (const tr::wire::path_pair_t& e : elements) tr::testing::emit_path_pair(body, e);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}
/** @brief A one-element bound `dst` — the spelling a terminus sees (RFC-0029 §4). */
std::vector<std::byte> b_path_ref_one(std::uint32_t index, std::uint32_t generation) {
    const tr::wire::path_pair_t e{.index = index, .generation = generation};
    return b_path_pairs(std::span<const tr::wire::path_pair_t>(&e, 1));
}
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}
using tr::testing::b_fwd;

/**
 * @brief A FWD whose op VALUE is present but EMPTY — the frame the two ingress arms
 *        classified differently (#870).
 *
 * The two peeks that drove ingress disagree about it ON PURPOSE:
 * `peek_fwd_dst_any` accepts a zero-length op VALUE (`fwd_pre_t::op_body_len` documents that
 * reading: "the peek does NOT reject an empty op — such a frame falls through to the terminus
 * decode"), while `peek_fwd_op` answers `nullopt` for it. The terminus resolver reads the
 * opcode with `load_le`, which yields 0 from an empty body — `fwd_op_t::READ` — so the frame
 * is a resolvable READ and its disposition is OBSERVABLE as a reply, not merely as a drop by
 * one road or another.
 */
using tr::testing::b_fwd_no_op;

/**
 * @brief Build a rope over `bytes` split at the given cut points (each cut is a link boundary).
 *
 * Every link owns its own heap segment — a genuine scatter-gather
 * frame the router must walk without flattening.
 */
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
/** @brief A span link: records every send()'s bytes (the downstream egress under test). */
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

/**
 * @brief A rope-delivering link (ADR-0053 §5): hands the frame up as the rope it is, exercising the
 *        router's no-flatten forward path.
 */
class fake_rope_link_t : public transport_t {
   public:
    /** @brief Records every send, because a rope link is not always inbound-only. */
    void send(std::span<const std::byte> frame) override {
        sent_.emplace_back(frame.begin(), frame.end());
    }
    [[nodiscard]] bool delivers_ropes() const override { return true; }
    void inject(tr::view::rope_t frame) { rx_.deliver_rope(std::move(frame)); }
    std::vector<std::vector<std::byte>>& sent() { return sent_; }

   private:
    std::vector<std::vector<std::byte>> sent_;
};

/**
 * @brief Route `frame` (as a rope, split at `cuts`) through a fresh forwarder and return what the
 *        "up" child received.
 *
 * An empty graph — this node only forwards.
 */
std::vector<std::vector<std::byte>> forward_as_rope(std::span<const std::byte> frame,
                                                    std::span<const std::size_t> cuts) {
    graph_t g;
    fwd_router_t router(g);
    fake_rope_link_t cli;
    fake_link_t up;
    (void)router.add_child("cli", cli);  // inbound (rope) link
    (void)router.add_child("up", up);    // the dst-resolved forward child
    cli.inject(rope_split(frame, cuts));
    return std::move(up.sent());
}

/**
 * @brief Same as @ref forward_as_rope, but with an explicit FAILABLE seam for the egress iov.
 *
 * The rope forward path is the only one whose iov length is chosen by the SENDER (link count x
 * region count), so it is the only one that can be starved from the wire (#596).
 */
std::vector<std::vector<std::byte>> forward_as_rope_with(std::span<const std::byte> frame,
                                                         std::span<const std::size_t> cuts,
                                                         tr::mem::block_source_t& rx) {
    graph_t g;
    fwd_router_t router(g, {.rx = &rx});
    fake_rope_link_t cli;
    fake_link_t up;
    (void)router.add_child("cli", cli);
    (void)router.add_child("up", up);
    cli.inject(rope_split(frame, cuts));
    return std::move(up.sent());
}

/**
 * @brief A source that forwards to the heap and counts what it served.
 *
 * Attribution, not budgeting: the point of the per-child topology (ADR-0067 §3) is WHICH
 * source a frame draws from, so the test needs to tell two live sources apart.
 */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    explicit counting_source_t(const char* n) noexcept : tr::mem::block_source_t(n) {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        ++served;
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        ::operator delete(p, bytes, std::align_val_t{align});
    }
    int served = 0; /**< @brief Allocations served since construction. */
};

/**
 * @brief Route a rope in over a child that carries its OWN failable source (ADR-0067 §3).
 *
 * The router keeps a different source as its default, so the two counters answer the only
 * question that matters: did the inbound child's frame draw from the child's slab, or from
 * the shared one the ADR forbids on this path?
 */
std::vector<std::vector<std::byte>> forward_as_rope_per_child(
    std::span<const std::byte> frame, std::span<const std::size_t> cuts,
    tr::mem::block_source_t& child_rx, tr::mem::block_source_t& router_default) {
    graph_t g;
    fwd_router_t router(g, {.rx = &router_default});
    fake_rope_link_t cli;
    fake_link_t up;
    (void)router.add_child("cli", cli, &child_rx);  // inbound link brings its own slab
    (void)router.add_child("up", up);               // forward child falls back to the default
    cli.inject(rope_split(frame, cuts));
    return std::move(up.sent());
}

/** @brief The oracle: route the identical `frame` contiguously (the span-cursor path). */
std::vector<std::vector<std::byte>> forward_contiguous(std::span<const std::byte> frame) {
    graph_t g;
    fwd_router_t router(g);
    fake_link_t cli;
    fake_link_t up;
    (void)router.add_child("cli", cli);
    (void)router.add_child("up", up);
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

    // Two cuts straddling both the /up segment record and the src PATH header.
    {
        const std::array<std::size_t, 2> cuts{6, 11};
        const auto got = forward_as_rope(frame, cuts);
        check(got == oracle, "a 3-link split across segment + header routes byte-identically");
    }

    // RFC-0018 FALSIFIER 5, named: a packed `[u8 len]` record STRADDLING a link boundary.
    //
    // The sweep above already covers this — every interior cut of a packed frame lands
    // inside a record for most of its range — but the RFC states the case explicitly
    // ("a `[u8 len]` record can straddle a link boundary exactly as a TLV header can"), and a
    // sweep that happens to include it does not SAY it. This case does: a deeper `dst` whose
    // records are cut at three places the packed walk is specifically exposed at — between a
    // length byte and its first payload byte, mid-payload, and exactly ON a length byte, which
    // is the one-byte "header" the rope cursor now has to stitch where it used to stitch four.
    {
        const std::vector<std::byte> deep =
            b_fwd(fwd_op_t::READ, b_path({"up", "aa", "bbbb", "sensor"}), b_path({"reply-ep"}));
        const auto deep_oracle = forward_contiguous(deep);
        check(deep_oracle.size() == 1, "control: the deep frame forwards contiguously");
        // Offsets into the frame: FWD hdr(4) + op TLV(5) + dst PATH hdr(4) = 13, so the dst
        // body starts at 13 and its records are 03'up' 02'aa' 04'bbbb' 06'sensor'.
        constexpr std::size_t kDstBody = 13;
        const std::array<std::size_t, 3> cuts{
            kDstBody + 1,      // between the first length byte and its payload
            kDstBody + 4 + 1,  // mid-payload of the SECOND record
            kDstBody + 6,      // exactly ON the third record's length byte
        };
        const auto got = forward_as_rope(deep, cuts);
        check(!deep_oracle.empty() && got == deep_oracle,
              "a packed record straddling a link boundary routes byte-identically (falsifier 5)");

        // And the whole-sweep form of the same claim over the deeper frame, so no single
        // boundary of a multi-record address is left unexercised.
        int deep_mismatches = 0;
        for (std::size_t cut = 1; cut < deep.size(); ++cut) {
            const std::array<std::size_t, 1> one{cut};
            if (forward_as_rope(deep, one) != deep_oracle) ++deep_mismatches;
        }
        check(deep_mismatches == 0,
              "every 2-link split of a four-record dst routes byte-identically");
    }

    // A BOUND dst over a multi-link rope (RFC-0024 §5): the frame that made the
    // fragmentation invariant observable.
    //
    // The rope arm's routing gate is `peek_fwd_dst`, which answers "does this frame carry an
    // address this node can DESCEND" — it requires a canonical PATH whose first child is a
    // NAME. A bound dst was then a `PATH_REF` (now a `PATH` of PAIRs), so the gate said no, and
    // before the fix the frame fell through to the control arm, where `peek_control` refuses a FWD:
    // the operation vanished with no reply and no drop anyone could name. It was NOT the §5.3
    // validation drop — the element was never even validated — and it happened to every bound
    // operation on every transport that scatter-delivers (ADR-0053 ④b), while the canonical
    // spelling of the same operation, split the same way, answered normally.
    //
    // The check is the router's own invariant, stated as an equality over splits: FRAGMENTING
    // A FRAME MUST NOT CHANGE WHETHER IT IS APPLIED. So the same bound frame is injected
    // contiguously and at EVERY interior split, and every one of them must answer the reply
    // the 1-link case answers — with the canonical spelling swept alongside as the control
    // that says the harness, not the fix, is what the bound arm is being measured against.
    {
        std::printf("A BOUND (PATH_REF) dst over a multi-link rope (RFC-0024 §5):\n");
        const auto reply_count = [](const std::vector<std::byte>& f,
                                    std::span<const std::size_t> cuts) {
            graph_t g;
            const auto temp = path_t::parse("/sensor/temp");
            tr::graph::vertex_handle_t v = g.register_vertex(*temp, role_t::STORED_VALUE);
            (void)g.write(v, make_value(b_value_u32(0x04D2u)));
            fwd_router_t router(g);
            fake_rope_link_t in;
            (void)router.add_child("in", in);
            in.inject(rope_split(f, cuts));
            return in.sent().size();
        };
        // The binding is minted from a graph shaped exactly like the one above, so the slot
        // it names is the slot the probe graph hands out.
        graph_t shape;
        const auto temp = path_t::parse("/sensor/temp");
        tr::graph::vertex_handle_t sv = shape.register_vertex(*temp, role_t::STORED_VALUE);
        const std::optional<tr::graph::vertex_slot_t> slot = shape.vertex_slot(sv);
        check(slot.has_value(), "the target vertex is bindable (the mint side answers)");
        const std::vector<std::byte> bound = b_fwd(
            fwd_op_t::READ, b_path_ref_one(slot->index, slot->generation), b_path({"reply-ep"}));
        const std::vector<std::byte> canon =
            b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"reply-ep"}));

        check(reply_count(bound, std::span<const std::size_t>{}) == 1,
              "contiguous: a bound READ answers exactly one reply (the ablation)");
        int bound_silent = 0;
        int canon_silent = 0;
        int swept = 0;
        for (std::size_t cut = 1; cut < bound.size(); ++cut) {
            const std::array<std::size_t, 1> cuts{cut};
            ++swept;
            if (reply_count(bound, cuts) != 1) ++bound_silent;
        }
        for (std::size_t cut = 1; cut < canon.size(); ++cut) {
            const std::array<std::size_t, 1> cuts{cut};
            if (reply_count(canon, cuts) != 1) ++canon_silent;
        }
        check(swept > 0, "swept every interior split boundary of the bound frame");
        check(canon_silent == 0, "control: the canonical spelling answers at every split");
        check(bound_silent == 0, "a bound READ answers at EVERY split, exactly as contiguous");

        // A 3-link split whose cuts straddle the PAIR's escape header and land mid-ELEMENT — the
        // shape a reassembling transport actually produces, and the one a header-stitching
        // bug would survive the 2-link sweep to break.
        const std::array<std::size_t, 2> mid{6, 12};
        check(reply_count(bound, mid) == 1, "a 3-link split through the element array answers");
    }

    // The EMPTY-op FWD (#870): the last frame the two hand-written ingress arms disposed of
    // differently, and the reason they are now ONE templated driver.
    //
    // Ingress classification was written twice — once for each cursor shape — and the copies
    // had drifted at their tails. The span arm concluded "FWD ⇒ terminus" unconditionally;
    // the rope arm asked `peek_fwd_op` one more time and fell through to the CONTROL sink
    // when it answered nullopt, where `peek_control` refuses a FWD and the frame was dropped.
    // Among the frames that reach that tail, the one with an OBSERVABLE reply is a BOUND
    // `dst` the peek accepts carrying an op VALUE `peek_fwd_op` refuses — an empty one — so a
    // resolvable bound READ was answered contiguously and VANISHED when the identical bytes
    // arrived as a multi-link rope. It is NOT the whole set: a FWD whose `dst` the peek
    // REFUSES (a PATH whose first child is not a NAME, or a kind-NONE `dst`) with an empty op
    // reached the same tail and was dropped there too, and now shares the span arm's
    // disposition. The disposition kept is the span arm's: a FWD-classified frame is
    // data-plane, so it goes to the terminus and never to the control switch — for every
    // frame that reaches the tail, not only the resolvable one.
    //
    // Stated, as the bound block above states it, as an equality over splits — the router's
    // own invariant: FRAGMENTING A FRAME MUST NOT CHANGE WHETHER IT IS APPLIED.
    {
        std::printf("A FWD with an EMPTY op VALUE (#870) — one disposition on both arms:\n");
        const auto reply_count = [](const std::vector<std::byte>& f,
                                    std::span<const std::size_t> cuts) {
            graph_t g;
            const auto temp = path_t::parse("/sensor/temp");
            tr::graph::vertex_handle_t v = g.register_vertex(*temp, role_t::STORED_VALUE);
            (void)g.write(v, make_value(b_value_u32(0x04D2u)));
            fwd_router_t router(g);
            fake_rope_link_t in;
            (void)router.add_child("in", in);
            in.inject(rope_split(f, cuts));
            return in.sent().size();
        };
        graph_t shape;
        const auto temp = path_t::parse("/sensor/temp");
        tr::graph::vertex_handle_t sv = shape.register_vertex(*temp, role_t::STORED_VALUE);
        const std::optional<tr::graph::vertex_slot_t> slot = shape.vertex_slot(sv);
        check(slot.has_value(), "the target vertex is bindable (the mint side answers)");
        // A BOUND dst, because that is the only `dst` form whose classification reaches the
        // divergent tail: a canonical PATH concludes "terminus" one branch earlier, on both
        // arms, and so never showed the split.
        const std::vector<std::byte> empty_op =
            b_fwd_no_op(b_path_ref_one(slot->index, slot->generation), b_path({"reply-ep"}));

        check(reply_count(empty_op, std::span<const std::size_t>{}) == 1,
              "contiguous: an empty-op bound READ is resolved at the terminus and answers");
        int silent = 0;
        int swept = 0;
        for (std::size_t cut = 1; cut < empty_op.size(); ++cut) {
            const std::array<std::size_t, 1> cuts{cut};
            ++swept;
            if (reply_count(empty_op, cuts) != 1) ++silent;
        }
        check(swept > 0, "swept every interior split boundary of the empty-op frame");
        check(silent == 0,
              "and EVERY multi-link split answers it too — no arm sends it to the "
              "control switch");
        std::vector<std::size_t> every_byte;
        for (std::size_t i = 1; i < empty_op.size(); ++i) every_byte.push_back(i);
        check(reply_count(empty_op, every_byte) == 1, "one link per byte answers identically");
    }

    // A bound FORWARDER hop over a multi-link rope (RFC-0024 §3.4/§5, car 3). The terminus
    // arm above proves a bound frame is APPLIED at every split; this proves the other half —
    // that a bound frame with a residual longer than one element is FORWARDED at every split,
    // with the same bytes on the egress as the contiguous route. The two together are the
    // fragmentation invariant for the whole bound form: splitting a frame changes neither
    // whether it is applied nor what the next hop receives.
    {
        std::printf("A BOUND FORWARDER hop over a multi-link rope (RFC-0024 §3.4):\n");
        // `/up` is the connection vertex of the child named "up" — a child's mount run IS its
        // connection vertex's canonical key, which is the whole of the element→link join.
        const auto forward_bound = [](const std::vector<std::byte>& f,
                                      std::span<const std::size_t> cuts) {
            graph_t g;
            (void)g.register_vertex(path_t("/up"), role_t::STORED_VALUE);
            fwd_router_t router(g);
            fake_rope_link_t cli;
            fake_link_t up;
            (void)router.add_child("cli", cli);
            (void)router.add_child("up", up);
            cli.inject(rope_split(f, cuts));
            return std::move(up.sent());
        };
        graph_t shape;
        const tr::graph::vertex_handle_t uv =
            shape.register_vertex(path_t("/up"), role_t::STORED_VALUE);
        const std::optional<tr::graph::vertex_slot_t> hop = shape.vertex_slot(uv);
        check(hop.has_value(), "the connection vertex is bindable");
        // Two elements: this node's own hop, and the terminus's reference to the target. The
        // second is opaque here — only the next host can read it, which is the point.
        const tr::wire::path_ref_element_t els[2] = {
            {.index = hop->index, .generation = hop->generation},
            {.index = 0x0000BEEFu, .generation = 7u}};
        const std::vector<std::byte> ref = b_path_pairs(els);
        const std::vector<std::byte> bfwd =
            b_fwd(fwd_op_t::READ, ref, b_path({"reply-ep"}), {}, b_value_u32(9));

        const auto boracle = forward_bound(bfwd, std::span<const std::size_t>{});
        check(boracle.size() == 1, "contiguous: the bound forward hop egresses exactly once");
        if (boracle.size() == 1) {
            const auto dec = tr::wire::decode(boracle[0]);
            const bool one_pair = dec && dec->children.size() >= 3 &&
                                  dec->children[1].type == type_t::PATH &&
                                  dec->children[1].payload.size() == tr::wire::kPathPairRecordBytes;
            check(one_pair, "the egress dst is a PATH with ONE PAIR — this hop consumed its own");
            if (one_pair) {
                check(tr::wire::path_pair_at(dec->children[1].payload, 0) == els[1],
                      "and it is the NEXT host's element, untouched");
                check(tr::wire::equal(dec->children[2],
                                      *tr::wire::decode(b_path({"cli", "reply-ep"}))),
                      "src grew by the inbound mount exactly as a canonical hop grows it");
            }
        }
        int bmismatch = 0;
        for (std::size_t cut = 1; cut < bfwd.size(); ++cut) {
            const std::array<std::size_t, 1> cuts{cut};
            if (forward_bound(bfwd, cuts) != boracle) ++bmismatch;
        }
        check(bmismatch == 0, "every 2-link split forwards byte-identically to the contiguous hop");
        std::vector<std::size_t> every_byte;
        for (std::size_t i = 1; i < bfwd.size(); ++i) every_byte.push_back(i);
        check(forward_bound(bfwd, every_byte) == boracle,
              "one link per byte forwards byte-identically (the element itself straddles links)");
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
        (void)router.add_child("in", in);  // reply goes back over the inbound link
        const std::uint32_t kWritten = 0x0BADF00Du;
        const std::vector<std::byte> wframe = b_fwd(
            fwd_op_t::WRITE, b_path({"sensor"}), b_path({"reply-ep"}), {}, b_value_u32(kWritten));
        const std::array<std::size_t, 1> cuts{wframe.size() / 2};
        in.inject(rope_split(wframe, cuts));
        const auto stored = g.read(v);
        check(stored.has_value(), "/sensor readable after a multi-link rope WRITE terminus");
        if (stored) {
            const auto inner = tr::wire::decode((*stored)->only());
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
        (void)router.add_child("in", in);

        const std::uint32_t kWritten = 0x0C0FFEE0u;
        const std::vector<std::byte> plain = b_fwd(fwd_op_t::WRITE, b_path({"sensor"}),
                                                   b_path({"reply-ep"}), {}, b_value_u32(kWritten));
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
            const auto inner = tr::wire::decode((*stored)->only());
            check(inner && inner->type == type_t::VALUE && inner->payload.size() == 4 &&
                      tr::detail::load_le<std::uint32_t>(inner->payload) == kWritten,
                  "LKV updated to the CRC-verified value");
        }
    }

    // #596: the rope forward hop's egress iov is the one allocation on this path whose
    // ELEMENT COUNT a peer chooses — one sub-span per link crossed, per region. It used to
    // be a `std::pmr::vector`, so exhaustion threw, and on -fno-exceptions that is abort():
    // a peer-reachable reboot behind no ACL. It now draws from the failable seam and refuses
    // by value. These cases run the SAME maximally fragmented rope through three seams.
    {
        std::printf("Rope forward-hop egress iov is failable, not throwing (#596):\n");
        // One link per byte — the largest sub-span count this frame can produce.
        std::vector<std::size_t> every_byte;
        for (std::size_t i = 1; i < frame.size(); ++i) every_byte.push_back(i);

        // A seam that serves nothing: the very first growth is refused.
        const auto starved = forward_as_rope_with(frame, every_byte, tr::mem::null_source());
        check(starved.empty(), "a starved iov seam DROPS the forward frame (no abort, no send)");

        // Not a partial send: a truncated FWD on the wire would be worse than none, so the
        // check above is specifically that NOTHING was emitted, not that something short was.
        check(starved.size() == 0, "and emits no truncated frame either");

        // A bounded seam with room forwards byte-identically to the contiguous oracle. Sized
        // generously on purpose: `block_array_t` grows 8 -> 16 -> 32 -> ..., and a bump source
        // never reclaims the block it just outgrew, so the peak draw is the SUM of the
        // capacities, not the last one (see mem_source.hpp's scope-lifetime warning).
        std::array<std::byte, 8192> slab{};
        tr::mem::bump_source_t bounded{slab, tr::mem::null_source()};
        check(forward_as_rope_with(frame, every_byte, bounded) == oracle,
              "a bounded-but-sufficient iov seam forwards byte-identically to the oracle");

        // And the default seam is unchanged — this is the path every existing check above ran.
        check(forward_as_rope_with(frame, every_byte, tr::mem::heap_source()) == oracle,
              "the default heap seam is byte-identical (no behaviour change when it fits)");
    }

    // ── ADR-0067 §3: the inbound child's OWN source is the one that serves ──────────────
    //
    // Not a micro-optimization: a pool_source_t shared across receive threads was measured
    // at ~1/15 of its single-thread rate on 12 cores (ADR-0060 erratum 1), so "which source"
    // is a correctness-of-topology question. Without a test, a refactor that reverts to the
    // router's shared `rx_` would keep every other assertion in this file green.
    {
        std::printf("\nper-child failable source (ADR-0067 3):\n");
        std::vector<std::size_t> every_byte;
        for (std::size_t i = 1; i < frame.size(); ++i) every_byte.push_back(i);

        counting_source_t child{"child"};
        counting_source_t shared{"router-default"};
        const auto out = forward_as_rope_per_child(frame, every_byte, child, shared);

        check(out == oracle, "a per-child source forwards byte-identically to the oracle");
        check(child.served > 0, "the inbound child's OWN source served the rope iov");
        check(shared.served == 0,
              "and the router's shared default was never touched on that frame");

        // The fallback still works, so this is additive: a child given no source of its own
        // draws from the router's, which is what every existing call site relies on.
        counting_source_t only_default{"router-default"};
        graph_t g2;
        fwd_router_t r2(g2, {.rx = &only_default});
        fake_rope_link_t cli2;
        fake_link_t up2;
        (void)r2.add_child("cli", cli2);
        (void)r2.add_child("up", up2);
        cli2.inject(rope_split(frame, every_byte));
        check(std::move(up2.sent()) == oracle, "a child with no source of its own still routes");
        check(only_default.served > 0, "drawing from the router's default, as before");
    }

    return tr::testing::summary("fwd_rope_forward");
}
