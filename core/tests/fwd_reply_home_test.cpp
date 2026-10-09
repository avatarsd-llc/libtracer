/**
 * @file
 * @brief #2042 — the RESULT of a forwarded write reaches the originator behind node 0's link.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Nodes 0..H in a line, every link a connection vertex made by `transport_vertex_t` (the
 * `dn`/`up` modules), and an `app` link on node 0 that the originator injects through. Node H
 * holds `/sink`. The originator sends `FWD{WRITE, dst, src = <empty PATH>, VALUE}`.
 *
 * RFC-0004 Amendment 2 §"Scope boundary" keeps §B's accumulation on every hop, including an
 * empty `src`: an empty seed is "name me by the link I arrive on". So node 0 grows `src` by
 * `net/app/o`, the terminus answers, and the RESULT walks home. At node 0 the reply's `dst` is
 * then exactly that one mount run — and before this fix the mount descent read "a `dst` naming
 * the mount exactly" as an address of the connection vertex itself, so node 0 took the REPLY
 * as its own and dropped it: no frame to the originator and no counter moved.
 *
 * A REPLY's `dst` is a return route, never the address of a local vertex, so a REPLY that
 * names a point-to-point mount exactly egresses over that link with an empty `dst` (the
 * reference/05 rule for a hop that consumes the final element). Asserted for H = 1 and 3, for
 * both spellings of the request (a PAIR chain and the canonical NAME runs), plus the control
 * that a REQUEST naming a mount exactly still terminates at the connection vertex.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_vertex.hpp"
#include "pair_body.hpp"
#include "test_support.hpp"
#include "tlv_tree.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::net::transport_vertex_t;
using tr::testing::check;
using tr::testing::check_quiet;
using tr::wire::opt_t;
using tr::wire::type_t;

constexpr std::size_t kMaxNodes = 4;

/** @brief In-process link end: `send` counts the frame, keeps it, and hands it to the peer. */
struct wire_link_t : transport_t {
    wire_link_t* peer = nullptr;
    std::size_t frames = 0;
    std::vector<std::byte> last;
    void inject(std::span<const std::byte> f) { rx_.deliver_borrowed(f); }
    void send(std::span<const std::byte> f) override {
        ++frames;
        last.assign(f.begin(), f.end());
        if (peer != nullptr) peer->inject(last);
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::vector<std::byte> flat;
        for (const auto& s : iov) flat.insert(flat.end(), s.begin(), s.end());
        send(std::span<const std::byte>(flat));
    }
};

/** @brief One node: a graph, its router, and the connection vertices over them. */
struct node_t {
    graph_t g;
    fwd_router_t r{g};
    transport_vertex_t net{g, r};
};

