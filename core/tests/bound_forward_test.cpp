/**
 * @file
 * @brief RFC-0024 car 3 — the bound-path FORWARDER hop and the origin-side bind, end to end.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `bound_path_test.cpp` owns what a `PATH_REF` means at a TERMINUS, against a bare resolver.
 * This owns the two halves that need a network to exist at all:
 *
 * ```
 *   client ──(net/uplink/a)──▶ A ──(A: net/uplink/b)──▶ B  [/sensor/temp]
 * ```
 *
 * - the **chain is learned on the way back**, with no flag (RFC-0029 §6.2, #2245): B seeds the
 *   reply's `src` with its PAIR for the target, A prepends its own as it relays the reply
 *   ("never nothing": its NAME run when it holds no PAIR), and the origin prepends the one
 *   only it can know — its reference to its first-hop connection vertex (§4.1);
 * - the **forwarder hop** (§3.4/§5): a bound `dst` with a residual longer than one
 *   element consumes ITS element — bounds, generation, ACL at the dereferenced vertex — and
 *   forwards the remainder, growing `src` exactly as the canonical spelling does. The route
 *   composition is asserted BYTE-IDENTICAL against the canonical run, because a return route
 *   that differed by one byte would still route and would no longer be the same protocol.
 *
 * Every refusal is shown non-vacuous by ablation: the same frame, one field sound, lands.
 *
 * Production wiring throughout (the RFC-0014 lesson): every link is bound through the in-band
 * creator endpoint `write /net/<module>/conn <- SPEC{…}`, so each connection vertex — the thing
 * a forwarder's element names — is created by `transport_vertex.cpp` and never hand-spelled.
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "graph_sinks.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/loopback.hpp"
#include "libtracer/path_pair.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "pair_body.hpp"
#include "test_support.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

namespace {

using namespace std::chrono_literals;
using tr::graph::acl_right_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;

/** @brief What `bound_send` handed its callback, copied out for the test's checks. The links
 *         here are the test's own and outlive every check, so keeping one is safe. */
struct dispatched_t {
    tr::net::transport_t* link = nullptr;
    std::vector<std::byte> dst;
};

/** @brief The callback a test hands a bracketed send: copy the link and the `dst` out. */
void keep_dispatch(void* ctx, tr::net::transport_t& link, std::span<const std::byte> dst) {
    auto& out = *static_cast<dispatched_t*>(ctx);
    out.link = &link;
    out.dst.assign(dst.begin(), dst.end());
}

/** @brief `bound_send` through @ref keep_dispatch: the dispatch, or nullopt on a refusal. */
std::optional<dispatched_t> bound_dispatch(const fwd_router_t& r, const path_t& path,
                                           acl_right_t right) {
    dispatched_t out;
    if (!r.bound_send(path, right, &keep_dispatch, &out)) return std::nullopt;
    return out;
}
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::path_ref_element_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::mailbox_t;

/** @brief A heap-owned view over @p bytes (the graph stores owning views). */
view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief Canonical `PATH{ NAME… }` bytes for @p segs. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief Canonical `VALUE` bytes holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

using tr::testing::b_fwd_raw_op;

/**
 * @brief The bound spelling of an address over @p elements: a `PATH` of PAIR elements
 *        (RFC-0029 §4), head first. The `PATH_REF` (`0x14`) spelling is no longer an address.
 */
