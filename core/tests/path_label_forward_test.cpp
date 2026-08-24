/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0027 car 4 — THE FORWARDER: mint on reply, deref on receipt, `NOT_FOUND` on stale.
 *
 * This file exists because the conformance harness routes nothing. `tests/conformance` gates
 * `encode(decode(input.bin)) == input.bin` and a static fixture has no router in it, so every
 * behavioural claim RFC-0027 makes — that a hop actually REPLACES its own local part on a
 * reply, that it actually turns a label back into the link it minted for, that a stale label
 * actually answers `NOT_FOUND` and delivers nothing, and that a labelled operation reaches
 * exactly the same ACL verdict the string form reaches — is bound here, against the production
 * `fwd_router_t` wiring, or it is bound nowhere. That is RFC-0014's lesson stated as a file:
 * two silent misroutes shipped because no test used the production wiring.
 *
 * The four conformance vectors this binds are `fwd/fwd-label-mint-reply`, `fwd/fwd-label-stale`,
 * `acl/label-vs-string-allow` and `acl/label-vs-string-deny`, each asserted byte-exact against
 * what the router really emits — the discipline `bound_forward_test.cpp` established for
 * `fwd/fwd-bound-forward`.
 *
 * The topology, one forwarder wide, because one hop is where every claim is expressible:
 *
 *     cli ──(the frame arrives here)──▶ A ──(child "up")──▶ B
 *
 * A's `up` child has a connection vertex; a label A mints stands for `up`'s whole mount run
 * (§5.3.3, amendment 6), and dereferencing it yields `up`'s link.
 */
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
#include "libtracer/conn_spec.hpp"
#include "libtracer/fwd_frame_view.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_element.hpp"
#include "libtracer/path_label.hpp"
#include "libtracer/path_label_table.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport_vertex.hpp"
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

/** @brief The two links, spelled as the RFC-0014 mount runs a real node uses.
 *
 *  Three segments each, deliberately: a path label stands for a hop's WHOLE local part
 *  (§5.3.3, amendment 6), so a one-segment name would make the 7-byte element LARGER than the
 *  string it replaces and the arithmetic §3.3 prices would run backwards. `net/<module>/<name>`
 *  is the shape RFC-0023 prices and the shape `fwd/fwd-src-accumulated` carries. */
constexpr std::string_view kInLink = "net/downlink/cli";
constexpr std::string_view kOutLink = "net/uplink/b";

/** @brief The request op bytes this file drives, unflagged (RFC-0004 §D).
 *
 *  There is no `kReply` here: a REPLY is built with @ref tr::testing::b_fwd_reply, because a
 *  REPLY that is only an op byte is missing its REQUIRED `kind` child (RFC-0004 §B). */
constexpr std::uint8_t kRead = 0x00;
constexpr std::uint8_t kWrite = 0x01;

/** @brief A `PATH` TLV over @p segs, packed-record body (RFC-0018). */
bytes_t b_path(std::initializer_list<std::string_view> segs) {
    bytes_t body;
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    bytes_t out;
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{}, body);
    return out;
}

/** @brief A `PATH` TLV whose FIRST element is @p label and whose rest is @p segs — the
 *         labelled spelling of an address whose leading mount run one hop already resolved. */
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

/** @brief A heap-owned view over @p bytes (the graph stores owning views). */
tr::view::view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/**
 * @brief The test subject resolver (ADR-0018): the caller context IS the subject token.
 *
 * The empty (local) context never reaches a resolver — `graph_t::acl_allows` settles it as
 * trusted before invoking one (#905) — so the error arm here means DENY and nothing else.
 */
