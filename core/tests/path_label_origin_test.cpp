/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0027 origin car — ADOPT the minted `src`, SPEND it as `dst`, FALL BACK on `NOT_FOUND`.
 *
 * `path_label_forward_test.cpp` binds the HOP: a minting forwarder rewrites its own local part
 * of a reply's `src`, a label turns back into the link it was minted for, a stale one answers
 * `tr::path::not_found`. What it says about itself in its own header is that it pins *"the hop's
 * emission, not the round trip"* — and erratum 2 and erratum 4 both end on the same open item:
 * the origin-side adoption of that minted `src`. This file is that round trip, closed against
 * the production `fwd_router_t` on BOTH ends:
 *
 *     cli ──(the origin: adopt, then spend)──▶ A ──(child "up")──▶ B
 *
 * The origin is a `fwd_router_t` with a child and NO label table: the ruling (2026-08-24) is
 * that an origin caches the handed spelling and stands up no mint table of its own, so its own
 * first-hop local part is prepended as LITERAL segments and the cached spelling is mixed by
 * design (§5.2, §6.3). Every claim below is asserted through the public origin surface —
 * `adopt_path_label`, `label_dispatch`, `fall_back_on_label_refusal` — and the frames it produces
 * are fed to a real minting hop, so what is bound here is what a deployment does.
 *
 * `tests/conformance/vectors/v1/fwd/fwd-label-stale/description.md` says of its bytes: *"these
 * are the bytes an origin sends after caching the spelling `fwd/fwd-label-mint-reply` came back
 * with"*. Until this car those bytes were hand-built by a test. Section 3 below emits them from
 * the origin and pins them byte-exact.
 */
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/error.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_element.hpp"
#include "libtracer/path_label.hpp"
#include "libtracer/path_label_table.hpp"
#include "libtracer/tlv_emit.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::path_label_table_t;
using tr::testing::b_fwd_raw_op;
using tr::testing::b_fwd_reply;
using tr::testing::bytes_t;
using tr::testing::check;
using tr::wire::path_element_at;
using tr::wire::path_element_kind_t;
using tr::wire::path_label_t;

/** @brief The origin's own first hop — the one hop no peer ever sees (§4.1). */
constexpr std::string_view kOriginLink = "net/uplink/a";
/** @brief A's inbound child (the origin) and A's outbound child (B), as A names them. */
constexpr std::string_view kInLink = "net/downlink/cli";
constexpr std::string_view kOutLink = "net/uplink/b";

constexpr std::uint8_t kRead = 0x00;

/** @brief A `PATH` TLV over @p segs, packed-record body (RFC-0018). */
bytes_t b_path(std::initializer_list<std::string_view> segs) {
    bytes_t body;
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    bytes_t out;
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{}, body);
    return out;
}

/** @brief The packed BODY of a `PATH` over @p segs — no TLV envelope. */
bytes_t b_path_body(std::initializer_list<std::string_view> segs) {
    bytes_t body;
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    return body;
}

/** @brief A `PATH` TLV whose FIRST element is @p label and whose rest is @p segs. */
bytes_t b_path_labelled(path_label_t label, std::initializer_list<std::string_view> segs) {
    bytes_t body;
    (void)tr::wire::emit_path_label(body, label);
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    bytes_t out;
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{}, body);
    return out;
}

/** @brief A `VALUE` TLV carrying one little-endian `u32`. */
bytes_t b_value_u32(std::uint32_t v) {
    std::array<std::byte, 4> b{};
    for (std::size_t i = 0; i < 4; ++i) b[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    bytes_t out;
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{}, b);
    return out;
}

/** @brief A `STATUS { ERROR { VALUE u16 } }` payload — `assemble_error_reply`'s shape by hand. */
bytes_t b_error_status(tr::wire::err_t code) {
    const auto raw = static_cast<std::uint16_t>(code);
    const std::array<std::byte, 2> id{static_cast<std::byte>(raw & 0xFFu),
                                      static_cast<std::byte>(raw >> 8)};
    bytes_t value;
    tr::wire::emit_tlv(value, tr::wire::type_t::VALUE, tr::wire::opt_t{}, id);
    bytes_t err;
    tr::wire::emit_tlv(err, tr::wire::type_t::ERROR, tr::wire::opt_t{.pl = true}, value);
    bytes_t status;
    tr::wire::emit_tlv(status, tr::wire::type_t::STATUS, tr::wire::opt_t{.pl = true}, err);
    return status;
}

