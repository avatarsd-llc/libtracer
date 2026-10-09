/**
 * @file
 * @brief #1645 — a node originates a forwarded READ, WRITE or append through
 *        `fwd_router_t::originate`, with no wire bytes built by the caller and no fake
 *        inbound link.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two nodes, A and B, joined by an in-memory link whose frames queue until the test pumps
 * them, so a reply arrives AFTER `originate` returns — the asynchronous shape every real
 * transport has. A mounts the link as `/net/mem/b`, B as `/net/mem/a`.
 *
 * What is pinned:
 *   - READ, WRITE and a `:subscribers[]` append each reach B and pair their reply to the
 *     caller's record, never to the router-wide `on_reply` sink;
 *   - B sees `src` = the origin's generated route and nothing else: the origin is not a hop,
 *     so no inbound mount is prepended and no made-up child name rides the frame;
 *   - two outstanding requests answered in REVERSE order each get their own answer;
 *   - `cancel` (the caller's deadline) disarms the record, and the late reply falls to the
 *     `on_reply` sink instead;
 *   - a local `dst` is answered synchronously into the record;
 *   - the argument refusals.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_path;
using tr::testing::check;
using tr::testing::make_value;

/** @brief An opaque `VALUE` TLV holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> raw(4);
    tr::detail::store_le<std::uint32_t>(raw, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

/** @brief `SUBSCRIBER{ PATH target }` — the minimal wire subscriber. */
std::vector<std::byte> b_subscriber(const std::vector<std::byte>& target) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, target);
    return out;
}

/** @brief A link that QUEUES every frame sent over it until the test pumps it. */
class queue_link_t : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        q_.emplace_back(frame.begin(), frame.end());
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::vector<std::byte> flat;
        for (const auto& s : iov) flat.insert(flat.end(), s.begin(), s.end());
        q_.push_back(std::move(flat));
    }
    std::deque<std::vector<std::byte>> q_; /**< @brief Frames sent and not yet delivered. */
};

/** @brief What a record's sink saw: how many replies, and the last one's bytes and tree. */
struct got_t {
    int calls = 0;
    /** @brief The reply's bytes — owned here, since the decoded tree borrows from them. */
    std::vector<std::byte> bytes;
    std::optional<tr::wire::tlv_t> reply; /**< @brief Decoded over @ref bytes. */
};

/** @brief The record sink: copy the reply out of the rope, then decode the copy. */
void on_got(void* ctx, const tr::view::rope_t& reply) {
    auto* g = static_cast<got_t*>(ctx);
    ++g->calls;
    const tr::view::view_t m = reply.materialize();
    g->bytes.assign(m.bytes().begin(), m.bytes().end());
    g->reply.reset();
    if (auto d = tr::wire::decode(g->bytes)) g->reply = std::move(*d);
}

/** @brief The reply's `kind` (child 3), or 0xFF when there is none. */
std::uint8_t kind_of(const got_t& g) {
    if (!g.reply || g.reply->children.size() < 4) return 0xFF;
    return tr::detail::load_le<std::uint8_t>(g.reply->children[3].payload);
}

/** @brief The reply's `u32` result payload (child 4), if it carries one. */
std::optional<std::uint32_t> u32_of(const got_t& g) {
    if (!g.reply || g.reply->children.size() < 5) return std::nullopt;
    const auto& p = g.reply->children[4];
    if (p.type != type_t::VALUE || p.payload.size() != 4) return std::nullopt;
    return tr::detail::load_le<std::uint32_t>(p.payload);
}

/** @brief The `u32` a vertex holds, if any. */
std::optional<std::uint32_t> stored_u32(const graph_t& g, vertex_handle_t v) {
    const auto ref = g.read(v);
    if (!ref || !*ref || (*ref)->link_count() != 1) return std::nullopt;
    const auto tlv = tr::wire::decode((*ref)->only());
    if (!tlv || tlv->payload.size() != 4) return std::nullopt;
    return tr::detail::load_le<std::uint32_t>(tlv->payload);
}

/** @brief A and B, each with one stored vertex, joined by two queues. */
struct pair_t {
    graph_t ga;
    graph_t gb;
    fwd_router_t a{ga};
    fwd_router_t b{gb};
    queue_link_t a_to_b;  // A's child: what A sends toward B
    queue_link_t b_to_a;  // B's child: what B sends toward A
    vertex_handle_t temp = gb.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    vertex_handle_t hum = gb.register_vertex(*path_t::parse("/sensor/hum"), role_t::STORED_VALUE);
    vertex_handle_t probe = ga.register_vertex(*path_t::parse("/probe"), role_t::STORED_VALUE);
    int stray = 0;              /**< @brief Replies that reached A's `on_reply` sink. */
    std::vector<std::byte> src; /**< @brief The `src` B saw on the last request. */

