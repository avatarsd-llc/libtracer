/**
 * @file
 * @brief A reused session slot gets a fresh name, so a forwarded subscription made by the
 *        previous session is reaped rather than delivered to the new one.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Node A runs a REAL peer-named `tcp_server_transport_t`; node B is reached from A over an
 * in-memory pipe. A client session S1 dials A and subscribes to B's `/ctl/state` through A,
 * so B stores a return route that ends in S1's session name on A. S1 hangs up and S2 dials,
 * landing in the SAME slot. B still holds S1's route; B's next delivery must not reach S2.
 *
 * With the tenancy generation in the session name (`p<slot>~<gen>`), S2's name differs from
 * S1's, so A refuses B's delivery along the stale route and the existing refused-route
 * reclaim removes B's edge. The test pins both halves: S2 receives nothing, and B's edge is
 * gone. A control delivery to S1 first proves the route worked while S1 was live.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "graph_sinks.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/path.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_tcp.hpp"
#include "test_support.hpp"
#include "test_values.hpp"
#include "tlv_tree.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::testing::b_fwd;
using tr::testing::check;
using tr::testing::make_value;
using tr::wire::opt_t;
using tr::wire::type_t;

using bytes_t = std::vector<std::byte>;

/** @brief Poll @p f until it holds or the deadline expires; the final answer is returned. */
template <class Fn>
bool wait_until(Fn f, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return f();
}

