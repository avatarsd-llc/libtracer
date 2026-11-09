/**
 * @file
 * @brief The bus-NAME-hop refusal reads nothing past `src`, so a deep frame draws nothing from
 *        the receiving link's source and is answered (#2479; was #1648's spill pin).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The refusal used to validate the whole frame with `wire::tlv_node_t::over`, whose walk spilled
 * into the receiving link's source once a peer nested a payload deeper than its inline slots: a
 * peer-provoked cost, drawn there because the receiver pays. The refusal now answers from what
 * the routing peek read and the one `src` locator (#2479, maintainer ruling: no whole-frame
 * re-validation), so the cost is gone rather than charged.
 *
 * @section instrument The instrument
 *
 * The inbound link is registered with its own source, armed to refuse every draw. A shallow
 * control is answered; the deep frame must be answered too, with the source never asked.
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
    void send(std::span<const std::span<const std::byte>>) override { ++received; }
};

/** @brief A multi-peer (bus) link with a fixed name→endpoint table (`fwd_flatten_backend_test`). */
struct bus_link_impl_t : transport_t, tr::net::bus_link_t {
    std::vector<std::pair<std::string, p2p_link_t*>> peers; /**< @brief name → endpoint. */
    std::size_t broadcasts = 0; /**< @brief Frames sent at the bus endpoint itself. */
    void send(std::span<const std::byte>) override { ++broadcasts; }
    void send(std::span<const std::span<const std::byte>>) override { ++broadcasts; }
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
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::vector<std::byte>& f = sent.emplace_back();
        for (const std::span<const std::byte>& s : iov) f.insert(f.end(), s.begin(), s.end());
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
        (void)rx_.deliver_rope(std::move(r));
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

/** @brief The rejection arm, span and rope: a deep refused hop draws nothing and is answered. */
void test_reject_walk_draws_from_the_link(bool ropes) {
    std::printf("the bus-name rejection of a deep frame draws nothing from the link (%s):\n",
                ropes ? "rope" : "span");
    graph_t g;
    fwd_router_t router(g);
    bus_link_impl_t bus;
    p2p_link_t alice;
    bus.peers.emplace_back("alice", &alice);
    arming_source_t link_rx;
    rec_link_t in(ropes);
    (void)router.attach_link("net/ws-server/srv", bus);
    (void)router.attach_link("net/ws-client/in", in, &link_rx);

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
    check(link_rx.asked() == asked_before, "the deep frame never asks the link's source");
    check(in.sent.size() == 2, "and is answered like the shallow one");
    check(bus.broadcasts == 0 && alice.received == 0, "and nothing went over the bus");
}

}  // namespace

int main() {
    // The rejection presupposes the ADR-0044 bus module; a bus-closed build mounts the bus as a
    // point-to-point link and never reaches it, so the suite states a skip there.
    if constexpr (!tr::net::kBusLinks)
        return tr::testing::skipped("fwd_ingress_walk_source", "bus module closed");
    test_reject_walk_draws_from_the_link(/*ropes=*/false);
    test_reject_walk_draws_from_the_link(/*ropes=*/true);
    return tr::testing::summary("fwd_ingress_walk_source");
}
