/**
 * @file
 * @brief A PAIR-spelled hop is authorized where the NAME spelling of the same hop is.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0029 slice S1 gives a forwarded operation one more way to name the hop it crosses: a
 * PAIR element inside its `PATH` `dst`. The mount it crosses decides its authorization, not
 * the spelling, so this file drives every spelling of one crossing at the wire seam, against
 * the production `fwd_router_t`, over in-memory links, on one call stack:
 *
 * - the NAME spelling, through a bus mount (`<mount>/<peer>`) and a point-to-point mount;
 * - the PAIR HOP: a PAIR naming the point-to-point connection vertex, then a NAME tail;
 * - the PAIR naming a bus session's ANCHOR as the last element (the bound delivery into a
 *   session). The anchor sits outside the path tree, so no ancestor `:acl` reaches it; the
 *   delivery is authorized at the mount it crosses, as `<mount>/<peer>` is.
 *
 * Both connection vertices carry one `:acl`: `admin` holds every right, `p0` holds WRITE, `p1`
 * holds nothing. A frame from a bus session is evaluated under that session's peer name, so
 * every refusal from `p1` is paired with the identical frame from `p0` landing — the control
 * that makes each refusal the ACL's, and the proof that an allowed PAIR forwards.
 *
 * A last case mounts the bus with NO connection vertex. Enforcing, it has nothing to grant a
 * right, so the PAIR naming one of its sessions' anchors is refused; the same graph without a
 * subject resolver forwards it (the control).
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_pair.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"
#include "pair_body.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::net::peer_handle_t;
using tr::testing::check;
using tr::testing::make_value;
using tr::wire::opt_t;
using tr::wire::path_pair_t;
using tr::wire::type_t;
using bytes_t = std::vector<std::byte>;

/** @brief The bus mount the two sessions are accepted under. */
constexpr std::string_view kBus = "net/srv/bus";
/** @brief The point-to-point mount a hop crosses. */
constexpr std::string_view kP2p = "net/tcp/x";

/** @brief The test resolver (ADR-0018): the caller context IS the subject token. */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    const auto* p = reinterpret_cast<const std::byte*>(caller.data());
    return subject_token_t(p, p + caller.size());
}

/** @brief One inheritable ALLOW ACE for @p subject over @p mask. */
tr::graph::ace_t allow(std::string_view subject, std::uint32_t mask) {
    const auto* p = reinterpret_cast<const std::byte*>(subject.data());
    return tr::graph::ace_t{
        .type = tr::graph::ace_type_t::ALLOW,
        .flags = tr::graph::kAceInherit,
        .subject = bytes_t(p, p + subject.size()),
        .access_mask = mask,
        .expires_ns = 0,
    };
}

/** @brief `admin` holds every right, `p0` holds WRITE, everyone else nothing. */
bytes_t mount_acl() {
    const tr::graph::ace_t aces[2] = {
        allow("admin", 0xFFFFFFFFu),
        allow("p0", static_cast<std::uint32_t>(acl_right_t::WRITE)),
    };
    return tr::graph::encode_acl(std::span<const tr::graph::ace_t>(aces, 2));
}

