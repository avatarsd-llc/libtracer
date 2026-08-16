/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0027 §6.1 point 3 — THE TERMINUS: the residual half of the reply-leg rewrite.
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
    path_label_table_t labels{std::pmr::get_default_resource(), 64, 16};
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

    return tr::testing::summary("path_label_terminus");
}
