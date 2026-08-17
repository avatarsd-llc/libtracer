/**
 * @file
 * @brief The #830 ablation: `target_canonical_resolves()` proves WHICH of
 *        `dispatch_edge_target`'s two resolve spellings a local target edge actually took.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #1174's binding and its `find_ptr` fallback are **behaviourally identical by design** — the
 * whole point of drop-never-misroute is that a refused deref falls through to the canonical
 * walk and the delivery still lands. That is exactly why an assertion on deliveries measures
 * nothing here: the value arrives either way, so a test that only counts arrivals passes
 * against a build in which the binding is never minted, never dereferenced, or dereferenced
 * and silently ignored.
 *
 * `graph_t::target_canonical_resolves()` is the one observable that separates them, and it is
 * asymmetric on purpose: it counts ONLY the fallback, so the bound leg carries no atomic. The
 * ablation is therefore a counter test, and it is only non-vacuous with CONTROL arms that make
 * it MOVE — otherwise "it stayed 0" is indistinguishable from a counter nobody increments.
 *
 * Four cases, three of which are the controls:
 *
 *  1. **Bound** — target registered before the subscribe, so `admit_subscriber` mints. The
 *     counter must not move across N deliveries. This is the claim.
 *  2. **Never-minted control** — the same edge subscribed BEFORE its target exists. The mint
 *     rule leaves it unbound and it keeps the canonical spelling forever: the counter moves
 *     once per delivery. This proves the counter is reachable and that case 1's zero is a
 *     fact about the binding rather than about the instrument.
 *  3. **Stale-generation control** — a bound edge whose target is RETIRED and then revived at
 *     the same path. `deref_vertex_slot` refuses the stale generation and the leg falls back,
 *     so the counter starts moving on an edge that was previously silent. This is the one that
 *     proves the deref is actually consulted per delivery, not once.
 *  4. **The fallback still DELIVERS.** RFC-0024 §5.3 says a bound-form op that fails validation
 *     is dropped — but this is not a bound-form op, it is a local cache over a canonical one,
 *     and #1174's rule is fall-back-never-drop. Case 3's revived target must therefore receive
 *     the value, not lose it. A counter that moved while deliveries stopped would be a
 *     regression wearing a passing ablation's clothes.
 *
 * The bench (`bench/bench_target_binding.cpp`) asserts the same counter on its own timed runs,
 * so no point there can quote a leg it did not take. This suite is where the RULE lives.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;
using tr::view::view_t;

/** @brief How many deliveries each arm drives — enough that an off-by-one cannot hide. */
constexpr std::size_t kDeliveries = 64;

/** @brief The producer every arm writes to. */
constexpr const char* kSrc = "/src";

/**
 * @brief A SUBSCRIBER TLV naming @p key as the edge's local target — the WIRE subscribe form.
 *
 * `graph_t::subscribe(path, callback)` cannot produce this: its edges carry a null
 * `target_key` and so never enter `dispatch_edge_target` at all.
 */
