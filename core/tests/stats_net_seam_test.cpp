/**
 * @file
 * @brief RFC-0010 Amendment 2 (#1503 residual) — the NET-PLANE `:stats` seams: `router`,
 *        `labels` and `link`, sampled through the seam the router registers UP.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Amendment 1 stopped the census at the graph because L4 cannot reach DOWN into the net
 * plane. Amendment 2 inverts the DIRECTION rather than the dependency: `fwd_router_t`'s
 * constructor installs a sixth `{fn, ctx}` sampler on the graph beside the five it already
 * installs, so the router / label-table / per-link counters become wire-readable and L4
 * still names nothing below it.
 *
 * What a case here has to prove — the same bar `router_drop_stats_test.cpp` sets:
 *
 *   - the block is LIVE, not a shape: a counter this test provokes shows up in the block a
 *     later read returns, so a seam wired to the wrong accessor (or to nothing) fails;
 *   - the POSITIVE CONTROL is a graph with NO router: every net-plane spelling must answer
 *     `SCHEMA_NOT_FOUND` there, which is both the amendment's "this node does not publish
 *     that seam" rule and the proof that the passing cases are the SAMPLER's doing;
 *   - §D.2 still holds across the new classes: an unserved net-plane NAME is refused
 *     caller-INDEPENDENTLY, so nothing about who asked can be read off the answer.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::acl_right_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::check;
using tr::testing::make_value;

std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The test resolver (ADR-0018): the caller context IS the subject token. */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
}

/** @brief A link whose interface-level drop counters this test dictates outright. */
class counting_link_t : public transport_t {
   public:
    void send(std::span<const std::byte>) override {}
    [[nodiscard]] tr::net::transport_drop_stats_t drop_stats() const noexcept override {
        return counters;
    }
    tr::net::transport_drop_stats_t counters{}; /**< @brief What this link will report. */
};

/** @brief Read `path:field` as @p caller — the handle+field overload the FWD resolver uses. */
tr::graph::result_t<tr::view::rope_t> read_as(graph_t& g, const char* path,
                                              std::string_view caller) {
    const auto p = path_t::parse(path);
    if (!p) return std::unexpected(status_t::INVALID_PATH);
    const auto v = g.find(p->key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    return g.read(*v, p->field(), caller);
}

/** @brief The flattened wire bytes of a field read (empty on error). */
std::vector<std::byte> read_bytes(graph_t& g, const char* path, std::string_view caller = {}) {
    const auto r = read_as(g, path, caller);
    if (!r) return {};
    const tr::view::view_t flat = r->flatten();
    const auto span = flat.bytes();
    return std::vector<std::byte>(span.begin(), span.end());
}

/** @brief The u64 value of the member named @p noun in a decoded census block, or `-1`. */
std::uint64_t counter(const tr::wire::tlv_t& block, std::string_view noun) {
    for (std::size_t i = 0; i + 1 < block.children.size(); i += 2) {
        if (block.children[i].type != type_t::NAME) continue;
        if (tr::detail::as_string_view(block.children[i].payload) != noun) continue;
        if (block.children[i + 1].type != type_t::VALUE) break;
        if (block.children[i + 1].payload.size() != 8) break;
        return tr::detail::load_le<std::uint64_t>(block.children[i + 1].payload);
    }
    return static_cast<std::uint64_t>(-1);
}

/** @brief Lower-case hex of @p b, for the byte-exact pins. */
std::string hex(std::span<const std::byte> b) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (std::byte x : b) {
        out.push_back(d[std::to_integer<unsigned>(x) >> 4]);
        out.push_back(d[std::to_integer<unsigned>(x) & 0xF]);
    }
    return out;
}

/** @brief True iff the read SUCCEEDED — the affirmative half of @ref status_of. */
bool reads_ok(graph_t& g, const char* path, std::string_view caller = {}) {
    return read_as(g, path, caller).has_value();
}

/** @brief The status a read reported; only ever called where the read must FAIL. */
status_t status_of(graph_t& g, const char* path, std::string_view caller = {}) {
    const auto r = read_as(g, path, caller);
    return r ? status_t::NOT_FOUND : r.error();
}

