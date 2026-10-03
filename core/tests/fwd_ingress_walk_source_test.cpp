/**
 * @file
 * @brief #1648 review — the router's two in-place ingress reads (the `on_inbound` observer and
 *        the bus-NAME-hop rejection) spill a deep walk into the RECEIVING LINK's source, never
 *        the process heap.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `wire::tlv_node_t::over` walks a frame on a stack of inline slots and spills into a
 * `block_source_t` only when the frame nests deeper than those slots. A peer chooses the depth,
 * so the spill is a peer-provoked cost, and the standing rule (receiver pays) is that it is
 * drawn from the receiving link's own source: `rx_for(inbound_ctx)`, the source the terminus
 * arena and the iov tables already draw from.
 *
 * @section instrument The instrument
 *
 * Each link is registered with its own source that refuses on command, while the router's
 * default `rx` stays the heap. A refused spill is `TLV_NESTING_TOO_DEEP`, so with the link's
 * source refusing, a deep frame is NOT observed and NOT answered. Pointed at the heap instead,
 * the spill would succeed and the frame would be observed or answered: that is the
 * discriminating outcome. Every case also asserts the link's source was ASKED (so the case is
 * not vacuous), runs a shallow control that needs no spill, and ends with the source serving
 * and the same deep frame going through.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;

/** @brief A `block_source_t` that delegates to the heap until armed, then refuses. */
class arming_source_t final : public tr::mem::block_source_t {
   public:
    arming_source_t() noexcept : block_source_t("test_link_rx") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        ++asked_;
        if (armed_) return nullptr;
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }

    /** @brief Refuse every subsequent draw. */
    void arm() noexcept { armed_ = true; }
    /** @brief Serve again. */
    void disarm() noexcept { armed_ = false; }
    /** @brief Draws asked of this source so far, served or refused. */
    [[nodiscard]] int asked() const noexcept { return asked_; }

   private:
    bool armed_ = false;
    int asked_ = 0;
};

/** @brief A point-to-point endpoint that counts what it was handed (a bus peer's slot). */
struct p2p_link_t : transport_t {
    std::size_t received = 0; /**< @brief Frames this endpoint was handed. */
    void send(std::span<const std::byte>) override { ++received; }
};

/** @brief A multi-peer (bus) link with a fixed name→endpoint table (`fwd_flatten_backend_test`). */
struct bus_link_impl_t : transport_t, tr::net::bus_link_t {
    std::vector<std::pair<std::string, p2p_link_t*>> peers; /**< @brief name → endpoint. */
    std::size_t broadcasts = 0; /**< @brief Frames sent at the bus endpoint itself. */
    void send(std::span<const std::byte>) override { ++broadcasts; }
    tr::net::bus_link_t* bus() override { return this; }
    transport_t* peer_link(std::string_view name) override {
        for (auto& [n, l] : peers) {
            if (n == name) return l;
        }
        return nullptr;
    }
    void enumerate_peers(const tr::net::bus_link_t::peer_visitor_t& visit) const override {
        for (const auto& [n, l] : peers) visit(n);
    }
    /** @brief The handle's index into @ref peers is its name. */
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t peer,
                                             std::span<char>) const override {
        if (!peer.valid() || peer.index >= peers.size()) return {};
        return peers[peer.index].first;
    }
};

/** @brief A link that records what the router sends back and pushes frames upward. */
class rec_link_t : public transport_t {
   public:
    explicit rec_link_t(bool ropes = false) : ropes_(ropes) {}
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    [[nodiscard]] bool delivers_ropes() const override { return ropes_; }
    /** @brief Deliver @p frame upward: as a multi-link rope on a rope link, else borrowed. */
    void inject(std::span<const std::byte> frame) {
        if (!ropes_) {
            rx_.deliver_borrowed(frame);
            return;
        }
        tr::view::rope_t r;
        const std::size_t step = (frame.size() + 3) / 4;
        for (std::size_t off = 0; off < frame.size(); off += step) {
            const std::size_t n = std::min(step, frame.size() - off);
            tr::view::segment_ptr_t seg = tr::view::heap_alloc(n);
            std::memcpy(seg->bytes.data(), frame.data() + off, n);
            r.append(tr::view::view_t::over(std::move(seg)));
        }
        rx_.deliver_rope(std::move(r));
    }
    std::vector<std::vector<std::byte>> sent; /**< @brief Frames the router handed down. */

   private:
    bool ropes_ = false;
};

