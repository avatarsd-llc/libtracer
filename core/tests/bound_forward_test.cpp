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
 * - the **mint accumulates on the way back** (§7.1): B answers with its element, A prepends
 *   its own as it forwards the reply, and the origin prepends the one only it can know —
 *   its reference to its first-hop connection vertex (§4.1);
 * - the **forwarder hop** (§3.4/§5): a `PATH_REF` `dst` with a residual longer than one
 *   element consumes ITS element — bounds, generation, ACL at the dereferenced vertex — and
 *   forwards the remainder, growing `src` exactly as the canonical spelling does. The route
 *   composition is asserted BYTE-IDENTICAL against the canonical run, because a return route
 *   that differed by one byte would still route and would no longer be the same protocol.
 *
 * Every refusal is shown non-vacuous by ablation: the same frame, one field sound, lands.
 *
 * Production wiring throughout (the RFC-0014 lesson): every link is bound through the in-band
 * `/net:children[]` SPEC door, so each connection vertex — the thing a forwarder's element
 * names — is created by `transport_vertex.cpp` and never hand-spelled.
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/loopback.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::acl_right_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::graph::subject_token_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::path_ref_element_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A heap-owned view over @p bytes (the graph stores owning views). */
view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief Canonical `PATH{ NAME… }` bytes for @p segs. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
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

/** @brief `FWD{ op, dst, src, payload? }` with a RAW op byte (the mint flag is a flag bit). */
std::vector<std::byte> b_fwd_raw(std::uint8_t op_byte, const std::vector<std::byte>& dst,
                                 const std::vector<std::byte>& src,
                                 const std::vector<std::byte>& payload = {}) {
    std::vector<std::byte> body;
    const std::byte ob{op_byte};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&ob, 1));
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), src.begin(), src.end());
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/** @brief A `PATH_REF` TLV over @p elements — the bound spelling of an address. */
std::vector<std::byte> b_path_ref(std::span<const path_ref_element_t> elements) {
    std::vector<std::byte> out;
    (void)tr::wire::emit_path_ref(out, elements);
    return out;
}

/** @brief A connection-creation SPEC with no transport `kind` (every link is provided). */
view_t conn_spec(std::string_view type, std::string_view name) {
    std::vector<std::byte> cfg;
    tr::wire::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(conn_role_t::DIAL)};
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));

    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, type);
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    tr::wire::emit_name(body, "config");
    tr::wire::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, cfg);

    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
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

/** @brief A bounded mailbox for the client's terminating REPLY frames. */
struct mailbox_t {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::vector<std::byte>> q;

