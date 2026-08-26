/**
 * @file
 * @brief #1460 phase (b) — RFC-0025 conformance vector 12 `stream/receiver-ring-flood`,
 *        PLANE-ISOLATION arm: a flood that exhausts the NET-plane store must leave the
 *        GRAPH plane still allocating, **under** the flood rather than after it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0079 measures composition arms but never asks the question that makes per-injection-point
 * sourcing load-bearing: when one plane's store is exhausted, does another plane still work?
 * That is the whole argument for "one knob per target" over one shared pool — and until this
 * suite it was unasserted, so a shared-source regression would have passed every test in the
 * tree and surfaced only as a whole-node stall under inbound flood.
 *
 * ### The experiment
 *
 * Two `bump_source_t`s of the **same** size over caller slabs, each with `null_source()`
 * upstream so the slab IS the cap and there is no global heap to fall back to:
 *
 *  - the NET plane's store is `fwd_router_t`'s injected `label_src`/`rx` — the `#603` /
 *    `fe6adcd6` seam. It is drained by a peer-shaped flood of `ADVERTISE` frames fed through
 *    the public `on_frame` door, which is exactly the pre-ACL, receive-thread path #603 was
 *    filed against;
 *  - the GRAPH plane's store is `graph_t`'s injected `ctl` seam (ADR-0065 / ADR-0079).
 *
 * The **only** difference between the two arms below is whether those are two sources or one.
 * Capacity is identical in both, and the flood runs to exhaustion, so nothing here turns on
 * how big the slabs are — only on whether the planes are fenced off from each other.
 *
 * ### What each arm asserts
 *
 *  - @ref test_graph_plane_survives_a_net_plane_flood — the per-plane (ADR-0079 default)
 *    composition. With the net store proven exhausted and inbound pressure still being
 *    applied between every step: vertex creation succeeds, an `assign` publish is readable
 *    back byte-exact, an edge add dispatches, and the graph's **own** injected failable seam
 *    still serves a composed subtree read. This is the claim the issue exists for.
 *  - @ref test_folded_sources_let_the_flood_starve_the_graph — the ABLATION, run as a control
 *    arm so the instrument is proven reachable inside the suite rather than only by hand: the
 *    same node with the per-plane injection deleted (both planes on one source) answers
 *    `BACKPRESSURE` to the identical composed read. A green ablation is a vacuous suite, and
 *    an in-suite control is the only way this file can say so about itself.
 *
 * ### Which arm carries the ablation, and why the others do not — stated honestly
 *
 * Of the four graph-plane steps, only the composed read moves when the sources are folded, and
 * that is a fact about the tree's current state rather than about this test. ADR-0079's
 * **stage 2 (the graph placement store) is not built**: `graph_t::register_vertex_key_span`
 * still reaches `std::make_unique<vertex_t>` on the global heap, and an LKV publish still runs
 * through the `std::pmr` adapter, so neither draws from the injected `ctl` seam yet. The
 * composed subtree read (`read_subtree_folded`, whose walk stack is a `block_array_t` over
 * `ctl_`) is today the one graph-plane allocation a deployer can actually fence — so it is the
 * one the fold can starve. The other three steps are kept because the flood must not stall,
 * corrupt or deadlock them either, and because they become ablation-sensitive for free once
 * stage 2 lands; they are labelled in their prose so nobody quotes them as the lever.
 *
 * ### Vector coverage — one arm of three, by division of labour
 *
 * RFC-0025 §7 vector 12 has three arms. This suite populates the **plane-isolation** arm only.
 * The receiving STREAM ring's own §4.4 pressure contract (best-effort shed + `FLOW_ADDRESS_
 * SHIFT_GAP` + loss accounting; reliable ⇒ `FLOW_BACKPRESSURE`) and its recovery half needed the
 * ring the fused #1461 + #1462 PR moved onto the receiving vertex, so they could not be asserted
 * when this file landed. They are now, in the sibling `core/tests/ring_pressure_test.cpp`, which
 * drives the same vector's other two arms through the graph's own store verb. See the
 * `HARNESS.md` row, which says the same thing where a reader of the vector table will find it.
 *
 * Runs single-threaded, so it is meaningful under ASan+UBSan (`-fno-sanitize-recover=all`) and
 * costs nothing under TSan beyond the store's own internal locking.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::view::rope_t;

using tr::testing::check;
using tr::testing::make_value;

/**
 * @brief Each plane's slab, in bytes — the SAME for both, in both arms.
 *
 * Small enough that a few hundred `ADVERTISE` frames drain it, large enough that a composed
 * read of the little tree below fits comfortably in an untouched one. The number is not
 * load-bearing: the flood runs until the source refuses, so a bigger slab only means more
 * frames, and the folded arm starves at any size.
 */
