/**
 * @file
 * @brief RFC-0004 Amendment 2 (#1502, ruling on #1491) — a zero-length `src` is
 *        "no reply requested": the terminus applies the WRITE and stays SILENT.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A reply is something the origin REQUESTS by wiring a return route; it is not something the
 * library imposes. The `src` PATH of a `FWD` is simultaneously the return route and the
 * acknowledgement request, so an EMPTY `src` is the request not made. #1491 measured what
 * imposing it costs: a bulk WS ingest spent ~60% of its per-batch time on `FWD{REPLY}`
 * frames the producer discards, and its throughput knee sat exactly at the async TX pool
 * depth the REPLIES travel through.
 *
 * The amendment's six conformance vectors are the six cases below, each asserted as
 * **zero frames on the wire** rather than as "no reply was parsed": a frame emitted to a
 * zero-length route is unroutable garbage, and counting sends is the only assertion that
 * catches it. Every silent case is paired with a **positive control** on the same router —
 * the identical frame with a one-segment `src`, which must still draw exactly one reply.
 * Without the control, a router that had simply stopped working would pass every vector.
 *
 * @section why_empty Why EMPTY and not OMITTED
 *
 * `parse_fwd` reads the FWD child run POSITIONALLY (RFC-0004 §B child order), and a WRITE's
 * payload may itself be `PATH`-typed (RFC-0024 §7.1 amendment 2 gave that shape back). An
 * omitted `src` and a `PATH` payload are therefore the same byte sequence, so omission
 * cannot be the encoding. The empty child leaves the grammar closed and every existing
 * parser reading the frame it always read.
 *
 * @section f Vector (f) — the delivery that stops answering itself
 *
 * A full-route fan-out delivery is already emitted as
 * `FWD{WRITE, dst=<return route>, src=<empty PATH>}` (`fwd_router.cpp`, the default arm).
 * Before this amendment its receiver assembled a `FWD{REPLY}` addressed to an empty path —
 * the §B.1 defect #1491 documented. Vector (f) drives a REAL producer fan-out into a REAL
 * consumer node and asserts the consumer applies the value and emits nothing: the fix,
 * receiver-side, with **no change to the emitter**.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
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
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_fwd_mint;
using tr::testing::b_path;
using tr::testing::check;
using tr::testing::make_value;

/** @brief A subject token / ACE subject over @p s — the tests-only string→bytes adapter. */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The zero-length `PATH` — the amendment's marker, spelled once. */
std::vector<std::byte> b_src_empty() { return b_path(std::initializer_list<std::string_view>{}); }

/** @brief A one-segment `src` — the ordinary "reply to me" route, for every control. */
std::vector<std::byte> b_src_wired() { return b_path({"cli"}); }

/** @brief An opaque `VALUE` TLV holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> raw(4);
    tr::detail::store_le<std::uint32_t>(raw, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

/** @brief `FIELD{ NAME "subscribers", VALUE u8 index_mode=ELEMENT }` — the `:subscribers[]`
 *         append selector (RFC-0004 §C). */
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "subscribers");
    const std::byte mode{1};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&mode, 1));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief `SUBSCRIBER{ PATH target }` — the minimal wire subscriber (no QoS child). */
std::vector<std::byte> b_subscriber(const std::vector<std::byte>& target) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, target);
    return out;
}

/**
 * @brief A transport that COUNTS and keeps every frame the router sends it.
 *
 * Counting is the assertion: the amendment's claim is "zero frames emitted", and a reply
 * addressed to a zero-length route would decode as a well-formed `FWD{REPLY}` — inspecting
 * what came back could never distinguish it from silence.
 */
class rec_link_t : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        const std::lock_guard lock(m_);
        sent_.emplace_back(frame.begin(), frame.end());
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        const std::lock_guard lock(m_);
        std::vector<std::byte> flat;
        for (const auto& s : iov) flat.insert(flat.end(), s.begin(), s.end());
        sent_.push_back(std::move(flat));
    }
    /** @brief Take every frame captured since the last drain. */
    std::vector<std::vector<std::byte>> drain() {
        const std::lock_guard lock(m_);
        return std::exchange(sent_, {});
    }
    std::size_t count() {
        const std::lock_guard lock(m_);
        return sent_.size();
    }

   private:
    std::mutex m_;
    std::vector<std::vector<std::byte>> sent_;
};

