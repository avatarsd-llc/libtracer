/**
 * @file
 * @brief RFC-0004 §E.1 — a MID-CHAIN reconnect self-heals: `clear_link` sweeps the cross-link
 *        ingress bindings whose downstream half crossed the cleared link (#716).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A 3-node chain of in-process routers, wired by relay links that carry each node's sends
 * into its peer's receive path:
 *
 *     origin A  --"down"/"up"-->  forwarder M  --"down"/"up"-->  terminus C
 *
 * A advertises a compact flow to `/down/sensor` and streams `COMPACT`s; M swaps the label
 * MPLS-style; C expands it and writes `/sensor`. Then M's downstream link reconnects — both
 * of its endpoints (M and C) call `clear_link`, which is what a transport does on (re)connect.
 *
 * The hazard this pins: M's forwarding binding is stored under the INBOUND link ("up") while
 * its `down_link` names the OUTBOUND one ("down"), so clearing "down" alone leaves that
 * binding alive and aimed at an out-label that died with the erased table. A never saw the
 * reconnect, so A never re-advertises; M forwards the dead out-label; C NACKs; M answers the
 * NACK out of the table it just erased and returns silently. The flow drops every delivery,
 * forever, and the origin is never told.
 *
 * Assertions (the #716 proof):
 *   - steady state: a COMPACT streamed by A is written byte-exact into C's `/sensor`;
 *   - the sweep is real: after M clears "down", M holds NO ingress binding on "up" either;
 *   - RECOVERY, not merely the NACK: A keeps streaming exactly as it did before, and C's
 *     `/sensor` takes a NEW value again — end to end, with no external re-advertise;
 *   - the cascade is the SHIPPED one: A's re-advertise is what A's own `on_nack` emits, and
 *     the frames observed on the wire are only ADVERTISE / COMPACT / HANDLE_NACK;
 *   - a TERMINUS binding is never swept (it has no downstream half): C's binding for a second,
 *     unrelated flow survives a `clear_link` of an unrelated link.
 *
 * Deterministic and event-driven: no threads, no sleeps — frames are pumped between the
 * relay links until the chain is quiescent, with a bounded round cap.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

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

/**
 * @brief One end of a point-to-point link: records what its router sends, and injects what the
 *        far end sent into that router's receive path.
 *
 * Sends are QUEUED rather than handed straight across, so the chain runs as a pump with an
 * explicit, observable round structure — a re-entrant hand-off would hide the ordering the
 * cascade under test depends on.
 */
class relay_link_t final : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        pending_.emplace_back(frame.begin(), frame.end());
    }
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }

    /** @brief Take everything queued since the last drain (leaving the queue empty). */
    std::vector<std::vector<std::byte>> drain() {
        std::vector<std::vector<std::byte>> out;
        out.swap(pending_);
        return out;
    }

   private:
    std::vector<std::vector<std::byte>> pending_;
};

/** @brief A bidirectional wire: whatever end @p a sends arrives at end @p b, and vice versa. */
struct wire_pair_t {
    relay_link_t* a = nullptr;
    relay_link_t* b = nullptr;
};

/** @brief Every frame observed crossing the chain during one pump, by TLV type. */
struct traffic_t {
    int advertise = 0;
    int compact = 0;
    int nack = 0;
    int other = 0;
};

void classify(traffic_t& t, std::span<const std::byte> frame) {
    const auto tlv = tr::wire::decode(frame);
    if (!tlv) {
        ++t.other;
        return;
    }
    switch (tlv->type) {
        case type_t::ADVERTISE:
            ++t.advertise;
            break;
        case type_t::COMPACT:
            ++t.compact;
            break;
        case type_t::HANDLE_NACK:
            ++t.nack;
            break;
        default:
            ++t.other;
            break;
    }
}

/**
 * @brief Carry queued frames across every wire until the chain is quiescent.
 *
 * A round drains BOTH ends of every wire before injecting anything, so a frame produced in
 * round N is delivered in round N+1 and never overtakes one produced earlier. The cap bounds
 * a chain that will not settle; a settled chain returns well inside it.
 * @return The frames observed, by type, over all rounds.
 */