constexpr std::size_t kSlabBytes = 4096;

/** @brief How many `ADVERTISE` frames one burst of inbound pressure carries. */
constexpr std::uint16_t kBurst = 512;

/** @brief This node's name for the link the flood arrives on. */
constexpr std::string_view kInLink = "uplink";

/** @brief A probe big enough that "the source served it" means something. */
constexpr std::size_t kProbeBytes = 64;

/** @brief A packed `PATH` TLV over @p segs — canonical/key context, so `opt.pl` stays 0. */
[[nodiscard]] std::vector<std::byte> packed_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) {
        (void)tr::wire::emit_path_segment(body, s);
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{}, body);
    return out;
}

/**
 * @brief One node wired for the experiment — two plane stores, or one when @p folded.
 *
 * The slabs are members so they outlive the sources, and the sources outlive the graph and the
 * router that borrow them; declaration order IS that lifetime order and must not be shuffled.
 */
struct node_t {
    /**
     * @brief Wire the node. @p folded deletes the per-plane injection: the graph's `ctl` seam
     *        becomes the very source the net plane draws from, which is ADR-0079's "one slab,
     *        whole stack" and the ablation this suite is gated on.
     */
    explicit node_t(bool folded)
        : net_src(net_slab, tr::mem::null_source()),
          graph_src(graph_slab, tr::mem::null_source()),
          g(folded ? &net_src : &graph_src),
          router(g, &net_src, &net_src) {}

    /** @brief The NET plane's slab. */
    alignas(std::max_align_t) std::array<std::byte, kSlabBytes> net_slab{};
    /** @brief The GRAPH plane's slab — untouched by the flood unless the arm is folded. */
    alignas(std::max_align_t) std::array<std::byte, kSlabBytes> graph_slab{};
    tr::mem::bump_source_t net_src;   /**< @brief Hard-bounded: no heap behind it. */
    tr::mem::bump_source_t graph_src; /**< @brief Same size, same hard bound. */
    graph_t g;                        /**< @brief The graph plane. */
    fwd_router_t router; /**< @brief The net plane, drawing label + rx from `net_src`. */
};

/**
 * @brief Apply one burst of inbound pressure: @p count `ADVERTISE` frames from label @p first.
 *
 * Each frame goes through the public `on_frame` door — the same entry a transport receive
 * thread uses — and lands on `on_advertise`'s TERMINUS arm, because the route matches no
 * registered mount. That arm re-encodes the route into a block drawn from the injected label
 * source and then binds it, so every frame is two draws on the net-plane store and nothing
 * else. Distinct labels, so no frame is a free in-place rebind.
 *
 * @return How many bindings the store actually holds afterwards.
 */
std::size_t apply_pressure(node_t& n, std::uint16_t first, std::uint16_t count) {
    const std::vector<std::byte> route = packed_path({"plane", "net", "flood"});
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint16_t label = static_cast<std::uint16_t>(first + i);
        if (label == 0) continue;  // 0 is the reserved label, never bound
        const std::vector<std::byte> frame = tr::net::encode_advertise(label, route);
        n.router.on_frame(kInLink, frame);
    }
    return n.router.handles().ingress_count();
}

/** @brief Is the net-plane store refusing right now? A direct read, not an inference. */
[[nodiscard]] bool net_store_is_exhausted(node_t& n) {
    void* const p = n.net_src.try_alloc(kProbeBytes, alignof(std::max_align_t));
    if (p != nullptr) n.net_src.release(p, kProbeBytes, alignof(std::max_align_t));
    return p == nullptr;
}