/** @brief A transport that records every frame handed to it — the egress under assertion. */
class span_sink_t final : public tr::net::transport_t {
   public:
    std::vector<bytes_t> sent; /**< @brief Frames this link was asked to send, in order. */
    void send(std::span<const std::byte> f) override { sent.emplace_back(f.begin(), f.end()); }
    void send(std::span<const std::span<const std::byte>> iov) override {
        bytes_t joined;
        for (const std::span<const std::byte> s : iov)
            joined.insert(joined.end(), s.begin(), s.end());
        sent.push_back(std::move(joined));
    }
};

/** @brief The bytes of conformance vector @p case_dir's `input.bin`. */
bytes_t vector_bytes(std::string_view case_dir) {
    const std::filesystem::path p =
        std::filesystem::path{LIBTRACER_VECTORS_DIR} / case_dir / "input.bin";
    std::ifstream in(p, std::ios::binary);
    check(in.good(), "the conformance vector's input.bin opened");
    const std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return bytes_t(first, first + raw.size());
}

/** @brief Hexdump @p b, for diagnosing a byte-exact failure without a debugger. */
std::string hex(std::span<const std::byte> b) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (const std::byte x : b) {
        out.push_back(kDigits[static_cast<std::uint8_t>(x) >> 4]);
        out.push_back(kDigits[static_cast<std::uint8_t>(x) & 0x0Fu]);
    }
    return out;
}

/** @brief The `src` PATH body of an emitted FWD frame — the region §6.1's rewrite lands in. */
std::optional<bytes_t> src_body_of(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec) return std::nullopt;
    const tr::wire::tlv_t* dst = nullptr;
    for (const tr::wire::tlv_t& c : dec->children) {
        if (c.type != tr::wire::type_t::PATH) continue;
        if (dst == nullptr) {
            dst = &c;
            continue;
        }
        return bytes_t(c.payload.begin(), c.payload.end());  // the SECOND PATH is `src`
    }
    return std::nullopt;
}

/** @brief The `dst` PATH body of an emitted FWD frame. */
std::optional<bytes_t> dst_body_of(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec) return std::nullopt;
    for (const tr::wire::tlv_t& c : dec->children)
        if (c.type == tr::wire::type_t::PATH) return bytes_t(c.payload.begin(), c.payload.end());
    return std::nullopt;
}

/**
 * @brief One forwarder A with a labelling table, an inbound `cli` link and an outbound `up`.
 *
 * The same assembly `path_label_forward_test.cpp` uses, for the same reason: the hop under the
 * origin must be the production one, or the round trip this file claims to close is closed
 * against a mock of the half that was already tested.
 */
struct hop_t {
    graph_t g;
    fwd_router_t r{g};
    path_label_table_t labels{&tr::mem::heap_source(), 64, 16};
    span_sink_t cli;
    span_sink_t up;

    /** @brief Wire A up with `mint` deciding whether it labels at all (§6.3's default is off). */
    explicit hop_t(bool mint) {
        (void)g.register_vertex(path_t("/net"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/net/uplink"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/net/uplink/b"), role_t::STORED_VALUE);
        if (mint) r.configure_path_labels(&labels);
        (void)r.add_child(std::string(kInLink), cli);
        (void)r.add_child(std::string(kOutLink), up);
    }
};

/**
 * @brief The ORIGIN: a router with one child toward A, and deliberately no label table.
 *
 * No `configure_path_labels` call, and that is the ruling rather than an omission: the origin
 * stands up no mint table of its own (amendment 7 keeps the table opt-in and OFF by default, the
 * per-hop saving is a WIDE claim, and an origin is very often the narrowest node on the route).
 */
struct origin_t {
    graph_t g;
    fwd_router_t r{g};
    span_sink_t uplink;

    origin_t() {
        (void)g.register_vertex(path_t("/net"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/net/uplink"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/net/uplink/a"), role_t::STORED_VALUE);
        (void)r.add_child(std::string(kOriginLink), uplink);
    }
};

/**
 * @brief Drive one request in and one reply back through @p h — §6.1's round trip, at the hop.
 *
 * Returns the minted reply A relays toward the origin, which is the frame the origin adopts.
 */
