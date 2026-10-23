/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0029 slice S1 — the PAIR path element and the per-hop arm that consumes it.
 *
 * The element is RFC-0024's owner-issued `(u32 index, u32 generation)` spelled INSIDE a packed
 * `PATH` body as the escape record `00 16 08 <u32 LE><u32 LE>` (§4.1, §5.1), so NAMEs and
 * PAIRs mix per element and one `PATH` is the only address form. This file binds, against the
 * production `fwd_router_t` + `op_resolver_t` wiring (the conformance harness routes nothing):
 *
 * - the CODEC: encode, decode, the length clause that tells a PAIR from a malformed record
 *   (RFC-0027's retired `len = 4` label included), and the six §13.4 S1 vectors byte-exact against
 * the emitters;
 * - §6 step 2: a PAIR the owner cannot validate answers `NOT_FOUND`, forwards nothing,
 *   applies nothing;
 * - §6 step 3: a connection vertex WITH a tail is a hop whose egress is byte-identical to the
 *   NAME spelling's; the LAST element is the terminus (a connection vertex named last reads
 *   its own value); a tail below an ordinary vertex is `INVALID_PATH`;
 * - §5.3: the retired `PATH_REF` (`0x14`) is refused as a `dst`;
 * - §6.4: the hop's authorization is the same verdict whether the hop is spelled as a NAME
 *   run or as a PAIR — the one gate, `bound_egress`, reached from both arms.
 *
 * Topology — one node, two links, because a hop and a terminus both happen at ONE node:
 *
 *     cli ──(net/downlink/cli)──▶ N ──(net/uplink/b)──▶ b      N holds /sensor/temp
 */
#include "libtracer/path_pair.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "graph_sinks.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_element.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "pair_body.hpp"
#include "test_support.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

namespace {

using tr::graph::acl_right_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::testing::b_fwd_raw_op;
using tr::testing::bytes_t;
using tr::testing::check;
using tr::wire::path_element_at;
using tr::wire::path_element_kind_t;
using tr::wire::path_pair_t;

/** @brief The inbound link, spelled as the three-segment mount run a real node uses. */
constexpr std::string_view kInLink = "net/downlink/cli";
/** @brief The egress link whose connection vertex a hop's PAIR names. */
constexpr std::string_view kOutLink = "net/uplink/b";

/** @brief The op bytes this file drives (RFC-0004 §D). */
constexpr std::uint8_t kRead = 0x00;
constexpr std::uint8_t kWrite = 0x01;
constexpr std::uint8_t kReply = 0x03;

/** @brief The registered error identities these refusals carry (`tr::path::*`). */
constexpr auto kNotFound = tr::wire::err_t::PATH_NOT_FOUND;
constexpr auto kInvalid = tr::wire::err_t::PATH_INVALID;

/** @brief A `PATH` TLV over @p segs, packed-record body (RFC-0018). */
bytes_t b_path(std::initializer_list<std::string_view> segs) {
    bytes_t body;
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    bytes_t out;
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{}, body);
    return out;
}

/** @brief One element of a chain: a NAME segment or a PAIR (RFC-0029 §4.1). */
struct el_t {
    std::string_view name{}; /**< @brief The NAME, when @ref is_pair is false. */
    path_pair_t pair{};      /**< @brief The PAIR, when @ref is_pair is true. */
    bool is_pair = false;    /**< @brief Which of the two this element is. */
};
/** @brief A NAME element. */
el_t nm(std::string_view s) { return el_t{.name = s}; }
/** @brief A PAIR element. */
el_t pr(path_pair_t p) { return el_t{.pair = p, .is_pair = true}; }

