/**
 * @file
 * @brief #833 — a REFUSED forwarding bind must strand neither the out-label it took nor the
 *        egress route retained with it, through the production `on_advertise` wiring.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The forwarding arm has to take its downstream label BEFORE it can bind the inbound swap,
 * because the binding names that label. When the bind then refuses — the inbound link's
 * ingress table is at its injected bound here; the #827 epoch guard in the field — the hop
 * returns without advertising. What it took was therefore left in the LIVE downstream table
 * with no ingress binding aiming at it and no peer that has ever seen it, reclaimable only by
 * that link's next `clear_link`: one label out of the saturating 16-bit space, plus the
 * retained route bytes, per refused route.
 *
 * Driven through `fwd_router_t` rather than `route_handle_t` on purpose (`route_handle_test`
 * covers the primitive): the take, the bind and the unwind are three separate calls in
 * `on_advertise`, and a unit test of the primitive cannot say they are wired in that order.
 *
 * Shape of the refusal: the bound is 2 per table, and the two flows ahead of the refusal name
 * the SAME downstream route — that fills the INGRESS table (one binding per inbound label)
 * while #913's dedup leaves the EGRESS table at one entry. It is the only arrangement in
 * which the BIND is the step that refuses; with distinct routes the egress table fills in
 * step and `ensure_egress` refuses first, which takes nothing and never leaked.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

/** @brief The per-link table bound this node is built with — ingress AND egress (#603). */
constexpr std::size_t kBound = 2;

/** @brief A PATH TLV over @p segs, built with the production emitters. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        (void)tr::wire::emit_path_segment(body, s);
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A 4-byte VALUE payload, the COMPACT body. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

/** @brief The frame's TLV type, or nullopt when it does not decode. */
std::optional<type_t> frame_type(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec) return std::nullopt;
    return dec->type;
}

/** @brief The label carried as a route-handle frame's first child (ADVERTISE / HANDLE_NACK). */
std::uint16_t frame_label(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->children.empty() || dec->children[0].payload.size() < 2) return 0;
    return tr::detail::load_le<std::uint16_t>(dec->children[0].payload);
}

/** @brief A link that records every frame the router sends it, and can inject one inbound. */
class fake_link_t : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        sent_.emplace_back(frame.begin(), frame.end());
    }
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    [[nodiscard]] const std::vector<std::vector<std::byte>>& sent() const { return sent_; }

   private:
    std::vector<std::vector<std::byte>> sent_;
};

/**
 * @brief The whole gate: fill the inbound ingress table, then advertise one more route.
 *
 * Counted against the pre-advertise census rather than an absolute, because "returns to the
 * value it had before the refused advertise" is the property, not "is 1".
 */
