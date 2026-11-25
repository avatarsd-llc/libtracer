/**
 * @file
 * @brief #2923: a subscription push to a peer-named session reaches that session and no other.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A session on a peer-named TCP or WS listener subscribes to `/sink`; a write pushes the value
 * to it over its own directed endpoint. Between the push resolving that endpoint and offering
 * the frame to the session's egress, the session leaves and a new connection takes the same
 * slot. The new session did not subscribe, so it must not receive the push, and the push is
 * dropped and counted in `dropped_tx`.
 *
 * The push is parked at that point by the fault-injection seam `directed_send_hook`. A FLAT
 * listener's subscription belongs to the listener as a whole (ADR-0082), so its pushes go to
 * the listener and are not directed to a session; it has nothing to check here.
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_ws.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;
using tr::testing::make_value;

using namespace std::chrono_literals;

/** @brief Records every frame a client receives. */
struct frame_sink_t {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::vector<std::byte>> frames;

    /** @brief The receiver callable (bound by address via set_receiver(F&)). */
    void operator()(std::span<const std::byte> f) {
        {
            const std::lock_guard lock(m);
            frames.emplace_back(f.begin(), f.end());
        }
        cv.notify_all();
    }
    /** @brief True once a frame containing @p bytes arrived before @p timeout. */
    bool wait_holds(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return holds_locked(bytes); });
    }
    /** @brief True if any frame received so far contains @p bytes. */
    bool holds(std::span<const std::byte> bytes) {
        const std::lock_guard lock(m);
        return holds_locked(bytes);
    }

   private:
    bool holds_locked(std::span<const std::byte> bytes) const {
        return std::ranges::any_of(frames, [&](const std::vector<std::byte>& f) {
            return !std::ranges::search(f, bytes).empty();
        });
    }
};

/** @brief An opaque `VALUE` TLV holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> raw(4);
    tr::detail::store_le<std::uint32_t>(raw, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

/** @brief `FWD{WRITE /sink:subscribers[], SUBSCRIBER{PATH /cli}}`: subscribe to `/sink`. */
std::vector<std::byte> b_subscribe() {
    std::vector<std::byte> field_body;
    tr::wire::emit_name(field_body, "subscribers");
    const std::byte element[1] = {std::byte{1}};  // index_mode = ELEMENT (append)
    tr::wire::emit_tlv(field_body, type_t::VALUE, opt_t{}, element);
    std::vector<std::byte> field;
    tr::wire::emit_tlv(field, type_t::FIELD, opt_t{.pl = true}, field_body);
    std::vector<std::byte> sub;
    tr::wire::emit_tlv(sub, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path({"cli"}));
    return b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_path({"cli"}), field, sub);
}

/** @brief `FWD{READ, dst=/other, src=/cli}`. */
std::vector<std::byte> b_read_other() {
    return b_fwd(fwd_op_t::READ, b_path({"other"}), b_path({"cli"}));
}

/** @brief Poll until @p pred holds or @p limit passes. */
template <class Pred>
bool eventually(Pred pred, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

/** @brief Open @p c and wait until a READ of `/other` is answered: the session is up. */
template <class Client>
bool answered(Client& c, frame_sink_t& rx) {
    return eventually(
        [&] {
            c.send(b_read_other());
            return rx.wait_holds(b_value_u32(2), 50ms);
        },
        3s);
}

/** @brief What the directed-send seam does once: A leaves and C takes A's slot. */
struct reuse_t {
    void (*leave)() = nullptr;     /**< @brief Drop A's client. */
    bool (*take_slot)() = nullptr; /**< @brief Connect C and wait until it holds slot 0. */
    bool fired = false;
    bool reused = false;
};
reuse_t g_reuse;

/** @brief The per-kind state the seam's plain-function callbacks reach. */
template <class Server, class Client>
struct kind_state_t {
    static inline std::unique_ptr<Client> a;
    static inline std::unique_ptr<Client> c;
    static inline frame_sink_t* rx_c = nullptr;
    static inline Server* server = nullptr;
    static inline std::uint16_t port = 0;
};

template <class Server, class Client, class Config>
void push_to_a_reused_slot(const char* kind) {
    std::printf("%s listener (peer-named): a slot reused between a push's resolve and its send:\n",
                kind);
    if constexpr (!tr::graph::kFaultInjection || !tr::net::kBusLinks) {
        std::printf("  SKIP: this build binds no fault-injection seams or no bus links\n");
        return;
    } else {
        using st = kind_state_t<Server, Client>;
        graph_t g;
        const vertex_handle_t sink =
            g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
        const vertex_handle_t other =
            g.register_vertex(*path_t::parse("/other"), role_t::STORED_VALUE);
        (void)g.write(sink, make_value(b_value_u32(1)));
        (void)g.write(other, make_value(b_value_u32(2)));
        fwd_router_t router(g);
        Server server(0, Config{.max_peers = 4, .peer_named = true});
        check(router.attach_link("cli", server), "  the listener is a routed child");
        frame_sink_t rx_a;
        frame_sink_t rx_c;
        st::server = &server;
        st::port = server.local_port();
        st::rx_c = &rx_c;
        st::a = std::make_unique<Client>("127.0.0.1", st::port);
        st::a->set_receiver(rx_a);
        (void)st::a->start_receiving();
        check(answered(*st::a, rx_a), "  session A is open and answered");
        st::a->send(b_subscribe());
        check(eventually(
                  [&] {
                      (void)g.write(sink, make_value(b_value_u32(5)));
                      return rx_a.wait_holds(b_value_u32(5), 50ms);
                  },
                  3s),
              "  A subscribed: a write pushes to A");

        g_reuse = reuse_t{
            .leave = [] { st::a.reset(); },
            .take_slot =
                [] {
                    st::c = std::make_unique<Client>("127.0.0.1", st::port);
                    st::c->set_receiver(*st::rx_c);
                    (void)st::c->start_receiving();
                    const tr::net::peer_handle_t next{
                        tr::wire::pair_t{.index = 0, .generation = 2}};
                    return answered(*st::c, *st::rx_c) && st::server->peer_link_of(next) != nullptr;
                },
        };
        tr::net::detail::directed_send_hook = [] {
            if (std::exchange(g_reuse.fired, true)) return;
            g_reuse.leave();                       // A leaves ...
            g_reuse.reused = g_reuse.take_slot();  // ... and C holds A's slot, next tenancy
        };
        const std::uint64_t dropped0 = server.drop_stats().dropped_tx;
        (void)g.write(sink, make_value(b_value_u32(9)));
        tr::net::detail::directed_send_hook = nullptr;
        check(g_reuse.fired && g_reuse.reused, "  C took A's slot while the push was in flight");
        std::this_thread::sleep_for(300ms);
        check(!rx_c.holds(b_value_u32(9)), "  and C never receives A's push");
        check(server.drop_stats().dropped_tx > dropped0, "  the push is dropped and counted");
        st::c.reset();
        st::a.reset();
        (void)router.remove_child("cli");
    }
}

}  // namespace

int main() {
    push_to_a_reused_slot<tr::net::tcp_server_transport_t, tr::net::tcp_transport_t,
                          tr::net::tcp_server_config_t>("tcp");
    push_to_a_reused_slot<tr::net::ws_server_transport_t, tr::net::ws_client_transport_t,
                          tr::net::ws_server_config_t>("ws");
    return tr::testing::summary("peer_push_tenancy");
}
