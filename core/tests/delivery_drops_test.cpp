/**
 * @file
 * @brief Per-cause delivery-drop counters (`graph_t::delivery_drops`).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A path-target subscription edge — the form a wire `SUBSCRIBER` produces — delivers by
 * re-dispatching into the target vertex. Three conditions make that impossible, and all
 * three are specified to drop the ONE delivery while the write itself succeeds: the target
 * resolves to nothing, the target's `:acl` denies the edge's stored caller (the #81 fan-in
 * gate), or the nothrow delivery clone cannot be allocated (#477).
 *
 * Dropping is correct. Dropping INVISIBLY is what the counters fix: before them a node
 * whose target had been retired dropped every delivery for the rest of its life with
 * nothing anywhere to say so, and — the reason this was noticed at all (#619) — a benchmark
 * of that leg reported the cost of not delivering as though it were the cost of delivering.
 *
 * So the assertions here are two-sided on purpose. Each drop case checks that the WRITE
 * still succeeded and the counter moved; and the happy path checks every counter stays at
 * zero, which is the real regression risk — an increment accidentally moved onto the
 * delivering path would be invisible in behaviour and would make the counters lie.
 *
 * `out_of_memory` and `fan_out_truncated` are exercised in `graph_oom_softfail_test`, not here —
 * that file already owns the allocator-failure harness (`tr::detail::probe_fail_hook`), so the
 * assertions live beside the injection rather than duplicating it. This file's own header used
 * to say the counter was unreachable "which no test harness in this tree provides"; that stopped
 * being true when the hook landed, and the path had in fact been exercised there for some time
 * with the counter simply never checked. #896 found three more paths in that same state — a
 * delivery abandoned with no counter at all — which is why the zero-assertions below are
 * whole-struct: a cause that stops being counted must fail something.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief A VALUE TLV carrying one byte, as an owned view. */
tr::view::view_t value_u8(std::uint8_t x) {
    std::vector<std::byte> out;
    const std::byte payload[1] = {std::byte{x}};
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
    return make_value(out);
}

/** @brief A SUBSCRIBER TLV naming a single-segment target path (the wire subscribe form). */
tr::view::view_t subscriber_tlv(std::string_view target_segment) {
    std::vector<std::byte> packed_body;
    (void)tr::wire::emit_path_segment(packed_body, target_segment);
    tlv_t path{.type = type_t::PATH, .payload = packed_body};  // packed, PL = 0
    tlv_t sub{.type = type_t::SUBSCRIBER};
    sub.opt.pl = true;
    sub.children.push_back(path);
    return make_value(tr::wire::encode(sub));
}

/** @brief ALLOW @p subject exactly @p mask, as ACL bytes. */
std::vector<std::byte> allow_only(std::string_view subject, acl_right_t right) {
    const std::vector<tr::graph::ace_t> aces{tr::graph::ace_t{
        .subject = as_bytes(subject), .access_mask = static_cast<std::uint32_t>(right)}};
    return tr::graph::encode_acl(aces);
}

/**
 * @brief The test resolver (ADR-0018): the caller context IS the subject token.
 *
 * The empty (local) context never reaches a resolver — `graph_t::acl_allows` settles it
 * as trusted before invoking one (#905) — so the error arm here means DENY, nothing else.
 */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
}

// --- the happy path: nothing drops, and the counters say so ------------------------------
void test_delivering_edge_drops_nothing() {
    std::printf("a path-target edge that delivers increments NOTHING:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/sink"), role_t::STORED_VALUE);
    vertex_handle_t src = g.register_vertex(path_t("/src"), role_t::STORED_VALUE);
    check(g.write(path_t("/src:subscribers[]"), subscriber_tlv("sink")).has_value(),
          "subscribe by target path");

    check(g.write(src, value_u8(0x11)).has_value(), "the write succeeds");
    check(g.read(path_t("/sink")).has_value(), "and the delivery reached the target");

    const auto d = g.delivery_drops();
    check(d.no_target == 0 && d.denied == 0 && d.out_of_memory == 0 && d.fan_out_truncated == 0,
          "every drop counter is still zero on the delivering path");
}

// --- cause 1: the target resolves to nothing ---------------------------------------------
void test_missing_target_is_counted() {
    std::printf("\na target that resolves to no vertex — counted, write still succeeds:\n");
    graph_t g;
    vertex_handle_t src = g.register_vertex(path_t("/src"), role_t::STORED_VALUE);
    // No /ghost vertex is ever registered, so the edge's key resolves to nothing.
    check(g.write(path_t("/src:subscribers[]"), subscriber_tlv("ghost")).has_value(),
          "an edge may name a target that does not exist");

    check(g.write(src, value_u8(0x22)).has_value(),
          "the write SUCCEEDS — one leg dropping is not a write failure");
    const auto d1 = g.delivery_drops();
    check(d1.no_target == 1, "the no_target drop is counted once");
    check(d1.denied == 0 && d1.out_of_memory == 0 && d1.fan_out_truncated == 0,
          "and is not confused with another cause");

    check(g.write(src, value_u8(0x23)).has_value(), "a second write also succeeds");
    check(g.delivery_drops().no_target == 2, "counting is monotonic, once per dropped delivery");
}

// --- cause 2: the target's fan-in gate denies the edge's stored caller --------------------
void test_denied_fan_in_is_counted() {
    std::printf("\nthe target's :acl denies the edge's caller — counted separately:\n");
    graph_t g;
    g.configure_subject_resolver(caller_is_subject, nullptr);
    (void)g.register_vertex(path_t("/sink"), role_t::STORED_VALUE);
    vertex_handle_t src = g.register_vertex(path_t("/src"), role_t::STORED_VALUE);

    // The edge stores "peer-a" as its delivery caller (#81): the SUBSCRIBE runs under it,
    // and every delivery this edge makes is gated by the TARGET's :acl under that same
    // context. The source carries no :acl, so the subscribe itself is ungated.
    const auto sub_field = path_t::parse("/src:subscribers[]");
    check(sub_field.has_value(), "the subscribe field-path parses");
    check(g.write(src, sub_field->field(), subscriber_tlv("sink"), "peer-a").has_value(),
          "subscribe under caller peer-a");
    // Now authorize only peer-b to WRITE the target. Written with the empty (local, trusted)
    // context so setting the policy is not itself gated.
    check(g.write(path_t("/sink:acl"), make_value(allow_only("peer-b", acl_right_t::WRITE)))
              .has_value(),
          "the target authorizes peer-b only");

    check(g.write(src, value_u8(0x33)).has_value(), "the write still succeeds");
    const auto d = g.delivery_drops();
    check(d.denied == 1, "the denied drop is counted once");
    check(d.no_target == 0, "and NOT as a missing target — the target resolved fine");
    check(!g.read(path_t("/sink")).has_value(), "the target really did not receive the value");
}

}  // namespace

int main() {
    std::printf("delivery-drop counters (graph_t::delivery_drops):\n\n");
    test_delivering_edge_drops_nothing();
    test_missing_target_is_counted();
    test_denied_fan_in_is_counted();
    return tr::testing::summary("delivery_drops");
}
