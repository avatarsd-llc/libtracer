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

#include "libtracer/byteorder.hpp"
#include "libtracer/mem_source.hpp"
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
/** @brief A one-element `PATH_REF` dst — the BOUND spelling of an address (RFC-0024 §4). */
std::vector<std::byte> b_path_ref_one(std::uint32_t index, std::uint32_t generation) {
    const tr::wire::path_ref_element_t e{.index = index, .generation = generation};
    std::vector<std::byte> out;
    (void)tr::wire::emit_path_ref(out, std::span<const tr::wire::path_ref_element_t>(&e, 1));
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

/**
 * @brief A backend whose bytes are ordinary host memory but whose segments are TAGGED
 *        `DEVICE` — the vehicle for driving `rope_t::materialize()` to refuse.
 *
 * `rope_t::flatten` returns an EMPTY view for a rope that is not `all_host()`, because a
 * CPU `memcpy` of device bytes would fault (ADR-0024). That refusal — not only a heap OOM
 * — is a way `materialize()` hands back nothing, and it is the one a test can trigger
 * deterministically: the COMPACT ingress arm materializes through the DEFAULT
 * `mem::heap_backend()`, so no failing backend can be injected there.
 *
 * The shape is the sanctioned one, not a contrivance. `mem_space_t`'s own contract says a
 * DEVICE segment "may back only an opaque VALUE payload, with the header/trailer kept in a
 * HOST segment (a heterogeneous host+device rope)" — which is exactly a COMPACT whose
 * payload VALUE body arrived in device memory. `peek_control` reads only headers, all of
 * which stay HOST here, so the frame parses normally and reaches the materialize.
 */
class device_tag_backend_t final : public tr::mem::mem_backend_t {
   public:
    device_tag_backend_t() noexcept : mem_backend_t("test_device_tag") {}

    tr::view::segment_t* alloc(std::size_t size,
                               tr::mem::alloc_hint_t = tr::mem::alloc_hint_t::NONE) override {
        auto* raw = static_cast<std::byte*>(::operator new(size, std::nothrow));
        if (raw == nullptr) return nullptr;
        auto* seg = new (std::nothrow) tr::view::segment_t(this, std::span<std::byte>(raw, size));
        if (seg == nullptr) {
            ::operator delete(raw);
            return nullptr;
        }
        return seg;
    }

    void destroy(tr::view::segment_t* seg) noexcept override {
        ::operator delete(seg->bytes.data());
        delete seg;
    }

    [[nodiscard]] tr::mem::mem_space_t space() const noexcept override {
        return tr::mem::mem_space_t::DEVICE;
    }
};

device_tag_backend_t g_device_backend;

/** @brief `make_value`'s twin over @ref device_tag_backend_t — a DEVICE-tagged link. */
tr::view::view_t make_device_value(std::span<const std::byte> bytes) {
    tr::view::segment_t* seg = g_device_backend.alloc(bytes.size());
    if (seg == nullptr) return tr::view::view_t{};
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(tr::view::segment_ptr_t::adopt(seg));
}

/**
 * @brief `rope_split` with the FINAL link allocated DEVICE-tagged: header bytes stay
 *        CPU-addressable, the trailing payload body does not.
 */
tr::view::rope_t rope_split_device_tail(std::span<const std::byte> bytes, std::size_t cut) {
    tr::view::rope_t r;
    if (cut > 0) r.append(make_value(bytes.subspan(0, cut)));
    if (cut < bytes.size()) r.append(make_device_value(bytes.subspan(cut)));
    return r;
}

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

/**
 * @brief The label carried by an ADVERTISE frame — its first child, a 2-byte opaque VALUE.
 *
 * Needed because labels are PER-LINK: when this node re-advertises downstream it allocates a
 * FRESH label for that link rather than reusing the inbound one, so a NACK fixture that reuses
 * the inbound label looks up a route that was never bound and gets the silent return.
 */
[[nodiscard]] std::uint16_t advertise_label(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->children.empty() || dec->children[0].payload.size() < 2) return 0;
    return tr::detail::load_le<std::uint16_t>(dec->children[0].payload);
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
    /**
     * @brief Records every send, because a rope link is not always inbound-only.
     *
     * `on_nack` re-advertises back on the link the NACK ARRIVED on, so for the self-heal the
     * inbound link is also the egress. Discarding sends here made that whole path unobservable
     * — the test could not tell a served NACK from a dropped one.
     */
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
    router.add_child("cli", cli);  // inbound (rope) link
    router.add_child("up", up);    // the dst-resolved forward child
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
    fwd_router_t router(g, std::pmr::get_default_resource(), &rx);
    fake_rope_link_t cli;
    fake_link_t up;
    router.add_child("cli", cli);
    router.add_child("up", up);
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
    fwd_router_t router(g, std::pmr::get_default_resource(), &router_default);
    fake_rope_link_t cli;
    fake_link_t up;
    router.add_child("cli", cli, &child_rx);  // inbound link brings its own slab
    router.add_child("up", up);               // forward child falls back to the default
    cli.inject(rope_split(frame, cuts));
    return std::move(up.sent());
}

