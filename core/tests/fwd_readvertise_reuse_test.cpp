/**
 * @file
 * @brief #913 — a re-advertise cycle must burn no downstream label and leak no egress entry.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A peer re-advertises for reasons this node does not control: a reconnect loop, a flapping
 * link, a producer that re-advertises on every "up" event — which is exactly what RFC-0004 §E.1
 * asks it to do. Both of this node's ADVERTISE-emitting sites used to mint UNCONDITIONALLY:
 * `fwd_router_t::advertise` on the producer side, and `on_advertise`'s forwarding arm on the
 * mid-chain swap. So every cycle consumed one more of the link's 16-bit labels and appended one
 * more egress entry, and neither is reclaimed individually — only a whole-link `clear_link`
 * gives them back. A long-lived node behind a flapping link therefore walks its label space to
 * exhaustion and its egress table to the bound; on a memory-bounded node that ends as a reboot,
 * and short of it as a permanent loss of compaction.
 *
 * What is asserted here is the COUNT, not that traffic still flows: across 51 identical
 * re-advertise cycles the number of DISTINCT downstream labels stays 1 and the egress table
 * stays at one entry, at every node of a two-hop chain. Delivery is asserted too, but on its
 * own that is precisely the observation that let the leak ship — the frames kept arriving.
 *
 * The complement is asserted alongside: a genuinely NEW route still mints a fresh label and
 * still takes a second entry, so this is reuse of an identical binding rather than a cap.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

/** @brief #884's shape: drive 51 cycles, and every count under test must still read one. */
constexpr int kCycles = 51;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// --- wire builders (canonical bytes via the production emit helpers) ----------
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        const std::vector<std::byte> n = b_name(s);
        body.insert(body.end(), n.begin(), n.end());
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

/** @brief The label carried by an ADVERTISE frame — its first child, a 2-byte opaque VALUE. */
[[nodiscard]] std::uint16_t advertise_label(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->children.empty() || dec->children[0].payload.size() < 2) return 0;
    return tr::detail::load_le<std::uint16_t>(dec->children[0].payload);
}

/** @brief How many DISTINCT labels a run of ADVERTISE frames carried (the label-space census). */
[[nodiscard]] std::size_t distinct_labels(const std::vector<std::vector<std::byte>>& frames) {
    std::set<std::uint16_t> seen;
    for (const std::vector<std::byte>& f : frames) seen.insert(advertise_label(f));
    return seen.size();
}

// --- fake transports ----------------------------------------------------------

/** @brief A link that records every frame its router sends, and can inject one inbound. */
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
 * @brief One end of a point-to-point wire: whatever this router sends is handed straight into
 *        the peer router's receive path.
 *
 * A direct hand-off rather than the queued pump `fwd_reconnect_selfheal_test` needs: the frames
 * here are ADVERTISE and COMPACT travelling strictly downstream, so nothing re-enters and the
 * ordering the cascade tests depend on is not at stake.
 */
class hop_link_t final : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        if (peer_ != nullptr) peer_->inject(frame);
    }
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    void connect(hop_link_t& peer) noexcept { peer_ = &peer; }

   private:
    hop_link_t* peer_ = nullptr;
};

/** @brief The u32 currently stored at @p v, or nullopt if unwritten / not a u32 VALUE. */
std::optional<std::uint32_t> stored_u32(const graph_t& g, tr::graph::vertex_handle_t v) {
    const auto rope = g.read(v);
    if (!rope) return std::nullopt;
    const auto inner = tr::wire::decode((*rope)->only());
    if (!inner || inner->type != type_t::VALUE || inner->payload.size() != 4) return std::nullopt;
    return tr::detail::load_le<std::uint32_t>(inner->payload);
}

/**
 * @brief The forwarding arm: an upstream that re-advertises the SAME route mints ONE out-label.
 *
 * The inbound label is held fixed, which is what a peer whose own `advertise` reuses will
 * actually send. The node under test is mid-chain: it strips "down" and swaps MPLS-style.
 */