/** @brief A `PATH` TLV over a MIXED chain of NAME and PAIR elements (RFC-0029 §4.2). */
bytes_t b_chain(std::initializer_list<el_t> els) {
    bytes_t body;
    for (const el_t& e : els) {
        if (e.is_pair)
            tr::testing::emit_path_pair(body, e.pair);
        else
            (void)tr::wire::emit_path_segment(body, e.name);
    }
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

/** @brief The bytes a local `read` of @p text answers, flattened (empty on a miss). */
bytes_t held(const graph_t& g, std::string_view text) {
    const auto r = g.read(path_t(std::string(text)));
    bytes_t out;
    std::vector<std::span<const std::byte>> iov;
    if (r.has_value() && (**r).try_to_iovec(iov))
        for (const std::span<const std::byte> s : iov) out.insert(out.end(), s.begin(), s.end());
    return out;
}

/** @brief A heap-owned view over @p bytes (the graph stores owning views). */
tr::view::view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief The test subject resolver (ADR-0018): the caller context IS the subject token. */
std::expected<void, tr::wire::err_t> caller_is_subject(void*, std::string_view caller,
                                                       tr::mem::bytes_t& out) {
    const auto* p = reinterpret_cast<const std::byte*>(caller.data());
    return tr::testing::subject_into(out, std::vector<std::byte>(p, p + caller.size()));
}

/** @brief An ACL granting @p subject exactly @p mask, and nothing else. */
bytes_t allow_acl(std::string_view subject, std::uint32_t mask) {
    const auto* p = reinterpret_cast<const std::byte*>(subject.data());
    const tr::graph::ace_t ace{
        .type = tr::graph::ace_type_t::ALLOW,
        .flags = 0,
        .subject = bytes_t(p, p + subject.size()),
        .access_mask = mask,
        .expires_ns = 0,
    };
    return tr::graph::encode_acl(std::span<const tr::graph::ace_t>(&ace, 1));
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

/** @brief True when @p t carries the registered error identity @p want, at any depth. */
bool carries_error(const tr::wire::tlv_t& t, tr::wire::err_t want) {
    if (t.type == tr::wire::type_t::VALUE && t.payload.size() == 2) {
        const auto lo = static_cast<std::uint16_t>(t.payload[0]);
        const auto hi = static_cast<std::uint16_t>(t.payload[1]);
        if (static_cast<std::uint16_t>(lo | (hi << 8)) == static_cast<std::uint16_t>(want))
            return true;
    }
    for (const tr::wire::tlv_t& c : t.children)
        if (carries_error(c, want)) return true;
    return false;
}

/** @brief Does the frame @p f carry the error identity @p want? */
bool frame_is_error(std::span<const std::byte> f, tr::wire::err_t want) {
    const auto dec = tr::wire::decode(f);
    return dec && carries_error(*dec, want);
}

/**
 * @brief Everything a reply says that is NOT its `src`: the op, the route home, the kind and the
 *        answer — the VERDICT §6.4 requires to be spelling-independent.
 *
 * The `src` is the request's `dst` echoed, so it differs by spelling BY CONSTRUCTION.
 */
std::optional<tr::wire::tlv_t> verdict_of(std::span<const std::byte> frame) {
    auto dec = tr::wire::decode(frame);
    if (!dec) return std::nullopt;
    int paths = 0;
    for (auto it = dec->children.begin(); it != dec->children.end(); ++it) {
        if (it->type != tr::wire::type_t::PATH) continue;
        if (++paths != 2) continue;  // the FIRST PATH is `dst`, the second is `src`
        dec->children.erase(it);
        break;
    }
    return std::move(*dec);
}

/**
 * @brief One node N: an inbound `cli` link, an egress `b` link whose connection vertex exists,
 *        and a local `/sensor/temp`. Assembled through the router's public surface.
 */
struct node_t {
    graph_t g;
    fwd_router_t r{g};
    span_sink_t cli;
    span_sink_t b;

    node_t() {
        (void)g.register_vertex(path_t("/sensor"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
        (void)g.write(path_t("/sensor/temp"), owned(b_value_u32(1234)));
        // The connection vertex FIRST, so `add_child` records its slot (the bound-path join).
        (void)g.register_vertex(path_t("/net"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/net/uplink"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/net/uplink/b"), role_t::STORED_VALUE);
        (void)g.write(path_t("/net/uplink/b"), owned(b_value_u32(77)));
        (void)r.attach_link(std::string(kInLink), cli);
        (void)r.attach_link(std::string(kOutLink), b);
    }

    /** @brief The pair this node issues for the vertex at @p text. */
    path_pair_t pair_of(std::string_view text) const {
        const auto v = g.find(path_t(std::string(text)).key());
        check(v.has_value(), "the vertex to pair exists");
        const auto slot = g.vertex_slot(*v);
        check(slot.has_value(), "…and is pairable");
        return path_pair_t{.index = slot->index, .generation = slot->generation};
    }

    /** @brief Drive @p frame in on `cli`. */
    void in(std::span<const std::byte> frame) { r.on_frame(kInLink, frame); }
    /** @brief Retire the vertex at @p text, so the node can no longer issue a PAIR for it. */
    void retire(std::string_view text) {
        const auto v = g.find(path_t(std::string(text)).key());
        check(v.has_value() && g.retire(*v), "the connection vertex retires");
    }

    /** @brief Drive @p frame in on `b`. */
    void in_b(std::span<const std::byte> frame) { r.on_frame(kOutLink, frame); }
};

/** @brief `/reply-ep` — the return route every request here carries. */
bytes_t reply_ep() { return b_path({"reply-ep"}); }

// ---------------------------------------------------------------------------------------------

void codec() {
    std::printf("\n1) the PAIR element codec (§4.1, §5.1)\n");
    const path_pair_t p{.index = 0x01020304, .generation = 0x0A0B0C0D};
    bytes_t body;
    tr::testing::emit_path_pair(body, p);
    const bytes_t want{std::byte{0x00}, std::byte{0x16}, std::byte{0x08}, std::byte{0x04},
                       std::byte{0x03}, std::byte{0x02}, std::byte{0x01}, std::byte{0x0D},
                       std::byte{0x0C}, std::byte{0x0B}, std::byte{0x0A}};
    check(body == want, "a PAIR is 00 16 08 <u32 LE index><u32 LE generation> — 11 bytes");
    check(body.size() == tr::wire::kPathPairRecordBytes, "…which is kPathPairRecordBytes");
    const auto back = tr::wire::path_pair_at(body, 0);
    check(back && *back == p, "path_pair_at reads back the same (index, generation)");
    const tr::wire::path_element_t el = path_element_at(body, 0);
    check(el.kind == path_element_kind_t::PAIR && el.pair == p && el.bytes == 11,
          "the element walker classifies it PAIR and decodes the pair");

    // Every value is structurally a pair: validity is the owner's deref, never the codec's.
    bytes_t zero;
    tr::testing::emit_path_pair(zero, path_pair_t{});
    check(path_element_at(zero, 0).kind == path_element_kind_t::PAIR,
          "(0, 0) is a PAIR too — the deref refuses it, the codec does not (§6 step 2)");

    // The length clause: 8 is a PAIR, anything else refuses — `len = 4` included, the
    // RFC-0027 label element RFC-0029 §5.3 retired.
    for (const std::uint8_t len : {std::uint8_t{0}, std::uint8_t{4}, std::uint8_t{5},
                                   std::uint8_t{7}, std::uint8_t{9}, std::uint8_t{12}}) {
        bytes_t bad{std::byte{0x00}, std::byte{0x16}, std::byte{len}};
        bad.resize(bad.size() + len, std::byte{0x01});
        check(path_element_at(bad, 0).kind == path_element_kind_t::MALFORMED,
              "a kind 0x16 record whose len is not 8 is MALFORMED — the address, not the frame");
        check(!tr::wire::path_pair_at(bad, 0).has_value(), "…and path_pair_at reads nothing");
        check(tr::wire::packed_record_span(bad, 0) == 3u + len,
              "…while the record still steps over by its declared length");
    }

    // Mixed chains: walk, census, re-emit.
    bytes_t mixed;
    (void)tr::wire::emit_path_segment(mixed, "net");
    tr::testing::emit_path_pair(mixed, p);
    (void)tr::wire::emit_path_segment(mixed, "temp");
    const tr::wire::path_element_census_t c = tr::wire::path_element_census(mixed);
    check(c.well_formed && c.elements == 3 && c.segments == 2 && c.pairs == 1,
          "a mixed body walks to 2 NAMEs + 1 PAIR");
    check(!tr::wire::packed_path_valid_key(mixed),
          "a chain is a frame path and never a key (§5.1; v1.md §3.1)");
    bytes_t re;
    tr::wire::path_element_cursor_t cur(mixed);
    while (const auto e = cur.next()) check(tr::wire::emit_path_element(re, *e), "re-emits");
    check(re == mixed, "emit_path_element round-trips a mixed chain byte-exact");
}

void vectors() {
    std::printf("\n2) the §13.4 S1 conformance vectors, byte-exact against the emitters\n");
    const path_pair_t target{.index = 3, .generation = 1};
    const path_pair_t conn{.index = 7, .generation = 1};

    check(b_chain({nm("net"), pr(target), nm("temp")}) == vector_bytes("path/element-pair-encode"),
          "path/element-pair-encode");

    bytes_t bad_body;
    (void)tr::wire::emit_path_segment(bad_body, "sensor");
    const std::array<std::byte, 7> seven{std::byte{3}, std::byte{0}, std::byte{0}, std::byte{0},
                                         std::byte{1}, std::byte{0}, std::byte{0}};
    (void)tr::wire::emit_path_escape(bad_body, tr::wire::kPathPairKind, seven);
    bytes_t bad;
    tr::wire::emit_tlv(bad, tr::wire::type_t::PATH, tr::wire::opt_t{}, bad_body);
    check(bad == vector_bytes("path/element-pair-bad-len"), "path/element-pair-bad-len");
    check(path_element_at(bad_body, 7).kind == path_element_kind_t::MALFORMED,
          "…and its second record is MALFORMED");

    check(b_fwd_raw_op(kRead, b_chain({pr(conn), pr(target)}), reply_ep()) ==
              vector_bytes("fwd/pair-hop-egress"),
          "fwd/pair-hop-egress");
    check(
        b_fwd_raw_op(kRead, b_chain({pr(target)}), reply_ep()) == vector_bytes("fwd/pair-terminus"),
        "fwd/pair-terminus");
    check(b_fwd_raw_op(kRead, b_chain({pr(conn)}), reply_ep()) ==
              vector_bytes("fwd/pair-last-element-connection-vertex-is-facet"),
          "fwd/pair-last-element-connection-vertex-is-facet");

    // The refusal reply, as the production router builds it. Which slot the vector's 3:1
    // names on this node is a fact about registration order, so drive a pair that CANNOT
    // validate here and compare the reply byte-exact against the vector's shape.
    node_t m;
    const path_pair_t live = m.pair_of("/sensor/temp");
    const path_pair_t stale{.index = live.index, .generation = live.generation + 1};
    m.in(b_fwd_raw_op(kRead, b_chain({pr(stale)}), reply_ep()));
    check(m.cli.sent.size() == 1 && frame_is_error(m.cli.sent.back(), kNotFound),
          "a stale pair is answered tr::path::not_found");
    // The vector's pair is 3:1; rebuild the node's answer with that spelling for the compare.
    bytes_t expect_body;
    tr::testing::emit_path_pair(expect_body, stale);
    bytes_t vec = vector_bytes("fwd/pair-stale-generation-not-found");
    // Patch the vector's echoed pair to this node's stale pair: the SHAPE is what is pinned,
    // and the pair's value is a fact about whichever owner answered.
    const bytes_t vec_pair = [] {
        bytes_t b;
        tr::testing::emit_path_pair(b, path_pair_t{.index = 3, .generation = 1});
        return b;
    }();
    const auto at = std::search(vec.begin(), vec.end(), vec_pair.begin(), vec_pair.end());
    check(at != vec.end(), "the vector echoes pair 3:1 in its src");
    if (at != vec.end()) std::copy(expect_body.begin(), expect_body.end(), at);
    check(m.cli.sent.back() == vec, "fwd/pair-stale-generation-not-found is the router's answer");
}

/** @brief Hex of @p b, for the message a byte-exact vector compare prints on a mismatch. */
std::string hex(std::span<const std::byte> b) {
    std::string out;
    for (const std::byte x : b) {
        char buf[3];
        std::snprintf(buf, sizeof buf, "%02X", static_cast<unsigned>(x));
        out += buf;
    }
    return out;
}

/** @brief @p got equals vector @p name's bytes; prints both on a mismatch. */
void check_vector(std::span<const std::byte> got, std::string_view name) {
    const bytes_t want = vector_bytes(name);
    const bool same = std::ranges::equal(got, want);
    if (!same) std::printf("  got  %s\n  want %s\n", hex(got).c_str(), hex(want).c_str());
    check(same, std::string(name).c_str());
}

/** @brief Replace every occurrence of @p from's encoding in @p f by @p to's (same length). */
void repair(bytes_t& f, path_pair_t from, path_pair_t to) {
    bytes_t a;
    bytes_t b;
    tr::testing::emit_path_pair(a, from);
    tr::testing::emit_path_pair(b, to);
    for (auto at = std::search(f.begin(), f.end(), a.begin(), a.end()); at != f.end();
         at = std::search(at + 1, f.end(), a.begin(), a.end()))
        std::copy(b.begin(), b.end(), at);
}

void learning_vectors() {
    std::printf("\n2b) the §13.4 S2 vectors (#2245), byte-exact against the production router\n");
    // The vectors spell the target as 3:1 and a connection vertex as 7:1; which slots this node
    // hands out is a fact about registration order, so the router's output is re-spelled with
    // the vectors' pairs before the compare. The SHAPE is what is pinned.
    const path_pair_t vec_target{.index = 3, .generation = 1};
    const path_pair_t vec_conn{.index = 7, .generation = 1};

    // fwd/fwd-reply-result: the terminus seeds a success reply's `src` with its target PAIR.
    {
        node_t n;
        n.in(b_fwd_raw_op(kRead, b_path({"sensor", "temp"}),
                          b_path({"net", "downlink", "a", "net", "downlink", "cli", "reply-ep"})));
        check(n.cli.sent.size() == 1, "the READ is answered on the inbound link");
        bytes_t f = n.cli.sent.empty() ? bytes_t{} : n.cli.sent.back();
        repair(f, n.pair_of("/sensor/temp"), vec_target);
        check_vector(f, "fwd/fwd-reply-result");
    }

    // fwd/reply-src-accumulates-forward-route-*: a relaying hop prepends its element to `src`.
    const bytes_t reply = tr::testing::b_fwd_reply(tr::graph::reply_kind_t::RESULT,
                                                   b_path({"net", "downlink", "cli", "reply-ep"}),
                                                   b_chain({pr(vec_target)}), b_value_u32(1234));
    {
        node_t n;  // `b` has a connection vertex: the hop's element is its PAIR
        n.in_b(reply);
        bytes_t f = n.cli.sent.empty() ? bytes_t{} : n.cli.sent.back();
        repair(f, n.pair_of("/net/uplink/b"), vec_conn);
        check_vector(f, "fwd/reply-src-accumulates-forward-route-pair");
    }
    {
        // No PAIR to give: a link cannot exist without its connection vertex (#1940), so the
        // hop that cannot issue one is the hop whose vertex has retired under it (§4.4).
        node_t n;
        n.retire("/net/uplink/b");
        n.in_b(reply);
        check_vector(n.cli.sent.empty() ? bytes_t{} : n.cli.sent.back(),
                     "fwd/reply-src-accumulates-forward-route-name");
    }

    // fwd/subscribe-reverse-*: a relayed subscribe with no `0x15` child gets one, created with
    // the hop's element followed by the request's `src` as it arrived (Amendment 1).
    bytes_t sel;
    {
        bytes_t body;
        tr::wire::emit_tlv(body, tr::wire::type_t::NAME, tr::wire::opt_t{},
                           std::as_bytes(std::span<const char>("subscribers", 11)));
        const std::array<std::byte, 1> element{std::byte{0x01}};
        tr::wire::emit_tlv(body, tr::wire::type_t::VALUE, tr::wire::opt_t{}, element);
        tr::wire::emit_tlv(sel, tr::wire::type_t::FIELD, tr::wire::opt_t{.pl = true}, body);
    }
    bytes_t sub;
    tr::wire::emit_tlv(sub, tr::wire::type_t::SUBSCRIBER, tr::wire::opt_t{.pl = true}, reply_ep());
    const bytes_t subscribe = b_fwd_raw_op(kWrite, b_path({"net", "uplink", "b", "sensor", "temp"}),
                                           reply_ep(), sel, sub);
    {
        node_t n;  // the arrival's connection vertex: the hop's element is its PAIR
        n.in(subscribe);
        bytes_t f = n.b.sent.empty() ? bytes_t{} : n.b.sent.back();
        repair(f, n.pair_of("/net/downlink/cli"), vec_conn);
        check_vector(f, "fwd/subscribe-reverse-list-unflagged");
    }
    {
        node_t n;  // `cli`'s vertex retired under the link: the hop's NAME run
        n.retire("/net/downlink/cli");
        n.in(subscribe);
        check_vector(n.b.sent.empty() ? bytes_t{} : n.b.sent.back(),
                     "fwd/subscribe-reverse-path-name-fallback");
    }
}

void terminus() {
    std::printf("\n3) §6 step 3: the LAST element is the terminus, same answer as the NAME\n");
    node_t n;
    const path_pair_t temp = n.pair_of("/sensor/temp");
    n.in(b_fwd_raw_op(kRead, b_path({"sensor", "temp"}), reply_ep()));
    n.in(b_fwd_raw_op(kRead, b_chain({pr(temp)}), reply_ep()));
    check(n.cli.sent.size() == 2, "both spellings are answered on the inbound link");
    check(n.b.sent.empty(), "…and neither is forwarded");
    const auto by_name = verdict_of(n.cli.sent[0]);
    const auto by_pair = verdict_of(n.cli.sent[1]);
    check(by_name && by_pair && tr::wire::encode(*by_name) == tr::wire::encode(*by_pair),
          "the PAIR terminus returns the NAME terminus's verdict byte-for-byte");

    // A WRITE through the pair lands on the same vertex.
    n.in(b_fwd_raw_op(kWrite, b_chain({pr(temp)}), reply_ep(), {}, b_value_u32(99)));
    check(held(n.g, "/sensor/temp") == b_value_u32(99),
          "a PAIR-spelled WRITE is applied by the same apply_op");

    std::printf("\n4) §6 step 3: a connection vertex named LAST is a terminus, not a hop\n");
    const path_pair_t conn = n.pair_of("/net/uplink/b");
    const std::size_t before = n.cli.sent.size();
    n.in(b_fwd_raw_op(kRead, b_chain({pr(conn)}), reply_ep()));
    n.in(b_fwd_raw_op(kRead, b_path({"net", "uplink", "b"}), reply_ep()));
    check(n.b.sent.empty(), "nothing egresses over the link the vertex names");
    check(n.cli.sent.size() == before + 2, "both spellings are answered here");
    const auto facet_pair = verdict_of(n.cli.sent[before]);
    const auto facet_name = verdict_of(n.cli.sent[before + 1]);
    check(
        facet_pair && facet_name && tr::wire::encode(*facet_pair) == tr::wire::encode(*facet_name),
        "…with the verdict the NAME spelling of the mount itself gets (RFC-0004 §A)");
    check(!frame_is_error(n.cli.sent[before], kNotFound) &&
              !frame_is_error(n.cli.sent[before], kInvalid),
          "…and it is a RESULT, the connection vertex's own value");
}

void hop() {
    std::printf(
        "\n5) §6 step 3: a connection vertex WITH a tail is a hop — byte-identical egress\n");
    node_t n;
    const path_pair_t conn = n.pair_of("/net/uplink/b");
    // The NAME spelling and the PAIR spelling of the same hop, the same tail.
    n.in(b_fwd_raw_op(kRead, b_path({"net", "uplink", "b", "sensor", "temp"}), reply_ep()));
    n.in(b_fwd_raw_op(kRead, b_chain({pr(conn), nm("sensor"), nm("temp")}), reply_ep()));
    check(n.b.sent.size() == 2, "both spellings egress over the link the element names");
    check(n.cli.sent.empty(), "…and neither is answered here");
    check(n.b.sent.size() == 2 && n.b.sent[0] == n.b.sent[1],
          "the forwarded frame is BYTE-IDENTICAL: dst shrunk by exactly the consumed element, "
          "src grown canonically by the inbound mount run (§6.1)");

    // A PAIR tail rides through untouched for the next node to consume.
    const path_pair_t far{.index = 3, .generation = 1};
    n.in(b_fwd_raw_op(kRead, b_chain({pr(conn), pr(far)}), reply_ep()));
    check(n.b.sent.size() == 3, "a PAIR tail is forwarded");
    const auto dec = tr::wire::decode(n.b.sent.back());
    bool tail_ok = false;
    if (dec)
        for (const tr::wire::tlv_t& c : dec->children)
            if (c.type == tr::wire::type_t::PATH) {
                bytes_t want;
                tr::testing::emit_path_pair(want, far);
                tail_ok = bytes_t(c.payload.begin(), c.payload.end()) == want;
                break;
            }
    check(tail_ok, "…as exactly the next node's element");
}

void refusals() {
    std::printf("\n6) §6 step 2 / §6.3: every refusal of a pair is NOT_FOUND, nothing moves\n");
    node_t n;
    const path_pair_t temp = n.pair_of("/sensor/temp");
    const path_pair_t conn = n.pair_of("/net/uplink/b");
    const auto refused = [&](std::span<const std::byte> frame, tr::wire::err_t want,
                             const char* what) {
        const std::size_t c0 = n.cli.sent.size();
        const std::size_t b0 = n.b.sent.size();
        n.in(frame);
        check(n.cli.sent.size() == c0 + 1 && frame_is_error(n.cli.sent.back(), want), what);
        check(n.b.sent.size() == b0, "…and nothing was forwarded");
    };
    refused(b_fwd_raw_op(kRead, b_chain({pr({temp.index, temp.generation + 1})}), reply_ep()),
            kNotFound, "a moved generation answers NOT_FOUND");
    refused(b_fwd_raw_op(kRead, b_chain({pr({0x00FFFFFFu, 1})}), reply_ep()), kNotFound,
            "an index out of range answers NOT_FOUND");
    refused(
        b_fwd_raw_op(kRead, b_chain({pr({conn.index, conn.generation + 1}), nm("x")}), reply_ep()),
        kNotFound, "a stale HOP element answers NOT_FOUND, not a silent drop");
    refused(
        b_fwd_raw_op(kRead, b_chain({pr({temp.index, tr::graph::kGenerationSaturated}), nm("x")}),
                     reply_ep()),
        kNotFound, "a saturated generation is refused on the honouring side");

    // A tail below a vertex that is not an egress names nothing (§6 step 3).
    refused(b_fwd_raw_op(kRead, b_chain({pr(temp), nm("x")}), reply_ep()), kInvalid,
            "a tail below an ordinary vertex is INVALID_PATH");

    // A retired vertex: the pair minted before the retire no longer validates.
    {
        node_t m;
        (void)m.g.register_vertex(path_t("/sensor/gone"), role_t::STORED_VALUE);
        const path_pair_t gone = m.pair_of("/sensor/gone");
        const auto v = m.g.find(path_t("/sensor/gone").key());
        check(v && m.g.retire(*v).has_value(), "the vertex retires");
        m.in(b_fwd_raw_op(kWrite, b_chain({pr(gone)}), reply_ep(), {}, b_value_u32(5)));
        check(m.cli.sent.size() == 1 && frame_is_error(m.cli.sent.back(), kNotFound),
              "a pair to a retired vertex answers NOT_FOUND");
        check(!m.g.find(path_t("/sensor/gone").key()).has_value(), "…and creates nothing");
    }

    std::printf(
        "\n7) §5.1 / §5.3: malformed element and the retired PATH_REF refuse the address\n");
    {
        bytes_t body;
        const std::array<std::byte, 7> seven{};
        (void)tr::wire::emit_path_escape(body, tr::wire::kPathPairKind, seven);
        bytes_t dst;
        tr::wire::emit_tlv(dst, tr::wire::type_t::PATH, tr::wire::opt_t{}, body);
        refused(b_fwd_raw_op(kRead, dst, reply_ep()), kInvalid,
                "a kind 0x16 len 7 head answers INVALID_PATH");
    }
    {
        // RFC-0029 §5.3 retired RFC-0027's `len = 4` label element, so a `kind = 0x16`,
        // `len = 4` record is malformed like any other length but 8 (v1.md §3), wherever it
        // stands: refused at the head and after a NAME, and a WRITE through it applies nothing.
        const std::array<std::byte, 4> four{std::byte{0x01}, std::byte{0x00}, std::byte{0x01},
                                            std::byte{0x00}};
        bytes_t head_body;
        (void)tr::wire::emit_path_escape(head_body, tr::wire::kPathPairKind, four);
        bytes_t head_dst;
        tr::wire::emit_tlv(head_dst, tr::wire::type_t::PATH, tr::wire::opt_t{}, head_body);
        refused(b_fwd_raw_op(kRead, head_dst, reply_ep()), kInvalid,
                "a kind 0x16 len 4 head (the retired label) answers INVALID_PATH");
        bytes_t tail_body;
        (void)tr::wire::emit_path_segment(tail_body, "sensor");
        (void)tr::wire::emit_path_escape(tail_body, tr::wire::kPathPairKind, four);
        bytes_t tail_dst;
        tr::wire::emit_tlv(tail_dst, tr::wire::type_t::PATH, tr::wire::opt_t{}, tail_body);
        refused(b_fwd_raw_op(kWrite, tail_dst, reply_ep(), {}, b_value_u32(4242)), kInvalid,
                "a len 4 element after a NAME answers INVALID_PATH");
        check(held(n.g, "/sensor/temp") == b_value_u32(1234), "…and the WRITE applied nothing");
    }
    {
        const std::array<tr::wire::path_ref_element_t, 2> els{conn, temp};
        bytes_t dst;
        check(tr::wire::emit_path_ref(dst, els), "a PATH_REF body still encodes (0x15 uses it)");
        const std::size_t before = n.cli.sent.size();
        n.in(b_fwd_raw_op(kRead, dst, reply_ep()));
        check(n.cli.sent.size() == before + 1 && frame_is_error(n.cli.sent.back(), kInvalid),
              "a PATH_REF (0x14) dst is refused INVALID_PATH — retired as an address");
        check(n.b.sent.empty(), "…and is never routed");
        const std::array<tr::wire::path_ref_element_t, 1> one{temp};
        bytes_t dst1;
        (void)tr::wire::emit_path_ref(dst1, one);
        n.in(b_fwd_raw_op(kWrite, dst1, reply_ep(), {}, b_value_u32(4242)));
        check(held(n.g, "/sensor/temp") == b_value_u32(1234),
              "…and a one-element PATH_REF is never applied as a terminus either");
    }
    {
        const std::size_t before = n.cli.sent.size();
        n.in(b_fwd_raw_op(kReply, b_chain({pr(conn), nm("x")}), reply_ep()));
        check(n.cli.sent.size() == before && n.b.sent.empty(),
              "a PAIR-headed REPLY is dropped — a reply's dst is canonical (§6.1)");
    }
}

void authorization() {
    std::printf("\n8) §6.4: the hop's verdict is the same whichever way the hop is spelled\n");
    node_t n;
    n.g.set_hook(&tr::graph::graph_hooks_t::subject_lookup, {caller_is_subject, nullptr});
    // READ and nothing else through the egress connection vertex, for the inbound subject.
    (void)n.g.write(path_t("/net/uplink/b:acl"),
                    owned(allow_acl(kInLink, static_cast<std::uint32_t>(acl_right_t::READ))));
    const path_pair_t conn = n.pair_of("/net/uplink/b");
    const bytes_t by_name = b_path({"net", "uplink", "b", "sensor", "temp"});
    const bytes_t by_pair = b_chain({pr(conn), nm("sensor"), nm("temp")});

    n.in(b_fwd_raw_op(kRead, by_name, reply_ep()));
    n.in(b_fwd_raw_op(kRead, by_pair, reply_ep()));
    check(n.b.sent.size() == 2 && n.cli.sent.empty(),
          "the granted right (READ) crosses the hop in BOTH spellings");

    n.in(b_fwd_raw_op(kWrite, by_name, reply_ep(), {}, b_value_u32(1)));
    n.in(b_fwd_raw_op(kWrite, by_pair, reply_ep(), {}, b_value_u32(1)));
    check(n.b.sent.size() == 2, "the ungranted right (WRITE) crosses in NEITHER spelling");
    check(n.cli.sent.size() == 2 && frame_is_error(n.cli.sent[0], kNotFound) &&
              frame_is_error(n.cli.sent[1], kNotFound),
          "…and both are answered the same refusal");
    check(n.cli.sent.size() == 2 && n.cli.sent[0] != n.cli.sent[1] &&
              verdict_of(n.cli.sent[0]).has_value() &&
              tr::wire::encode(*verdict_of(n.cli.sent[0])) ==
                  tr::wire::encode(*verdict_of(n.cli.sent[1])),
          "…verdict byte-identical, only the echoed spelling differs");

    // A REPLY crossing the same hop is routed, never authorized (it answers an authorized op).
    n.in(tr::testing::b_fwd_reply(tr::graph::reply_kind_t::RESULT, by_name, reply_ep(),
                                  b_value_u32(1)));
    check(n.b.sent.size() == 3, "a NAME-routed REPLY still crosses a hop the ACL gates");
}

}  // namespace

int main() {
    codec();
    vectors();
    learning_vectors();
    terminus();
    hop();
    refusals();
    authorization();
    return tr::testing::summary("path_pair");
}