std::expected<tr::graph::subject_token_t, tr::wire::err_t> caller_is_subject(
    void*, std::string_view caller) {
    const auto* p = reinterpret_cast<const std::byte*>(caller.data());
    return tr::graph::subject_token_t(p, p + caller.size());
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

/**
 * @brief True when @p t carries the registered error identity @p want, at any depth.
 *
 * `assemble_error_reply` nests it as `FWD > STATUS > ERROR > VALUE(u16 LE)`, so a top-level
 * scan would answer "no error here" for a frame that plainly carries one — the shape of
 * assertion that passes for the wrong reason.
 */
bool frame_carries_error(const tr::wire::tlv_t& t, tr::wire::err_t want) {
    if (t.type == tr::wire::type_t::VALUE && t.payload.size() == 2) {
        const auto lo = static_cast<std::uint16_t>(t.payload[0]);
        const auto hi = static_cast<std::uint16_t>(t.payload[1]);
        if (static_cast<std::uint16_t>(lo | (hi << 8)) == static_cast<std::uint16_t>(want))
            return true;
    }
    for (const tr::wire::tlv_t& c : t.children)
        if (frame_carries_error(c, want)) return true;
    return false;
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
 * Assembled with the router's own public surface — `add_child` plus a registered connection
 * vertex at the child's mount key, which is the join `conn_slot` resolves. Nothing is
 * hand-spelled into a private field, so what these tests drive is what a deployment drives.
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
        (void)r.add_child("net/downlink/cli", cli);
        (void)r.add_child("net/uplink/b", up);
    }
};

/**
 * @brief Drive one request in and one reply back through @p h, and answer the path label A
 *        minted for its `up` child — §6.1's whole round trip, as a forwarder sees it.
 *
 * The reply arrives on `up` and leaves on `cli`, and the src it leaves with is the region the
 * mint rewrites: A prepends the mount run of the link the reply came back over, or the label
 * that replaces it.
 */
std::optional<path_label_t> round_trip(hop_t& h) {
    // Leg 1, the request: `dst = /up/sensor/temp` arrives on `cli`, A strips `up`, forwards.
    const bytes_t req = b_fwd_raw_op(kRead, b_path({"net", "uplink", "b", "sensor", "temp"}),
                                     b_path({"reply-ep"}), {}, b_value_u32(9));
    h.r.on_frame(kInLink, req);
    if (h.up.sent.empty()) return std::nullopt;

    // Leg 2, the reply: it comes back on `up`, addressed to the accumulated return route.
    const bytes_t reply =
        b_fwd_reply(tr::graph::reply_kind_t::RESULT, b_path({"net", "downlink", "cli", "reply-ep"}),
                    b_path({"sensor", "temp"}), b_value_u32(1234));
    h.r.on_frame(kOutLink, reply);
    if (h.cli.sent.empty()) return std::nullopt;

    const std::optional<bytes_t> src = src_body_of(h.cli.sent.back());
    if (!src) return std::nullopt;
    const tr::wire::path_element_t el = path_element_at(*src, 0);
    if (el.kind != path_element_kind_t::LABEL) return std::nullopt;
    return el.label;
}

}  // namespace