traffic_t pump(std::initializer_list<wire_pair_t> wires, int max_rounds = 16) {
    traffic_t seen;
    for (int round = 0; round < max_rounds; ++round) {
        std::vector<std::pair<relay_link_t*, std::vector<std::vector<std::byte>>>> in_flight;
        for (const wire_pair_t& w : wires) {
            in_flight.emplace_back(w.b, w.a->drain());
            in_flight.emplace_back(w.a, w.b->drain());
        }
        bool moved = false;
        for (auto& [dst, frames] : in_flight) {
            for (const std::vector<std::byte>& f : frames) {
                moved = true;
                classify(seen, f);
                dst->inject(f);
            }
        }
        if (!moved) break;
    }
    return seen;
}

/** @brief The u32 currently stored at @p v, or nullopt if unwritten / not a u32 VALUE. */
std::optional<std::uint32_t> stored_u32(const graph_t& g, tr::graph::vertex_handle_t v) {
    const auto rope = g.read(v);
    if (!rope) return std::nullopt;
    const auto inner = tr::wire::decode((*rope)->only());
    if (!inner || inner->type != type_t::VALUE || inner->payload.size() != 4) return std::nullopt;
    return tr::detail::load_le<std::uint32_t>(inner->payload);
}

/**
 * @brief The chain under test: origin A -> forwarder M -> terminus C, wired and registered.
 *
 * Each node names its links "up" / "down" from its own point of view, which is what makes the
 * defect visible: M's binding for the flow lives under "up" while it points at "down".
 */
struct chain_t {
    graph_t ga;
    graph_t gm;
    graph_t gc;
    fwd_router_t a{ga};
    fwd_router_t m{gm};
    fwd_router_t c{gc};
    relay_link_t a_down;  // A's end of the A<->M wire
    relay_link_t m_up;    // M's end of the A<->M wire
    relay_link_t m_down;  // M's end of the M<->C wire
    relay_link_t c_up;    // C's end of the M<->C wire
    tr::graph::vertex_handle_t sensor;

    chain_t() : sensor(gc.register_vertex(*path_t::parse("/sensor"), role_t::STORED_VALUE)) {
        a.add_child("down", a_down);
        m.add_child("up", m_up);
        m.add_child("down", m_down);
        c.add_child("up", c_up);
    }

    traffic_t settle() { return pump({wire_pair_t{&a_down, &m_up}, wire_pair_t{&m_down, &c_up}}); }
};

/**
 * @brief Establish the compact flow and stream one value: A advertises /down/sensor, M swaps,
 *        C writes.
 * @return A's label for the flow (0 ⇒ the advertise did not take).
 */
std::uint16_t establish(chain_t& ch) {
    const std::uint16_t label = ch.a.advertise("down", b_path({"down", "sensor"}));
    (void)ch.settle();
    return label;
}