/**
 * @brief §D.4 — `:stats.router.drops` serves the whole `router_stats_t` block, LIVE.
 *
 * The block is checked twice around a provoked malformed frame: a seam wired to a stale
 * copy, or to some other accessor, cannot pass the second read.
 */
void test_router_seam_is_live() {
    std::printf("RFC-0010 Am.2 §D.4: :stats.router.drops is the router's whole block, live:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    fwd_router_t router(g);

    const auto fresh_bytes = read_bytes(g, "/sink:stats.router.drops");
    const auto fresh = tr::wire::decode(fresh_bytes);
    check(fresh && fresh->type == type_t::SETTINGS && fresh->opt.pl,
          "the answer is ONE structured SETTINGS TLV, the Amendment 1 §D.3 shape");
    check(fresh && fresh->children.size() == 14, "seven NAME/VALUE pairs — every per-cause noun");
    check(fresh && counter(*fresh, "malformed_rx") == 0 && counter(*fresh, "arena_dropped") == 0 &&
              counter(*fresh, "flatten_dropped") == 0,
          "…and all seven are zero on a router that has seen nothing");
    // These bytes are the `settings/stats-seam-net-router` conformance vector.
    const std::string expect =
        "0b40e000"                                          // SETTINGS PL=1, len=224
        "02000f00666c617474656e5f64726f70706564"            // NAME "flatten_dropped"
        "010008000000000000000000"                          // VALUE u64 = 0
        "02001300666f72776172645f696f765f64726f70706564"    // "forward_iov_dropped"
        "010008000000000000000000"                          // VALUE u64 = 0
        "02000d006172656e615f64726f70706564"                // NAME "arena_dropped"
        "010008000000000000000000"                          // VALUE u64 = 0
        "02001000617373656d626c655f64726f70706564"          // NAME "assemble_dropped"
        "010008000000000000000000"                          // VALUE u64 = 0
        "020011007265706c795f696f765f64726f70706564"        // NAME "reply_iov_dropped"
        "010008000000000000000000"                          // VALUE u64 = 0
        "0200140064656c69766572795f696f765f64726f70706564"  // "delivery_iov_dropped"
        "010008000000000000000000"                          // VALUE u64 = 0
        "02000c006d616c666f726d65645f7278"                  // NAME "malformed_rx"
        "010008000000000000000000";                         // VALUE u64 = 0
    check(hex(fresh_bytes) == expect, "the block is byte-exact (the stats-seam-net-router vector)");
    if (hex(fresh_bytes) != expect) std::printf("  actual: %s\n", hex(fresh_bytes).c_str());

    // A FWD envelope with `.pl` cleared: a FWD body is a child list, never an opaque
    // payload, so the decode refuses it and the router counts one `malformed_rx`.
    std::vector<std::byte> body;
    const auto op = tr::testing::fwd_op_child(static_cast<std::uint8_t>(fwd_op_t::WRITE));
    body.insert(body.end(), op.begin(), op.end());
    const auto dst = tr::testing::b_path({"sink"});
    body.insert(body.end(), dst.begin(), dst.end());
    const auto src = tr::testing::b_path({"origin"});
    body.insert(body.end(), src.begin(), src.end());
    router.on_frame("up", tr::testing::fwd_envelope(body, /*structured=*/false));
    check(router.drop_stats().malformed_rx == 1, "the instrument: the drop really was counted");

    const auto after_bytes = read_bytes(g, "/sink:stats.router.drops");
    const auto after = tr::wire::decode(after_bytes);
    check(after && counter(*after, "malformed_rx") == 1,
          "the SAME field now reports it — the seam samples the router, it does not cache");
    check(after && counter(*after, "arena_dropped") == 0,
          "…and only that noun moved: the per-cause split survives the wire crossing");
}

/**
 * @brief §D.4 — `:stats.labels` and `:stats.link.<child>`, including the per-link identity.
 *
 * Two links with DIFFERENT counters, so a seam that ignored the sub-key and answered for
 * "the links" would fail. `labels_used` appears only with label switching on, which is the
 * "a seam names only the nouns it has" rule (Amendment 1 §D.3) applied per node.
 */
void test_link_and_label_seams() {
    std::printf("RFC-0010 Am.2 §D.4: :stats.link.<child> is PER LINK; :stats.labels.table:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    fwd_router_t router(g);

    counting_link_t up;
    counting_link_t down;
    up.counters = {.dropped_rx = 3, .malformed_rx = 5, .dropped_tx = 7};
    down.counters = {.dropped_rx = 11, .malformed_rx = 0, .dropped_tx = 0};
    check(router.add_child("up", up) && router.add_child("down", down), "two links registered");

    const auto up_bytes = read_bytes(g, "/sink:stats.link.up");
    const auto up_block = tr::wire::decode(up_bytes);
    check(up_block && counter(*up_block, "dropped_rx") == 3 &&
              counter(*up_block, "malformed_rx") == 5 && counter(*up_block, "dropped_tx") == 7,
          "the block carries THIS link's three interface-level drop nouns");
    const auto down_bytes = read_bytes(g, "/sink:stats.link.down");
    const auto down_block = tr::wire::decode(down_bytes);
    check(down_block && counter(*down_block, "dropped_rx") == 11,
          "…and the OTHER child's sub-key answers for the other link");
    check(up_block && up_block->children.size() == 6,
          "no labels_used noun with label switching off — a seam names only what it has");

    // The label plane. With a mint table installed the per-link occupancy noun appears.
    check(reads_ok(g, "/sink:stats.labels.table"),
          "the label-plane seam answers even with switching off (its counters exist)");
    tr::net::path_label_table_t labels;
    router.configure_path_labels(&labels);
    const auto lab_bytes = read_bytes(g, "/sink:stats.labels.table");
    const auto lab = tr::wire::decode(lab_bytes);
    check(lab && counter(*lab, "labels_exhausted") == 0 && counter(*lab, "refused_bindings") == 0 &&
              counter(*lab, "label_not_found") == 0 && counter(*lab, "label_resolves") == 0,
          "the four label-plane nouns are present");
    const auto with_labels = tr::wire::decode(read_bytes(g, "/sink:stats.link.up"));
    check(with_labels && counter(with_labels.value(), "labels_used") == 0,
          "…and the link block GREW its labels_used noun once switching is on");

    // A link that was never registered is not a seam; nor is one that has been removed.
    check(status_of(g, "/sink:stats.link.nosuch") == status_t::SCHEMA_NOT_FOUND,
          "an unregistered child's sub-key is SCHEMA_NOT_FOUND, not an empty block");
    check(router.remove_child("down"), "the down link is removed");
    check(status_of(g, "/sink:stats.link.down") == status_t::SCHEMA_NOT_FOUND,
          "a REMOVED link's sub-key answers SCHEMA_NOT_FOUND — never a frozen block");
    check(reads_ok(g, "/sink:stats.link.up"), "control: the survivor still reads");
}

/**
 * @brief The POSITIVE CONTROL — no router, no net-plane census.
 *
 * Every case above passes only because a sampler is registered; this is what proves it. It
 * is also the amendment's normative sentence: an unregistered seam answers
 * `SCHEMA_NOT_FOUND`, which Amendment 1 §Compatibility already spells "this node does not
 * publish that seam".
 */
void test_no_router_publishes_no_net_seam() {
    std::printf("RFC-0010 Am.2: with NO router constructed, every net seam is not_found:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    for (const char* spelling : {"/sink:stats.router.drops", "/sink:stats.labels.table",
                                 "/sink:stats.link.up", "/sink:stats.link.anything"})
        check(status_of(g, spelling) == status_t::SCHEMA_NOT_FOUND, spelling);
    check(read_bytes(g, "/sink:stats.graph.delivery").size() > 8,
          "control: the GRAPH's own seams are unaffected — Amendment 1 stands untouched");
}

/** @brief §D.1 — the new classes are node-scoped too: every vertex answers identically. */
void test_net_seams_are_node_scoped() {
    std::printf("RFC-0010 Am.2 §D.1: the net-plane seams are node-scoped as well:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.register_vertex(*path_t::parse("/actuator/relay"), role_t::STORED_VALUE);
    fwd_router_t router(g);
    counting_link_t up;
    up.counters = {.dropped_rx = 2, .malformed_rx = 0, .dropped_tx = 0};
    check(router.add_child("up", up), "one link registered");

    const auto a = read_bytes(g, "/sensor/temp:stats.link.up");
    const auto b = read_bytes(g, "/actuator/relay:stats.link.up");
    check(!a.empty() && a == b, "two unrelated vertices return BYTE-IDENTICAL link blocks");
}

/**
 * @brief §D.2 — an UNSERVED net-plane NAME is refused caller-INDEPENDENTLY.
 *
 * The hazard the recognition probe exists for: the sampler decides validity, and if that
 * decision sat BELOW the READ gate a denied caller would get `PERMISSION_DENIED` where an
 * admitted one got `SCHEMA_NOT_FOUND` — one spelling, two answers split by who asked.
 */
void test_unserved_net_names_are_caller_independent() {
    std::printf("RFC-0010 Am.2 §D.2: an unserved net-plane NAME is caller-INDEPENDENT:\n");
    graph_t g;
    g.configure_subject_resolver(caller_is_subject, nullptr);
    (void)g.register_vertex(*path_t::parse("/dev"), role_t::STORED_VALUE);
    fwd_router_t router(g);
    counting_link_t up;
    check(router.add_child("up", up), "one link registered");

    const std::vector<ace_t> grant{
        ace_t{.type = tr::graph::ace_type_t::ALLOW,
              .flags = 0,
              .subject = as_bytes("alice"),
              .access_mask = static_cast<std::uint32_t>(acl_right_t::READ),
              .expires_ns = 0}};
    check(g.write(path_t("/dev:acl"), make_value(tr::graph::encode_acl(grant))).has_value(),
          "an ACL granting only alice is installed on /dev");

    for (const char* spelling :
         {"/dev:stats.router", "/dev:stats.router.nope", "/dev:stats.labels.nope",
          "/dev:stats.link.nosuch", "/dev:stats.link.up.extra", "/dev:stats.link.up[0]"}) {
        check(status_of(g, spelling, "alice") == status_t::SCHEMA_NOT_FOUND &&
                  status_of(g, spelling, "mallory") == status_t::SCHEMA_NOT_FOUND,
              spelling);
    }
    // The control: a SERVED net seam really does split on the gate — §D.5 is unchanged.
    check(read_bytes(g, "/dev:stats.link.up", "alice").size() > 8 &&
              status_of(g, "/dev:stats.link.up", "mallory") == status_t::PERMISSION_DENIED,
          "control: a RECOGNISED net seam is served to alice and denied to mallory");
}

/** @brief §D.6 — the new classes are read-only, like every other `:stats` spelling. */
void test_net_seams_are_read_only() {
    std::printf("RFC-0010 Am.2 §D.6: a write to a net-plane seam is ENOTTY:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/dev"), role_t::STORED_VALUE);
    fwd_router_t router(g);
    const auto p = path_t::parse("/dev:stats.router.drops");
    check(p.has_value(), "the spelling parses");
    const auto vh = g.find(p->key());
    check(vh.has_value(), "the vertex resolves");
    const auto w = g.write(*vh, p->field(), make_value({std::uint8_t{1}}), {});
    check(!w.has_value() && w.error() == status_t::SCHEMA_NOT_FOUND,
          "the census has no write door, on the net-plane classes either");
}

}  // namespace

int main() {
    test_router_seam_is_live();
    test_link_and_label_seams();
    test_no_router_publishes_no_net_seam();
    test_net_seams_are_node_scoped();
    test_unserved_net_names_are_caller_independent();
    test_net_seams_are_read_only();
    std::printf("stats_net_seam_test: OK\n");
    return 0;
}