    pair_t() {
        (void)a.attach_link("net/mem/b", a_to_b);
        (void)b.attach_link("net/mem/a", b_to_a);
        (void)gb.write(temp, make_value(b_value_u32(21)));
        (void)gb.write(hum, make_value(b_value_u32(55)));
        a.on_reply([](void* c, const tr::view::rope_t&) { ++static_cast<pair_t*>(c)->stray; },
                   this);
        b.on_inbound(
            [](void* c, std::string_view, const tr::wire::tlv_node_t& fwd) {
                std::size_t i = 0;
                for (const tr::wire::tlv_node_t n : fwd.children())
                    if (i++ == 2) {
                        static_cast<pair_t*>(c)->src.assign(n.wire().begin(), n.wire().end());
                        return;
                    }
            },
            this);
    }
    /** @brief Deliver A's queued frames to B. */
    void to_b() {
        while (!a_to_b.q_.empty()) {
            auto f = std::move(a_to_b.q_.front());
            a_to_b.q_.pop_front();
            b.on_frame("net/mem/a", f);
        }
    }
    /** @brief Deliver B's queued frames to A, newest first when @p reversed. */
    void to_a(bool reversed = false) {
        while (!b_to_a.q_.empty()) {
            std::vector<std::byte> f;
            if (reversed) {
                f = std::move(b_to_a.q_.back());
                b_to_a.q_.pop_back();
            } else {
                f = std::move(b_to_a.q_.front());
                b_to_a.q_.pop_front();
            }
            a.on_frame("net/mem/b", f);
        }
    }
};

void remote_read() {
    std::printf("READ: reply paired to the record, src carries no made-up child\n");
    pair_t n;
    got_t got;
    fwd_router_t::origin_t slot{&on_got, &got};
    const path_t dst("/net/mem/b/sensor/temp");
    check(n.a.originate(slot, fwd_op_t::READ, dst).has_value(), "  originate accepted");
    check(slot.armed(), "  record armed while the request is outstanding");
    n.to_b();
    // B's src is exactly the origin's ONE generated record: the origin is no hop, so no
    // inbound mount was prepended and no made-up child name rides the frame.
    const auto src = tr::wire::decode(n.src);
    check(src && src->type == type_t::PATH && src->payload.size() == 11 &&
              src->payload[0] == std::byte{10} && src->payload[1] == std::byte{'~'} &&
              src->payload[2] == std::byte{'o'},
          "  src = the generated one-record `~o…` route, nothing else");
    n.to_a();
    check(got.calls == 1, "  record sink ran once");
    check(kind_of(got) == static_cast<std::uint8_t>(reply_kind_t::RESULT), "  kind = RESULT");
    check(u32_of(got) == 21u, "  the stored value came back");
    check(!slot.armed(), "  record disarmed by its reply");
    check(n.stray == 0, "  on_reply sink untouched");
}

void remote_write() {
    std::printf("WRITE: applied at B, acknowledged into the record\n");
    pair_t n;
    got_t got;
    fwd_router_t::origin_t slot{&on_got, &got};
    const auto v = b_value_u32(99);
    check(n.a.originate(slot, fwd_op_t::WRITE, path_t("/net/mem/b/sensor/temp"), v).has_value(),
          "  originate accepted");
    n.to_b();
    n.to_a();
    check(got.calls == 1 && kind_of(got) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
          "  RESULT into the record");
    check(stored_u32(n.gb, n.temp) == 99u, "  B stored the value");
    check(n.stray == 0, "  on_reply sink untouched");
}

void remote_append() {
    std::printf("append (`:subscribers[]`): binds at B, deliveries reach reply_to\n");
    pair_t n;
    got_t got;
    fwd_router_t::origin_t slot{&on_got, &got};
    const path_t reply_to("/probe");
    const auto sub = b_subscriber(b_path({"probe"}));
    check(n.a.originate(slot, fwd_op_t::WRITE, path_t("/net/mem/b/sensor/temp:subscribers[]"), sub,
                        &reply_to)
              .has_value(),
          "  originate accepted");
    n.to_b();
    n.to_a();
    check(got.calls == 1 && kind_of(got) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
          "  RESULT into the record");
    check(n.stray == 0, "  on_reply sink untouched");
    // The subscription is live: a write at B is delivered home to A's /probe.
    (void)n.gb.write(n.temp, make_value(b_value_u32(31)));
    n.to_a();
    check(stored_u32(n.ga, n.probe) == 31u, "  a write at B lands on A's /probe");
}