std::vector<std::byte> b_path_ref(std::span<const path_ref_element_t> elements) {
    std::vector<std::byte> body;
    for (const path_ref_element_t& e : elements) tr::testing::emit_path_pair(body, e);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A `PATH` over an already-spelled element @p body (a learned chain's tail). */
std::vector<std::byte> b_path_body(std::span<const std::byte> body) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/**
 * @brief A relayed `FWD{REPLY, kind=RESULT}` toward `/cli/reply-ep` whose `src` is @p src_body:
 *        what a hop receives back over the link its request left on.
 */
std::vector<std::byte> b_relayed_reply(std::span<const std::byte> src_body) {
    std::vector<std::byte> body;
    const std::byte op_reply{static_cast<std::uint8_t>(fwd_op_t::REPLY)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op_reply, 1));
    const std::vector<std::byte> rdst = b_path({"cli", "reply-ep"});
    const std::vector<std::byte> rsrc = b_path_body(src_body);
    body.insert(body.end(), rdst.begin(), rdst.end());
    body.insert(body.end(), rsrc.begin(), rsrc.end());
    const std::byte kind{static_cast<std::uint8_t>(reply_kind_t::RESULT)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&kind, 1));
    const std::vector<std::byte> payload = b_value_u32(5);
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/** @brief A connection-creation SPEC with no transport `kind` (every link is provided). */
view_t conn_spec(std::string_view name) { return tr::net::conn_spec_t(name).view(); }

/** @brief The creator endpoint of @p module — the one wire door that creates a connection. */
path_t conn_endpoint(std::string_view module) {
    std::string text = "/net/";
    text += module;
    text += "/conn";
    return path_t(text);
}

/**
 * @brief Every FWD a node saw inbound on one named link — COUNTED, not latched.
 *
 * Counted because half the claims here are that a frame did NOT arrive: a drop at a mid-chain
 * hop is invisible at the terminus, so the assertion is "B's inbound count did not move while
 * the identical sound frame moves it by one".
 */
struct hop_probe_t {
    std::mutex m;
    std::condition_variable cv;
    std::string link;
    std::size_t seen = 0;
    std::vector<std::byte> dst;
    std::vector<std::byte> src;

    void observe(std::string_view on, std::span<const std::byte> frame) {
        if (on != link) return;
        const auto dec = tr::wire::decode(frame);
        if (!dec || dec->type != type_t::FWD || dec->children.size() < 3) return;
        {
            const std::lock_guard lock(m);
            dst = tr::wire::encode(dec->children[1]);
            src = tr::wire::encode(dec->children[2]);
            ++seen;
        }
        cv.notify_all();
    }
    /** @brief Wait until at least @p n frames have arrived. */
    bool wait_for_count(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, budget, [&] { return seen >= n; });
    }
    std::size_t count() {
        const std::lock_guard lock(m);
        return seen;
    }
    std::vector<std::byte> snap_dst() {
        const std::lock_guard lock(m);
        return dst;
    }
    std::vector<std::byte> snap_src() {
        const std::lock_guard lock(m);
        return src;
    }
};

/** @brief A transport that only records what was sent — the vector pin's egress. */
class span_sink_t final : public tr::net::transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    std::vector<std::vector<std::byte>> sent;
};

/**
 * @brief The test resolver (ADR-0018): the caller context IS the subject token.
 *
 * The empty (local) context never reaches a resolver — `graph_t::acl_allows` settles it
 * as trusted before invoking one (#905) — so the error arm here means DENY, nothing else.
 */
std::expected<void, tr::wire::err_t> caller_is_subject(void*, std::string_view caller,
                                                       tr::mem::bytes_t& out, std::uint64_t&) {
    const auto* p = reinterpret_cast<const std::byte*>(caller.data());
    return tr::testing::subject_into(out, std::vector<std::byte>(p, p + caller.size()));
}

/** @brief An ACL granting @p subject exactly @p mask. */
std::vector<std::byte> allow_acl(std::string_view subject, std::uint32_t mask) {
    const auto* p = reinterpret_cast<const std::byte*>(subject.data());
    const tr::graph::ace_t ace{
        .type = tr::graph::ace_type_t::ALLOW,
        .flags = 0,
        .subject = std::span<const std::byte>(p, subject.size()),
        .access_mask = mask,
        .expires_ns = 0,
    };
    return tr::graph::encode_acl(std::span<const tr::graph::ace_t>(&ace, 1));
}

constexpr std::uint8_t kRead = static_cast<std::uint8_t>(fwd_op_t::READ);
constexpr std::uint8_t kWrite = static_cast<std::uint8_t>(fwd_op_t::WRITE);
constexpr auto kBudget = 5000ms;
/** @brief The budget a DROP is asserted over — long enough that a delivered frame would
 *         have arrived (the positive path above it completes in well under a millisecond),
 *         short enough that six of them do not dominate the suite. */
constexpr auto kDropBudget = 500ms;

std::uint8_t value_u8(const tlv_t& v) { return tr::detail::load_le<std::uint8_t>(v.payload); }

/**
 * @brief A heap-backed source a test can CLOSE — the client router's link plane, so the
 *        origin's bind scratch and `dst` block can be refused on cue (#1779).
 */