/** @brief The `u32` a vertex currently holds, or `nullopt` if it holds nothing usable. */
std::optional<std::uint32_t> stored_u32(const graph_t& g, vertex_handle_t v) {
    const auto ref = g.read(v);  // the trusted local context — never gated
    if (!ref || !*ref) return std::nullopt;
    if ((*ref)->link_count() != 1) return std::nullopt;
    const auto tlv = tr::wire::decode((*ref)->only());
    if (!tlv || tlv->payload.size() != 4) return std::nullopt;
    return tr::detail::load_le<std::uint32_t>(tlv->payload);
}

/** @brief The test subject resolver (ADR-0018): the caller context IS the subject token. */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
}

/** @brief An `ACL` TLV granting @p right to @p subject alone (ALLOW-only, ADR-0020). */
std::vector<std::byte> acl_allowing(std::string_view subject, acl_right_t right) {
    const std::vector<tr::graph::ace_t> aces{
        tr::graph::ace_t{.type = tr::graph::ace_type_t::ALLOW,
                         .flags = 0,
                         .subject = as_bytes(subject),
                         .access_mask = static_cast<std::uint32_t>(right),
                         .expires_ns = 0}};
    return tr::graph::encode_acl(aces);
}

constexpr std::uint32_t kSeed = 0x11111111u;
constexpr std::uint32_t kQuiet = 0x22222222u;
constexpr std::uint32_t kLoud = 0x33333333u;

/** @brief A node under test: a graph with one `/sink` vertex behind a recording link. */
struct node_t {
    graph_t graph;
    fwd_router_t router{graph};
    rec_link_t link;
    vertex_handle_t sink;

    node_t() : sink(graph.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE)) {
        (void)router.add_child("cli", link);
        (void)graph.write(sink, make_value(b_value_u32(kSeed)));
        (void)link.drain();
    }
};

/**
 * @brief Vector (a) — an empty-src WRITE to a writable target is APPLIED and answers nothing.
 */
void vector_a_write_applied_silently() {
    std::printf("(a) empty-src WRITE, writable target:\n");
    node_t n;
    n.router.on_frame(
        "cli", b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_src_empty(), {}, b_value_u32(kQuiet)));
    check(stored_u32(n.graph, n.sink) == kQuiet, "  the write was APPLIED");
    check(n.link.count() == 0, "  ZERO frames emitted (no FWD{REPLY})");

    // Control: the identical write with a wired `src` still draws exactly one reply, so the
    // silence above is the amendment and not a router that has stopped answering.
    n.router.on_frame(
        "cli", b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_src_wired(), {}, b_value_u32(kLoud)));
    check(stored_u32(n.graph, n.sink) == kLoud, "  control: the wired-src write applied too");
    check(n.link.count() == 1, "  control: a wired src DOES draw exactly one reply");
}

/**
 * @brief Vector (b) — an ACL-denied empty-src WRITE is NOT applied and is dropped SILENTLY.
 *
 * COMPACT parity (#974's ruling, reference/05 §route-handle): *"a denied delivery is dropped
 * like any other unwritable one"*. No new drop policy is invented here — the existing one is
 * the one that applies, and the anti-enumeration property is preserved for free: a peer that
 * asked for no answer learns nothing about what it may not write.
 */
void vector_b_denied_write_dropped_silently() {
    std::printf("(b) empty-src WRITE, ACL-denied target:\n");
    node_t n;
    const auto acl_path = path_t::parse("/sink:acl");
    check(n.graph.write(*acl_path, make_value(acl_allowing("peer-z", acl_right_t::WRITE)))
              .has_value(),
          "  an ACL granting WRITE to `peer-z` ALONE is installed");
    n.graph.configure_subject_resolver(caller_is_subject, nullptr);  // enforcement on
    (void)n.link.drain();

    n.router.on_frame(
        "cli", b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_src_empty(), {}, b_value_u32(kQuiet)));
    check(stored_u32(n.graph, n.sink) == kSeed, "  the write was NOT applied (ACL evaluated)");
    check(n.link.count() == 0, "  ZERO frames emitted (no addressed ERROR to an empty route)");

    // Control: the same denial WITH a return route is still ANSWERED — the amendment removes
    // the reply, never the enforcement, and a wired origin still learns it was refused.
    n.router.on_frame(
        "cli", b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_src_wired(), {}, b_value_u32(kQuiet)));
    check(stored_u32(n.graph, n.sink) == kSeed, "  control: still not applied");
    check(n.link.count() == 1, "  control: a wired src DOES get the denial reply");
}