[[nodiscard]] std::vector<std::byte> path_tlv(const std::vector<std::string>& segs) {
    std::vector<std::byte> body;
    for (const std::string& s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief Nodes 0..hops wired in a line through `transport_vertex_t`, `/sink` at the far end. */
struct chain_t {
    std::size_t hops;
    std::vector<std::unique_ptr<node_t>> nodes;
    std::array<wire_link_t, kMaxNodes> down{};
    std::array<wire_link_t, kMaxNodes> up{};
    wire_link_t origin;  // node 0's `app` link; its far end is the originator (no peer here)
    std::size_t delivered = 0;

    static void count(void* ctx, const tr::graph::value_t&) { ++*static_cast<std::size_t*>(ctx); }

    explicit chain_t(std::size_t h) : hops(h) {
        for (std::size_t i = 0; i <= hops; ++i) nodes.push_back(std::make_unique<node_t>());
        const auto bind = [&](std::size_t i, const char* mod, const std::string& name,
                              transport_t& link) {
            node_t& n = *nodes[i];
            n.net.provide_link(mod, name, link);
            (void)n.net.register_module(mod, mod, tr::net::conn_role_t::DIAL);
            const auto st = n.g.write(path_t(std::string("/net/") + mod + "/conn"),
                                      tr::net::conn_spec_t(name).view());
            check_quiet(st.has_value(), "connection vertex created");
        };
        for (std::size_t i = 0; i < hops; ++i) {
            down[i].peer = &up[i + 1];
            up[i + 1].peer = &down[i];
            bind(i, "dn", "n" + std::to_string(i + 1), down[i]);
            bind(i + 1, "up", "n" + std::to_string(i), up[i + 1]);
        }
        bind(0, "app", "o", origin);
        (void)nodes[hops]->g.register_vertex(*path_t::parse("/sink"), role_t::STREAM);
        (void)nodes[hops]->g.subscribe(*path_t::parse("/sink"), &chain_t::count, &delivered);
    }

    /** @brief `dst` as a PAIR chain: node i's `/net/dn/n<i+1>` slot, then the sink's. */
    [[nodiscard]] std::vector<std::byte> pair_dst() {
        std::vector<std::byte> body;
        const auto slot_of = [&](std::size_t i, const std::string& p) {
            graph_t& g = nodes[i]->g;
            const auto v = g.find(path_t(p).key());
            const auto slot = v ? g.vertex_slot(*v) : std::nullopt;
            check_quiet(slot.has_value(), "vertex slot found");
            if (slot) tr::testing::emit_path_pair(body, *slot);
        };
        for (std::size_t i = 0; i < hops; ++i) slot_of(i, "/net/dn/n" + std::to_string(i + 1));
        slot_of(hops, "/sink");
        std::vector<std::byte> out;
        tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
        return out;
    }

    /** @brief The same route spelled canonically: `net/dn/n<i+1>` per hop, then `sink`. */
    [[nodiscard]] std::vector<std::byte> name_dst() const {
        std::vector<std::string> segs;
        for (std::size_t i = 0; i < hops; ++i) {
            segs.push_back("net");
            segs.push_back("dn");
            segs.push_back("n" + std::to_string(i + 1));
        }
        segs.push_back("sink");
        return path_tlv(segs);
    }
};

/** @brief The frame is `FWD{REPLY, dst = <empty PATH>, …, kind = RESULT}`. */
[[nodiscard]] bool is_result_home(std::span<const std::byte> f) {
    const auto dec = tr::wire::decode(f);
    if (!dec || dec->type != type_t::FWD || dec->children.size() < 4) return false;
    const auto& c = dec->children;
    const auto u8 = [](const tr::wire::tlv_t& t) {
        return t.payload.size() == 1 ? static_cast<std::uint8_t>(t.payload[0]) : 0xFFu;
    };
    return (u8(c[0]) & 0x3Fu) == static_cast<std::uint8_t>(fwd_op_t::REPLY) &&
           c[1].type == type_t::PATH && c[1].payload.empty() && c[1].children.empty() &&
           u8(c[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT);
}

void forwarded_write_result_reaches_origin(std::size_t hops, bool pair) {
    std::printf("H=%zu, %s-spelled dst, empty src:\n", hops, pair ? "PAIR" : "NAME");
    chain_t ch(hops);
    const std::vector<std::byte> body(64, std::byte{0xAB});
    std::vector<std::byte> value;
    tr::wire::emit_tlv(value, type_t::VALUE, opt_t{}, body);
    const auto frame = tr::testing::b_fwd(fwd_op_t::WRITE, pair ? ch.pair_dst() : ch.name_dst(),
                                          path_tlv({}), {}, value);
    ch.origin.inject(frame);
    check(ch.delivered == 1, "  the sink receives the write once");
    for (std::size_t i = 1; i <= hops; ++i)
        check(ch.up[i].frames == 1, "  one RESULT frame on every up link");
    check(ch.origin.frames == 1, "  the originator's link receives the RESULT (was 0, #2042)");
    check(is_result_home(ch.origin.last), "  ... as FWD{REPLY, dst = <empty PATH>, RESULT}");
}

void request_naming_a_mount_exactly_still_terminates() {
    std::printf("control: a REQUEST naming a mount exactly addresses its connection vertex\n");
    chain_t ch(1);
    ch.origin.inject(
        tr::testing::b_fwd(fwd_op_t::READ, path_tlv({"net", "dn", "n1"}), path_tlv({"reply-ep"})));
    check(ch.down[0].frames == 0, "  nothing egresses over the named link");
    check(ch.origin.frames == 1, "  the terminus answers the originator");
}

}  // namespace

int main() {
    std::printf("#2042 — a forwarded write's RESULT reaches the originator\n\n");
    for (const std::size_t h : {std::size_t{1}, std::size_t{3}})
        for (const bool pair : {true, false}) forwarded_write_result_reaches_origin(h, pair);
    request_naming_a_mount_exactly_still_terminates();
    return tr::testing::summary("fwd_reply_home");
}