/**
 * @brief Drive the flood until the net-plane store refuses, and report that it did.
 *
 * Bursts rather than one long loop so the label space (16 bits, and the reserved 0) is never
 * the thing that ends the flood: a slab this size is drained inside the first burst or two.
 */
void flood_net_plane_to_exhaustion(node_t& n) {
    std::size_t bound = 0;
    std::uint16_t next = 1;
    for (int burst = 0; burst < 8 && !net_store_is_exhausted(n); ++burst) {
        bound = apply_pressure(n, next, kBurst);
        next = static_cast<std::uint16_t>(next + kBurst);
    }
    check(bound > 0, "flood: the net plane bound labels while its store could serve them");
    check(bound < kBurst, "flood: and REFUSED rather than growing without bound");
    check(net_store_is_exhausted(n), "flood: the net-plane store is exhausted, not merely busy");
}

/**
 * @brief Assert the flood is STILL on, then apply another burst that changes nothing.
 *
 * Called between every graph-plane step, so each of those assertions is taken while the net
 * plane is exhausted and under pressure. A suite that drained first and checked the graph
 * afterwards would prove nothing — the store would have been idle at the moment it mattered.
 */
void hold_the_flood(node_t& n, std::string_view where) {
    check(net_store_is_exhausted(n), where);
    const std::size_t before = n.router.handles().ingress_count();
    (void)apply_pressure(n, 40000, kBurst);
    check(n.router.handles().ingress_count() == before,
          "flood: further inbound frames are refused by value, binding nothing");
}

/** @brief Records that a delivery reached the subscriber, and the byte it carried. */
struct sink_t {
    std::size_t deliveries = 0; /**< @brief How many values arrived. */
    std::uint8_t last = 0;      /**< @brief The first byte of the most recent one. */
    /** @brief The subscriber callback — `graph_t::subscribe`'s zero-erasure sugar binds this. */
    void operator()(const rope_t& value) {
        ++deliveries;
        last = std::to_integer<std::uint8_t>(value.only().bytes()[0]);
    }
};

/**
 * @brief Build the little tree the composed read walks, under @p root.
 *
 * Two levels with values on the leaves, so `read_subtree_folded` has a real subtree to fold
 * and its walk stack has to grow at least once — which is the allocation the fold starves.
 */
void seed_subtree(graph_t& g, const char* root) {
    for (const std::string_view leaf : {"a", "b", "c"}) {
        const std::string key = std::string(root) + "/" + std::string(leaf);
        const vertex_handle_t v = g.register_vertex(*path_t::parse(key), role_t::STORED_VALUE);
        check(g.assign(v, rope_t{make_value({0x11})}).has_value(),
              "setup: a leaf value landed before the flood");
    }
}

/**
 * @brief THE CLAIM — with the net plane's store exhausted and pressure still applied, the
 *        graph plane still allocates.
 */