void test_midchain_reconnect_self_heals() {
    std::printf("Mid-chain reconnect self-heal (A -> M -> C, only M's down link reconnects):\n");
    chain_t ch;

    const std::uint16_t label = establish(ch);
    check(label != 0, "A allocated a label and advertised the compact flow downstream");

    constexpr std::uint32_t kBefore = 0x11112222u;
    ch.a.send_compact("down", label, b_value_u32(kBefore));
    (void)ch.settle();
    check(stored_u32(ch.gc, ch.sensor) == kBefore,
          "steady state: the COMPACT reaches C byte-exact");

    // The reconnect. A transport calls clear_link on BOTH endpoints of the link that came
    // back; A is on the OTHER side of M and sees nothing at all, so it never re-advertises.
    ch.m.clear_link("down");
    ch.c.clear_link("up");
    check(ch.m.handles().ingress_count() == 0,
          "the sweep erased M's ingress binding on \"up\" — its downstream half crossed \"down\"");

    // A streams on, exactly as before: same link, same label, no external prompt. Recovery has
    // to come from the machinery, not from the test.
    constexpr std::uint32_t kAfter = 0x33334444u;
    ch.a.send_compact("down", label, b_value_u32(kAfter));
    const traffic_t seen = ch.settle();

    check(stored_u32(ch.gc, ch.sensor) == kBefore,
          "the delivery that raced the reconnect is dropped, not misrouted (C still holds the "
          "old value)");
    check(seen.nack >= 1, "the miss draws a HANDLE_NACK back toward the origin");
    check(seen.advertise >= 2,
          "the NACK prompts A to re-advertise, and M re-advertises a fresh out-label to C");
    check(seen.other == 0, "the cascade emits ONLY already-specified route-handle frames");

    // ... and the NEXT delivery lands. This is the assertion the issue turns on: recovery, not
    // merely a NACK. Without the cross-link sweep C never sees another value on this flow.
    constexpr std::uint32_t kHealed = 0x55556666u;
    ch.a.send_compact("down", label, b_value_u32(kHealed));
    (void)ch.settle();
    check(stored_u32(ch.gc, ch.sensor) == kHealed,
          "the flow HEALED end to end — a later delivery from A reaches C again");

    // And it stays healed: the rebuilt binding is an ordinary steady-state binding.
    constexpr std::uint32_t kSteady = 0x77778888u;
    ch.a.send_compact("down", label, b_value_u32(kSteady));
    const traffic_t after = ch.settle();
    check(stored_u32(ch.gc, ch.sensor) == kSteady, "the healed flow keeps delivering");
    check(after.nack == 0 && after.advertise == 0,
          "and it is back to lean COMPACTs only — no repeated re-advertise loop");
}

void test_terminus_binding_is_not_swept() {
    std::printf("A terminus binding has no downstream half and survives an unrelated clear:\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sensor"), role_t::STORED_VALUE);
    fwd_router_t node(g);
    relay_link_t up;
    relay_link_t other;
    node.add_child("up", up);
    node.add_child("other", other);

    constexpr std::uint16_t kLabel = 7;
    up.inject(tr::net::encode_advertise(kLabel, b_path({"sensor"})));
    check(node.handles().ingress_count() == 1, "the terminus binding was learned");

    node.clear_link("other");
    check(node.handles().ingress_count() == 1,
          "clearing an unrelated link leaves the terminus binding alone");

    constexpr std::uint32_t kValue = 0x0BADF00Du;
    up.inject(tr::net::encode_compact(kLabel, b_value_u32(kValue)));
    const auto v = g.find(path_t::parse("/sensor")->key());
    check(v.has_value() && stored_u32(g, *v) == kValue,
          "and the flow still delivers locally after the unrelated clear");
}

void test_sweep_is_scoped_to_the_cleared_link() {
    std::printf("The sweep is SCOPED — a sibling flow through a different link is untouched:\n");
    graph_t g;
    fwd_router_t node(g);
    relay_link_t up;
    relay_link_t left;
    relay_link_t right;
    node.add_child("up", up);
    node.add_child("left", left);
    node.add_child("right", right);

    up.inject(tr::net::encode_advertise(1, b_path({"left", "sensor"})));
    up.inject(tr::net::encode_advertise(2, b_path({"right", "sensor"})));
    check(node.handles().ingress_count() == 2,
          "two forwarding bindings learned, one per downstream");

    node.clear_link("left");
    check(node.handles().ingress_count() == 1,
          "clearing \"left\" swept only the binding whose downstream half crossed it");

    // The survivor is the "right" one, and it still forwards.
    right.drain();
    up.inject(tr::net::encode_compact(2, b_value_u32(0xDEADBEEFu)));
    check(right.drain().size() == 1, "the sibling flow through \"right\" still forwards");
}

}  // namespace

int main() {
    std::printf("== fwd_reconnect_selfheal_test (RFC-0004 §E.1, #716) ==\n");
    test_midchain_reconnect_self_heals();
    test_terminus_binding_is_not_swept();
    test_sweep_is_scoped_to_the_cleared_link();
    return tr::testing::summary("fwd_reconnect_selfheal");
}