/** @brief A `VALUE` TLV carrying one byte. */
bytes_t b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief A `PATH` TLV whose body is the PAIR @p head followed by the NAMEs @p tail. */
bytes_t b_pair_path(path_pair_t head, std::initializer_list<std::string_view> tail = {}) {
    bytes_t body;
    tr::testing::emit_path_pair(body, head);
    for (const std::string_view s : tail) (void)tr::wire::emit_path_segment(body, s);
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A WRITE of one byte at @p dst, with an empty `src`. */
bytes_t b_write(std::span<const std::byte> dst) {
    return tr::testing::b_fwd(fwd_op_t::WRITE, dst, tr::testing::b_path({}), {}, b_value_u8(0x5A));
}

/** @brief A counting point-to-point transport. */
struct recorder_t : tr::net::transport_t {
    std::size_t n = 0; /**< @brief Frames sent through this link. */
    void send(std::span<const std::byte>) override { ++n; }
};

/**
 * @brief An in-memory BUS transport: a frame arrives tagged with its peer's handle, and a
 *        delivery to a peer lands on that peer's counting endpoint.
 */
struct fake_bus_t final : tr::net::transport_t, tr::net::bus_link_t {
    /** @brief Undirected sends are not part of this facet's contract; drop. */
    void send(std::span<const std::byte>) override {}
    tr::net::bus_link_t* bus() override { return this; }
    void enumerate_peers(const peer_visitor_t& visit) const override {
        for (const std::string& name : names_) visit(name);
    }
    tr::net::transport_t* peer_link(std::string_view peer) override {
        const auto it = peers_.find(std::string(peer));
        return it == peers_.end() ? nullptr : it->second.get();
    }
    [[nodiscard]] std::string_view peer_name(peer_handle_t peer, std::span<char>) const override {
        return peer.valid() && peer.index < names_.size() ? std::string_view(names_[peer.index])
                                                          : std::string_view{};
    }
    /** @brief Admit @p name as an accepting listener does: endpoint first, then the arrival
     *         notifier (which registers the session's anchor). */
    void arrive(std::string_view name) {
        peers_[std::string(name)] = std::make_unique<recorder_t>();
        names_.emplace_back(name);
        notify_peer_up(handle(name), name);
    }
    /** @brief Deliver @p frame as one sent by session @p name. */
    void inject(std::string_view name, std::span<const std::byte> frame) {
        peer_rx_.deliver_borrowed(handle(name), frame);
    }
    /** @brief The frames delivered to session @p name. */
    std::size_t delivered(std::string_view name) const { return peers_.at(std::string(name))->n; }

   private:
    peer_handle_t handle(std::string_view name) const {
        for (std::size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == name)
                return peer_handle_t{
                    tr::wire::pair_t{.index = static_cast<std::uint32_t>(i), .generation = 1}};
        return {};
    }
    std::map<std::string, std::unique_ptr<recorder_t>> peers_; /**< @brief name → endpoint. */
    std::vector<std::string> names_; /**< @brief handle index → peer name. */
};

/** @brief The node under test: a bus mount with sessions `p0` and `p1`, a point-to-point
 *         mount, and the one `:acl` on every connection vertex that exists. */
struct node_t {
    graph_t g;
    fwd_router_t router{g};
    fake_bus_t bus;
    recorder_t p2p;

    /** @brief @p enforce installs the subject resolver; @p bus_vertex registers the bus
     *         mount's connection vertex before it is mounted. */
    explicit node_t(bool enforce = true, bool bus_vertex = true) {
        if (enforce) {
            auto hooks = g.hooks();
            hooks.subject_resolver = {caller_is_subject, nullptr};
            g.set_hooks(hooks);
        }
        if (bus_vertex) (void)g.register_vertex(path_t("/net/srv/bus"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/net/tcp/x"), role_t::STORED_VALUE);
        check(router.add_child(std::string(kBus), bus), "bus mounted");
        check(router.add_child(std::string(kP2p), p2p), "point-to-point child mounted");
        const bytes_t acl = mount_acl();
        check(g.write(path_t("/net/tcp/x:acl"), make_value(acl)).has_value(),
              ":acl written on the point-to-point connection vertex");
        if (bus_vertex)
            check(g.write(path_t("/net/srv/bus:acl"), make_value(acl)).has_value(),
                  ":acl written on the bus connection vertex");
        bus.arrive("p0");
        bus.arrive("p1");
    }
    ~node_t() {
        (void)router.remove_child(kBus);
        (void)router.remove_child(kP2p);
    }

    /** @brief The PAIR naming bus session @p peer's anchor. */
    path_pair_t anchor(std::string_view peer) const {
        const std::optional<vertex_handle_t> a =
            g.find_session_anchor(fwd_router_t::session_anchor_id(kBus, peer));
        const auto slot = a ? g.vertex_slot(*a) : std::nullopt;
        check(slot.has_value(), "the session anchor has a slot");
        return slot.value_or(path_pair_t{});
    }
    /** @brief The PAIR naming the point-to-point mount's connection vertex. */
    path_pair_t p2p_vertex() const {
        const std::optional<vertex_handle_t> v = g.find(path_t("/net/tcp/x").key());
        const auto slot = v ? g.vertex_slot(*v) : std::nullopt;
        check(slot.has_value(), "the point-to-point connection vertex has a slot");
        return slot.value_or(path_pair_t{});
    }
    /** @brief Does session @p from's WRITE at @p dst add one frame to @p seen()? */
    template <class Seen>
    bool lands(std::string_view from, std::span<const std::byte> dst, Seen&& seen) {
        const std::size_t before = seen();
        bus.inject(from, b_write(dst));
        return seen() > before;
    }
};

/** @brief 1 + 3: the one denied `:acl` refuses every spelling of the crossing, and the
 *         identical frame from an allowed caller lands — an allowed PAIR forwards. */
void one_acl_every_spelling() {
    std::printf("one :acl at the mount decides the NAME, PAIR-hop and PAIR-anchor spellings:\n");
    node_t n;
    const auto at_p0 = [&] { return n.bus.delivered("p0"); };
    const auto at_p1 = [&] { return n.bus.delivered("p1"); };
    const auto at_p2p = [&] { return n.p2p.n; };

    check(!n.lands("p1", tr::testing::b_path({"net", "srv", "bus", "p0"}), at_p0),
          "NAME, bus mount: p1's WRITE into p0's session is refused");
    check(n.lands("p0", tr::testing::b_path({"net", "srv", "bus", "p1"}), at_p1),
          "NAME, bus mount: p0's WRITE into p1's session lands (the control)");
    check(!n.lands("p1", tr::testing::b_path({"net", "tcp", "x", "foo"}), at_p2p),
          "NAME, point-to-point mount: p1's WRITE through it is refused");
    check(n.lands("p0", tr::testing::b_path({"net", "tcp", "x", "foo"}), at_p2p),
          "NAME, point-to-point mount: p0's WRITE through it lands (the control)");

    check(!n.lands("p1", b_pair_path(n.p2p_vertex(), {"foo"}), at_p2p),
          "PAIR hop: p1's WRITE through the point-to-point connection vertex is refused");
    check(n.lands("p0", b_pair_path(n.p2p_vertex(), {"foo"}), at_p2p),
          "PAIR hop: p0's WRITE through it forwards (the control)");

    check(!n.lands("p1", b_pair_path(n.anchor("p0")), at_p0),
          "PAIR anchor: p1's WRITE into p0's session is refused at the mount");
    check(n.lands("p0", b_pair_path(n.anchor("p1")), at_p1),
          "PAIR anchor: p0's WRITE into p1's session forwards (the control)");
}

/** @brief 2: a mount with no connection vertex refuses a PAIR into one of its sessions. */
void vertexless_mount() {
    std::printf("a mount with no connection vertex refuses a PAIR anchor under enforcement:\n");
    const auto forwards = [](bool enforce) {
        node_t n(enforce, false);
        check(!n.g.find(path_t("/net/srv/bus").key()).has_value(),
              "the bus mount has no connection vertex");
        return n.lands("p0", b_pair_path(n.anchor("p1")), [&] { return n.bus.delivered("p1"); });
    };
    check(!forwards(true), "enforcing: a PAIR into a session of a vertex-less mount is refused");
    check(forwards(false), "not enforcing: the same PAIR forwards (the control)");
}

}  // namespace

int main() {
    if constexpr (!tr::net::kBusLinks)
        return tr::testing::skipped("pair_hop_acl",
                                    "this build closed the ADR-0044 bus module out "
                                    "(kBusLinks = false); no session anchor exists to name");
    one_acl_every_spelling();
    vertexless_mount();
    return tr::testing::summary("pair_hop_acl");
}