    void push(std::vector<std::byte> v) {
        {
            const std::lock_guard lock(m);
            q.push_back(std::move(v));
        }
        cv.notify_all();
    }
    std::optional<std::vector<std::byte>> wait(std::chrono::milliseconds budget) {
        std::unique_lock lock(m);
        if (!cv.wait_for(lock, budget, [this] { return !q.empty(); })) return std::nullopt;
        auto v = std::move(q.front());
        q.erase(q.begin());
        return v;
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

/** @brief The raw bytes of a conformance vector's `input.bin`. */
std::vector<std::byte> vector_bytes(std::string_view case_dir) {
    const std::filesystem::path p =
        std::filesystem::path{LIBTRACER_VECTORS_DIR} / case_dir / "input.bin";
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    return out;
}

/** @brief The subject resolver A uses: the inbound link's NAME is the subject. */
std::optional<subject_token_t> caller_is_subject(std::string_view caller) {
    if (caller.empty()) return std::nullopt;  // trusted local caller
    const auto* p = reinterpret_cast<const std::byte*>(caller.data());
    return subject_token_t(p, p + caller.size());
}

/** @brief An ACL granting @p subject exactly @p mask. */
std::vector<std::byte> allow_acl(std::string_view subject, std::uint32_t mask) {
    const auto* p = reinterpret_cast<const std::byte*>(subject.data());
    const tr::graph::ace_t ace{
        .type = tr::graph::ace_type_t::ALLOW,
        .flags = 0,
        .subject = std::vector<std::byte>(p, p + subject.size()),
        .access_mask = mask,
        .expires_ns = 0,
    };
    return tr::graph::encode_acl(std::span<const tr::graph::ace_t>(&ace, 1));
}

constexpr std::uint8_t kMint = tr::graph::kFwdOpFlagMintRequest;
constexpr std::uint8_t kRead = static_cast<std::uint8_t>(fwd_op_t::READ);
constexpr std::uint8_t kWrite = static_cast<std::uint8_t>(fwd_op_t::WRITE);
constexpr auto kBudget = 5000ms;
/** @brief The budget a DROP is asserted over — long enough that a delivered frame would
 *         have arrived (the positive path above it completes in well under a millisecond),
 *         short enough that six of them do not dominate the suite. */
constexpr auto kDropBudget = 500ms;

std::uint8_t value_u8(const tlv_t& v) { return tr::detail::load_le<std::uint8_t>(v.payload); }

}  // namespace

int main() {
    std::printf("RFC-0024 car 3: bound forwarder hop + origin bind, client -> A -> B\n");

    // ----- three nodes, production wiring ------------------------------------------------
    graph_t g_cli;
    fwd_router_t r_cli(g_cli);
    transport_vertex_t net_cli(g_cli, r_cli);

    graph_t g_a;
    g_a.set_subject_resolver(caller_is_subject);  // A gates its own relays by inbound link
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

    const auto mk = [](graph_t& g, std::string_view name) {
        return g.write(path_t("/net:children[]"), conn_spec("client", name));
    };
    check(mk(g_cli, "a").has_value() && mk(g_a, "cli").has_value() && mk(g_a, "b").has_value() &&
              mk(g_b, "a").has_value(),
          "all four connections created through /net:children[] SPEC (production wiring)");

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

    // ===== 1) the canonical mint round trip, and what each host contributed ==============
    std::printf("The mint accumulates on the reply's way back (§7.1):\n");
    const std::vector<std::byte> canonical_dst = b_path({"net", "uplink", "b", "sensor", "temp"});
    ch_cli.a().send(b_fwd_raw(kRead | kMint, canonical_dst, b_path({"reply-ep"})));

    check(at_b.wait_for_count(1, kBudget), "B received the canonical request A forwarded");
    const std::vector<std::byte> canonical_src_at_b = at_b.snap_src();
    check(canonical_src_at_b == b_path({"net", "downlink", "cli", "reply-ep"}),
          "B saw src grown by A's FULL mount run — the canonical route composition");

    const auto minted = inbox.wait(kBudget);
    check(minted.has_value(), "the client received the mint reply");
    path_ref_element_t elem_b{};
    path_ref_element_t elem_a{};
    if (minted) {
        const auto dec = tr::wire::decode(*minted);
        check(dec.has_value() && !dec->children.empty() &&
                  dec->children.back().type == type_t::PATH_REF,
              "the reply's LAST child is the accumulated PATH_REF (§7.1)");
        if (dec && !dec->children.empty() && dec->children.back().type == type_t::PATH_REF) {
            const tlv_t& acc = dec->children.back();
            check(tr::wire::path_ref_element_count(acc.payload.size()) == 2,
                  "TWO elements came back: B's for the target, A's for its next hop");
            if (tr::wire::path_ref_element_count(acc.payload.size()) == 2) {
                elem_a = tr::wire::path_ref_element_at(acc.payload, 0);
                elem_b = tr::wire::path_ref_element_at(acc.payload, 1);
            }
            // Each element is the MINTING host's own — read back out of that host's own graph.
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

        // The origin's own element is the one no peer can supply (§4.1), and adopt_binding
        // is what puts it on the front.
        check(dec.has_value() && r_cli.adopt_binding(target, "net/uplink/a", *dec),
              "the client adopts the binding, stacking its OWN first-hop element under it");
    }
    check(target.binding().bound && target.binding().elements.size() == 3,
          "the path is bound to a THREE-host route: client, A, B");
    check(target.key().size() > 0, "and it still holds its canonical bytes — the fallback");

    // ===== 2) the bound write, forwarded by A, applied at B ==============================
    std::printf("A bound WRITE traverses the same two hops (§3.4/§5):\n");
    const std::uint32_t kWritten = 0x12345678u;
    const auto dispatch = r_cli.bound_dispatch(target, acl_right_t::WRITE);
    check(dispatch.has_value(), "the origin resolves its OWN element 0 to the link out");
    check(dispatch &&
              dispatch->dst ==
                  b_path_ref(
                      std::span<const path_ref_element_t>(target.binding().elements).subspan(1)),
          "and puts the RESIDUAL on the wire — its own element is consumed here, not sent");
    if (dispatch) {
        dispatch->link->send(
            b_fwd_raw(kWrite, dispatch->dst, b_path({"reply-ep"}), b_value_u32(kWritten)));
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
    std::printf("A stale MID-CHAIN element drops at the consuming hop, not later (§5.3):\n");
    {
        const std::size_t before = at_b.count();
        const std::size_t before_cli = at_cli.count();
        const path_ref_element_t stale[2] = {
            {.index = elem_a.index, .generation = elem_a.generation + 1}, elem_b};
        ch_cli.a().send(
            b_fwd_raw(kWrite, b_path_ref(stale), b_path({"reply-ep"}), b_value_u32(0xDEADBEEFu)));
        check(!at_b.wait_for_count(before + 1, kDropBudget),
              "B never sees it — A refused the element it was asked to consume");
        check(!at_cli.wait_for_count(before_cli + 1, kDropBudget),
              "and it does not come BACK either: a drop, not a bounce onto the inbound link");
        check(!inbox.wait(kDropBudget).has_value(), "and nothing answers: a drop, not an error");
    }
    // ===== 4) an out-of-range mid-chain index drops AT A =================================
    {
        const std::size_t before = at_b.count();
        const std::size_t before_cli = at_cli.count();
        const path_ref_element_t absurd[2] = {{.index = 0xFFFFFFFFu, .generation = 0}, elem_b};
        ch_cli.a().send(
            b_fwd_raw(kWrite, b_path_ref(absurd), b_path({"reply-ep"}), b_value_u32(0xDEADBEEFu)));
        check(!at_b.wait_for_count(before + 1, kDropBudget),
              "a peer-chosen u32 maximum mid-chain drops at A rather than faulting");
        check(!at_cli.wait_for_count(before_cli + 1, kDropBudget),
              "and nothing goes back down the inbound link");
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
        ch_cli.a().send(
            b_fwd_raw(kWrite, b_path_ref(wrong), b_path({"reply-ep"}), b_value_u32(0xDEADBEEFu)));
        check(!at_b.wait_for_count(before + 1, kDropBudget),
              "a VALID element that names no egress drops — a vref is an address, not a route");
        check(!at_cli.wait_for_count(before_cli + 1, kDropBudget),
              "and it is not resolved to the inbound link instead — the drop is a drop");
    }
    // ===== 6) the ablation: the sound binding still lands ================================
    {
        const std::size_t before = at_b.count();
        const auto again = r_cli.bound_dispatch(target, acl_right_t::WRITE);
        check(again.has_value(), "the binding is still good");
        if (again)
            again->link->send(
                b_fwd_raw(kWrite, again->dst, b_path({"reply-ep"}), b_value_u32(0x0BADF00Du)));
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
        const auto denied = r_cli.bound_dispatch(target, acl_right_t::WRITE);
        if (denied)
            denied->link->send(
                b_fwd_raw(kWrite, denied->dst, b_path({"reply-ep"}), b_value_u32(0xFEEDFACEu)));
        check(!at_b.wait_for_count(before + 1, kDropBudget),
              "a bound WRITE through a relay that grants only READ drops at that relay");

        const auto allowed = r_cli.bound_dispatch(target, acl_right_t::READ);
        if (allowed) allowed->link->send(b_fwd_raw(kRead, allowed->dst, b_path({"reply-ep"})));
        check(at_b.wait_for_count(before + 1, kBudget),
              "and the bound READ the SAME ACL grants goes through — the denial is the ACL's");
        (void)inbox.wait(kBudget);
    }

    // ===== 8) the conformance vectors, byte-exact against what this hop emits ============
    //
    // The harness routes nothing (HARNESS.md §"the execution model has ONE forwarder"), so a
    // vector for a FORWARDED bound frame can only be gated here: the pair is the frame a hop
    // receives and the frame it puts on the wire, and this is the one place both exist.
    std::printf("The forwarded-PATH_REF vectors, byte-exact against the hop (§9.4):\n");
    {
        // A bare forwarder: `/up` is the connection vertex of the child named "up", so the
        // graph hands out slot 1 (the structural root is slot 0) at generation 0.
        graph_t g;
        (void)g.register_vertex(path_t("/up"), role_t::STORED_VALUE);
        fwd_router_t r(g);
        span_sink_t cli;
        span_sink_t up;
        r.add_child("cli", cli);
        r.add_child("up", up);

        const path_ref_element_t route[2] = {{.index = 1, .generation = 0},
                                             {.index = 0x0000BEEFu, .generation = 7}};
        const std::vector<std::byte> inbound =
            b_fwd_raw(kRead, b_path_ref(route), b_path({"reply-ep"}), b_value_u32(9));
        r.on_frame("cli", inbound);
        check(up.sent.size() == 1, "the hop forwarded exactly one frame");
        check(inbound == vector_bytes("fwd/fwd-bound-forward"),
              "fwd/fwd-bound-forward is byte-exact the frame a forwarder receives");
        check(up.sent.size() == 1 && up.sent[0] == vector_bytes("fwd/fwd-bound-forwarded"),
              "fwd/fwd-bound-forwarded is byte-exact what this hop puts on the wire");
        if (up.sent.size() == 1) {
            std::printf("    inbound  = ");
            for (const std::byte b : inbound) std::printf("%02x", std::to_integer<unsigned>(b));
            std::printf("\n    egress   = ");
            for (const std::byte b : up.sent[0]) std::printf("%02x", std::to_integer<unsigned>(b));
            std::printf("\n");
        }
    }

    // ===== 9) a hop that cannot mint STRIPS the answer (§7.1, car-3 erratum) =============
    //
    // The safety half of the accumulation. A mint list that SKIPS a hop is not a shorter
    // route, it is a wrong one: the origin consumes its own element, the frame reaches the
    // hop that could not contribute with exactly one element left, and that hop — believing
    // itself the terminus — dereferences an element minted on a DIFFERENT host against its
    // own vertex map, where the same index and generation are an ordinary live vertex. So a
    // hop that cannot contribute refuses the whole exchange instead.
    std::printf("A hop that cannot mint strips the answer rather than shortening it:\n");
    {
        // One reply, two nodes that differ in exactly one fact: whether the link the reply
        // arrived on has a connection vertex to mint from.
        const path_ref_element_t from_terminus{.index = 9, .generation = 1};
        std::vector<std::byte> mint_tail;
        (void)tr::wire::emit_path_ref(mint_tail,
                                      std::span<const path_ref_element_t>(&from_terminus, 1));
        std::vector<std::byte> reply_body;
        const std::byte op_reply{static_cast<std::uint8_t>(fwd_op_t::REPLY)};
        tr::wire::emit_tlv(reply_body, type_t::VALUE, opt_t{},
                           std::span<const std::byte>(&op_reply, 1));
        const std::vector<std::byte> rdst = b_path({"cli", "reply-ep"});
        const std::vector<std::byte> rsrc = b_path({"sensor"});
        reply_body.insert(reply_body.end(), rdst.begin(), rdst.end());
        reply_body.insert(reply_body.end(), rsrc.begin(), rsrc.end());
        const std::byte kind{static_cast<std::uint8_t>(reply_kind_t::RESULT)};
        tr::wire::emit_tlv(reply_body, type_t::VALUE, opt_t{},
                           std::span<const std::byte>(&kind, 1));
        const std::vector<std::byte> payload = b_value_u32(5);
        reply_body.insert(reply_body.end(), payload.begin(), payload.end());
        reply_body.insert(reply_body.end(), mint_tail.begin(), mint_tail.end());
        std::vector<std::byte> reply;
        tr::wire::emit_tlv(reply, type_t::FWD, opt_t{.pl = true}, reply_body);

        const auto relay = [&](bool with_conn_vertex) {
            graph_t g;
            if (with_conn_vertex) (void)g.register_vertex(path_t("/up"), role_t::STORED_VALUE);
            fwd_router_t r(g);
            span_sink_t cli;
            span_sink_t up;
            r.add_child("cli", cli);
            r.add_child("up", up);
            r.on_frame("up", reply);  // the reply comes back over the link the request left on
            return std::move(cli.sent);
        };

        const auto contributed = relay(true);
        check(contributed.size() == 1, "the reply is forwarded on toward the origin");
        if (contributed.size() == 1) {
            const auto dec = tr::wire::decode(contributed[0]);
            check(dec && !dec->children.empty() && dec->children.back().type == type_t::PATH_REF &&
                      tr::wire::path_ref_element_count(dec->children.back().payload.size()) == 2,
                  "a hop that CAN mint prepends its element — two, in route order");
            if (dec && !dec->children.empty() && dec->children.back().type == type_t::PATH_REF &&
                tr::wire::path_ref_element_count(dec->children.back().payload.size()) == 2) {
                const tlv_t& acc = dec->children.back();
                check(tr::wire::path_ref_element_at(acc.payload, 1) == from_terminus,
                      "the terminus's element is untouched, and stays LAST");
                check(tr::wire::path_ref_element_at(acc.payload, 0).index == 1u,
                      "and this hop's own element — /up's slot — goes in FRONT of it");
            }
        }

        const auto stripped = relay(false);
        check(stripped.size() == 1, "the same reply is still forwarded when nothing can be minted");
        if (stripped.size() == 1) {
            const auto dec = tr::wire::decode(stripped[0]);
            check(dec.has_value() && !dec->children.empty() &&
                      dec->children.back().type != type_t::PATH_REF,
                  "but the mint answer is STRIPPED — never relayed one element short");
            check(dec.has_value() && dec->children.size() == 5,
                  "what is left is an ordinary reply: op, dst, src, kind, payload");
            check(stripped[0].size() == contributed[0].size() - 20,
                  "20 bytes lighter: the 12 it did not add, plus the 8 it removed");
        }
    }

    // ===== 10) a FULL mint list is stripped too — the erratum names it (§7.1) ============
    //
    // "Cannot contribute" is not only "has no vertex to mint from": a list already at the
    // element cap cannot be extended either, and the erratum names a full list among the
    // MUST-STRIP cases for the reason every other one is there. Relaying it would hand the
    // origin a route that SKIPS this hop, and a skipped hop is the §5.3 misroute: it receives
    // a residual minted on a different host, finds one element left, believes itself the
    // terminus, and dereferences that element against its own vertex map.
    //
    // The two arms differ in ONE element of the inbound list, so the strip cannot be an
    // accident of the frame's shape.
    std::printf("A mint list already at the element cap is stripped, not relayed (§7.1):\n");
    {
        const auto reply_with = [&](std::size_t n) {
            std::vector<path_ref_element_t> acc(n);
            for (std::size_t i = 0; i < n; ++i)
                acc[i] = path_ref_element_t{.index = static_cast<std::uint32_t>(100 + i),
                                            .generation = 1};
            std::vector<std::byte> mint_tail;
            (void)tr::wire::emit_path_ref(mint_tail, std::span<const path_ref_element_t>(acc));
            std::vector<std::byte> body;
            const std::byte op_reply{static_cast<std::uint8_t>(fwd_op_t::REPLY)};
            tr::wire::emit_tlv(body, type_t::VALUE, opt_t{},
                               std::span<const std::byte>(&op_reply, 1));
            const std::vector<std::byte> rdst = b_path({"cli", "reply-ep"});
            const std::vector<std::byte> rsrc = b_path({"sensor"});
            body.insert(body.end(), rdst.begin(), rdst.end());
            body.insert(body.end(), rsrc.begin(), rsrc.end());
            const std::byte kind{static_cast<std::uint8_t>(reply_kind_t::RESULT)};
            tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&kind, 1));
            const std::vector<std::byte> payload = b_value_u32(5);
            body.insert(body.end(), payload.begin(), payload.end());
            body.insert(body.end(), mint_tail.begin(), mint_tail.end());
            std::vector<std::byte> out;
            tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
            return out;
        };
        const auto relay = [&](const std::vector<std::byte>& reply) {
            graph_t g;
            (void)g.register_vertex(path_t("/up"), role_t::STORED_VALUE);
            fwd_router_t r(g);
            span_sink_t cli;
            span_sink_t up;
            r.add_child("cli", cli);
            r.add_child("up", up);
            r.on_frame("up", reply);
            return std::move(cli.sent);
        };

        const auto under = relay(reply_with(tr::wire::kMaxPathRefElements - 1));
        check(under.size() == 1, "a list one short of the cap is forwarded");
        if (under.size() == 1) {
            const auto dec = tr::wire::decode(under[0]);
            check(dec.has_value() && !dec->children.empty() &&
                      dec->children.back().type == type_t::PATH_REF &&
                      tr::wire::path_ref_element_count(dec->children.back().payload.size()) ==
                          tr::wire::kMaxPathRefElements,
                  "and this hop's element fills it exactly to the cap");
        }

        const auto full = relay(reply_with(tr::wire::kMaxPathRefElements));
        check(full.size() == 1, "a list AT the cap is still forwarded — the reply is not dropped");
        if (full.size() == 1) {
            const auto dec = tr::wire::decode(full[0]);
            check(dec.has_value() && !dec->children.empty() &&
                      dec->children.back().type != type_t::PATH_REF,
                  "but its mint answer is STRIPPED — a hop that cannot extend must not relay");
            check(dec.has_value() && dec->children.size() == 5,
                  "what is left is an ordinary reply: op, dst, src, kind, payload");
        }
    }

    // ===== 11) an opcode with no known right is dropped, never charged READ =============
    //
    // §6.2 says the ACL is evaluated for "the operation's own right". A hop that does not
    // know an opcode does not know its right, so it cannot evaluate §6.2 for it — and the
    // initialized READ is a GUESS, not an answer. Guessing READ is how a future write-like
    // opcode crosses a READ-only gate. The two arms below are the same frame with one byte
    // changed, so the refusal is the opcode's and nothing else's.
    std::printf("An opcode this build cannot price is dropped at a bound hop (§6.2):\n");
    {
        const auto forward_op = [&](std::uint8_t op_byte) {
            graph_t g;
            (void)g.register_vertex(path_t("/up"), role_t::STORED_VALUE);
            fwd_router_t r(g);
            span_sink_t cli;
            span_sink_t up;
            r.add_child("cli", cli);
            r.add_child("up", up);
            const path_ref_element_t route[2] = {{.index = 1, .generation = 0},
                                                 {.index = 0x0000BEEFu, .generation = 7}};
            r.on_frame("cli",
                       b_fwd_raw(op_byte, b_path_ref(route), b_path({"reply-ep"}), b_value_u32(9)));
            return up.sent.size();
        };
        check(forward_op(kRead) == 1, "the known opcode forwards — the route itself is sound");
        check(forward_op(0x05) == 0,
              "and the SAME frame with an unpriced opcode drops instead of being charged READ");
    }

    // ===== 12) a mint for a RETIRED connection vertex strips ============================
    //
    // `retire` bumps the generation AND clears `registered_`, so an element minted between a
    // retire and the revival that follows already carries the number the SUCCESSOR tenancy
    // will validate under — the validate-on-use stamp (#511) defeated by the side that ISSUES
    // the element rather than the side that honours one. The mint refuses a placeholder for
    // exactly the reason `deref_vertex_slot` does, and a hop that cannot mint strips.
    std::printf("A mint for a retired connection vertex strips rather than stamping (§4.4):\n");
    {
        const path_ref_element_t from_terminus{.index = 9, .generation = 1};
        std::vector<std::byte> mint_tail;
        (void)tr::wire::emit_path_ref(mint_tail,
                                      std::span<const path_ref_element_t>(&from_terminus, 1));
        std::vector<std::byte> body;
        const std::byte op_reply{static_cast<std::uint8_t>(fwd_op_t::REPLY)};
        tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op_reply, 1));
        const std::vector<std::byte> rdst = b_path({"cli", "reply-ep"});
        const std::vector<std::byte> rsrc = b_path({"sensor"});
        body.insert(body.end(), rdst.begin(), rdst.end());
        body.insert(body.end(), rsrc.begin(), rsrc.end());
        const std::byte kind{static_cast<std::uint8_t>(reply_kind_t::RESULT)};
        tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&kind, 1));
        const std::vector<std::byte> payload = b_value_u32(5);
        body.insert(body.end(), payload.begin(), payload.end());
        body.insert(body.end(), mint_tail.begin(), mint_tail.end());
        std::vector<std::byte> reply;
        tr::wire::emit_tlv(reply, type_t::FWD, opt_t{.pl = true}, body);

        const auto relay = [&](bool retire_it) {
            graph_t g;
            const tr::graph::vertex_handle_t up_v =
                g.register_vertex(path_t("/up"), role_t::STORED_VALUE);
            fwd_router_t r(g);
            span_sink_t cli;
            span_sink_t up;
            r.add_child("cli", cli);
            r.add_child("up", up);
            if (retire_it) (void)g.retire(up_v);
            r.on_frame("up", reply);
            return std::move(cli.sent);
        };

        const auto live = relay(false);
        check(live.size() == 1, "the live connection vertex mints");
        if (live.size() == 1) {
            const auto dec = tr::wire::decode(live[0]);
            check(dec.has_value() && !dec->children.empty() &&
                      dec->children.back().type == type_t::PATH_REF,
                  "and the answer carries this hop's element");
        }
        const auto retired = relay(true);
        check(retired.size() == 1, "the reply through a RETIRED connection vertex still forwards");
        if (retired.size() == 1) {
            const auto dec = tr::wire::decode(retired[0]);
            check(dec.has_value() && !dec->children.empty() &&
                      dec->children.back().type != type_t::PATH_REF,
                  "but nothing is minted for a placeholder — the answer is stripped");
        }
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

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