/** @brief A PATH TLV of @p segs. */
bytes_t b_path(std::initializer_list<std::string_view> segs) {
    bytes_t body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A VALUE TLV of one byte. */
bytes_t b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief A VALUE TLV of a little-endian u32. */
bytes_t b_value_u32(std::uint32_t v) {
    bytes_t p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

/** @brief FIELD{ NAME "subscribers", VALUE index_mode=ELEMENT } — the `:subscribers[]` append. */
bytes_t b_field_subscribers_append() {
    bytes_t body;
    tr::wire::emit_name(body, "subscribers");
    const bytes_t mode = b_value_u8(1);
    body.insert(body.end(), mode.begin(), mode.end());
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief SUBSCRIBER{ PATH target } with default settings. */
bytes_t b_subscriber(const bytes_t& target) {
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, target);
    return out;
}

/** @brief One half of an in-memory A<->B pair: send() parks frames; the test pumps them. */
class pipe_link_t : public transport_t {
   public:
    void send(std::span<const std::byte> f) override {
        const std::lock_guard lock(m_);
        q_.emplace_back(f.begin(), f.end());
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        bytes_t flat;
        for (const auto& s : iov) flat.insert(flat.end(), s.begin(), s.end());
        const std::lock_guard lock(m_);
        q_.push_back(std::move(flat));
    }
    void inject(std::span<const std::byte> f) { rx_.deliver_borrowed(f); }
    std::vector<bytes_t> take() {
        const std::lock_guard lock(m_);
        return std::exchange(q_, {});
    }

   private:
    std::mutex m_;
    std::vector<bytes_t> q_;
};

/** @brief Move every parked frame across the pair once, both ways. */
void pump(pipe_link_t& a_to_b, pipe_link_t& b_to_a) {
    for (const bytes_t& f : a_to_b.take()) b_to_a.inject(f);
    for (const bytes_t& f : b_to_a.take()) a_to_b.inject(f);
}

/** @brief A client session of A's listener that counts FWD{WRITE} deliveries carrying a u32. */
struct client_t {
    explicit client_t(std::uint16_t port)
        : link(std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", port,
                                                          tr::net::tcp_config_t{})) {
        link->set_receiver(rx);
        link->start_receiving();
    }
    /** @brief How many deliveries of @p want this session received. */
    int deliveries(std::uint32_t want) {
        const std::lock_guard lock(m);
        int n = 0;
        for (const bytes_t& f : frames) {
            const auto d = tr::wire::decode(f);
            if (!d || d->type != type_t::FWD || d->children.empty()) continue;
            const auto& op = d->children[0];
            if (op.payload.empty() ||
                std::to_integer<int>(op.payload[0]) != static_cast<int>(fwd_op_t::WRITE))
                continue;
            const auto& last = d->children.back();
            if (last.type == type_t::VALUE && last.payload.size() == 4 &&
                tr::detail::load_le<std::uint32_t>(last.payload) == want)
                ++n;
        }
        return n;
    }
    std::mutex m;
    std::vector<bytes_t> frames;
    std::function<void(std::span<const std::byte>)> rx = [this](std::span<const std::byte> f) {
        const std::lock_guard lock(m);
        frames.emplace_back(f.begin(), f.end());
    };
    std::unique_ptr<tr::net::tcp_transport_t> link;
};

/** @brief The SLOT part of a session name — spelled out here so the test reads the same on
 *         a build whose names carry no generation. */
std::string_view slot_part(std::string_view name) { return name.substr(0, name.find('~')); }

/** @brief The listener's audible peer names. */
std::vector<std::string> peers_of(const tr::net::tcp_server_transport_t& s) {
    std::vector<std::string> out;
    s.enumerate_peers([&out](std::string_view n) { out.emplace_back(n); });
    return out;
}

void run() {
    graph_t ga;
    graph_t gb;
    fwd_router_t ra(ga);
    fwd_router_t rb(gb);
    pipe_link_t a_to_b;
    pipe_link_t b_to_a;
    check(ra.attach_link("b", a_to_b), "A mounts B as b");
    check(rb.attach_link("a", b_to_a), "B mounts A as a");
    tr::net::tcp_server_transport_t server(0, {.max_peers = 1, .peer_named = true});
    check(server.ok(), "A's listener bound an ephemeral port");
    check(ra.attach_link("srv", server), "A mounts its listener as srv");
    const auto v = gb.register_vertex(*path_t::parse("/ctl/state"), role_t::STORED_VALUE);
    const auto subscribers = [&] {
        const auto s = tr::testing::read_subscribers(gb, v);
        return s ? s->size() : std::size_t{0};
    };

    // ---- S1 subscribes to B's /ctl/state through A -----------------------------------------
    auto s1 = std::make_unique<client_t>(server.local_port());
    check(wait_until([&] { return peers_of(server).size() == 1; }), "S1 is audible on A");
    const std::string s1_name = peers_of(server).front();
    std::printf("  S1 session name: %s\n", s1_name.c_str());
    s1->link->send(b_fwd(fwd_op_t::WRITE, b_path({"b", "ctl", "state"}), b_path({"sink0"}),
                         b_field_subscribers_append(), b_subscriber(b_path({"sink0"}))));
    check(wait_until([&] {
              pump(a_to_b, b_to_a);
              return subscribers() == 1;
          }),
          "B holds one subscriber edge for S1");

    // Control: the route works while S1 is live.
    (void)gb.write(v, make_value(b_value_u32(0x1111)));
    check(wait_until([&] {
              pump(a_to_b, b_to_a);
              return s1->deliveries(0x1111) == 1;
          }),
          "control: the live S1 receives its delivery");

    // ---- S1 departs; S2 takes the same slot -------------------------------------------------
    s1.reset();
    check(wait_until([&] { return peers_of(server).empty(); }), "S1 departed A");
    auto s2 = std::make_unique<client_t>(server.local_port());
    check(wait_until([&] { return peers_of(server).size() == 1; }), "S2 is audible on A");
    const std::string s2_name = peers_of(server).front();
    std::printf("  S2 session name: %s\n", s2_name.c_str());
    check(slot_part(s2_name) == slot_part(s1_name), "S2 reused S1's slot");
    check(s2_name != s1_name, "the reused slot carries a FRESH session name");
    check(subscribers() == 1, "B still holds S1's edge: nothing told B about the departure");

    // ---- B delivers along S1's stale route --------------------------------------------------
    (void)gb.write(v, make_value(b_value_u32(0x2222)));
    check(wait_until([&] {
              pump(a_to_b, b_to_a);
              return subscribers() == 0;
          }),
          "A refused the stale route and B's edge was reaped");
    (void)gb.write(v, make_value(b_value_u32(0x3333)));
    for (int i = 0; i < 8; ++i) pump(a_to_b, b_to_a);
    std::this_thread::sleep_for(50ms);  // let any stray frame cross the socket
    check(s2->deliveries(0x2222) == 0, "the new occupant receives none of S1's deliveries");
    check(s2->deliveries(0x3333) == 0, "…and none after the edge is gone");
}

}  // namespace

int main() {
    // The session names under test are a peer-named listener's; a build with no bus module
    // has no such listener. Skip, do not crash.
    if constexpr (!tr::net::kBusLinks)
        return tr::testing::skipped("slot_reuse_reaps_forwarded_sub",
                                    "this build closed the bus module out (kBusLinks = false); "
                                    "no listener names its sessions");
    std::printf("a reused session slot gets a fresh name; a stale forwarded route is reaped:\n");
    run();
    return tr::testing::summary("slot_reuse_reaps_forwarded_sub");
}