/** @brief The oracle: route the identical `frame` contiguously (the span-cursor path). */
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

    // A BOUND dst over a multi-link rope (RFC-0024 §5): the frame that made the
    // fragmentation invariant observable.
    //
    // The rope arm's routing gate is `peek_fwd_dst`, which answers "does this frame carry an
    // address this node can DESCEND" — it requires a canonical PATH whose first child is a
    // NAME. A bound dst is a `PATH_REF`, so the gate says no, and before the fix the frame
    // fell through to the control arm, where `peek_control` refuses a FWD: the operation
    // vanished with no reply and no drop anyone could name. It was NOT the §5.3 validation
    // drop — the element was never even validated — and it happened to every bound operation
    // on every transport that scatter-delivers (ADR-0053 ④b), while the canonical spelling
    // of the same operation, split the same way, answered normally.
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
            router.add_child("in", in);
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

        // A 3-link split whose cuts straddle the PATH_REF header and land mid-ELEMENT — the
        // shape a reassembling transport actually produces, and the one a header-stitching
        // bug would survive the 2-link sweep to break.
        const std::array<std::size_t, 2> mid{6, 12};
        check(reply_count(bound, mid) == 1, "a 3-link split through the element array answers");
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
            router.add_child("cli", cli);
            router.add_child("up", up);
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
        std::vector<std::byte> ref;
        (void)tr::wire::emit_path_ref(ref, std::span<const tr::wire::path_ref_element_t>(els));
        const std::vector<std::byte> bfwd =
            b_fwd(fwd_op_t::READ, ref, b_path({"reply-ep"}), b_value_u32(9));

        const auto boracle = forward_bound(bfwd, std::span<const std::size_t>{});
        check(boracle.size() == 1, "contiguous: the bound forward hop egresses exactly once");
        if (boracle.size() == 1) {
            const auto dec = tr::wire::decode(boracle[0]);
            check(dec && dec->children.size() >= 3 && dec->children[1].type == type_t::PATH_REF &&
                      tr::wire::path_ref_element_count(dec->children[1].payload.size()) == 1,
                  "the egress dst is a PATH_REF with ONE element — this hop consumed its own");
            if (dec && dec->children.size() >= 3 && dec->children[1].type == type_t::PATH_REF &&
                tr::wire::path_ref_element_count(dec->children[1].payload.size()) == 1) {
                check(tr::wire::path_ref_element_at(dec->children[1].payload, 0) == els[1],
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
        router.add_child("in", in);  // reply goes back over the inbound link
        const std::uint32_t kWritten = 0x0BADF00Du;
        const std::vector<std::byte> wframe =
            b_fwd(fwd_op_t::WRITE, b_path({"sensor"}), b_path({"reply-ep"}), b_value_u32(kWritten));
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
            const auto inner = tr::wire::decode((*stored)->only());
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
            const auto inner = tr::wire::decode((*stored)->only());
            check(inner && inner->type == type_t::VALUE && inner->payload.size() == 4 &&
                      tr::detail::load_le<std::uint32_t>(inner->payload) == kVal,
                  "LKV updated to the label-compacted value (payload sub-rope decoded)");
        }
    }

    // `on_control_rope`'s `if (!frame.all_host()) return;` is LOAD-BEARING, and nothing
    // asserted it. This pins it, and pins what it costs to lose.
    //
    // Downstream of that guard, nothing else stops a heterogeneous rope. `on_control_rope`
    // materializes the COMPACT payload sub-rope and hands the result to `on_compact`
    // WITHOUT checking it; `rope_t::flatten` returns an EMPTY view for a rope that is not
    // `all_host()` (a CPU memcpy of device bytes would fault — ADR-0024); and
    // `view::over_bytes` maps an empty span to an ENGAGED-empty optional by design ("a
    // legitimately-empty input"), so `on_compact`'s `if (!payload_view) return;` does not
    // fire either. The empty rope reaches `graph_.write`, which stores it and reports
    // success — the subscriber's last-known value replaced by nothing.
    //
    // Verified by ablation, not by reading: deleting the `all_host` line makes the final
    // check below fail with the LKV holding an empty value. Restoring it passes.
    //
    // The assertion is on the value SURVIVING, not on the absence of a write: asserting
    // "nothing happened" would pass just as well if the frame never reached the arm at
    // all. The preceding good delivery is what makes the survival meaningful.
    {
        std::printf("heterogeneous (host+device) COMPACT rope is dropped at the door:\n");
        graph_t g;
        const auto sensor = path_t::parse("/sensor");
        tr::graph::vertex_handle_t v = g.register_vertex(*sensor, role_t::STORED_VALUE);
        fwd_router_t router(g);
        fake_rope_link_t in;
        router.add_child("in", in);
        const std::uint16_t kLabel = 0x0044u;
        in.inject(rope_split(tr::net::encode_advertise(kLabel, b_path({"sensor"})),
                             std::array<std::size_t, 0>{}));

        // A good all-HOST delivery first — proves the vehicle and seeds the value that the
        // un-flattenable frame must not be able to erase.
        const std::uint32_t kGood = 0xA5A5A5A5u;
        in.inject(rope_split(tr::net::encode_compact(kLabel, b_value_u32(kGood)),
                             std::array<std::size_t, 1>{4}));
        {
            const auto stored = g.read(v);
            check(stored.has_value(), "the good COMPACT landed (vehicle works)");
            if (stored) {
                const auto inner = tr::wire::decode((*stored)->only());
                check(inner && inner->payload.size() == 4 &&
                          tr::detail::load_le<std::uint32_t>(inner->payload) == kGood,
                      "LKV seeded with the good value");
            }
        }

        // Now the same frame shape with the payload VALUE's 4 BODY bytes in a DEVICE
        // segment: headers stay HOST so `peek_control` parses it, but the payload sub-rope
        // is heterogeneous, so `materialize()` refuses and returns empty.
        const std::uint32_t kPoison = 0xDEADBEEFu;
        const std::vector<std::byte> comp = tr::net::encode_compact(kLabel, b_value_u32(kPoison));
        const tr::view::rope_t het = rope_split_device_tail(comp, comp.size() - 4);
        check(het.link_count() == 2 && !het.all_host(),
              "the fixture really is a heterogeneous host+device rope");
        check(het.subrope(0, comp.size()).materialize().empty(),
              "materialize() really does refuse this rope — the empty view the arm would "
              "otherwise apply");
        in.inject(het);

        const auto after = g.read(v);
        check(after.has_value(), "the vertex still holds a value");
        if (after) {
            const auto inner = tr::wire::decode((*after)->only());
            check(inner && inner->payload.size() == 4 &&
                      tr::detail::load_le<std::uint32_t>(inner->payload) == kGood,
                  "the heterogeneous COMPACT did NOT overwrite the LKV with an empty value");
        }
    }

    // A corrupt-CRC COMPACT must be dropped on the ROPE control arm too. `compact_cache_test`
    // pins this for the span arm and says in its own comment that "nothing else in the suite
    // would notice if that argument were dropped" — which was exactly true of the rope arm,
    // where VERIFY was never passed at all. Same frame, same corruption: fragmenting it must
    // not change whether it is applied.
    {
        std::printf("corrupt-CRC COMPACT is dropped on the ROPE arm too (verify-before-apply):\n");
        graph_t g;
        const auto sensor = path_t::parse("/sensor");
        tr::graph::vertex_handle_t v = g.register_vertex(*sensor, role_t::STORED_VALUE);
        fwd_router_t router(g);
        fake_rope_link_t in;
        router.add_child("in", in);
        const std::uint16_t kLabel = 0x0043u;
        in.inject(rope_split(tr::net::encode_advertise(kLabel, b_path({"sensor"})),
                             std::array<std::size_t, 0>{}));

        // Re-emit a COMPACT carrying a whole-frame CRC-32C trailer.
        const std::uint32_t kGood = 0x0BADF00Du;
        const std::vector<std::byte> plain = tr::net::encode_compact(kLabel, b_value_u32(kGood));
        tr::wire::tlv_t crc_tlv = *tr::wire::decode(plain);
        crc_tlv.opt.cr = true;
        const std::vector<std::byte> crc_frame = tr::wire::encode(crc_tlv);
        check(crc_frame.size() == plain.size() + 4, "the CRC frame carries a 4-byte trailer");

        // Split so the corrupted byte and the trailer land in DIFFERENT links — the shape a
        // contiguous arm cannot produce, and the one a stitching cursor has to get right.
        const std::array<std::size_t, 1> cuts{crc_frame.size() - 2};

        // Intact-with-CRC must deliver, or the drop below proves nothing.
        in.inject(rope_split(crc_frame, cuts));
        const auto good = g.read(v);
        check(good.has_value(), "an intact CRC-carrying COMPACT still delivers as a rope");

        // Now corrupt a BODY byte under that trailer: grammar stays valid, CRC breaks.
        std::vector<std::byte> corrupt = crc_frame;
        corrupt[corrupt.size() - 5] ^= std::byte{0xFF};
        in.inject(rope_split(corrupt, cuts));
        const auto after = g.read(v);
        check(after.has_value(), "the LKV still holds a value");
        if (after) {
            const auto inner = tr::wire::decode((*after)->only());
            check(inner && inner->payload.size() == 4 &&
                      tr::detail::load_le<std::uint32_t>(inner->payload) == kGood,
                  "a corrupt-CRC multi-link COMPACT is DROPPED — the LKV holds the last good "
                  "value");
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
        fwd_router_t r2(g2, std::pmr::get_default_resource(), &only_default);
        fake_rope_link_t cli2;
        fake_link_t up2;
        r2.add_child("cli", cli2);
        r2.add_child("up", up2);
        cli2.inject(rope_split(frame, every_byte));
        check(std::move(up2.sent()) == oracle, "a child with no source of its own still routes");
        check(only_default.served > 0, "drawing from the router's default, as before");
    }

    // HANDLE_NACK over a multi-link rope (#667). The gap this closes is not "one more opcode":
    // a control frame misrouted into the FWD routing arm is SILENT — no error, no egress, no
    // counter — so "nothing was sent" cannot tell correct handling from the bug. Ablating the
    // rope routing gate was measured to take nack from 1 to 0 with the whole suite still green.
    //
    // The observable is an ADVERTISE back on the link the NACK ARRIVED on, not a stale-label
    // callback: `on_nack` looks up `egress_route(inbound, label)` and returns SILENTLY when no
    // route is bound. So the fixture must bind one first, which it does the way the wire does —
    // an inbound ADVERTISE naming a child makes this node re-advertise downstream, and THAT is
    // what records the egress route on the downstream link.
    {
        std::printf("HANDLE_NACK self-heal over a multi-link rope (#667):\n");
        constexpr std::uint16_t kLabel = 0x2468u;

        // The oracle: the same NACK routed contiguously, on a fixture built the same way.
        const auto build = [](auto& router, auto& cli, auto& up) {
            router.add_child("cli", cli);
            router.add_child("up", up);
            // Binds the egress route for ("up", kLabel) by making this node re-advertise.
            cli.inject(tr::net::encode_advertise(kLabel, b_path({"up", "sensor"})));
        };

        std::vector<std::vector<std::byte>> oracle;
        std::uint16_t down_label = 0;
        {
            graph_t g;
            fwd_router_t router(g);
            fake_link_t cli;
            fake_link_t up;
            build(router, cli, up);
            check(up.sent().size() == 1, "the fixture's ADVERTISE bound an egress route on 'up'");
            down_label = up.sent().empty() ? 0 : advertise_label(up.sent()[0]);
            check(down_label != 0, "the downstream ADVERTISE carries the link's own label");
            up.sent().clear();
            up.inject(tr::net::encode_handle_nack(down_label));
            oracle = std::move(up.sent());
        }
        check(oracle.size() == 1, "a contiguous HANDLE_NACK re-advertises exactly one frame");
        if (!oracle.empty()) {
            const auto dec = tr::wire::decode(oracle[0]);
            check(dec && dec->type == type_t::ADVERTISE,
                  "the self-heal emits an ADVERTISE, on the link the NACK arrived on");
        }

        // Every interior split of the NACK frame must reproduce it byte-for-byte.
        const std::vector<std::byte> nack = tr::net::encode_handle_nack(down_label);
        int checked = 0;
        int mismatches = 0;
        for (std::size_t cut = 1; cut < nack.size(); ++cut) {
            graph_t g;
            fwd_router_t router(g);
            fake_link_t cli;
            fake_rope_link_t up;  // rope-delivering AND recording — the NACK arrives here
            router.add_child("cli", cli);
            router.add_child("up", up);
            cli.inject(tr::net::encode_advertise(kLabel, b_path({"up", "sensor"})));
            up.sent().clear();
            const std::size_t cuts[] = {cut};
            up.inject(rope_split(nack, cuts));
            ++checked;
            if (up.sent() != oracle) ++mismatches;
        }
        check(checked > 0, "swept every interior split of the NACK frame");
        check(mismatches == 0, "every 2-link NACK split self-heals byte-identically");

        // And the adversarial extreme: one link per byte.
        {
            graph_t g;
            fwd_router_t router(g);
            fake_link_t cli;
            fake_rope_link_t up;
            router.add_child("cli", cli);
            router.add_child("up", up);
            cli.inject(tr::net::encode_advertise(kLabel, b_path({"up", "sensor"})));
            up.sent().clear();
            std::vector<std::size_t> every_byte;
            for (std::size_t i = 1; i < nack.size(); ++i) every_byte.push_back(i);
            up.inject(rope_split(nack, every_byte));
            check(up.sent() == oracle, "one-link-per-byte NACK rope self-heals byte-identically");
        }

        // #667's unconfirmed rider, pinned by #715 and RULED by #716. `clear_link("up")` drops
        // that link's whole table, and `on_nack` re-advertises from exactly the `egress_route`
        // the table held — so a NACK arriving back on "up" after a (re)connect still has
        // nothing to answer from, and sending nothing DOWNSTREAM remains correct. What #716
        // changed is the other direction, which this fixture also holds: the ingress binding
        // stored under "cli" pointed its downstream half at "up", so `clear_link` now sweeps it
        // too. The recovery is therefore UPSTREAM — the client's next COMPACT misses and draws
        // the ordinary stale-label HANDLE_NACK, which prompts it to re-advertise — rather than
        // "only on a fresh advertise" someone else has to think to send. The end-to-end cascade
        // is proven in `fwd_reconnect_selfheal_test`; both legs are pinned here.
        {
            graph_t g;
            fwd_router_t router(g);
            fake_link_t cli;
            fake_rope_link_t up;
            router.add_child("cli", cli);
            router.add_child("up", up);
            cli.inject(tr::net::encode_advertise(kLabel, b_path({"up", "sensor"})));
            const std::uint16_t lbl = up.sent().empty() ? 0 : advertise_label(up.sent()[0]);
            router.clear_link("up");  // what a transport calls on (re)connect
            up.sent().clear();
            cli.sent().clear();
            std::vector<std::size_t> every_byte;
            for (std::size_t i = 1; i < nack.size(); ++i) every_byte.push_back(i);
            up.inject(rope_split(tr::net::encode_handle_nack(lbl), every_byte));
            check(up.sent().empty(),
                  "after clear_link a NACK arriving back on the cleared link still sends NOTHING "
                  "downstream — the route it would re-advertise from is the one clear_link "
                  "erased, and re-advertising into a link that just reconnected would be wrong");
            // The #716 half: the cross-link binding went with it, so the UPSTREAM is told.
            check(router.handles().ingress_count() == 0,
                  "clear_link also swept the \"cli\" ingress binding whose downstream half "
                  "crossed \"up\" (#716) — the stale out-label cannot be forwarded any more");
            cli.inject(tr::net::encode_compact(kLabel, b_value_u32(0xFEEDBEEFu)));
            const auto back = cli.sent().size() == 1 ? tr::wire::decode(cli.sent()[0])
                                                     : decltype(tr::wire::decode(cli.sent()[0])){};
            check(cli.sent().size() == 1 && back.has_value() && back->type == type_t::HANDLE_NACK,
                  "and the client's next COMPACT draws a HANDLE_NACK upstream, which is what "
                  "makes it re-advertise (the origin learns, instead of streaming into a hole)");
        }

        // The silent-return leg, asserted so it cannot be mistaken for the bug it resembles:
        // with NO egress route bound, on_nack returns before sending, and that is CORRECT.
        {
            graph_t g;
            fwd_router_t router(g);
            fake_link_t cli;
            fake_rope_link_t up;
            router.add_child("cli", cli);
            router.add_child("up", up);
            std::vector<std::size_t> every_byte;
            for (std::size_t i = 1; i < nack.size(); ++i) every_byte.push_back(i);
            up.inject(rope_split(nack, every_byte));
            check(up.sent().empty(),
                  "a NACK for an unbound label sends nothing — the silent "
                  "return is by design, and is why the bound case above is "
                  "the assertion that has teeth");
        }
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