class gate_source_t final : public tr::mem::block_source_t {
   public:
    gate_source_t() noexcept : tr::mem::block_source_t("gate") {}
    /** @brief Serve from the heap unless closed. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        return closed ? nullptr : tr::mem::heap_source().try_alloc(bytes, align);
    }
    /** @brief Return to the heap. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }
    bool closed = false; /**< @brief Refuse every request while set. */
};

/**
 * @brief The `ERROR` code a `FWD{REPLY}` frame carries, or `0` when it is not an ERROR reply.
 *
 * RFC-0029 §6: every refusal of a PAIR element is ANSWERED, so the refusal cases below assert
 * the code that came back rather than the silence the RFC-0024 drop used to leave.
 */
std::uint16_t error_code_of(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->children.size() < 5) return 0;
    if (value_u8(dec->children[3]) != static_cast<std::uint8_t>(reply_kind_t::ERROR)) return 0;
    const tlv_t& status = dec->children[4];
    if (status.type != type_t::STATUS || status.children.empty()) return 0;
    const tlv_t& err = status.children[0];
    if (err.type != type_t::ERROR || err.children.empty()) return 0;
    return tr::detail::load_le<std::uint16_t>(err.children[0].payload);
}

/** @brief `tr::path::not_found` — the one answer every PAIR refusal carries (RFC-0029 §6). */
constexpr std::uint16_t kNotFound = 0x0020;

}  // namespace