/**
 * @brief Vectors (c) and (d) — an empty-src READ / AWAIT is MALFORMED and dropped.
 *
 * The result has nowhere to go, so the frame asks for an answer and refuses to receive one.
 * Dropped at the TERMINUS and not NACKed, for the reason the whole clause exists: there is
 * no route to carry a NACK. Forwarders stay opcode-agnostic — the check sits where `op` is
 * already switched.
 */
void vectors_c_d_read_await_dropped() {
    std::printf("(c)/(d) empty-src READ and AWAIT:\n");
    node_t n;
    n.router.on_frame("cli", b_fwd(fwd_op_t::READ, b_path({"sink"}), b_src_empty()));
    check(n.link.count() == 0, "  (c) empty-src READ: ZERO frames emitted");
    n.router.on_frame("cli", b_fwd(fwd_op_t::AWAIT, b_path({"sink"}), b_src_empty()));
    check(n.link.count() == 0, "  (d) empty-src AWAIT: ZERO frames emitted");

    // Control: a wired READ answers. AWAIT is deliberately not controlled here — it blocks
    // for its default timeout, which a READ on the same resolver already rules out.
    n.router.on_frame("cli", b_fwd(fwd_op_t::READ, b_path({"sink"}), b_src_wired()));
    check(n.link.count() == 1, "  control: a wired-src READ DOES answer");
}

/**
 * @brief Vector (e) — an empty-src MINT-FLAGGED WRITE is malformed and dropped.
 *
 * A mint answer is a `PATH_REF` that rides the REPLY alone (RFC-0024 §7.5): asking for a
 * bound path over a route that does not exist is a contradiction on one frame. It is dropped
 * rather than downgraded to a plain silent write, because silently ignoring the flag would
 * leave the origin waiting for a handle it will never be told it cannot have.
 */
void vector_e_mint_flagged_dropped() {
    std::printf("(e) empty-src MINT-FLAGGED WRITE:\n");
    node_t n;
    n.router.on_frame("cli", b_fwd_mint(fwd_op_t::WRITE, b_path({"sink"}), b_src_empty(), {},
                                        b_value_u32(kQuiet)));
    check(n.link.count() == 0, "  ZERO frames emitted");
    check(stored_u32(n.graph, n.sink) == kSeed, "  and the write was NOT applied (malformed)");

    n.router.on_frame("cli", b_fwd_mint(fwd_op_t::WRITE, b_path({"sink"}), b_src_wired(), {},
                                        b_value_u32(kLoud)));
    check(n.link.count() == 1, "  control: a wired-src mint request DOES answer");
    check(stored_u32(n.graph, n.sink) == kLoud, "  control: and applies");
}

/**
 * @brief Vector (f) — a full-route SUBSCRIPTION DELIVERY is applied at the consumer and
 *        draws no reply. The §B.1 defect, fixed receiver-side.
 *
 * Two real nodes: the producer binds a remote subscriber over its link and fans out on a
 * write; the frame it emits — unchanged by this amendment — is fed to the consumer's router,
 * whose link must stay silent. The producer's own emitter is asserted to still put
 * `src=<empty PATH>` on the wire, because the amendment's correctness depends on it.
 */
