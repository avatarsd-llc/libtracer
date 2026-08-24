/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0027 §6.1 point 3 and §7.2 — THE TERMINUS, both halves: the residual it MINTS on the
 * reply leg, and the labelled residual it DEREFERENCES when that label comes back.
 *
 * §6.1 point 2 is the forwarder's ("each hop rewrites its own local part") and is bound by
 * `path_label_forward_test.cpp`. Point 3 is this file's: *"the terminus does the same for the
 * residual it resolved."* Without it the reply that reaches the origin is minted at every hop
 * but the LAST, §6.1 point 4's "fully-minted src" is unreachable, and §12.4 axis 2 — the axis
 * §3.3 nominates as the one that decides — has no labelled residual to time.
 *
 * It exists as a test for the reason car 4's does: the conformance harness routes nothing, so
 * every behavioural claim is bound against the production `fwd_router_t` + `op_resolver_t`
 * wiring or it is bound nowhere (RFC-0014's lesson — two silent misroutes shipped because no
 * test used the production wiring).
 *
 * The topology is one node deep, because a terminus is where the residual stops:
 *
 *     cli ──(the request arrives here)──▶ T, which resolves `/sensor/temp` locally
 *
 * The reply's `src` IS the request's `dst` (the reply builder's swap), and at a terminus the
 * request's `dst` IS the residual — so the region the rewrite lands in and the part being
 * rewritten are the same bytes, and §6.1's "replaces, never appends" is literal here.
 *
 * The DEREF half (sections 9 onward, #1363) closes that loop on the same one-node topology,
 * and it can only be tested by closing it: the label a deref consumes is one this very node
 * minted, so the mint sections above are its fixture and the round trip is the test. Until it
 * existed a labelled residual addressed at a LOCAL vertex took §7.2's counted `NOT_FOUND` —
 * `route_label_forward` resolved a label only through `bound_egress`, which answers for
 * connection vertices and nothing else — and §12.4 axis 2, the axis §3.3 nominates as the one
 * that decides, had no labelled residual to time.
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
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_element.hpp"
#include "libtracer/path_label.hpp"
#include "libtracer/path_label_table.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::path_label_table_t;
using tr::testing::b_fwd_raw_op;
using tr::testing::bytes_t;
using tr::testing::check;
using tr::wire::path_element_at;
using tr::wire::path_element_kind_t;
using tr::wire::path_label_t;

/** @brief The inbound link, spelled as the three-segment mount run a real node uses. */
constexpr std::string_view kInLink = "net/downlink/cli";

/** @brief The op bytes this file drives (RFC-0004 §D); bit 7 is RFC-0024's mint request. */
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

/**
 * @brief A `PATH` TLV whose WHOLE body is one label element — the labelled residual (§6.1).
 *
 * There is no string beside it, deliberately: a label REPLACES the bytes of the part it stands
 * for, and a terminus that could fall back to a name sitting next to the label would not be
 * testing §7.2's refusal at all.
 */
bytes_t b_path_label(path_label_t label) {
    bytes_t body;
    (void)tr::wire::emit_path_label(body, label);
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

/** @brief The `src` PATH body of an emitted FWD frame — the region §6.1 point 3 rewrites. */
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
 * scan would answer "no error here" for a frame that plainly carries one.
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

/**
 * @brief Everything a reply says that is NOT its address: the `op`, the `kind` and the answer.
 *
 * §8.2's comparison at a terminus is made over exactly this. The two spellings' replies differ
 * in the `src` region **by construction** — that region is the compression — so comparing whole
 * frames would assert the opposite of what the RFC asks for. What must be identical is the
 * VERDICT: the same kind, the same status identity, the same bytes of answer. Concatenated in
 * frame order, so a re-ordering is a difference too.
 */
std::optional<tr::wire::tlv_t> answer_of(std::span<const std::byte> frame) {
    auto decoded = tr::wire::decode(frame);
    if (!decoded) return std::nullopt;
    std::optional<tr::wire::tlv_t> dec = std::move(*decoded);
    int paths = 0;
    for (auto it = dec->children.begin(); it != dec->children.end(); ++it) {
        if (it->type != tr::wire::type_t::PATH) continue;
        if (++paths != 2) continue;  // the FIRST PATH is `dst`, the second is `src`
        dec->children.erase(it);
        break;
    }
    return dec;
}

/** @brief Does @p frame carry a trailing `PATH_REF` — RFC-0024's bound-path mint answer? */
bool carries_bound_mint(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->children.empty()) return false;
    return dec->children.back().type == tr::wire::type_t::PATH_REF;
}

/** @brief The `dst` PATH body of a FWD frame — what a terminus reply's `src` is built from. */
std::optional<bytes_t> dst_body_of(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec) return std::nullopt;
    for (const tr::wire::tlv_t& c : dec->children)
        if (c.type == tr::wire::type_t::PATH) return bytes_t(c.payload.begin(), c.payload.end());
    return std::nullopt;
}

/**
 * @brief One TERMINUS node: an inbound `cli` link, a local `/sensor/temp` holding a value,
 *        and a mint table it either has or has not been given (§6.3's default is not having).
 *
 * Assembled through the router's own public surface — `add_child` plus `configure_path_labels`
 * — so what these tests drive is what a deployment drives.
 */
struct node_t {
    graph_t g;
    fwd_router_t r{g};
    path_label_table_t labels{&tr::mem::heap_source(), 64, 16};
    span_sink_t cli;

    /** @brief Wire T up with @p mint deciding whether it labels at all. */
    explicit node_t(bool mint) {
        (void)g.register_vertex(path_t("/sensor"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/sensor/humidity"), role_t::STORED_VALUE);
        (void)g.write(path_t("/sensor/temp"), owned(b_value_u32(1234)));
        (void)g.write(path_t("/sensor/humidity"), owned(b_value_u32(55)));
        if (mint) r.configure_path_labels(&labels);
        (void)r.add_child(std::string(kInLink), cli);
    }
};

/** @brief A READ of @p segs arriving on `cli` — the frame a terminus resolves locally. */
bytes_t read_request(std::initializer_list<std::string_view> segs) {
    return b_fwd_raw_op(kRead, b_path(segs), b_path({"reply-ep"}));
}

/**
 * @brief Drive one request in and answer the path label T minted for the residual it resolved.
 *
 * `std::nullopt` when the reply's `src` opens with anything but a label — which is the
 * un-injected node's answer and the whole of §6.3's default.
 */
std::optional<path_label_t> terminate(node_t& n, std::initializer_list<std::string_view> segs = {
                                                     "sensor", "temp"}) {
    n.r.on_frame(kInLink, read_request(segs));
    if (n.cli.sent.empty()) return std::nullopt;
    const std::optional<bytes_t> src = src_body_of(n.cli.sent.back());
    if (!src || src->empty()) return std::nullopt;
    const tr::wire::path_element_t el = path_element_at(*src, 0);
    if (el.kind != path_element_kind_t::LABEL) return std::nullopt;
    return el.label;
}

}  // namespace

int main() {
    std::printf("RFC-0027 §6.1 point 3: the terminus mints for the residual it resolved\n");

    // ===== 1) §6.3 — a terminus with no table mints NOTHING, and that is CONFORMANT =======
    std::printf("\n1) the default: no injected table, so the residual stays a string (§6.3)\n");
    {
        node_t n(/*mint=*/false);
        check(!terminate(n).has_value(), "a terminus with no mint table never labels");
        const std::optional<bytes_t> src = src_body_of(n.cli.sent.back());
        check(src.has_value() && path_element_at(*src, 0).kind == path_element_kind_t::SEGMENT,
              "…the reply's src still opens with a literal segment");
        // Byte-for-byte the request's dst, echoed as the reply's src: the shipped behaviour,
        // which this change must not move for any host that does not mint.
        check(src.has_value() && *src == *dst_body_of(read_request({"sensor", "temp"})),
              "…and it is the request's dst echoed byte-identically — today's reply exactly");
        check(n.labels.live_count() == 0 && n.labels.refused_mints() == 0,
              "an un-injected table is never even consulted");
    }

    // ===== 2) §6.1 point 3 — the label REPLACES the residual, and the frame gets SHORTER ==
    std::printf("\n2) the rewrite: the residual becomes one label element (§6.1 point 3)\n");
    {
        node_t n(/*mint=*/true);
        const std::optional<path_label_t> minted = terminate(n);
        check(minted.has_value(), "the terminus minted a path label for the residual");
        check(minted.has_value() && minted->valid(),
              "…and it is a usable label — a non-reserved generation");
        check(n.labels.live_count() == 1, "exactly ONE slot is live: one label per local part");

        const bytes_t labelled_src = *src_body_of(n.cli.sent.back());
        node_t plain(/*mint=*/false);
        (void)terminate(plain);
        const bytes_t string_src = *src_body_of(plain.cli.sent.back());

        // REPLACES, never appends — and at a terminus that is literal rather than accounted
        // for: the label stands where the whole residual stood, so the src is exactly one
        // 7-byte element and nothing else.
        check(labelled_src.size() == tr::wire::kPathLabelRecordBytes,
              "the reply's src is exactly one 7-byte label element and nothing else");
        check(path_element_at(labelled_src, 0).bytes == tr::wire::kPathLabelRecordBytes,
              "…the element is the ruled 7-byte escape record (§5.3.2)");
        check(string_src.size() == 12, "the residual it replaces is 12 packed bytes");
        check(labelled_src.size() < string_src.size(),
              "…so the reply got SHORTER — §6.1's zero-added-bytes, in the strong direction");
        check(n.cli.sent.back().size() + (string_src.size() - labelled_src.size()) ==
                  plain.cli.sent.back().size(),
              "…and the whole frame shrank by exactly the difference, nothing else moved");
        check(n.cli.sent.size() == 1, "exactly one reply left the node");
    }

    // ===== 3) §6.2 — the trigger is the FIRST fire and NO other condition =================
    std::printf("\n3) the trigger: first fire only — no counters, no thresholds, no aging\n");
    {
        node_t n(/*mint=*/true);
        const std::optional<path_label_t> first = terminate(n);
        check(first.has_value(), "the first terminated operation minted");
        std::optional<path_label_t> last;
        for (int i = 0; i < 4; ++i) last = terminate(n);
        check(last.has_value() && first.has_value() && *last == *first,
              "every later reply reuses the SAME label — the mint is once, not per frame");
        check(n.labels.live_count() == 1,
              "…and the table never grew: no counter, no threshold, no timer, no aging");
        check(n.labels.refused_mints() == 0, "nothing was refused on the way");
    }

    // ===== 4) §8.1 — POST-AUTH ONLY: a denied operation mints NOTHING =====================
    std::printf("\n4) §8.1: a denied operation answers denied and nothing else\n");
    {
        node_t n(/*mint=*/true);
        n.g.configure_subject_resolver(caller_is_subject, nullptr);
        // The subject IS the caller context, so a frame arriving on `cli` is subject `cli`.
        // WRITE and nothing else, so the READ below is refused AT THE VERTEX.
        (void)n.g.write(
            path_t("/sensor/temp:acl"),
            owned(allow_acl(kInLink, static_cast<std::uint32_t>(tr::graph::acl_right_t::WRITE))));
        const std::optional<path_label_t> minted = terminate(n);
        check(!minted.has_value(), "a DENIED read mints no label for the vertex it was denied");
        check(n.labels.live_count() == 0,
              "…and spends no slot: probing yields exists+denied, never a handle to it (§8.1)");
        check(!n.cli.sent.empty(), "…while the peer is still answered");
        const std::optional<bytes_t> src = src_body_of(n.cli.sent.back());
        check(src.has_value() && path_element_at(*src, 0).kind == path_element_kind_t::SEGMENT,
              "…with the ordinary string src an unlabelled node would have answered with");

        // The control, on the very same node: the right the ACL DOES grant mints normally, so
        // what section 4 shows is the gate and not a node that cannot mint at all.
        n.r.on_frame(kInLink, b_fwd_raw_op(kWrite, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                                           {}, b_value_u32(7)));
        check(n.labels.live_count() == 1, "the granted WRITE on the same vertex mints normally");
    }

    // ===== 5) §11.2 — the two compressions never meet on one frame ========================
    std::printf("\n5) §11.2: no path label is minted where a PATH_REF is already spelled\n");
    {
        node_t n(/*mint=*/true);
        // A mint-flagged request is ASKING for RFC-0024's bound spelling of this very address.
        // Answering it with a path label as well would put both compressions of one address on
        // one frame, which is what §11.2 refuses. The refusal is structural: the flag is read
        // where the mint decision is made, so there is no runtime switch to forget.
        n.r.on_frame(kInLink, b_fwd_raw_op(kRead | tr::graph::kFwdOpFlagMintRequest,
                                           b_path({"sensor", "temp"}), b_path({"reply-ep"})));
        check(n.labels.live_count() == 0, "a mint-flagged request acquires no path label");
        const std::optional<bytes_t> src = src_body_of(n.cli.sent.back());
        check(src.has_value() && path_element_at(*src, 0).kind == path_element_kind_t::SEGMENT,
              "…and its reply's src is the ordinary string spelling");
        // The control: the identical request without the flag mints, so what this shows is the
        // exclusion and not an inert code path.
        check(terminate(n).has_value(), "the same request unflagged mints, as the control");
        check(n.labels.live_count() == 1, "…exactly one slot, from the unflagged leg alone");
    }

    // ===== 6) one label per child: a SECOND residual stays a string =======================
    std::printf("\n6) one terminus label per child — the second residual stays a string\n");
    {
        node_t n(/*mint=*/true);
        check(terminate(n, {"sensor", "temp"}).has_value(), "the first residual is labelled");
        check(!terminate(n, {"sensor", "humidity"}).has_value(),
              "a second, different residual on the same child is left as the string it is");
        check(n.labels.live_count() == 1,
              "…and the table's row count stays bounded by LINKS, not by addresses touched");
        // The degrade is invisible to correctness (§6.3): the operation itself succeeded, and
        // the peer got the answer it asked for in the spelling it already understands.
        const std::optional<bytes_t> src = src_body_of(n.cli.sent.back());
        check(src.has_value() && *src == *dst_body_of(read_request({"sensor", "humidity"})),
              "…and the unlabelled reply is byte-identical to an unlabelling node's");
        check(terminate(n, {"sensor", "temp"}).has_value(),
              "the labelled residual keeps its label — the second one evicted nothing (§8.3)");
    }

    // ===== 7) §7.1 — the departure bump releases the terminus label too ===================
    std::printf("\n7) §7.1: the child departs, and its terminus label departs with it\n");
    {
        node_t n(/*mint=*/true);
        check(terminate(n).has_value(), "a label to invalidate");
        check(n.labels.live_count() == 1, "…live before the departure");
        check(n.r.remove_child(kInLink), "the child the label was minted for departed");
        check(n.labels.live_count() == 0,
              "…and its slot was released — nothing is told, the next frame discovers it (§7.3)");
    }

    // ===== 8) the conformance vector, byte-exact against what this terminus emits =========
    std::printf("\n8) the vector, byte-exact against the router (RFC-0014 discipline)\n");
    {
        node_t n(/*mint=*/true);
        check(terminate(n).has_value(), "a minted label to pin the vector against");
        const bytes_t emitted = n.cli.sent.back();
        check(emitted == vector_bytes("fwd/fwd-label-terminus-reply"),
              "fwd/fwd-label-terminus-reply is byte-exact what the terminus puts on the wire");
        if (emitted != vector_bytes("fwd/fwd-label-terminus-reply")) {
            std::printf("   emitted : %s\n", hex(emitted).c_str());
            std::printf("   vector  : %s\n",
                        hex(vector_bytes("fwd/fwd-label-terminus-reply")).c_str());
        }
    }

    // ===== 9) §7.2 — the ROUND TRIP: the label comes back and dereferences LOCALLY ========
    std::printf("\n9) the deref: a labelled residual resolves at the vertex it aliases (§7.2)\n");
    {
        node_t n(/*mint=*/true);
        const std::optional<path_label_t> label = terminate(n);
        check(label.has_value(), "the terminus minted a label to present back");
        const bytes_t string_reply = n.cli.sent.back();

        // The origin's next request, spelled the way §6.1 says it may be: the whole residual
        // is that one label and there is no string beside it to fall back to.
        const bytes_t labelled = b_fwd_raw_op(kRead, b_path_label(*label), b_path({"reply-ep"}));
        n.r.on_frame(kInLink, labelled);

        check(n.cli.sent.size() == 2, "the labelled request was ANSWERED");
        check(n.r.label_resolves() == 1, "…the deref landed on a LOCAL vertex and is counted");
        check(n.r.label_not_found() == 0, "…and nothing took §7.2's refusal");
        // The whole claim of the compression, in one compare: the answer is not merely correct,
        // it is the SAME frame. The reply's `src` is the echoed labelled `dst`, and the label a
        // fresh mint would produce is the one already there — so the two spellings converge on
        // one reply rather than on two that happen to mean the same thing.
        check(n.cli.sent.back() == string_reply,
              "the labelled read's reply is BYTE-IDENTICAL to the string read's");
        check(n.labels.live_count() == 1 && n.labels.refused_mints() == 0,
              "…and the deref spent no second slot: §6.1's rewrite has reached its fixed point");

        // What the origin actually bought, on the axis §12.4 axis 2 times: the request itself.
        check(labelled.size() < read_request({"sensor", "temp"}).size(),
              "the labelled REQUEST is shorter than the string one it replaces");
    }

    // ===== 10) §8.2 — the ACL verdict is the SAME for both spellings, at the terminus =====
    std::printf("\n10) §8.2: the labelled operation reaches the identical ACL verdict\n");
    {
        // Two nodes, identically gated, driven with the two spellings of one address. The claim
        // is not "the label path checks an ACL" — it is that it reaches the SAME answer, which
        // is why both arms run and are compared rather than one asserted against a constant.
        // At a terminus that reuse is structural: the deref hands a vertex to the resolver and
        // the gate is `graph_t::read`'s own `acl_allows`, which is the string spelling's gate
        // in the string spelling's place. There is no second implementation to disagree.
        for (const bool allow : {true, false}) {
            node_t lab(/*mint=*/true);
            node_t str(/*mint=*/true);
            const auto gate = [&](node_t& n) {
                // WRITE always, so both arms can MINT through a granted operation; READ only
                // on the allow arm, so the read under test is refused AT THE VERTEX.
                n.g.configure_subject_resolver(caller_is_subject, nullptr);
                const std::uint32_t mask =
                    static_cast<std::uint32_t>(tr::graph::acl_right_t::WRITE) |
                    (allow ? static_cast<std::uint32_t>(tr::graph::acl_right_t::READ) : 0u);
                (void)n.g.write(path_t("/sensor/temp:acl"), owned(allow_acl(kInLink, mask)));
            };
            gate(lab);
            gate(str);
            // The mint rides the granted WRITE, so the label exists in both arms and on both
            // sides of the gate — §8.1's post-auth rule supplies the fixture as well as the
            // property.
            // Driven on BOTH nodes, so the two arms differ in the SPELLING under test and in
            // nothing else — including the value the read then answers with.
            const bytes_t seed = b_fwd_raw_op(kWrite, b_path({"sensor", "temp"}),
                                              b_path({"reply-ep"}), {}, b_value_u32(7));
            lab.r.on_frame(kInLink, seed);
            str.r.on_frame(kInLink, seed);
            const std::optional<bytes_t> minted_src = src_body_of(lab.cli.sent.back());
            check(minted_src.has_value() &&
                      path_element_at(*minted_src, 0).kind == path_element_kind_t::LABEL,
                  "the granted WRITE minted the label this arm then presents back");
            const path_label_t label = path_element_at(*minted_src, 0).label;

            lab.r.on_frame(kInLink, b_fwd_raw_op(kRead, b_path_label(label), b_path({"reply-ep"})));
            str.r.on_frame(kInLink,
                           b_fwd_raw_op(kRead, b_path({"sensor", "temp"}), b_path({"reply-ep"})));

            const std::optional<tr::wire::tlv_t> la = answer_of(lab.cli.sent.back());
            const std::optional<tr::wire::tlv_t> sa = answer_of(str.cli.sent.back());
            check(la.has_value() && sa.has_value(), "both spellings answered a decodable frame");
            check(la.has_value() && sa.has_value() && tr::wire::equal(*la, *sa),
                  allow ? "ALLOW: the two spellings' answers are byte-identical"
                        : "DENY: the two spellings' answers are byte-identical");
            if (!allow) {
                const auto dec = tr::wire::decode(lab.cli.sent.back());
                // At a TERMINUS a denial is spelled `denied`, not `not_found`, and that is the
                // reuse rule rather than a departure from car 4's forwarder: §8.1 requires a
                // labelled probe to yield what a string probe yields, and here the string probe
                // yields `denied`. A hop answers `not_found` because there the string probe
                // yields nothing at all — same rule, different observable.
                check(dec.has_value() && frame_carries_error(*dec, tr::wire::err_t::ACCESS_DENIED),
                      "…and it is tr::access::denied (0x0050) — what the string probe yields");
            }
        }
    }

    // ===== 11) §7.2 — a label this host never minted is refused, and applies NOTHING ======
    std::printf("\n11) §7.2 at the terminus: an unknown label refuses and applies nothing\n");
    {
        node_t n(/*mint=*/true);
        // §4.1's node-scope rule from the receiving side. This one was minted nowhere.
        const path_label_t foreign{.index = 11, .generation = 5};
        const bytes_t write =
            b_fwd_raw_op(kWrite, b_path_label(foreign), b_path({"reply-ep"}), {}, b_value_u32(999));
        n.r.on_frame(kInLink, write);
        check(n.r.label_not_found() == 1, "…the refusal is COUNTED (§7.2's discipline)");
        check(n.r.label_resolves() == 0, "…and nothing resolved");
        check(!n.cli.sent.empty(),
              "…the sender is ANSWERED: §7.2 requires a NOT_FOUND-class error");
        const auto dec = tr::wire::decode(n.cli.sent.back());
        check(dec.has_value() && frame_carries_error(*dec, tr::wire::err_t::PATH_NOT_FOUND),
              "…and the code is tr::path::not_found (0x0020) — an address that does not resolve");
        // MUST NOT APPLY. The label named nothing, so the write must not have landed anywhere —
        // and in particular not on the vertex a plausible neighbouring slot would have named.
        const auto held = n.g.read(path_t("/sensor/temp"));
        bytes_t still;
        std::vector<std::span<const std::byte>> iov;
        if (held.has_value() && (**held).try_to_iovec(iov))
            for (const std::span<const std::byte> s : iov)
                still.insert(still.end(), s.begin(), s.end());
        check(still == b_value_u32(1234),
              "…and the WRITE it carried landed NOWHERE — MUST NOT apply (§7.2)");
        check(n.labels.live_count() == 0 && n.labels.retired_slots() == 0,
              "…and refusing mutates NO table state: no repair, no re-mint, no aging (§7.3)");
    }

    // ===== 12) §7.1 / §4.1 — a re-added child cannot present its predecessor's label ======
    std::printf("\n12) §7.1: the departure bump, and no impersonation by a re-added child\n");
    {
        node_t n(/*mint=*/true);
        const std::optional<path_label_t> label = terminate(n);
        check(label.has_value(), "a label to invalidate");
        check(n.r.remove_child(kInLink), "the child the label was minted for departed");
        check(n.labels.live_count() == 0, "…and its slot was released (§7.1)");
        // Nothing was TOLD — no withdraw frame, no unbind, no lease, no TTL (§7.3). The peer
        // that reconnects at the same NAME is a different peer identity, and presenting the
        // predecessor's label buys it exactly one NOT_FOUND.
        check(n.r.add_child(std::string(kInLink), n.cli), "a new child arrives at the same name");
        const std::size_t before = n.cli.sent.size();
        n.r.on_frame(kInLink, b_fwd_raw_op(kRead, b_path_label(*label), b_path({"reply-ep"})));
        check(n.r.label_resolves() == 0, "the successor's frame dereferences NOTHING");
        check(n.r.label_not_found() == 1, "…it takes the counted refusal (§7.2)");
        check(n.cli.sent.size() == before + 1, "…and is answered rather than silently dropped");
    }

    // ===== 13) §11.2 — a labelled request is not answered with a PATH_REF mint ============
    std::printf("\n13) §11.2: the two compressions never meet, on the deref leg either\n");
    {
        node_t n(/*mint=*/true);
        const std::optional<path_label_t> label = terminate(n);
        check(label.has_value(), "a label to present back");
        // The origin asks for RFC-0024's bound spelling of an address it is already addressing
        // in the labelled one. §11.2's second clause — "SHOULD NOT bind a PATH_REF over a path
        // whose elements are already labelled" — is what refuses; the operation itself is
        // unaffected (§6.3), which is why the reply is still a well-formed RESULT.
        n.r.on_frame(kInLink, b_fwd_raw_op(kRead | tr::graph::kFwdOpFlagMintRequest,
                                           b_path_label(*label), b_path({"reply-ep"})));
        check(n.r.label_resolves() == 1, "the mint-flagged labelled read still RESOLVED");
        check(!carries_bound_mint(n.cli.sent.back()),
              "…and its reply carries no PATH_REF: one address, one compression (§11.2)");
        // The control, on the same node: a mint-flagged STRING read does answer with one, so
        // what section 13 shows is the exclusion and not a node that cannot mint at all.
        n.r.on_frame(kInLink, b_fwd_raw_op(kRead | tr::graph::kFwdOpFlagMintRequest,
                                           b_path({"sensor", "humidity"}), b_path({"reply-ep"})));
        check(carries_bound_mint(n.cli.sent.back()),
              "the same flag on a string-spelled address mints normally, as the control");
    }

    // ===== 14) the deref vectors, byte-exact against what this terminus emits =============
    std::printf("\n14) the deref vectors, byte-exact against the router (RFC-0014 discipline)\n");
    {
        node_t n(/*mint=*/true);
        const std::optional<path_label_t> label = terminate(n);
        check(label.has_value(), "a minted label to pin the vectors against");
        const bytes_t request = b_fwd_raw_op(kRead, b_path_label(*label), b_path({"reply-ep"}));
        check(request == vector_bytes("fwd/fwd-label-terminus-deref"),
              "fwd/fwd-label-terminus-deref is byte-exact the labelled request a terminus derefs");
        if (request != vector_bytes("fwd/fwd-label-terminus-deref")) {
            std::printf("   emitted : %s\n", hex(request).c_str());
            std::printf("   vector  : %s\n",
                        hex(vector_bytes("fwd/fwd-label-terminus-deref")).c_str());
        }
        // …and the reply it earns is the mint vector's own bytes, which is point 4's
        // accumulation closing on itself: what the terminus minted is what comes back.
        n.r.on_frame(kInLink, request);
        check(n.cli.sent.back() == vector_bytes("fwd/fwd-label-terminus-reply"),
              "…and the reply it earns is the mint vector, unchanged — the loop is closed");

        // The negative, on a fresh node so the counters mean what they say.
        node_t s(/*mint=*/true);
        const path_label_t never{.index = 11, .generation = 5};
        s.r.on_frame(kInLink, b_fwd_raw_op(kRead, b_path_label(never), b_path({"reply-ep"})));
        check(!s.cli.sent.empty() &&
                  s.cli.sent.back() == vector_bytes("fwd/fwd-label-terminus-stale"),
              "fwd/fwd-label-terminus-stale is byte-exact the NOT_FOUND a terminus answers");
        if (!s.cli.sent.empty() &&
            s.cli.sent.back() != vector_bytes("fwd/fwd-label-terminus-stale")) {
            std::printf("   emitted : %s\n", hex(s.cli.sent.back()).c_str());
            std::printf("   vector  : %s\n",
                        hex(vector_bytes("fwd/fwd-label-terminus-stale")).c_str());
        }
    }

    return tr::testing::summary("path_label_terminus");
}