int main() {
    std::printf("RFC-0024 car 3: bound forwarder hop + origin bind, client -> A -> B\n");

    // ----- three nodes, production wiring ------------------------------------------------
    gate_source_t cli_link_src;
    graph_t g_cli;
    fwd_router_t r_cli(g_cli, {.link_src = &cli_link_src});
    transport_vertex_t net_cli(g_cli, r_cli);

    graph_t g_a;
    g_a.set_hook(&tr::graph::graph_hooks_t::subject_lookup,
                 {caller_is_subject, nullptr});  // A gates its own relays by inbound link
    fwd_router_t r_a(g_a);
    transport_vertex_t net_a(g_a, r_a);

    graph_t g_b;
    fwd_router_t r_b(g_b);
    transport_vertex_t net_b(g_b, r_b);

    const auto sensor = path_t::parse("/sensor");
    path_t target("/sensor/temp");
    (void)g_b.register_vertex(*sensor, role_t::STORED_VALUE);
    const tr::graph::vertex_handle_t vB = g_b.register_vertex(target, role_t::STORED_VALUE);
    const std::uint32_t kStored = 0xC0FFEE01u;
    (void)g_b.write(vB, owned(b_value_u32(kStored)));

    tr::net::loopback_channel_t ch_cli;  // client <-> A
    tr::net::loopback_channel_t ch_ab;   // A <-> B

    net_cli.provide_link("uplink", "a", ch_cli.a());
    net_a.provide_link("downlink", "cli", ch_cli.b());
    net_a.provide_link("uplink", "b", ch_ab.a());
    net_b.provide_link("downlink", "a", ch_ab.b());

    // A module is declared per staged link, and each declaration takes a `kind` of its own:
    // `register_module` keys a declaration on (kind, role), so two modules on one node that
    // shared a kind would be claiming one declaration and the second would be REFUSED
    // `PATH_IN_USE` (it used to silently rename the first).
    // The kind names no factory — a staged link is preferred over one — so it is the module's
    // own name, which is the only thing that distinguishes them here.
    (void)net_cli.register_module("uplink", "uplink", conn_role_t::DIAL);
    (void)net_a.register_module("downlink", "downlink", conn_role_t::DIAL);
    (void)net_a.register_module("uplink", "uplink", conn_role_t::DIAL);
    (void)net_b.register_module("downlink", "downlink", conn_role_t::DIAL);

    const auto mk = [](graph_t& g, std::string_view module, std::string_view name) {
        return g.write(conn_endpoint(module), conn_spec(name));
    };
    check(mk(g_cli, "uplink", "a").has_value() && mk(g_a, "downlink", "cli").has_value() &&
              mk(g_a, "uplink", "b").has_value() && mk(g_b, "downlink", "a").has_value(),
          "all four connections created through the /net/<module>/conn creator endpoint "
          "(production wiring)");

    hop_probe_t at_b;
    at_b.link = "net/downlink/a";
    r_b.on_raw([](void* ctx, std::string_view on,
                  std::span<const std::byte> f) { static_cast<hop_probe_t*>(ctx)->observe(on, f); },
               &at_b);

    // The BACKWARD half of every drop claim below. A refusal that turned into a misroute onto
    // the INBOUND link — the first child, the one the frame came from — would echo the frame
    // back toward the origin and B would still see nothing, so B's count alone cannot tell a
    // drop from a bounce. This counts what A puts back on the client's link.
    hop_probe_t at_cli;
    at_cli.link = "net/uplink/a";
    r_cli.on_raw(
        [](void* ctx, std::string_view on, std::span<const std::byte> f) {
            static_cast<hop_probe_t*>(ctx)->observe(on, f);
        },
        &at_cli);

    mailbox_t inbox;
    r_cli.on_reply(
        [](void* ctx, const tr::view::rope_t& reply) {
            const view_t mat = reply.materialize();
            const auto b = mat.bytes();
            static_cast<mailbox_t*>(ctx)->push(std::vector<std::byte>(b.begin(), b.end()));
        },
        &inbox);

    // ===== 1) the canonical round trip, and what each host contributed ==================
    std::printf("The chain accumulates in the reply's src on its way back (§6.2):\n");
    const std::vector<std::byte> canonical_dst = b_path({"net", "uplink", "b", "sensor", "temp"});
    ch_cli.a().send(b_fwd_raw_op(kRead, canonical_dst, b_path({"reply-ep"})));

    check(at_b.wait_for_count(1, kBudget), "B received the canonical request A forwarded");
    const std::vector<std::byte> canonical_src_at_b = at_b.snap_src();
    check(canonical_src_at_b == b_path({"net", "downlink", "cli", "reply-ep"}),
          "B saw src grown by A's FULL mount run — the canonical route composition");

    const auto minted = inbox.wait(kBudget);
    check(minted.has_value(), "the client received the reply");
    path_ref_element_t elem_b{};
    path_ref_element_t elem_a{};
    if (minted) {
        const auto dec = tr::wire::decode(*minted);
        const bool two = dec.has_value() && dec->children.size() > 2 &&
                         dec->children[2].type == type_t::PATH &&
                         dec->children[2].payload.size() == 2 * tr::wire::kPathPairRecordBytes;
        check(two, "the reply's src is TWO PAIRs: A's for its next hop, B's for the target");
        if (two) {
            const auto src_body = dec->children[2].payload;
            const auto pa = tr::wire::path_pair_at(src_body, 0);
            const auto pb = tr::wire::path_pair_at(src_body, tr::wire::kPathPairRecordBytes);
            if (pa) elem_a = {.index = pa->index, .generation = pa->generation};
            if (pb) elem_b = {.index = pb->index, .generation = pb->generation};
            // Each element is the ISSUING host's own — read back out of that host's own graph.
            const auto a_conn = g_a.find(path_t("/net/uplink/b").key());
            check(
                a_conn.has_value() && *g_a.vertex_slot(*a_conn) ==
                                          tr::graph::vertex_slot_t{.index = elem_a.index,
                                                                   .generation = elem_a.generation},
                "element 0 is A's own reference to its next-hop connection vertex");
            check(*g_b.vertex_slot(vB) == tr::graph::vertex_slot_t{.index = elem_b.index,
                                                                   .generation = elem_b.generation},
                  "element 1 is B's own reference to the TARGET vertex");
        }
        check(dec.has_value() && dec->children.size() == 5,
              "and nothing trails the payload — the RFC-0024 PATH_REF list is gone");

        // The origin's own element is the one no peer can supply (§4.1), and adopt_binding
        // is what puts it on the front.
        // The bind's element scratch is on the link plane (#1779): refused, it binds nothing.
        cli_link_src.closed = true;
        check(dec.has_value() && !r_cli.adopt_binding(target, "net/uplink/a",
                                                      *tr::wire::tlv_node_t::over(*minted)),
              "a refused element scratch binds nothing");
        check(!target.binding().bound, "…and the path stays canonical");
        cli_link_src.closed = false;
        check(dec.has_value() &&
                  r_cli.adopt_binding(target, "net/uplink/a", *tr::wire::tlv_node_t::over(*minted)),
              "the client adopts the binding, stacking its OWN first-hop element under it");
    }
    check(target.binding().bound &&
              target.binding().chain.size() == 3 * tr::wire::kPathPairRecordBytes,
          "the path is bound to a THREE-host route: client, A, B");
    check(target.key().size() > 0, "and it still holds its canonical bytes — the fallback");

    // ===== 2) the bound write, forwarded by A, applied at B ==============================
    std::printf("A bound WRITE traverses the same two hops (§3.4/§5):\n");
    const std::uint32_t kWritten = 0x12345678u;
    cli_link_src.closed = true;
    check(!bound_dispatch(r_cli, target, acl_right_t::WRITE).has_value(),
          "a refused `dst` block is no dispatch — the caller falls back to canonical (#1779)");
    cli_link_src.closed = false;
    const auto dispatch = bound_dispatch(r_cli, target, acl_right_t::WRITE);
    check(dispatch.has_value(), "the origin resolves its OWN element 0 to the link out");
    check(dispatch &&
              std::ranges::equal(dispatch->dst,
                                 b_path_body(std::span<const std::byte>(target.binding().chain)
                                                 .subspan(tr::wire::kPathPairRecordBytes))),
          "and puts the RESIDUAL on the wire — its own element is consumed here, not sent");
    if (dispatch) {
        dispatch->link->send(
            b_fwd_raw_op(kWrite, dispatch->dst, b_path({"reply-ep"}), {}, b_value_u32(kWritten)));
    }
    check(at_b.wait_for_count(2, kBudget), "B received the frame A forwarded on the BOUND route");
    const path_ref_element_t only_b[1] = {elem_b};
    check(at_b.snap_dst() == b_path_ref(only_b),
          "B saw dst shrunk to ONE element — A consumed its own (§4.1), byte-exact");
    check(at_b.snap_src() == canonical_src_at_b,
          "and src composed BYTE-IDENTICALLY to the canonical run — the reply route is the same");

    const auto wrote = inbox.wait(kBudget);
    check(wrote.has_value(), "the client received the reply to the bound write");
    if (wrote) {
        const auto dec = tr::wire::decode(*wrote);
        check(dec.has_value() && dec->children.size() >= 4 &&
                  value_u8(dec->children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
              "kind == RESULT — the bound spelling delivered");
    }
    const auto stored = g_b.read(vB);
    check(stored.has_value(), "B /sensor/temp readable after the bound write");
    if (stored) {
        const auto inner = tr::wire::decode((*stored)->only());
        check(inner && inner->type == type_t::VALUE && inner->payload.size() == 4 &&
                  tr::detail::load_le<std::uint32_t>(inner->payload) == kWritten,
              "and holds the forwarded value byte-exact — identical to the canonical spelling");
    }

    // ===== 3) a mid-chain generation mismatch drops AT A =================================
    std::printf("A stale MID-CHAIN element is refused at the consuming hop (RFC-0029 §6):\n");
    {
        const std::size_t before = at_b.count();
        const std::size_t before_cli = at_cli.count();
        const path_ref_element_t stale[2] = {
            {.index = elem_a.index, .generation = elem_a.generation + 1}, elem_b};
        ch_cli.a().send(b_fwd_raw_op(kWrite, b_path_ref(stale), b_path({"reply-ep"}), {},
                                     b_value_u32(0xDEADBEEFu)));
        check(!at_b.wait_for_count(before + 1, kDropBudget),
              "B never sees it — A refused the element it was asked to consume");
        check(at_cli.wait_for_count(before_cli + 1, kBudget),
              "the refusal comes BACK down the inbound link, as the reply");
        const auto refused = inbox.wait(kBudget);
        check(refused.has_value() && error_code_of(*refused) == kNotFound,
              "and it answers NOT_FOUND — never a delivery to the slot's new tenant");
    }
    // ===== 4) an out-of-range mid-chain index drops AT A =================================
    {
        const std::size_t before = at_b.count();
        const std::size_t before_cli = at_cli.count();
        const path_ref_element_t absurd[2] = {{.index = 0xFFFFFFFFu, .generation = 0}, elem_b};
        ch_cli.a().send(b_fwd_raw_op(kWrite, b_path_ref(absurd), b_path({"reply-ep"}), {},
                                     b_value_u32(0xDEADBEEFu)));
        check(!at_b.wait_for_count(before + 1, kDropBudget),
              "a peer-chosen u32 maximum mid-chain is refused at A rather than faulting");
        check(at_cli.wait_for_count(before_cli + 1, kBudget),
              "and the refusal goes back down the inbound link");
        const auto refused = inbox.wait(kBudget);
        check(refused.has_value() && error_code_of(*refused) == kNotFound,
              "as NOT_FOUND — out of range and stale are one answer");
    }
    // ===== 5) an element naming a NON-egress vertex of A drops ===========================
    {
        const std::size_t before = at_b.count();
        const std::size_t before_cli = at_cli.count();
        const auto net_root = g_a.find(path_t("/net").key());
        const std::optional<tr::graph::vertex_slot_t> root_slot =
            net_root ? g_a.vertex_slot(*net_root) : std::nullopt;
        check(root_slot.has_value(), "A's /net vertex is bindable — the element is well-formed");
        const path_ref_element_t wrong[2] = {{.index = root_slot ? root_slot->index : 0u,
                                              .generation = root_slot ? root_slot->generation : 0u},
                                             elem_b};
        ch_cli.a().send(b_fwd_raw_op(kWrite, b_path_ref(wrong), b_path({"reply-ep"}), {},
                                     b_value_u32(0xDEADBEEFu)));
        check(
            !at_b.wait_for_count(before + 1, kDropBudget),
            "a VALID element that names no egress is refused — a pair is an address, not a route");
        check(at_cli.wait_for_count(before_cli + 1, kBudget),
              "and it is answered, not resolved to the inbound link instead");
        const auto refused = inbox.wait(kBudget);
        check(refused.has_value() && error_code_of(*refused) != 0,
              "as an ERROR reply (a tail below a non-connection vertex, §6 step 3)");
    }
    // ===== 6) the ablation: the sound binding still lands ================================
    {
        const std::size_t before = at_b.count();
        const auto again = bound_dispatch(r_cli, target, acl_right_t::WRITE);
        check(again.has_value(), "the binding is still good");
        if (again)
            again->link->send(b_fwd_raw_op(kWrite, again->dst, b_path({"reply-ep"}), {},
                                           b_value_u32(0x0BADF00Du)));
        check(at_b.wait_for_count(before + 1, kBudget),
              "the SAME frame with a sound mid-chain element lands — every drop above is real");
        (void)inbox.wait(kBudget);
    }

    // ===== 7) a mid-chain ACL denial drops ===============================================
    std::printf("A mid-chain ACL denial drops at the relay (§6.2):\n");
    {
        // The right is evaluated at the DEREFERENCED vertex — here A's connection vertex for
        // its next hop — for the operation's own right, with the inbound link as the subject.
        // Granting READ alone therefore lets a bound READ through and refuses a bound WRITE.
        (void)g_a.write(
            path_t("/net/uplink/b:acl"),
            owned(allow_acl("net/downlink/cli", static_cast<std::uint32_t>(acl_right_t::READ))));
        const std::size_t before = at_b.count();
        const auto denied = bound_dispatch(r_cli, target, acl_right_t::WRITE);
        if (denied)
            denied->link->send(b_fwd_raw_op(kWrite, denied->dst, b_path({"reply-ep"}), {},
                                            b_value_u32(0xFEEDFACEu)));
        check(!at_b.wait_for_count(before + 1, kDropBudget),
              "a bound WRITE through a relay that grants only READ drops at that relay");

        const auto allowed = bound_dispatch(r_cli, target, acl_right_t::READ);
        if (allowed) allowed->link->send(b_fwd_raw_op(kRead, allowed->dst, b_path({"reply-ep"})));
        check(at_b.wait_for_count(before + 1, kBudget),
              "and the bound READ the SAME ACL grants goes through — the denial is the ACL's");
        (void)inbox.wait(kBudget);
    }

    // ===== 7b) the SAME hop spelled as a NAME run gets the SAME verdict ==================
    std::printf("The canonical-string spelling of that hop is gated identically:\n");
    {
        // The ACL from (7) still stands: READ only, at A's connection vertex for `b`, for the
        // inbound subject. A canonical `dst` that walks through that vertex by NAME must be
        // refused the WRITE the bound spelling is refused, and granted the READ it is granted.
        const auto first_hop = bound_dispatch(r_cli, target, acl_right_t::READ);
        check(first_hop.has_value(), "the client still has its first-hop link to A");
        const std::vector<std::byte> by_name = b_path({"net", "uplink", "b", "sensor", "temp"});
        const std::size_t before = at_b.count();
        if (first_hop)
            first_hop->link->send(
                b_fwd_raw_op(kWrite, by_name, b_path({"reply-ep"}), {}, b_value_u32(0xFEEDFACEu)));
        check(!at_b.wait_for_count(before + 1, kDropBudget),
              "a canonical WRITE through a relay that grants only READ stops at that relay");
        (void)inbox.wait(kBudget);  // its answer (the relay's refusal), so the next is the READ's
        if (first_hop) first_hop->link->send(b_fwd_raw_op(kRead, by_name, b_path({"reply-ep"})));
        check(at_b.wait_for_count(before + 1, kBudget),
              "and the canonical READ the SAME ACL grants goes through — the denial is the ACL's");
        (void)inbox.wait(kBudget);
    }

    // ===== 8) the conformance vectors, byte-exact against what this hop emits ============
    //
    // The harness routes nothing (HARNESS.md §"the execution model has ONE forwarder"), so a
    // vector for a FORWARDED bound frame can only be gated here: the pair is the frame a hop
    // receives and the frame it puts on the wire, and this is the one place both exist.
    std::printf("The forwarded-PATH_REF vectors are retired as addresses (RFC-0029 S1):\n");
    {
        // A bare forwarder: `/up` is the connection vertex of the child named "up", so the
        // graph hands out slot 1 (the structural root is slot 0) at generation 0.
        graph_t g;
        (void)g.register_vertex(path_t("/up"), role_t::STORED_VALUE);
        fwd_router_t r(g);
        span_sink_t cli;
        span_sink_t up;
        (void)r.attach_link("cli", cli);
        (void)r.attach_link("up", up);

        // A dst spelled as a PATH_REF (0x14) is no longer an address (its vector retired in
        // #2245): refused by type code, so the hop forwards nothing and answers the refusal
        // down the inbound link.
        const path_ref_element_t ref_route[2] = {{.index = 1, .generation = 0},
                                                 {.index = 0x0000BEEFu, .generation = 7}};
        std::vector<std::byte> ref_dst;
        (void)tr::wire::emit_path_ref(ref_dst, std::span<const path_ref_element_t>(ref_route));
        r.on_frame("cli", b_fwd_raw_op(kRead, ref_dst, b_path({"reply-ep"}), {}, b_value_u32(9)));
        check(up.sent.empty(), "a 0x14 dst is not forwarded");
        check(cli.sent.size() == 1 && error_code_of(cli.sent[0]) != 0,
              "and it is refused with an ERROR reply down the inbound link");

        // The same route spelled as PAIR elements forwards, consuming its head element.
        const path_ref_element_t route[2] = {{.index = 1, .generation = 0},
                                             {.index = 0x0000BEEFu, .generation = 7}};
        r.on_frame("cli", b_fwd_raw_op(kRead, b_path_ref(route), b_path({"reply-ep"}), {},
                                       b_value_u32(9)));
        check(up.sent.size() == 1, "the PAIR spelling of that route is forwarded exactly once");
        const auto dec =
            up.sent.empty() ? std::nullopt : std::optional(tr::wire::decode(up.sent[0]));
        check(dec && *dec && (*dec)->children.size() > 1 &&
                  tr::wire::encode((*dec)->children[1]) == b_path_ref(std::span(&route[1], 1)),
              "and its dst is the residual — the one element this hop did not consume");
    }

    // ===== 9) a hop with no PAIR grows the src by its NAME run, never nothing (§6.2) =====
    //
    // A learned chain that SKIPS a hop is not a shorter route, it is a wrong one: the hop that
    // could not contribute would receive a residual issued on a DIFFERENT host and dereference
    // it against its own vertex map. So a hop that holds no PAIR for the link the reply came
    // back on grows the `src` by its NAME run instead — the chain stays a full route, mixed.
    std::printf("A hop with no PAIR to give grows the reply src by its NAME run:\n");
    {
        std::vector<std::byte> seed;
        tr::testing::emit_path_pair(seed, tr::wire::path_pair_t{.index = 9, .generation = 1});
        const std::vector<std::byte> reply = b_relayed_reply(seed);

        // A link cannot exist without its connection vertex (#1940), so the hop that cannot
        // issue a PAIR is the one whose vertex has RETIRED under it: a placeholder issues
        // nothing (§4.4 — a PAIR for a retired vertex would stamp the successor's tenancy).
        const auto relay = [&](bool pairable) {
            graph_t g;
            const tr::graph::vertex_handle_t up_v =
                g.register_vertex(path_t("/up"), role_t::STORED_VALUE);
            fwd_router_t r(g);
            span_sink_t cli;
            span_sink_t up;
            (void)r.attach_link("cli", cli);
            (void)r.attach_link("up", up);
            if (!pairable) (void)g.retire(up_v);
            r.on_frame("up", reply);  // the reply comes back over the link the request left on
            return std::move(cli.sent);
        };
        const auto src_of = [](const std::vector<std::byte>& frame) {
            const auto dec = tr::wire::decode(frame);
            return dec && dec->children.size() > 2 && dec->children[2].type == type_t::PATH
                       ? std::vector<std::byte>(dec->children[2].payload.begin(),
                                                dec->children[2].payload.end())
                       : std::vector<std::byte>{};
        };

        const auto contributed = relay(true);
        check(contributed.size() == 1, "the reply is forwarded on toward the origin");
        std::vector<std::byte> want_pair;
        tr::testing::emit_path_pair(want_pair, tr::wire::path_pair_t{.index = 1, .generation = 0});
        want_pair.insert(want_pair.end(), seed.begin(), seed.end());
        check(contributed.size() == 1 && src_of(contributed[0]) == want_pair,
              "a hop that CAN issue prepends its PAIR — /up's slot, then the terminus's, in order");

        const auto named = relay(false);
        check(named.size() == 1, "the same reply is still forwarded when no PAIR can be issued");
        std::vector<std::byte> want_name;
        (void)tr::wire::emit_path_segment(want_name, "up");
        want_name.insert(want_name.end(), seed.begin(), seed.end());
        check(named.size() == 1 && src_of(named[0]) == want_name,
              "and the src grows by the hop's NAME run instead — never relayed one hop short");
    }

    // ===== 11) an opcode with no known right is dropped, never charged READ =============
    //
    // §6.2 says the ACL is evaluated for "the operation's own right". A hop that does not
    // know an opcode does not know its right, so it cannot evaluate §6.2 for it — and the
    // initialized READ is a GUESS, not an answer. Guessing READ is how a future write-like
    // opcode crosses a READ-only gate. The two arms below are the same frame with one byte
    // changed, so the refusal is the opcode's and nothing else's.
    //
    // On a READ-only gate, because that is where the guess would matter: since #1939 a bound
    // hop runs the ONE gate the NAME hop runs (RFC-0029 §6.4), and a graph that enforces no
    // ACL lets every op through in every spelling.
    std::printf("An opcode this build cannot price is dropped at a bound hop (§6.2):\n");
    {
        const auto forward_op = [&](std::uint8_t op_byte) {
            graph_t g;
            g.set_hook(&tr::graph::graph_hooks_t::subject_lookup, {caller_is_subject, nullptr});
            (void)g.register_vertex(path_t("/up"), role_t::STORED_VALUE);
            (void)g.write(path_t("/up:acl"),
                          owned(allow_acl("cli", static_cast<std::uint32_t>(acl_right_t::READ))));
            fwd_router_t r(g);
            span_sink_t cli;
            span_sink_t up;
            (void)r.attach_link("cli", cli);
            (void)r.attach_link("up", up);
            const path_ref_element_t route[2] = {{.index = 1, .generation = 0},
                                                 {.index = 0x0000BEEFu, .generation = 7}};
            r.on_frame("cli", b_fwd_raw_op(op_byte, b_path_ref(route), b_path({"reply-ep"}), {},
                                           b_value_u32(9)));
            return up.sent.size();
        };
        check(forward_op(kRead) == 1, "the known opcode forwards — the route itself is sound");
        check(forward_op(0x05) == 0,
              "and the SAME frame with an unpriced opcode drops instead of being charged READ");
    }

    // Quiesce the wire BEFORE anything a receive thread touches goes out of scope. The
    // channels are declared above the probe and the mailbox, so scope exit would destroy
    // those two FIRST and leave a delivery thread pushing into freed storage — a race TSan
    // reports against `~mailbox_t` and against the endpoint's own destructor. `shutdown()`
    // joins both receive threads, which is the contract loopback.hpp states and the reason
    // it exists: a test that only asserts on what arrived still has to say when nothing more
    // may arrive. Later cases in this file drop frames deliberately, so a reply for one of
    // them can still be in flight at the last assertion.
    ch_cli.shutdown();
    ch_ab.shutdown();

    return tr::testing::summary("bound_forward");
}