void vector_f_delivery_draws_no_reply() {
    std::printf("(f) subscription full-route delivery at the consumer:\n");
    // --- the producer -------------------------------------------------------------
    graph_t prod;
    fwd_router_t prod_router(prod);
    rec_link_t prod_link;
    (void)prod_router.add_child("consumer", prod_link);
    const vertex_handle_t src_v =
        prod.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    prod_router.on_frame("consumer",
                         b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"sink"}),
                               b_field_subscribers_append(), b_subscriber(b_path({"sink"}))));
    (void)prod_link.drain();  // discard the subscribe REPLY (its src was wired)

    check(prod.write(src_v, make_value(b_value_u32(kQuiet))).has_value(), "  producer writes");
    const auto delivered = prod_link.drain();
    check(delivered.size() == 1, "  exactly one delivery frame fanned out");
    if (delivered.size() != 1) return;

    // The emitter is UNCHANGED and must stay so: `src` is the zero-length PATH.
    const auto dec = tr::wire::decode(delivered[0]);
    bool empty_src_on_the_wire = false;
    if (dec && dec->type == type_t::FWD) {
        for (const tr::wire::tlv_t& c : dec->children)
            if (c.type == type_t::PATH && c.payload.empty()) empty_src_on_the_wire = true;
    }
    check(empty_src_on_the_wire, "  the delivery carries src=<empty PATH> (emitter unchanged)");

    // --- the consumer -------------------------------------------------------------
    node_t con;
    con.router.on_frame("cli", delivered[0]);
    check(stored_u32(con.graph, con.sink) == kQuiet, "  the consumer APPLIED the delivered value");
    check(con.link.count() == 0, "  and emitted ZERO frames (no FWD{REPLY, dst=<empty>})");
}

/**
 * @brief Derived clause — an empty-src remote SUBSCRIBE is malformed and dropped.
 *
 * Not one of the six lettered vectors, and asserted because the six leave it open: a
 * subscribe is a STANDING request for future frames, so the edge an empty-src subscribe
 * would bind carries the empty route as its `target` and every delivery down it would be the
 * unroutable `FWD{WRITE, dst=<empty>}` this amendment exists to stop — emitted forever
 * instead of once. It takes the same terminus drop as an empty-src READ.
 */
void derived_empty_src_subscribe_dropped() {
    std::printf("(derived) empty-src remote SUBSCRIBE:\n");
    node_t n;
    n.router.on_frame("cli", b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_src_empty(),
                                   b_field_subscribers_append(), b_subscriber(b_src_empty())));
    check(n.link.count() == 0, "  ZERO frames emitted");
    // No edge was bound: a later producer write fans out to nobody.
    check(n.graph.write(n.sink, make_value(b_value_u32(kQuiet))).has_value(), "  producer writes");
    check(n.link.count() == 0, "  and NO delivery follows — no subscription was bound");
}

/**
 * @brief An empty-src WRITE to an ADDRESS THAT DOES NOT RESOLVE is dropped silently.
 *
 * The `NOT_FOUND` arm is a different reply site from the ACL one, reached before `apply_op`
 * runs at all, and a mutation that silenced only the arm the vectors exercise would leave
 * this one emitting to an empty route.
 */
void unknown_dst_dropped_silently() {
    std::printf("(extra) empty-src WRITE to an unresolvable dst:\n");
    node_t n;
    n.router.on_frame(
        "cli", b_fwd(fwd_op_t::WRITE, b_path({"nowhere"}), b_src_empty(), {}, b_value_u32(kQuiet)));
    check(n.link.count() == 0, "  ZERO frames emitted (no addressed NOT_FOUND)");
    n.router.on_frame(
        "cli", b_fwd(fwd_op_t::WRITE, b_path({"nowhere"}), b_src_wired(), {}, b_value_u32(kQuiet)));
    check(n.link.count() == 1, "  control: a wired src DOES get the NOT_FOUND reply");
}

/**
 * @brief A payload-less empty-src WRITE — the `TYPE_MISMATCH` arm — is dropped silently too.
 */
void payloadless_write_dropped_silently() {
    std::printf("(extra) empty-src WRITE with no payload:\n");
    node_t n;
    n.router.on_frame("cli", b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_src_empty()));
    check(n.link.count() == 0, "  ZERO frames emitted (no addressed TYPE_MISMATCH)");
    n.router.on_frame("cli", b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_src_wired()));
    check(n.link.count() == 1, "  control: a wired src DOES get the TYPE_MISMATCH reply");
}

}  // namespace

int main() {
    std::printf("RFC-0004 Amendment 2 — empty src = unacknowledged (#1502)\n\n");
    vector_a_write_applied_silently();
    vector_b_denied_write_dropped_silently();
    vectors_c_d_read_await_dropped();
    vector_e_mint_flagged_dropped();
    vector_f_delivery_draws_no_reply();
    derived_empty_src_subscribe_dropped();
    unknown_dst_dropped_silently();
    payloadless_write_dropped_silently();
    return tr::testing::summary("empty_src_unacked");
}
