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
 * Every case runs twice (#1958): once with only the returning `subject_resolver` installed,
 * which reaches the gate through its adapter, and once with only the caller-storage
 * `subject_lookup` (#1781), which the gate and `acl_enforced()` read directly.
 *
 * - the NAME spelling through the BUS mount (`net/tcp-server/srv/<peer>`);
 * - the NAME spelling through the POINT-TO-POINT mount (`net/tcp/x/...`).
 *
 * The bound spelling of a session delivery is a PAIR naming the session's anchor since
 * RFC-0029 S1 retired the `PATH_REF` address; `pair_hop_acl_test` drives it, with every other
 * spelling, against the same `:acl`.
 *
 * A last case needs no bus and no listener: a point-to-point child with NO connection vertex in
 * the graph cannot be mounted at all (#1940), enforcing or not, so a NAME-spelled hop has no
 * link to cross; the same mount with its vertex registered forwards it (the control).
 *
 * The upgrade layout (#2003) pins the ACL layout the 0.18.1 upgrade note recommends, end to
 * end: an inheritable READ and SUBSCRIBE grant on `/net`, READ, WRITE and SUBSCRIBE on each
 * connection vertex once it exists, and no CREATE on `<module>/conn`. A listener session `p0`
 * subscribes, through the node's dial `net/up/b`, to a producer on a second node, and the
 * deliveries come back into its session. The `/net` grant alone (the 0.18.0-era layout) is the
 * control: the subscribe is refused at the dial. The same session's dial attempt through
 * `net/up/conn` is refused, and the identical attempt is admitted once the endpoint grants
 * CREATE. The case also pins that a graph has no root `:acl`: a write to `/:acl` answers
 * `NOT_FOUND`.
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
#include "libtracer/conn_spec.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/loopback.hpp"
#include "libtracer/path.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_vertex.hpp"
#include "test_support.hpp"
#include "test_values.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

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

/** @brief The same resolver in the caller-storage form (#1781): the token goes into @p out. */
std::expected<void, tr::wire::err_t> caller_is_subject_into(void*, std::string_view caller,
                                                            tr::mem::bytes_t& out) {
    if (!out.append(reinterpret_cast<const std::byte*>(caller.data()), caller.size()))
        return std::unexpected(tr::wire::err_t::ACCESS_DENIED);
    return {};
}

/** @brief Which one subject hook an enforcing graph installs (#1958). */
enum class hook_arm_t : std::uint8_t {
    RESOLVER, /**< @brief Only `subject_resolver`, reached through the adapter. */
    LOOKUP,   /**< @brief Only `subject_lookup`, read by the gate directly. */
};

/** @brief The arm the cases run under; `main` runs every case once per arm. */
hook_arm_t g_arm = hook_arm_t::RESOLVER;

/** @brief Install the current arm's subject hook, and only that one, on @p g. */
void enforce(graph_t& g) {
    auto hooks = g.hooks();
    if (g_arm == hook_arm_t::LOOKUP) {
        hooks.subject_lookup = {caller_is_subject_into, nullptr};
    } else {
        hooks.subject_resolver = {caller_is_subject, nullptr};
    }
    g.set_hooks(hooks);
    const auto back = g.hooks();
    tr::testing::check_quiet(
        g_arm == hook_arm_t::LOOKUP
            ? back.subject_lookup.fn != nullptr && back.subject_resolver.fn == nullptr
            : back.subject_resolver.fn != nullptr && back.subject_lookup.fn == nullptr,
        "exactly the arm's one subject hook is installed");
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

/** @brief An `:acl` of one ALLOW ACE for `EVERYONE@` over @p mask, @p flags as given. */
bytes_t everyone_acl(std::uint32_t mask, std::uint8_t flags) {
    tr::graph::ace_t ace = allow(tr::graph::kEveryoneSubject, mask);
    ace.flags = flags;
    return tr::graph::encode_acl(std::span<const tr::graph::ace_t>(&ace, 1));
}

/** @brief A `VALUE` TLV carrying one byte. */
bytes_t b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
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
        std::size_t writes = 0; /**< @brief Of @ref frames, the `FWD{WRITE}`s: deliveries. */
        void operator()(std::span<const std::byte> f) {
            const auto d = tr::wire::decode(f);
            const bool write = d && d->type == type_t::FWD && !d->children.empty() &&
                               d->children[0].type == type_t::VALUE &&
                               !d->children[0].payload.empty() &&
                               d->children[0].payload[0] == static_cast<std::byte>(fwd_op_t::WRITE);
            const std::lock_guard lk(m);
            ++frames;
            if (write) ++writes;
        }
    } sink;
    std::size_t count() {
        const std::lock_guard lk(sink.m);
        return sink.frames;
    }
    /** @brief The `FWD{WRITE}` frames this session received. */
    std::size_t writes() {
        const std::lock_guard lk(sink.m);
        return sink.writes;
    }
    std::unique_ptr<tr::net::tcp_transport_t> link;
};

/** @brief The node under test: a bus mount, a point-to-point mount, two sessions. */
struct node_t {
    graph_t g;
    fwd_router_t router{g};
    std::unique_ptr<tr::net::tcp_server_transport_t> server =
        std::make_unique<tr::net::tcp_server_transport_t>(
            0, tr::net::tcp_server_config_t{.max_peers = 4, .peer_named = true});
    recorder_t p2p;
    std::unique_ptr<client_t> p0;
    std::unique_ptr<client_t> p1;

    node_t() {
        enforce(g);
        check(server->ok(), "listener bound");
        (void)g.register_vertex(path_t("/net/tcp-server/srv"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/net/tcp/x"), role_t::STORED_VALUE);
        check(router.attach_link(std::string(kMount), *server), "bus listener mounted");
        check(router.attach_link("net/tcp/x", p2p), "point-to-point child mounted");
        const bytes_t acl = mount_acl();
        check(g.write(path_t("/net/tcp-server/srv:acl"), make_value(acl)).has_value() &&
                  g.write(path_t("/net/tcp/x:acl"), make_value(acl)).has_value(),
              ":acl written on both connection vertices");
        p0 = std::make_unique<client_t>(server->local_port());
        check(wait_until([&] { return anchor("p0").has_value(); }), "session p0 accepted");
        p1 = std::make_unique<client_t>(server->local_port());
        check(wait_until([&] { return anchor("p1").has_value(); }), "session p1 accepted");
    }
    ~node_t() { quiesce(); }

    /**
     * @brief Join the listener's receive thread, so no delivery is in flight afterwards.
     *
     * A session's frame is handled on the listener's receive thread, so a dial or write it
     * started can still be running when the case body returns. `remove_child` only stops new
     * forwards; the thread is joined by destroying the listener, so this closes the sessions,
     * unmounts, then resets @ref server. A case that owns state the handler reaches (a
     * `transport_vertex_t`, a link) calls this before that state goes out of scope (#2057).
     * Idempotent.
     */
    void quiesce() {
        p1.reset();
        p0.reset();
        (void)router.remove_child(kMount);
        (void)router.remove_child("net/tcp/x");
        server.reset();
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

void vertexless_mount() {
    std::printf("a mount with no connection vertex is refused, so a NAME hop crosses nothing:\n");
    const auto forwards = [](bool enforcing, bool with_vertex) {
        graph_t g;
        if (enforcing) enforce(g);
        fwd_router_t router{g};
        recorder_t bare;
        if (with_vertex) (void)g.register_vertex(path_t("/net/tcp/bare"), role_t::STORED_VALUE);
        check(router.add_child("net/tcp/bare", bare) == with_vertex,
              with_vertex ? "the child mounts at its connection vertex"
                          : "add_child refuses the child with no connection vertex");
        router.on_frame(
            "admin",
            tr::testing::b_fwd(fwd_op_t::WRITE, tr::testing::b_path({"net", "tcp", "bare", "foo"}),
                               tr::testing::b_path({}), {}, b_value_u8(0x5A)));
        const bool out = bare.n.load() != 0;
        (void)router.remove_child("net/tcp/bare");
        return out;
    };
    check(!forwards(true, false), "enforcing: nothing crosses a mount refused for no vertex");
    check(!forwards(false, false), "not enforcing: nor does it there");
    check(forwards(false, true), "with its vertex the same WRITE is forwarded (the control)");
}

/** @brief FIELD{ NAME "subscribers", VALUE u8 ELEMENT }: the `:subscribers[]` append. */
bytes_t b_field_subscribers_append() {
    bytes_t body;
    tr::wire::emit_tlv(body, type_t::NAME, opt_t{},
                       std::as_bytes(std::span<const char>("subscribers", 11)));
    const bytes_t mode = b_value_u8(1);
    body.insert(body.end(), mode.begin(), mode.end());
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief SUBSCRIBER{ PATH @p target }: a plain full-route subscriber. */
bytes_t b_subscriber(std::span<const std::byte> target) {
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, target);
    return out;
}

void upgrade_layout() {
    std::printf("the 0.18.1 upgrade layout admits subscribe and delivery, refuses a dial:\n");
    // The second node: the producer, reached over N's dial. It enforces nothing.
    graph_t g_b;
    fwd_router_t r_b{g_b};
    const vertex_handle_t temp = g_b.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    node_t n;
    tr::net::transport_vertex_t net{n.g, n.router};
    tr::net::loopback_channel_t ch;
    recorder_t b2;
    check(net.register_module("up", "up", tr::net::conn_role_t::DIAL).has_value(),
          "the dial module is declared");
    net.provide_link("up", "b", ch.a());
    net.provide_link("up", "b2", b2);
    check(r_b.attach_link("net/down/a", ch.b()), "the producer node mounts its link to N");
    check(n.g.write(path_t("/net/up/conn"), tr::net::conn_spec_t("b").view()).has_value(),
          "the application creates the dial net/up/b");

    // The layout's first entry: an inheritable READ and SUBSCRIBE grant on `/net`.
    const auto rs = static_cast<std::uint32_t>(acl_right_t::READ) |
                    static_cast<std::uint32_t>(acl_right_t::SUBSCRIBE);
    check(n.g.write(path_t("/net:acl"), make_value(everyone_acl(rs, tr::graph::kAceInherit)))
              .has_value(),
          "/net carries an inheritable READ and SUBSCRIBE grant");

    const bytes_t sub_dst = tr::testing::b_path({"net", "up", "b", "sensor", "temp"});
    const bytes_t reply_ep = tr::testing::b_path({"reply-ep"});
    // Waits for the answer, an admission's or a refusal's, so no sample races the subscribe.
    const auto subscribe = [&] {
        const std::size_t before = n.p0->count();
        n.p0->link->send(tr::testing::b_fwd(fwd_op_t::WRITE, sub_dst, reply_ep,
                                            b_field_subscribers_append(), b_subscriber(reply_ep)));
        return wait_until([&] { return n.p0->count() > before; });
    };
    std::uint8_t sample = 0x10;
    const auto delivered = [&] {
        const std::size_t before = n.p0->writes();
        (void)g_b.write(temp, make_value(b_value_u8(++sample)));
        return wait_until([&] { return n.p0->writes() > before; }, kDropBudget);
    };

    // The control: with only the `/net` grant, the subscribe needs WRITE at the dial.
    check(subscribe(), "/net grant alone: the subscribe is answered");
    check(!delivered(), "/net grant alone: the subscribe through the dial is refused");

    // The layout's second entry: READ, WRITE and SUBSCRIBE on each connection vertex.
    const auto rws = rs | static_cast<std::uint32_t>(acl_right_t::WRITE);
    check(n.g.write(path_t("/net/up/b:acl"), make_value(everyone_acl(rws, 0))).has_value() &&
              n.g.write(path_t("/net/tcp-server/srv:acl"), make_value(everyone_acl(rws, 0)))
                  .has_value(),
          "the dial and the listener carry READ, WRITE and SUBSCRIBE");
    check(subscribe(), "the subscribe is answered");
    check(delivered(), "the subscribe passes the dial and the delivery enters p0's session");
    check(delivered(), "... and every later delivery does too");

    // The layout's third entry: no CREATE on `<module>/conn`, so a session cannot dial.
    const bytes_t conn_dst = tr::testing::b_path({"net", "up", "conn"});
    const bytes_t spec = tr::net::conn_spec_t("b2").bytes();
    const auto dial_from_p0 = [&] {
        n.p0->link->send(tr::testing::b_fwd(fwd_op_t::WRITE, conn_dst, reply_ep, {}, spec));
        return wait_until([&] { return n.g.find(path_t("/net/up/b2").key()).has_value(); },
                          kDropBudget);
    };
    check(!dial_from_p0(), "no CREATE on net/up/conn: p0's dial attempt is refused");
    check(n.g.write(path_t("/net/up/conn:acl"),
                    make_value(everyone_acl(static_cast<std::uint32_t>(acl_right_t::CREATE), 0)))
              .has_value(),
          "(control) the endpoint is granted CREATE");
    check(dial_from_p0(), "(control) the identical dial attempt is admitted");

    // There is no graph-root `:acl`: the write answers NOT_FOUND and installs nothing.
    const auto root = n.g.write(path_t("/:acl"), make_value(everyone_acl(rws, 0)));
    check(!root.has_value() && root.error() == tr::graph::status_t::NOT_FOUND,
          "a write to /:acl answers NOT_FOUND");

    // The last dial may still be running on the listener's receive thread: destroying the
    // listener joins it, before `net`, `b2` and the channel it reaches go out of scope.
    n.quiesce();
    ch.shutdown();
    (void)r_b.remove_child("net/down/a");
}

}  // namespace

/** @brief Every case, under the arm in @ref g_arm. */
static int run_cases() {
    vertexless_mount();
    if constexpr (!tr::net::kBusLinks) return tr::testing::summary("mount_hop_acl");
    name_spelling();
    upgrade_layout();
    return tr::testing::summary("mount_hop_acl");
}

int main() {
    std::printf("== arm 1: only subject_resolver installed ==\n");
    g_arm = hook_arm_t::RESOLVER;
    (void)run_cases();
    std::printf("\n== arm 2: only subject_lookup installed ==\n");
    g_arm = hook_arm_t::LOOKUP;
    return run_cases();
}