void reordered_replies() {
    std::printf("two outstanding READs answered in reverse order\n");
    pair_t n;
    got_t g1;
    got_t g2;
    fwd_router_t::origin_t s1{&on_got, &g1};
    fwd_router_t::origin_t s2{&on_got, &g2};
    check(n.a.originate(s1, fwd_op_t::READ, path_t("/net/mem/b/sensor/temp")).has_value() &&
              n.a.originate(s2, fwd_op_t::READ, path_t("/net/mem/b/sensor/hum")).has_value(),
          "  both accepted");
    n.to_b();
    n.to_a(/*reversed=*/true);
    check(g1.calls == 1 && u32_of(g1) == 21u, "  first record got temp");
    check(g2.calls == 1 && u32_of(g2) == 55u, "  second record got hum");
}

void cancelled() {
    std::printf("cancel (the caller's deadline): the late reply goes to on_reply\n");
    pair_t n;
    got_t got;
    fwd_router_t::origin_t slot{&on_got, &got};
    check(n.a.originate(slot, fwd_op_t::READ, path_t("/net/mem/b/sensor/temp")).has_value(),
          "  originate accepted");
    check(n.a.cancel(slot), "  cancel disarmed an armed record");
    check(!n.a.cancel(slot), "  a second cancel finds nothing");
    n.to_b();
    n.to_a();
    check(got.calls == 0, "  record sink did not run");
    check(n.stray == 1, "  the reply fell to on_reply");
    {
        got_t g2;
        fwd_router_t::origin_t scoped{&on_got, &g2};
        check(n.a.originate(scoped, fwd_op_t::READ, path_t("/net/mem/b/sensor/temp")).has_value(),
              "  scoped originate accepted");
    }  // destroyed armed ⇒ cancelled
    n.to_b();
    n.to_a();
    check(n.stray == 2, "  a destroyed record's reply fell to on_reply");
}

void local_terminus() {
    std::printf("a local dst is answered synchronously into the record\n");
    pair_t n;
    (void)n.ga.write(n.probe, make_value(b_value_u32(7)));
    got_t got;
    fwd_router_t::origin_t slot{&on_got, &got};
    check(n.a.originate(slot, fwd_op_t::READ, path_t("/probe")).has_value(), "  accepted");
    check(got.calls == 1 && u32_of(got) == 7u, "  answered before originate returned");
    check(!slot.armed() && n.a_to_b.q_.empty(), "  disarmed, nothing on the wire");
}

void refusals() {
    std::printf("refusals\n");
    pair_t n;
    got_t got;
    fwd_router_t::origin_t slot{&on_got, &got};
    const path_t dst("/net/mem/b/sensor/temp");
    const auto v = b_value_u32(1);
    check(n.a.originate(slot, fwd_op_t::AWAIT, dst).error() == status_t::TYPE_MISMATCH,
          "  AWAIT is not originated here");
    check(n.a.originate(slot, fwd_op_t::READ, dst, v).error() == status_t::TYPE_MISMATCH,
          "  READ with a payload");
    check(n.a.originate(slot, fwd_op_t::WRITE, dst).error() == status_t::TYPE_MISMATCH,
          "  WRITE without a payload");
    const path_t via_mount("/net/mem/b/x");
    check(
        n.a.originate(slot, fwd_op_t::READ, dst, {}, &via_mount).error() == status_t::INVALID_PATH,
        "  reply_to through a mount");
    check(!slot.armed() && n.a_to_b.q_.empty(), "  nothing armed, nothing sent");
    check(n.a.originate(slot, fwd_op_t::READ, dst).has_value(), "  first originate");
    check(n.a.originate(slot, fwd_op_t::READ, dst).error() == status_t::BACKPRESSURE,
          "  an armed record is refused");
    check(n.a_to_b.q_.size() == 1, "  only the first went out");
}

}  // namespace

int main() {
    std::printf("#1645 — fwd_router_t::originate\n\n");
    remote_read();
    remote_write();
    remote_append();
    reordered_replies();
    cancelled();
    local_terminus();
    refusals();
    return tr::testing::summary("fwd_originate");
}