void test_graph_plane_survives_a_net_plane_flood() {
    std::printf(" per-plane (ADR-0079 default) — the graph plane under a net-plane flood:\n");
    node_t n(/*folded=*/false);
    seed_subtree(n.g, "/tree");
    const vertex_handle_t tree = n.g.register_vertex(*path_t::parse("/tree"), role_t::STORED_VALUE);

    flood_net_plane_to_exhaustion(n);

    // (1) Vertex creation. Ablation-INSENSITIVE today — `vertex_t` is still a global-heap
    // `make_unique` (ADR-0079 stage 2 unbuilt) — but the flood must not stall or corrupt it.
    hold_the_flood(n, "graph: the net store is exhausted as vertex creation is attempted");
    const tr::graph::result_t<vertex_handle_t> made =
        n.g.try_register_vertex(*path_t::parse("/graph/born-under-flood"), role_t::STORED_VALUE);
    check(made.has_value(), "graph: a vertex is created while the net plane is exhausted");

    // (2) An LKV publish, readable back byte-exact. Same insensitivity note as (1): the publish
    // runs through the `std::pmr` adapter, not the injected `ctl` seam.
    hold_the_flood(n, "graph: the net store is exhausted as the publish is attempted");
    check(n.g.assign(*made, rope_t{make_value({0x5A})}).has_value(),
          "graph: an assign publish succeeds under the flood");
    const tr::graph::result_t<tr::graph::value_ref_t> back = n.g.read(*made);
    check(back.has_value() && static_cast<bool>(*back), "graph: and the value reads back");
    check(back.has_value() && static_cast<bool>(*back) && (**back).only().bytes().size() == 1 &&
              std::to_integer<std::uint8_t>((**back).only().bytes()[0]) == 0x5A,
          "graph: byte-exact — the flood did not corrupt what the graph published");

    // (3) An edge add and a dispatch over it.
    hold_the_flood(n, "graph: the net store is exhausted as the edge is added");
    sink_t sink;
    const tr::graph::result_t<tr::graph::subscription_t> sub =
        n.g.subscribe(*path_t::parse("/graph/born-under-flood"), sink);
    check(sub.has_value(), "graph: an edge is admitted under the flood");
    check(n.g.write(*made, rope_t{make_value({0x77})}).has_value(),
          "graph: and a write over it is accepted");
    check(sink.deliveries == 1 && sink.last == 0x77, "graph: the dispatch reached the subscriber");
    if (sub.has_value()) check(n.g.unsubscribe(*sub).has_value(), "graph: the edge retires");

    // (4) THE LEVER. The graph's OWN injected failable seam: `read_subtree_folded` walks on a
    // `block_array_t` over `ctl_`, so this is the one step above that the fold can starve — and
    // therefore the one that makes the ablation red. See the file header.
    hold_the_flood(n, "graph: the net store is exhausted as the composed read is attempted");
    const tr::graph::result_t<rope_t> folded = n.g.read_subtree_folded(tree);
    check(folded.has_value(),
          "graph: the graph's own failable seam still serves a composed subtree read");
    check(folded.has_value() && folded->total_length() > 0,
          "graph: and the composed value is non-empty");

    check(net_store_is_exhausted(n),
          "graph: every assertion above was taken while the net plane was still exhausted");
}

/**
 * @brief THE ABLATION, as a control arm — delete the per-plane injection and the flood reaches
 *        across into the graph.
 *
 * Same slabs, same sizes, same flood, one source. If this arm went green the suite above would
 * be vacuous: "the graph plane still allocates" would be a claim about an allocation nothing
 * can starve. It reddens exactly the step the file header names as the lever, which is also
 * the shape a real shared-source regression would take.
 */
void test_folded_sources_let_the_flood_starve_the_graph() {
    std::printf("\n folded (the ABLATION) — one source for both planes:\n");
    node_t n(/*folded=*/true);
    seed_subtree(n.g, "/tree");
    const vertex_handle_t tree = n.g.register_vertex(*path_t::parse("/tree"), role_t::STORED_VALUE);

    // The graph's seam serves the composed read BEFORE the flood — so what follows is the
    // flood's doing and not a tree the walk could never have folded.
    check(n.g.read_subtree_folded(tree).has_value(),
          "ablation: the composed read is served while the shared source has room");

    flood_net_plane_to_exhaustion(n);
    hold_the_flood(n, "ablation: the shared store is exhausted as the composed read is attempted");

    const tr::graph::result_t<rope_t> folded = n.g.read_subtree_folded(tree);
    check(!folded.has_value(), "ablation: the graph plane is STARVED by the net plane's flood");
    check(!folded.has_value() && folded.error() == status_t::BACKPRESSURE,
          "ablation: and it says so as BACKPRESSURE — the injected store's exhaustion status");
}

}  // namespace

int main() {
    std::printf(
        "#1460 phase (b) — RFC-0025 vector 12 `stream/receiver-ring-flood`, plane isolation\n\n");
    test_graph_plane_survives_a_net_plane_flood();
    test_folded_sources_let_the_flood_starve_the_graph();
    return tr::testing::summary("plane_isolation");
}