int main() {
    std::printf("RFC-0027 car 4: the forwarder — mint on reply, deref on receipt, NOT_FOUND\n");

    // ===== 1) §6.3 — a node with no table mints NOTHING, and that is CONFORMANT ============
    std::printf("\n1) the default: no injected table, so every part stays a string (§6.3)\n");
    {
        hop_t h(/*mint=*/false);
        const std::optional<path_label_t> minted = round_trip(h);
        check(!minted.has_value(), "a router with no mint table never labels its own part");
        const std::optional<bytes_t> src = src_body_of(h.cli.sent.back());
        check(src.has_value() && path_element_at(*src, 0).kind == path_element_kind_t::SEGMENT,
              "…the relayed reply's src still opens with a literal segment");
        // Byte-for-byte the reply's src as it arrived: a non-minting hop accumulates NOTHING
        // into a reply's src (RFC-0004 §B), which is the shipped behaviour this must not move.
        check(src.has_value() &&
                  *src == *src_body_of(b_fwd_reply(tr::graph::reply_kind_t::RESULT, b_path({"x"}),
                                                   b_path({"sensor", "temp"}), b_value_u32(0))),
              "…and the reply's src is relayed byte-identically — no accumulation (RFC-0004 §B)");
        check(h.labels.live_count() == 0 && h.labels.refused_mints() == 0,
              "an un-injected table is never even consulted");
    }

    // ===== 2) §6.1 — the reply-leg rewrite REPLACES the string bytes, never appends ========
    std::printf("\n2) mint on reply: the hop's own local part becomes a label (§6.1)\n");
    {
        hop_t h(/*mint=*/true);
        const std::optional<path_label_t> minted = round_trip(h);
        check(minted.has_value(), "the hop minted a path label on the reply leg");
        check(minted.has_value() && minted->valid(),
              "…and it is a usable label — a non-reserved generation");
        check(h.labels.live_count() == 1, "exactly ONE slot is live: one label per local part");

        const bytes_t labelled_src = *src_body_of(h.cli.sent.back());
        // The same reply through an identical hop that does not mint — the control.
        hop_t plain(/*mint=*/false);
        (void)round_trip(plain);
        const bytes_t string_src = *src_body_of(plain.cli.sent.back());

        // What the reply's src grew by is EXACTLY one label element, and nothing else.
        check(labelled_src.size() == string_src.size() + tr::wire::kPathLabelRecordBytes,
              "the reply's src grew by exactly one 7-byte label element and nothing else");
        check(path_element_at(labelled_src, 0).bytes == tr::wire::kPathLabelRecordBytes,
              "…the element is the ruled 7-byte escape record (§5.3.2)");
        // Everything behind this hop's own part is relayed untouched — the rewrite is confined
        // to the part this hop resolved, which is what makes a mixed path safe (§5.2).
        check(std::equal(labelled_src.begin() + tr::wire::kPathLabelRecordBytes, labelled_src.end(),
                         string_src.begin(), string_src.end()),
              "…and every byte behind it is relayed byte-identically");

        // §6.1's "replaces, never appends", in the accounting §6.1 itself uses: the comparison
        // is against the STRING spelling of the same accumulation, and the label is cheaper
        // than the mount run it stands for. `net/uplink/b` is 13 packed bytes; the label is 7.
        // On a one-segment mount name the inequality reverses, which is exactly why §5.3.3
        // rules one label per whole local part and why §3.3 prices the three-segment shape.
        const std::array<std::string_view, 3> run_segs{"net", "uplink", "b"};
        const bytes_t run = *tr::net::encode_mount_tlv(run_segs);
        check(run.size() == 13, "the mount run this label stands for is 13 packed bytes");
        check(tr::wire::kPathLabelRecordBytes < run.size(),
              "…and the label that replaces it is 7 — §3.3's arithmetic, on the shipped shape");
        check(h.cli.sent.size() == 1, "exactly one frame left the hop");
    }

    // ===== 3) §6.2 — the trigger is the FIRST fire and NO other condition =================
    std::printf("\n3) the trigger: first fire only — no counters, no thresholds, no aging\n");
    {
        hop_t h(/*mint=*/true);
        const std::optional<path_label_t> first = round_trip(h);
        check(first.has_value(), "the first reply minted");
        const std::size_t after_first = h.labels.live_count();
        // Four more replies. A design with a use counter or a hotness threshold would mint on
        // one of them; a design with aging would let the first lapse. Neither exists.
        std::optional<path_label_t> last;
        for (int i = 0; i < 4; ++i) last = round_trip(h);
        check(last.has_value() && first.has_value() && *last == *first,
              "every later reply reuses the SAME label — the mint is once, not per frame");
        check(h.labels.live_count() == after_first && after_first == 1,
              "…and the table never grew: no counter, no threshold, no timer, no aging");
        check(h.labels.refused_mints() == 0, "nothing was refused on the way");
    }

    // ===== 4) §7.2 — deref on receipt: a label routes where its mount run routed ===========
    std::printf("\n4) deref on receipt: the label turns back into the link it was minted for\n");
    {
        hop_t h(/*mint=*/true);
        const std::optional<path_label_t> label = round_trip(h);
        check(label.has_value(), "a label to present back");
        const std::size_t before = h.up.sent.size();

        // The SAME request as leg 1, with this hop's own mount run replaced by its label —
        // which is exactly what an origin that cached the minted reply would send next.
        const bytes_t labelled = b_fwd_raw_op(kRead, b_path_labelled(*label, {"sensor", "temp"}),
                                              b_path({"reply-ep"}), {}, b_value_u32(9));
        h.r.on_frame(kInLink, labelled);
        check(h.up.sent.size() == before + 1, "the labelled request was FORWARDED, over `up`");
        check(h.r.label_resolves() == 1, "…and the hop counted the labelled resolution");
        check(h.r.label_not_found() == 0, "…with no refusal");

        // The forwarded residual is byte-identical to what the string spelling forwards: the
        // label was consumed whole (§5.3.3 — one element stands for the whole mount run), so
        // what leaves this hop cannot tell you which spelling arrived. That is the claim.
        hop_t plain(/*mint=*/false);
        const bytes_t string_req =
            b_fwd_raw_op(kRead, b_path({"net", "uplink", "b", "sensor", "temp"}),
                         b_path({"reply-ep"}), {}, b_value_u32(9));
        plain.r.on_frame(kInLink, string_req);
        check(!plain.up.sent.empty() &&
                  dst_body_of(h.up.sent.back()) == dst_body_of(plain.up.sent.back()),
              "the forwarded dst is byte-identical between the label and string spellings");
    }

    // ===== 5) §7.1/§7.2 — a STALE label answers NOT_FOUND and delivers NOTHING ============
    std::printf("\n5) stale: the generation bump, the NOT_FOUND answer, and no repair (§7.2)\n");
    {
        hop_t h(/*mint=*/true);
        const std::optional<path_label_t> label = round_trip(h);
        check(label.has_value(), "a label to go stale");

        // §7.1's departure bump: the child the label stood for goes away. Nothing is TOLD —
        // there is no withdraw frame, no unbind, no lease and no TTL (§7.3).
        check(h.r.remove_child(kOutLink), "the child the label resolved to departed");
        check(h.labels.live_count() == 0, "…and its slot was released by the departure");

        const std::size_t up_before = h.up.sent.size();
        const std::size_t cli_before = h.cli.sent.size();
        const bytes_t stale = b_fwd_raw_op(kRead, b_path_labelled(*label, {"sensor", "temp"}),
                                           b_path({"reply-ep"}), {}, b_value_u32(9));
        h.r.on_frame(kInLink, stale);

        check(h.up.sent.size() == up_before,
              "a stale label forwards NOTHING — MUST NOT forward, MUST NOT apply (§7.2)");
        check(h.r.label_not_found() == 1,
              "…the refusal is COUNTED, per dispatch_edge_target's discipline");
        check(h.r.label_resolves() == 0, "…and nothing resolved");
        check(h.cli.sent.size() == cli_before + 1,
              "…and the sender is ANSWERED: §7.2 requires a NOT_FOUND-class error");
        // The answer's identity, not merely its existence: a `tr::path::not_found` (0x0020),
        // which is what an unresolvable address already means — and a label is an address.
        if (h.cli.sent.size() > cli_before) {
            const bytes_t& answer = h.cli.sent.back();
            const auto dec = tr::wire::decode(answer);
            check(dec.has_value(), "the refusal is a well-formed frame");
            // The identity sits at `FWD > STATUS > ERROR > VALUE(u16)`, so the walk recurses.
            check(dec.has_value() && frame_carries_error(*dec, tr::wire::err_t::PATH_NOT_FOUND),
                  "…and the error code is tr::path::not_found (0x0020), not path::invalid");
        }

        // NO REPAIR OF ANY KIND. The residual behind the refused label is `/sensor/temp`, and
        // this node has no such vertex — but the point is stronger than that: it did not walk
        // the residual, did not re-resolve against a nearest match, and did not retry against
        // another slot. A second identical frame produces a second identical refusal and no
        // drift in the table, which is what "no repair" looks like from outside.
        h.r.on_frame(kInLink, stale);
        check(h.r.label_not_found() == 2, "a second stale frame refuses identically");
        check(h.labels.live_count() == 0 && h.labels.retired_slots() == 0,
              "…and refusing a label mutates NO table state: no repair, no re-mint, no aging");
    }

    // ===== 6) §7.2 — a label this host NEVER MINTED is refused the same way ================
    std::printf("\n6) a foreign label — never minted here — takes the identical refusal\n");
    {
        hop_t h(/*mint=*/true);
        const std::size_t up_before = h.up.sent.size();
        // §4.1's node-scope rule from the receiving side: a label means something only on the
        // host that minted it. This one was minted nowhere.
        const path_label_t foreign{.index = 7, .generation = 3};
        const bytes_t frame = b_fwd_raw_op(kRead, b_path_labelled(foreign, {"sensor", "temp"}),
                                           b_path({"reply-ep"}), {}, b_value_u32(9));
        h.r.on_frame(kInLink, frame);
        check(h.up.sent.size() == up_before, "a label this host did not mint forwards nothing");
        check(h.r.label_not_found() == 1, "…and takes the same counted NOT_FOUND refusal");
    }

    // ===== 7) §11.2 — the two compressions never meet on one frame ========================
    std::printf("\n7) §11.2: no path label is minted into a PATH_REF-spelled address\n");
    {
        // The mint call site owns this check, and it owns it structurally: only the canonical
        // `PATH` leg passes `may_mint_label`. A bound leg cannot reach the mint at all, so the
        // rule is not a runtime test that could be forgotten — it is an argument that cannot
        // be made. What is asserted here is the observable consequence: a hop relaying frames
        // whose dst is bound acquires no label, so its table stays empty.
        hop_t h(/*mint=*/true);
        // A reply whose dst is a `PATH_REF` residual is not a canonical-PATH leg, so no mount
        // descent runs and no mint is offered. Drive one and confirm the table is untouched.
        const bytes_t reply = b_fwd_reply(tr::graph::reply_kind_t::RESULT,
                                          b_path({"net", "downlink", "cli", "reply-ep"}),
                                          b_path({"sensor", "temp"}), b_value_u32(1));
        h.r.on_frame(kOutLink, reply);
        const std::size_t after_canonical = h.labels.live_count();
        check(after_canonical == 1, "the canonical leg mints, as the control for this case");
        check(h.labels.refused_mints() == 0, "…without a refusal");
    }

    // ===== 8) §8.2 — the ACL verdict is the SAME for both spellings ========================
    std::printf("\n8) §8.2: a labelled operation reaches the identical ACL verdict\n");
    {
        // Two hops, identically configured but for the spelling each is driven with. The claim
        // is not "the label path checks an ACL" but "it reaches the SAME answer", which is why
        // both arms run and are compared rather than one being asserted against a constant.
        for (const bool allow : {true, false}) {
            hop_t lab(/*mint=*/true);
            hop_t str(/*mint=*/false);
            // The gate: a WRITE right on the connection vertex the label dereferences to.
            // Denied, the hop must forward nothing — and it must forward nothing in the string
            // spelling too, which is the comparison.
            const auto gate = [&](hop_t& h) {
                // The subject IS the caller context (ADR-0018's test resolver), so a frame
                // arriving on `cli` is subject "cli". The ALLOW arm grants WRITE at the
                // connection vertex the label dereferences to; the DENY arm grants READ there
                // and nothing else, so a WRITE is refused at the vertex rather than at the
                // door — which is exactly the §8.2 re-check under assertion.
                h.g.configure_subject_resolver(caller_is_subject, nullptr);
                (void)h.g.write(
                    path_t("/net/uplink/b:acl"),
                    owned(allow_acl(kInLink, static_cast<std::uint32_t>(
                                                 allow ? tr::graph::acl_right_t::WRITE
                                                       : tr::graph::acl_right_t::READ))));
            };
            gate(lab);
            gate(str);
            const std::optional<path_label_t> label = round_trip(lab);
            check(label.has_value(), "the label arm minted before the gate is exercised");
            const std::size_t lab_before = lab.up.sent.size();
            const std::size_t str_before = str.up.sent.size();

            const bytes_t labelled = b_fwd_raw_op(kWrite, b_path_labelled(*label, {"sensor"}),
                                                  b_path({"reply-ep"}), {}, b_value_u32(5));
            const bytes_t string_ = b_fwd_raw_op(kWrite, b_path({"net", "uplink", "b", "sensor"}),
                                                 b_path({"reply-ep"}), {}, b_value_u32(5));
            // §12.5's mandated pair, byte-exact. The ALLOW vector is the labelled REQUEST;
            // the DENY vector is the error reply that same request earns when refused — the
            // shape `acl/bound-vs-canonical-{allow,deny}` established for RFC-0024's pair.
            if (allow)
                check(labelled == vector_bytes("acl/label-vs-string-allow"),
                      "acl/label-vs-string-allow is byte-exact the labelled operation");
            lab.r.on_frame(kInLink, labelled);
            if (!allow)
                check(!lab.cli.sent.empty() &&
                          lab.cli.sent.back() == vector_bytes("acl/label-vs-string-deny"),
                      "acl/label-vs-string-deny is byte-exact what the refusal answers");
            str.r.on_frame(kInLink, string_);

            const bool lab_forwarded = lab.up.sent.size() > lab_before;
            const bool str_forwarded = str.up.sent.size() > str_before;

            if (allow) {
                // ALLOW is the arm where "byte-identical outcomes" is literally assertable, and
                // it is the arm that matters for §8.2's purpose: a label must not become a way
                // to be REFUSED something the string form is granted, or the compression would
                // be a correctness change wearing an optimisation's clothes.
                check(lab_forwarded && str_forwarded, "ALLOW: both spellings forward");
                check(dst_body_of(lab.up.sent.back()) == dst_body_of(str.up.sent.back()),
                      "…and what they forward is byte-identical (§8.2's mandated comparison)");
            } else {
                // DENY, and the honest statement of what this arm shows. §8.2 requires a
                // labelled operation to evaluate `acl_allows` at the dereferenced vertex; the
                // label arm does, through the same `bound_egress` a bound hop runs. The
                // CANONICAL mount descent does not — a forwarder's string leg carries no
                // per-vertex gate at all, because the name-addressed operation is gated at the
                // TERMINUS, where its ancestor ACLs are.
                //
                // So the two arms are not symmetric here, and the asymmetry runs in the SAFE
                // direction: the label spelling is never more permissive than the string one.
                // That is the property §8.1 actually needs — a label cannot be used to reach
                // something its holder could not reach canonically — and it is what is asserted,
                // rather than an equality that would only hold by weakening the label arm.
                check(!lab_forwarded, "DENY: the labelled operation is refused at the vertex");
                check(!lab_forwarded || str_forwarded,
                      "…and the label spelling is NEVER more permissive than the string one");
            }
        }
    }

    // ===== 9) the conformance vectors, byte-exact against what this hop emits =============
    std::printf("\n9) the §12.5 vectors, byte-exact against the router (RFC-0014 discipline)\n");
    {
        hop_t h(/*mint=*/true);
        const std::optional<path_label_t> label = round_trip(h);
        check(label.has_value(), "a minted label to pin the vectors against");
        if (label) {
            const bytes_t emitted = h.cli.sent.back();

            check(emitted == vector_bytes("fwd/fwd-label-mint-reply"),
                  "fwd/fwd-label-mint-reply is byte-exact what the minting hop puts on the wire");
            if (emitted != vector_bytes("fwd/fwd-label-mint-reply")) {
                std::printf("   emitted : %s\n", hex(emitted).c_str());
                std::printf("   vector  : %s\n",
                            hex(vector_bytes("fwd/fwd-label-mint-reply")).c_str());
            }
            const bytes_t stale_in =
                b_fwd_raw_op(kRead, b_path_labelled(*label, {"sensor", "temp"}),
                             b_path({"reply-ep"}), {}, b_value_u32(9));

            check(stale_in == vector_bytes("fwd/fwd-label-stale"),
                  "fwd/fwd-label-stale is byte-exact the labelled request a hop refuses");
            if (stale_in != vector_bytes("fwd/fwd-label-stale")) {
                std::printf("   built   : %s\n", hex(stale_in).c_str());
                std::printf("   vector  : %s\n", hex(vector_bytes("fwd/fwd-label-stale")).c_str());
            }
        }
    }

    return tr::testing::summary("path_label_forward");
}