std::optional<bytes_t> hop_round_trip(hop_t& h) {
    const bytes_t req = b_fwd_raw_op(kRead, b_path({"net", "uplink", "b", "sensor", "temp"}),
                                     b_path({"reply-ep"}), {}, b_value_u32(9));
    h.r.on_frame(kInLink, req);
    if (h.up.sent.empty()) return std::nullopt;
    const bytes_t reply =
        b_fwd_reply(tr::graph::reply_kind_t::RESULT, b_path({"net", "downlink", "cli", "reply-ep"}),
                    b_path({"sensor", "temp"}), b_value_u32(1234));
    h.r.on_frame(kOutLink, reply);
    if (h.cli.sent.empty()) return std::nullopt;
    return h.cli.sent.back();
}

/** @brief The path the origin addresses: `/net/uplink/a` + the residual A and B resolve. */
path_t origin_target() { return path_t("/net/uplink/a/net/uplink/b/sensor/temp"); }

/** @brief True iff @p p still holds the canonical bytes @ref origin_target parses to.
 *
 *  The fallback's precondition, asserted rather than assumed: a spelling that discarded the
 *  canonical form would make `NOT_FOUND` unrecoverable instead of one failed operation. */
bool key_intact(const path_t& p) {
    const path_t canonical = origin_target();
    const std::span<const std::byte> a = p.key();
    const std::span<const std::byte> b = canonical.key();
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

}  // namespace