void refused_bind_leaves_no_strand() {
    std::printf("A refused forwarding bind returns the downstream table to its census (#833):\n");
    graph_t g;
    fwd_router_t node(g, &tr::mem::heap_source(), &tr::mem::heap_source(), &tr::mem::heap_backend(),
                      kBound);
    fake_link_t up;
    fake_link_t up2;
    fake_link_t down;
    (void)node.add_child("up", up);
    (void)node.add_child("up2", up2);
    (void)node.add_child("down", down);

    // Two established flows over ONE downstream route: "up" ends at its ingress bound while
    // "down" holds a single egress entry (#913).
    const std::vector<std::byte> shared = b_path({"down", "sensor"});
    for (std::uint16_t in : {std::uint16_t{10}, std::uint16_t{11}})
        up.inject(tr::net::encode_advertise(in, shared));
    check(node.handles().ingress_count() == kBound && node.handles().egress_count() == 1,
          "setup: the inbound ingress table is full, one shared egress entry behind it");
    const std::size_t egress_before = node.handles().egress_count();
    const std::size_t sent_before = down.sent().size();

    // The refusal: a NEW route takes a fresh downstream label, and the bind then refuses.
    constexpr std::uint16_t kRefused = 12;
    up.inject(tr::net::encode_advertise(kRefused, b_path({"down", "other"})));
    std::printf(
        "    census: egress %zu (was %zu) | ingress %zu | downstream frames %zu (was %zu)\n",
        node.handles().egress_count(), egress_before, node.handles().ingress_count(),
        down.sent().size(), sent_before);

    check(node.handles().ingress_count() == kBound,
          "the swap is refused — no third ingress binding");
    check(down.sent().size() == sent_before,
          "nothing was advertised downstream, so no peer ever saw the label it took");
    check(node.handles().egress_count() == egress_before,
          "and the downstream egress table is back to its pre-advertise count");

    // The label space came back too, not just the table slot. A new flow arriving on the
    // OTHER inbound link (its own ingress table is empty, so its bind succeeds) is handed the
    // label the refusal gave up — 2, the value the refused take held.
    up2.inject(tr::net::encode_advertise(1, b_path({"down", "third"})));
    check(down.sent().size() == sent_before + 1, "the next flow does advertise downstream");
    check(frame_type(down.sent().back()) == type_t::ADVERTISE, "and it is an ADVERTISE");
    check(frame_label(down.sent().back()) == 2,
          "carrying label 2 — the one the refused take handed back, not 3");

    // The refused flow degrades exactly as an unbindable label already did: its COMPACT
    // misses and draws the stale-label HANDLE_NACK that prompts a re-advertise.
    const std::size_t up_sent_before = up.sent().size();
    up.inject(tr::net::encode_compact(kRefused, b_value_u32(0x1234u)));
    check(up.sent().size() == up_sent_before + 1,
          "the refused flow's COMPACT draws one frame back");
    check(frame_type(up.sent().back()) == type_t::HANDLE_NACK &&
              frame_label(up.sent().back()) == kRefused,
          "and it is the HANDLE_NACK for that label (the documented self-heal)");
}

/**
 * @brief The other half of the gate: the established-flow reuse path is untouched.
 *
 * Same node, driven past a refusal, then re-advertised on an EXISTING flow. The unwind must
 * not have taken the shared entry that flow rides — it was reused, never minted, by the call
 * that was refused.
 */
void established_flow_survives_a_refusal() {
    std::printf("The established-flow reuse path is unaffected by a refusal (#833):\n");
    graph_t g;
    fwd_router_t node(g, &tr::mem::heap_source(), &tr::mem::heap_source(), &tr::mem::heap_backend(),
                      kBound);
    fake_link_t up;
    fake_link_t up2;
    fake_link_t down;
    (void)node.add_child("up", up);
    (void)node.add_child("up2", up2);
    (void)node.add_child("down", down);

    const std::vector<std::byte> shared = b_path({"down", "sensor"});
    for (std::uint16_t in : {std::uint16_t{10}, std::uint16_t{11}})
        up.inject(tr::net::encode_advertise(in, shared));
    const std::uint16_t established = frame_label(down.sent().back());
    check(established != 0, "setup: the established flow holds a downstream label");

    // A SECOND inbound link fills its own ingress table through another downstream link, so
    // its next advertise reuses "down"'s existing label and is then refused.
    fake_link_t down2;
    (void)node.add_child("down2", down2);
    for (std::uint16_t in : {std::uint16_t{20}, std::uint16_t{21}})
        up2.inject(tr::net::encode_advertise(in, b_path({"down2", "x"})));
    const std::size_t egress_before = node.handles().egress_count();
    up2.inject(tr::net::encode_advertise(22, shared));  // reuse, then refuse
    check(node.handles().egress_count() == egress_before,
          "a refusal that REUSED a label erases nothing — the entry is not its own take");
    std::array<std::byte, 256> est_route_buf{};
    check(node.handles().copy_egress_route("down", established, est_route_buf) != 0,
          "the established flow keeps the route retained under its label");

    // And it still re-advertises normally: same label, one more frame downstream.
    const std::size_t sent_before = down.sent().size();
    up.inject(tr::net::encode_advertise(10, shared));
    check(down.sent().size() == sent_before + 1,
          "a re-advertise on the established flow still goes out");
    check(frame_label(down.sent().back()) == established, "carrying the SAME downstream label");
    check(node.handles().egress_count() == egress_before, "and taking no new egress entry");
}

}  // namespace

int main() {
    std::printf("== fwd_bind_refusal_unwind_test (#833) ==\n");
    refused_bind_leaves_no_strand();
    established_flow_survives_a_refusal();
    return tr::testing::summary("fwd_bind_refusal_unwind");
}