[[nodiscard]] view_t subscriber_tlv(const std::string& key) {
    std::vector<std::byte> body;
    std::size_t pos = 1;  // past the leading '/'
    while (pos <= key.size()) {
        const std::size_t next = key.find('/', pos);
        const std::size_t end = next == std::string::npos ? key.size() : next;
        (void)tr::wire::emit_path_segment(body, std::string_view(key).substr(pos, end - pos));
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    tr::wire::tlv_t path{.type = tr::wire::type_t::PATH, .payload = body};  // packed, PL = 0
    tr::wire::tlv_t sub{.type = tr::wire::type_t::SUBSCRIBER};
    sub.opt.pl = true;
    sub.children.push_back(path);
    const std::vector<std::byte> bytes = tr::wire::encode(sub);
    return tr::testing::make_value(bytes);
}

/** @brief Write @ref kDeliveries values into @p src and report the counter's movement. */
[[nodiscard]] std::uint64_t drive(graph_t& g, vertex_handle_t src) {
    const std::uint64_t before = g.target_canonical_resolves();
    for (std::size_t i = 0; i < kDeliveries; ++i)
        (void)g.write(src, tr::testing::make_value(std::vector<std::byte>{std::byte{0x2A}}));
    return g.target_canonical_resolves() - before;
}

/** @brief Case 1 — a minted binding keeps the canonical walk out of the delivery path. */
void test_bound_edge_never_resolves_canonically() {
    graph_t g;
    const path_t tpath = *path_t::parse("/a/b/c/t");
    (void)g.register_vertex(tpath, role_t::STORED_VALUE);  // BEFORE the subscribe — mintable
    const vertex_handle_t src = g.register_vertex(*path_t::parse(kSrc), role_t::STORED_VALUE);
    check(g.write(*path_t::parse("/src:subscribers[]"), subscriber_tlv("/a/b/c/t")).has_value(),
          "bound: the target edge admitted");
    check(drive(g, src) == 0,
          "bound: target_canonical_resolves() did not move across 64 deliveries");
}

/** @brief Control A — an edge admitted before its target existed never binds, and says so. */
void test_edge_admitted_before_its_target_falls_back_every_time() {
    graph_t g;
    const vertex_handle_t src = g.register_vertex(*path_t::parse(kSrc), role_t::STORED_VALUE);
    // The target does NOT exist yet, so `admit_subscriber`'s mint finds nothing and the edge
    // keeps the canonical spelling. Registering it afterwards does not retro-mint: the mint
    // happens once, at admission.
    check(g.write(*path_t::parse("/src:subscribers[]"), subscriber_tlv("/a/b/c/t")).has_value(),
          "unbound: the target edge admitted with no target present");
    (void)g.register_vertex(*path_t::parse("/a/b/c/t"), role_t::STORED_VALUE);
    check(drive(g, src) == kDeliveries,
          "unbound: target_canonical_resolves() moved once per delivery");
}

/**
 * @brief Control B — a RETIRED-then-revived target makes a previously silent bound edge fall
 *        back, and the delivery still lands at the successor.
 *
 * This is the arm that proves the deref runs per delivery. An implementation that consulted
 * the binding once and cached the pointer would pass case 1 and control A and still misroute
 * here, which is the #603 class the generation stamp exists to close.
 */
void test_stale_generation_falls_back_and_still_delivers() {
    graph_t g;
    const path_t tpath = *path_t::parse("/a/b/c/t");
    const vertex_handle_t target = g.register_vertex(tpath, role_t::STORED_VALUE);
    const vertex_handle_t src = g.register_vertex(*path_t::parse(kSrc), role_t::STORED_VALUE);
    check(g.write(*path_t::parse("/src:subscribers[]"), subscriber_tlv("/a/b/c/t")).has_value(),
          "stale: the target edge admitted and minted");
    check(drive(g, src) == 0, "stale: bound and silent before the retire");

    const std::uint32_t gen_before = g.retire_generation(target);
    check(g.retire(target).has_value(), "stale: the target retired");
    // Revive at the SAME path. The slot index is unchanged and the generation has advanced, so
    // the edge's element now names a slot whose tenant it was never minted for.
    const vertex_handle_t revived = g.register_vertex(tpath, role_t::STORED_VALUE);
    check(g.retire_generation(revived) != gen_before,
          "stale: the revived tenant carries a different generation");

    check(drive(g, src) == kDeliveries, "stale: every delivery now takes the canonical fallback");
    // Fall-back, NEVER drop (#1174): the successor tenant must hold the value the fallback
    // resolved to it. A counter that moved while the value went nowhere is not a pass.
    check(g.read(tpath).has_value(), "stale: the revived target still received the delivery");
}

}  // namespace

int main() {
    std::printf("#830 target_binding_t ablation — which resolve leg did the edge take?\n\n");
    test_bound_edge_never_resolves_canonically();
    test_edge_admitted_before_its_target_falls_back_every_time();
    test_stale_generation_falls_back_and_still_delivers();
    return tr::testing::summary("target_binding_ablation");
}