/** @brief A `VALUE` nested @p depth `STATUS` levels deep — deeper than the walk's slots. */
std::vector<std::byte> b_nest(int depth) {
    std::vector<std::byte> nested;
    const std::byte one{1};
    tr::wire::emit_tlv(nested, type_t::VALUE, opt_t{}, std::span<const std::byte>(&one, 1));
    for (int i = 0; i < depth; ++i) {
        std::vector<std::byte> outer;
        tr::wire::emit_tlv(outer, type_t::STATUS, opt_t{.pl = true}, nested);
        nested.swap(outer);
    }
    return nested;
}

/** @brief Deeper than the walk's eight inline slots (the `tlv_children_test` depth). */
constexpr int kDeep = 12;

/** @brief The observer arm: a deep FWD spills into the link's source, not the heap. */
void test_observer_walk_draws_from_the_link() {
    std::printf("the on_inbound observer's walk spills into the receiving link's source:\n");
    graph_t g;
    fwd_router_t router(g);  // the router's default rx stays the heap
    arming_source_t link_rx;
    rec_link_t in;
    (void)router.add_child("in", in, &link_rx);
    int observed = 0;
    router.on_inbound(
        [](void* ctx, std::string_view, const tr::wire::tlv_node_t&) { ++*static_cast<int*>(ctx); },
        &observed);

    const std::vector<std::byte> dst = b_path({"nowhere"});
    const std::vector<std::byte> src = b_path({"origin"});
    const std::vector<std::byte> shallow = b_fwd(fwd_op_t::WRITE, dst, src, {}, b_nest(1));
    const std::vector<std::byte> deep = b_fwd(fwd_op_t::WRITE, dst, src, {}, b_nest(kDeep));

    link_rx.arm();
    in.inject(shallow);
    check(observed == 1, "control: a shallow FWD is observed with the link's source refusing");

    const int asked_before = link_rx.asked();
    in.inject(deep);
    check(link_rx.asked() > asked_before, "instrument: the walk ASKED the link's source");
    check(observed == 1, "a refused spill is TLV_NESTING_TOO_DEEP: the deep FWD is not observed");

    link_rx.disarm();
    in.inject(deep);
    check(observed == 2, "the same deep FWD is observed once the link's source serves");
}

/** @brief The rejection arm, span and rope: a deep refused hop spills into the link's source. */
void test_reject_walk_draws_from_the_link(bool ropes) {
    std::printf("the bus-name rejection's walk spills into the receiving link's source (%s):\n",
                ropes ? "rope" : "span");
    graph_t g;
    fwd_router_t router(g);
    bus_link_impl_t bus;
    p2p_link_t alice;
    bus.peers.emplace_back("alice", &alice);
    arming_source_t link_rx;
    rec_link_t in(ropes);
    (void)router.add_child("net/ws-server/srv", bus);
    (void)router.add_child("net/ws-client/in", in, &link_rx);

    // `srv` is the bus link's own NAME and `sensor` names no peer on it: the ADR-0073 §3
    // rejection, which ANSWERS a well-formed frame.
    const std::vector<std::byte> dst = b_path({"net", "ws-server", "srv", "sensor"});
    const std::vector<std::byte> src = b_path({"origin"});
    const std::vector<std::byte> shallow = b_fwd(fwd_op_t::WRITE, dst, src, {}, b_nest(1));
    const std::vector<std::byte> deep = b_fwd(fwd_op_t::WRITE, dst, src, {}, b_nest(kDeep));

    link_rx.arm();
    in.inject(shallow);
    check(in.sent.size() == 1, "control: a shallow refused hop is answered");

    const int asked_before = link_rx.asked();
    in.inject(deep);
    check(link_rx.asked() > asked_before, "instrument: the walk ASKED the link's source");
    check(in.sent.size() == 1, "a refused spill drops the deep frame by value: no answer");
    check(bus.broadcasts == 0 && alice.received == 0, "and nothing went over the bus");

    link_rx.disarm();
    in.inject(deep);
    check(in.sent.size() == 2, "the same deep frame is answered once the link's source serves");
}

}  // namespace

int main() {
    test_observer_walk_draws_from_the_link();
    // The rejection presupposes the ADR-0044 bus module; a bus-closed build mounts the bus as a
    // point-to-point link and never reaches it, so those cases are bound to the module here
    // rather than labelling the whole target out of the bus-closed leg.
    if constexpr (tr::net::kBusLinks) {
        test_reject_walk_draws_from_the_link(/*ropes=*/false);
        test_reject_walk_draws_from_the_link(/*ropes=*/true);
    } else {
        std::printf("bus module closed: the bus-name rejection cases are skipped\n");
    }
    return tr::testing::summary("fwd_ingress_walk_source");
}
