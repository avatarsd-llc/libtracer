/**
 * @file
 * @brief A forwarded WRITE through a mount is authorized at the mount's connection vertex.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One node with a subject resolver, one REAL TCP listener mounted as a bus at
 * `net/tcp-server/srv` and one point-to-point child at `net/tcp/x`, two real TCP client
 * sessions `p0` and `p1`. Both connection vertices carry the same `:acl`: `admin` holds every
 * right, `p0` holds WRITE, `p1` holds nothing. A frame from a bus session is evaluated under
 * that session's peer name, so every case below is a pair: `p1` is refused, and the identical
 * frame from `p0` is admitted — the control that makes each refusal the ACL's.
 *
 * - the NAME spelling through the BUS mount (`net/tcp-server/srv/<peer>`);
 * - the NAME spelling through the POINT-TO-POINT mount (`net/tcp/x/...`);
 * - the BOUND spelling of a session delivery: a one-element `PATH_REF` naming the peer's
 *   session anchor. An anchor sits outside the path tree, so no ancestor `:acl` reaches it;
 *   the delivery must still be authorized at the mount it crosses.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/path.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_tcp.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::acl_right_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::testing::check;
using tr::testing::make_value;
using tr::wire::opt_t;
using tr::wire::type_t;
using bytes_t = std::vector<std::byte>;

/** @brief The bus mount the two sessions are accepted under. */
constexpr std::string_view kMount = "net/tcp-server/srv";
/** @brief How long a refusal is watched for a frame that would have arrived. */
constexpr auto kDropBudget = 1500ms;

/** @brief Poll @p f until it holds or @p timeout passes (a test-side wait, not the library's). */
template <class Fn>
bool wait_until(Fn f, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return f();
}

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

/** @brief A one-element `PATH_REF` `dst`. */
bytes_t b_path_ref(tr::wire::path_ref_element_t e) {
    bytes_t out;
    (void)tr::wire::emit_path_ref(out, std::span<const tr::wire::path_ref_element_t>(&e, 1));
    return out;
}

/** @brief A counting point-to-point transport. */
struct recorder_t : tr::net::transport_t {
    std::atomic<std::size_t> n{0};
    void send(std::span<const std::byte>) override { ++n; }
};

/** @brief A real TCP client session with a counting receiver. */
struct client_t {
    explicit client_t(std::uint16_t port)
        : link(std::make_unique<tr::net::tcp_transport_t>(
              "127.0.0.1", port, tr::net::tcp_config_t{.defer_recv = true})) {
        link->set_receiver(sink);
        link->start_receiving();
    }
    /** @brief Counts the frames this session receives. */
    struct sink_t {
        std::mutex m;
        std::size_t frames = 0;
        void operator()(std::span<const std::byte>) {
            const std::lock_guard lk(m);
            ++frames;
        }
    } sink;
    std::size_t count() {
        const std::lock_guard lk(sink.m);
        return sink.frames;
    }
    std::unique_ptr<tr::net::tcp_transport_t> link;
};

/** @brief The node under test: a bus mount, a point-to-point mount, two sessions. */
struct node_t {
    graph_t g;
    fwd_router_t router{g};
    tr::net::tcp_server_transport_t server{0, {.max_peers = 4, .peer_named = true}};
    recorder_t p2p;
    std::unique_ptr<client_t> p0;
    std::unique_ptr<client_t> p1;

    node_t() {
        {
            auto hooks = g.hooks();
            hooks.subject_resolver = {caller_is_subject, nullptr};
            g.set_hooks(hooks);
        }
        check(server.ok(), "listener bound");
        (void)g.register_vertex(path_t("/net/tcp-server/srv"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/net/tcp/x"), role_t::STORED_VALUE);
        check(router.add_child(std::string(kMount), server), "bus listener mounted");
        check(router.add_child("net/tcp/x", p2p), "point-to-point child mounted");
        const bytes_t acl = mount_acl();
        check(g.write(path_t("/net/tcp-server/srv:acl"), make_value(acl)).has_value() &&
                  g.write(path_t("/net/tcp/x:acl"), make_value(acl)).has_value(),
              ":acl written on both connection vertices");
        p0 = std::make_unique<client_t>(server.local_port());
        check(wait_until([&] { return anchor("p0").has_value(); }), "session p0 accepted");
        p1 = std::make_unique<client_t>(server.local_port());
        check(wait_until([&] { return anchor("p1").has_value(); }), "session p1 accepted");
    }
    ~node_t() {
        p1.reset();
        p0.reset();
        (void)router.remove_child(kMount);
        (void)router.remove_child("net/tcp/x");
    }

    /** @brief The session anchor of bus peer @p peer, if it is accepted. */
    std::optional<vertex_handle_t> anchor(std::string_view peer) const {
        return g.find_session_anchor(fwd_router_t::session_anchor_id(kMount, peer));
    }
};

/** @brief Does @p from's WRITE at @p dst reach the counter @p seen? */
template <class Seen>
bool lands(client_t& from, std::span<const std::byte> dst, Seen&& seen) {
    const std::size_t before = seen();
    from.link->send(
        tr::testing::b_fwd(fwd_op_t::WRITE, dst, tr::testing::b_path({}), {}, b_value_u8(0x5A)));
    return wait_until([&] { return seen() > before; }, kDropBudget);
}

void name_spelling() {
    std::printf("the NAME spelling is authorized at every mount's connection vertex:\n");
    node_t n;
    const auto at_p0 = [&] { return n.p0->count(); };
    const auto at_p1 = [&] { return n.p1->count(); };
    const auto at_p2p = [&] { return n.p2p.n.load(); };
    check(!lands(*n.p1, tr::testing::b_path({"net", "tcp-server", "srv", "p0"}), at_p0),
          "bus mount: p1's WRITE to p0 by NAME is refused at the mount");
    check(lands(*n.p0, tr::testing::b_path({"net", "tcp-server", "srv", "p1"}), at_p1),
          "bus mount: p0's WRITE to p1 by NAME is admitted (the control)");
    check(!lands(*n.p1, tr::testing::b_path({"net", "tcp", "x", "foo"}), at_p2p),
          "point-to-point mount: p1's WRITE through it by NAME is refused");
    check(lands(*n.p0, tr::testing::b_path({"net", "tcp", "x", "foo"}), at_p2p),
          "point-to-point mount: p0's WRITE through it by NAME is admitted (the control)");
}

void bound_session_delivery() {
    std::printf("a bound delivery into a session is authorized at its mount:\n");
    node_t n;
    const auto element = [&](std::string_view peer) {
        const std::optional<vertex_handle_t> a = n.anchor(peer);
        const auto slot = a ? n.g.vertex_slot(*a) : std::nullopt;
        check(slot.has_value(), "the session anchor has a slot");
        return slot ? tr::wire::path_ref_element_t{.index = slot->index,
                                                   .generation = slot->generation}
                    : tr::wire::path_ref_element_t{};
    };
    const auto at_p0 = [&] { return n.p0->count(); };
    const auto at_p1 = [&] { return n.p1->count(); };
    check(!lands(*n.p1, b_path_ref(element("p0")), at_p0),
          "p1's bound WRITE into p0's session is refused at the mount");
    check(lands(*n.p0, b_path_ref(element("p1")), at_p1),
          "p0's bound WRITE into p1's session is admitted (the control)");
}

}  // namespace

int main() {
    if constexpr (!tr::net::kBusLinks)
        return tr::testing::skipped("mount_hop_acl", "no bus module");
    name_spelling();
    bound_session_delivery();
    return tr::testing::summary("mount_hop_acl");
}