void test_forwarding_arm_reuses_one_downstream_label() {
    std::printf("Forwarding arm — %d identical re-advertises consume ONE downstream label:\n",
                kCycles);
    graph_t g;
    fwd_router_t node(g);
    fake_link_t up;
    fake_link_t down;
    node.add_child("up", up);
    node.add_child("down", down);

    constexpr std::uint16_t kUpLabel = 7;
    const std::vector<std::byte> adv =
        tr::net::encode_advertise(kUpLabel, b_path({"down", "sensor"}));
    for (int i = 0; i < kCycles; ++i) up.inject(adv);

    // The census, printed before it is judged: a count is the evidence here, so the number
    // belongs in the log whether it passes or fails.
    std::printf("    census: %zu distinct out-labels, %zu egress, %zu ingress\n",
                distinct_labels(down.sent()), node.handles().egress_count(),
                node.handles().ingress_count());
    // The RECEIVER's view is unchanged: it still gets one valid ADVERTISE per cycle. Only the
    // label and the table entry are deduplicated, never the frame.
    check(static_cast<int>(down.sent().size()) == kCycles,
          "the downstream peer still sees one ADVERTISE per cycle");
    check(distinct_labels(down.sent()) == 1,
          "... every one of them carrying the SAME label: the label space grew by exactly one");
    check(node.handles().egress_count() == 1,
          "the egress table holds ONE entry after all the cycles, not one per cycle");
    check(node.handles().ingress_count() == 1, "and ONE ingress binding — rebound in place");

    // The complement: reuse is keyed on the ROUTE, so a genuinely new flow still mints.
    up.inject(tr::net::encode_advertise(8, b_path({"down", "other"})));
    check(distinct_labels(down.sent()) == 2, "a genuinely new route still mints a fresh label");
    check(node.handles().egress_count() == 2, "and still takes a second egress entry");
}

/** @brief The producer door: `advertise()` called once per (re)connect must reuse its label. */
void test_producer_advertise_reuses_its_label() {
    std::printf("Producer advertise() — %d calls for one route consume ONE label:\n", kCycles);
    graph_t g;
    fwd_router_t node(g);
    fake_link_t down;
    node.add_child("down", down);

    const std::vector<std::byte> route = b_path({"sensor"});
    std::set<std::uint16_t> returned;
    for (int i = 0; i < kCycles; ++i) returned.insert(node.advertise("down", route));
    std::printf("    census: %zu distinct labels returned, %zu egress\n", returned.size(),
                node.handles().egress_count());
    check(returned.size() == 1 && *returned.begin() != 0,
          "advertise() hands back the SAME non-zero label on every call");
    check(static_cast<int>(down.sent().size()) == kCycles,
          "and still puts an ADVERTISE on the wire each time (the self-heal is intact)");
    check(node.handles().egress_count() == 1, "ONE egress entry after all the re-advertises");

    const std::uint16_t fresh = node.advertise("down", b_path({"other"}));
    check(fresh != 0 && fresh != *returned.begin(), "a new route still mints a fresh label");
    check(node.handles().egress_count() == 2, "and still takes a second egress entry");
}

/**
 * @brief The whole chain: origin A -> forwarder M -> terminus C, flapped @ref kCycles times.
 *
 * Each node's state is counted separately, because the growth compounds: A mints per call, M
 * mints per inbound ADVERTISE, and C takes a fresh ingress binding for every distinct out-label
 * M invents. One unbounded site upstream is enough to make every node downstream of it grow.
 */
void test_chain_flap_grows_no_state() {
    std::printf("Two-hop chain — %d flaps leave every node holding ONE binding:\n", kCycles);
    graph_t ga;
    graph_t gm;
    graph_t gc;
    fwd_router_t a(ga);
    fwd_router_t m(gm);
    fwd_router_t c(gc);
    hop_link_t a_down;
    hop_link_t m_up;
    hop_link_t m_down;
    hop_link_t c_up;
    a_down.connect(m_up);
    m_up.connect(a_down);
    m_down.connect(c_up);
    c_up.connect(m_down);
    a.add_child("down", a_down);
    m.add_child("up", m_up);
    m.add_child("down", m_down);
    c.add_child("up", c_up);
    const tr::graph::vertex_handle_t sensor =
        gc.register_vertex(*path_t::parse("/sensor"), role_t::STORED_VALUE);

    const std::vector<std::byte> route = b_path({"down", "sensor"});
    std::uint16_t label = 0;
    for (int i = 0; i < kCycles; ++i) label = a.advertise("down", route);
    check(label != 0, "the last flap's advertise still produced a usable label");
    std::printf("    census: A egress %zu | M ingress %zu egress %zu | C ingress %zu\n",
                a.handles().egress_count(), m.handles().ingress_count(), m.handles().egress_count(),
                c.handles().ingress_count());

    check(a.handles().egress_count() == 1, "A holds ONE egress entry, not one per flap");
    check(m.handles().ingress_count() == 1, "M holds ONE ingress binding — A's label is stable");
    check(m.handles().egress_count() == 1, "M holds ONE egress entry — the swap minted once");
    check(c.handles().ingress_count() == 1,
          "C holds ONE terminus binding — M's out-label is stable");

    // Traffic on its own proves nothing here; it is asserted so a count that stays at one
    // cannot be a chain that stopped working.
    constexpr std::uint32_t kValue = 0x5A5A1234u;
    a.send_compact("down", label, b_value_u32(kValue));
    check(stored_u32(gc, sensor) == kValue, "and the flow still delivers end to end");
}

}  // namespace

int main() {
    std::printf("== fwd_readvertise_reuse_test (#913) ==\n");
    test_forwarding_arm_reuses_one_downstream_label();
    test_producer_advertise_reuses_its_label();
    test_chain_flap_grows_no_state();
    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