int main() {
    std::printf("RFC-0027 origin: adopt the minted src, spend it as dst, fall back on NOT_FOUND\n");

    // ===== 1) O-1/O-2 — the origin caches the HANDED spelling, verbatim ====================
    std::printf(
        "\n1) adopt: the handed spelling, with the origin's own part as LITERAL segments\n");
    {
        hop_t h(/*mint=*/true);
        const std::optional<bytes_t> minted = hop_round_trip(h);
        check(minted.has_value(), "the hop minted and relayed a reply for the origin to adopt");
        const bytes_t minted_src = *src_body_of(*minted);

        origin_t o;
        path_t target = origin_target();
        const auto dec = tr::wire::decode(*minted);
        check(dec.has_value() && o.r.adopt_path_label(target, kOriginLink, *dec),
              "the origin adopts the minted `src` (the analogue of adopt_binding, erratum 2)");
        check(target.path_label().cached, "…and the path came out carrying a labelled spelling");

        // VERBATIM, with one prepend and nothing else: no normalization, no completion pass.
        bytes_t expect = b_path_body({"net", "uplink", "a"});
        expect.insert(expect.end(), minted_src.begin(), minted_src.end());
        check(target.path_label().body == expect,
              "the cached body is the handed bytes with the origin's own local part prepended");
        if (target.path_label().body != expect) {
            std::printf("   cached : %s\n", hex(target.path_label().body).c_str());
            std::printf("   expect : %s\n", hex(expect).c_str());
        }

        // The origin minted NOTHING for its own hop, and the spelling says so on its face.
        check(path_element_at(target.path_label().body, 0).kind == path_element_kind_t::SEGMENT,
              "the origin's own first hop is spelled as literal segments — no mint table here");
        const tr::wire::path_element_census_t census =
            tr::wire::path_element_census(target.path_label().body);
        check(census.well_formed && census.labels == 1 && census.segments == 5,
              "…so the steady state is a MIXED spelling: one label, five literal segments (§5.2)");

        // The canonical bytes are never discarded — they are what the fallback falls back TO.
        check(key_intact(target),
              "…and the canonical bytes are untouched: the mint key and the fallback (§5.1)");
    }

    // ===== 2) O-1 — what the seam REFUSES, all of it `cache_path_label`'s ==================
    std::printf("\n2) the refusals are the cache's own — the seam adds none of its own\n");
    {
        // A hop that mints nothing is fully conformant (§6.3), and its reply has no label in it.
        hop_t plain(/*mint=*/false);
        const std::optional<bytes_t> unminted = hop_round_trip(plain);
        check(unminted.has_value(), "an un-injected hop still relays the reply");
        origin_t o;
        path_t target = origin_target();
        const auto dec = tr::wire::decode(*unminted);
        check(dec.has_value() && !o.r.adopt_path_label(target, kOriginLink, *dec),
              "a reply with NO label element is not adopted — a pure-string spelling is key()");
        check(!target.path_label().cached && key_intact(target),
              "…and the path is left exactly as it was: canonical, which always works");

        // §11.2 — the two compressions never meet on one address.
        hop_t h(/*mint=*/true);
        const std::optional<bytes_t> minted = hop_round_trip(h);
        path_t bound = origin_target();
        const tr::wire::path_ref_element_t e{.index = 1, .generation = 1};
        check(bound.bind(std::span<const tr::wire::path_ref_element_t>(&e, 1)),
              "a path bound to a PATH_REF route");
        const auto mdec = tr::wire::decode(*minted);
        check(mdec.has_value() && !o.r.adopt_path_label(bound, kOriginLink, *mdec),
              "a PATH_REF-bound path refuses the label spelling (§11.2)");

        // A reply that is not a routed FWD at all carries no `src` to adopt.
        const bytes_t no_src = b_fwd_raw_op(kRead, b_path({"x"}), {}, {}, b_value_u32(1));
        const auto ndec = tr::wire::decode(no_src);
        path_t p2 = origin_target();
        check(ndec.has_value() && !o.r.adopt_path_label(p2, kOriginLink, *ndec),
              "a frame with no `src` PATH is not adopted");
    }

    // ===== 3) O-3 — SPEND it: the labelled request, byte-exact against the vector ==========
    std::printf("\n3) spend: the cached spelling becomes the `dst` of the next request (§6.1)\n");
    {
        hop_t h(/*mint=*/true);
        const std::optional<bytes_t> minted = hop_round_trip(h);
        const bytes_t minted_src = *src_body_of(*minted);
        const path_label_t label = path_element_at(minted_src, 0).label;

        origin_t o;
        path_t target = origin_target();
        const auto dec = tr::wire::decode(*minted);
        check(dec.has_value() && o.r.adopt_path_label(target, kOriginLink, *dec), "adopted");

        const auto dispatch = o.r.label_dispatch(target);
        check(dispatch.has_value(), "the origin resolves its OWN literal head to the link out");
        check(dispatch && dispatch->link == &o.uplink, "…and it is the child that head names");
        check(dispatch && dispatch->dst == b_path_labelled(label, {"sensor", "temp"}),
              "…and what goes on the wire is the RESIDUAL: its own part consumed, not sent");
        check(key_intact(target),
              "…with the canonical bytes still held — a spend discards nothing");

        // The bytes `fwd/fwd-label-stale` says an origin sends. They were hand-built by a test
        // until this car; now the origin emits them.
        const bytes_t frame =
            b_fwd_raw_op(kRead, dispatch->dst, b_path({"reply-ep"}), {}, b_value_u32(9));
        check(frame == vector_bytes("fwd/fwd-label-stale"),
              "the frame the origin composes IS fwd/fwd-label-stale, byte-exact");
        if (frame != vector_bytes("fwd/fwd-label-stale")) {
            std::printf("   origin : %s\n", hex(frame).c_str());
            std::printf("   vector : %s\n", hex(vector_bytes("fwd/fwd-label-stale")).c_str());
        }

        // And the round trip closes: the hop that minted the label accepts what the origin
        // spelled with it, and forwards the same residual the string spelling forwards.
        const std::size_t before = h.up.sent.size();
        h.r.on_frame(kInLink, frame);
        check(h.up.sent.size() == before + 1, "A forwarded the origin's labelled request");
        check(h.r.label_resolves() == 1 && h.r.label_not_found() == 0,
              "…through the labelled resolution, with no refusal");
        hop_t plain(/*mint=*/false);
        plain.r.on_frame(kInLink,
                         b_fwd_raw_op(kRead, b_path({"net", "uplink", "b", "sensor", "temp"}),
                                      b_path({"reply-ep"}), {}, b_value_u32(9)));
        check(!plain.up.sent.empty() &&
                  dst_body_of(h.up.sent.back()) == dst_body_of(plain.up.sent.back()),
              "…and B receives byte-identically what the canonical spelling delivers");
    }

    // ===== 4) O-3 — the fallback: NOT_FOUND, clear, re-mint. Nothing else. =================
    std::printf("\n4) fall back: one failed operation is the entire cost (§7.2, §7.3)\n");
    {
        hop_t h(/*mint=*/true);
        const std::optional<bytes_t> minted = hop_round_trip(h);
        origin_t o;
        path_t target = origin_target();
        const auto dec = tr::wire::decode(*minted);
        check(dec.has_value() && o.r.adopt_path_label(target, kOriginLink, *dec), "adopted");
        const auto dispatch = o.r.label_dispatch(target);
        check(dispatch.has_value(), "and spendable");

        // §7.1's departure. Nothing is told — there is no withdraw frame, no unbind, no lease
        // and no TTL (§7.3); the next frame discovers it.
        check(h.r.remove_child(kOutLink), "the child the label stood for departed");
        const bytes_t frame =
            b_fwd_raw_op(kRead, dispatch->dst, b_path({"reply-ep"}), {}, b_value_u32(9));
        const std::size_t up_before = h.up.sent.size();
        h.r.on_frame(kInLink, frame);
        check(h.up.sent.size() == up_before, "the labelled request forwards nothing now");
        check(!h.cli.sent.empty(), "…and the origin is ANSWERED, not left waiting");

        // The refusal, as the production hop really spells it.
        const auto refusal = tr::wire::decode(h.cli.sent.back());
        check(refusal.has_value(), "the refusal is a well-formed frame");
        check(refusal.has_value() && o.r.fall_back_on_label_refusal(target, *refusal),
              "tr::path::not_found drops the cached spelling — the whole recovery (§7.2)");
        check(!target.path_label().cached, "…the spelling is forgotten");
        check(key_intact(target),
              "…the full-string path the origin still holds is the fallback, unchanged");
        check(!o.r.label_dispatch(target).has_value(),
              "…and there is nothing labelled left to spend: the next operation goes canonical");
        check(!target.binding().bound, "…nothing else on the path was touched");
        check(o.uplink.sent.empty(),
              "…and the recovery put NO frame on the wire: no withdraw, no unbind (§7.3)");

        // Re-mint from the next reply. The link comes back, the hop mints again, and the origin
        // adopts the new spelling — the fallback is complete, not a one-way downgrade.
        (void)h.r.add_child(std::string(kOutLink), h.up);
        const std::optional<bytes_t> reminted = hop_round_trip(h);
        const auto rdec = tr::wire::decode(*reminted);
        check(rdec.has_value() && o.r.adopt_path_label(target, kOriginLink, *rdec),
              "the next reply re-mints and the origin adopts it (§6.1)");
        check(o.r.label_dispatch(target).has_value(), "…and the path is spendable again");
    }

    // ===== 5) O-3 — the refusal is NARROW: only NOT_FOUND drops the spelling ===============
    std::printf("\n5) a different refusal says nothing about the spelling, and clears nothing\n");
    {
        hop_t h(/*mint=*/true);
        const std::optional<bytes_t> minted = hop_round_trip(h);
        origin_t o;
        path_t target = origin_target();
        const auto dec = tr::wire::decode(*minted);
        check(dec.has_value() && o.r.adopt_path_label(target, kOriginLink, *dec), "adopted");

        // A denial, an invalid address, backpressure: each is a real refusal and none of them
        // is evidence that the label went stale. Dropping the cache on one would re-mint a
        // spelling that was never the problem.
        for (const tr::wire::err_t other :
             {tr::wire::err_t::ACCESS_DENIED, tr::wire::err_t::PATH_INVALID}) {
            const bytes_t reply = b_fwd_reply(tr::graph::reply_kind_t::ERROR, b_path({"reply-ep"}),
                                              b_path({"x"}), b_error_status(other));
            const auto rdec = tr::wire::decode(reply);
            check(rdec.has_value() && !o.r.fall_back_on_label_refusal(target, *rdec),
                  "a non-NOT_FOUND error leaves the cached spelling alone");
        }
        check(target.path_label().cached, "…the spelling survived every one of them");

        // And a successful reply carrying a two-byte payload is not an error identity: the
        // reader walks STATUS > ERROR > VALUE, not "any 2-byte VALUE anywhere".
        bytes_t two_byte;
        const std::array<std::byte, 2> raw{std::byte{0x20}, std::byte{0x00}};
        tr::wire::emit_tlv(two_byte, tr::wire::type_t::VALUE, tr::wire::opt_t{}, raw);
        const bytes_t ok_reply = b_fwd_reply(tr::graph::reply_kind_t::RESULT, b_path({"reply-ep"}),
                                             b_path({"x"}), two_byte);
        const auto odec = tr::wire::decode(ok_reply);
        check(odec.has_value() && !o.r.fall_back_on_label_refusal(target, *odec),
              "a RESULT whose payload happens to be two bytes is not a NOT_FOUND refusal");
        check(target.path_label().cached, "…and the spelling is still there");
    }

    return tr::testing::summary("path_label_origin");
}
