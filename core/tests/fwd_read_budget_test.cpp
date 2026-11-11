/**
 * @file
 * @brief #2774 — the per-session READ budget under contention: a READ beyond a session's N-th
 *        in the current run is answered `tr::flow::backpressure` while two or more sessions
 *        READ in that run, and never otherwise.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ## What the tests assert
 *
 * - Off by default: `default_config_t::kReadBudget` is 0 and every member the budget adds is an
 *   empty type, so a build that does not opt in carries no byte of it (compile-time checks).
 * - The contention census: N+1 READs from each of two sessions in one run draw exactly one
 *   `BACKPRESSURE` reply each, the (N+1)-th; closing the run resets both counts.
 * - A session alone in a run is never refused, however many READs it sends; the first READ of
 *   a second session puts the first one's surplus under the budget.
 * - Under contention, with both sessions over budget: a WRITE, a SUBSCRIBE, a REPLY, a READ
 *   this node forwards and a READ through the public by-name door are never refused.
 * - `close_read_run` is safe from other threads: two closers racing each other while two
 *   sessions READ on their own threads leaves every READ answered exactly once (run under TSan).
 * - The session is the stream server's SLOT: two real TCP clients of one listener are two
 *   sessions, each counted in its own slot, on the flat tier and (where the build carries the
 *   bus module) the peer-named tier.
 *
 * The test preset turns the budget on at 254; a leg that binds its own fragment has it off,
 * and the behavioural cases print SKIP and the test exits 77.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/error.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_tcp.hpp"
#include "test_support.hpp"
#include "tlv_tree.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;

using bytes_t = std::vector<std::byte>;

static_assert(tr::graph::default_config_t::kReadBudget == 0, "the READ budget is off by default");
static_assert(std::is_empty_v<tr::net::no_read_tally_t> &&
                  std::is_empty_v<tr::net::read_census_t<false>>,
              "off, every member the budget adds is empty: 0 B with [[no_unique_address]]");
static_assert(sizeof(tr::net::read_tally_t) == 4, "on, a session's tally is one word");

/** @brief What a reply frame answered: a result, a `BACKPRESSURE`, or something else. */
enum class answer_t : std::uint8_t { RESULT, BACKPRESSURE, OTHER };

/** @brief Classify one `FWD{REPLY}` by its kind and, for an error, its registered code. */
answer_t classify(std::span<const std::byte> frame) {
    const auto d = tr::wire::decode(frame);
    if (!d || d->type != type_t::FWD || d->children.size() < 4) return answer_t::OTHER;
    const auto& c = d->children;
    if (c[3].type != type_t::VALUE || c[3].payload.size() != 1) return answer_t::OTHER;
    const auto kind = static_cast<reply_kind_t>(std::to_integer<std::uint8_t>(c[3].payload[0]));
    if (kind == reply_kind_t::RESULT) return answer_t::RESULT;
    if (kind != reply_kind_t::ERROR || c.size() != 5 || c[4].children.size() != 1)
        return answer_t::OTHER;
    const auto& err = c[4].children[0];
    if (err.children.size() != 1 || err.children[0].payload.size() != 2) return answer_t::OTHER;
    return tr::detail::load_le<std::uint16_t>(err.children[0].payload) ==
                   std::to_underlying(tr::wire::err_t::FLOW_BACKPRESSURE)
               ? answer_t::BACKPRESSURE
               : answer_t::OTHER;
}

/** @brief Tally of the replies one session received. */
struct answers_t {
    std::size_t result = 0;       /**< @brief RESULT replies. */
    std::size_t backpressure = 0; /**< @brief `tr::flow::backpressure` replies. */
    std::size_t other = 0;        /**< @brief Anything else. */
    void add(std::span<const std::byte> frame) {
        switch (classify(frame)) {
            case answer_t::RESULT:
                ++result;
                break;
            case answer_t::BACKPRESSURE:
                ++backpressure;
                break;
            case answer_t::OTHER:
                ++other;
                break;
        }
    }
};

/** @brief A point-to-point link — one session — that records the replies sent down it. */
struct session_link_t : transport_t {
    answers_t got;
    std::size_t sent = 0; /**< @brief Frames sent down this link, replies or not. */
    void send(std::span<const std::byte> f) override {
        ++sent;
        got.add(f);
    }
    void send(std::span<const std::span<const std::byte>> parts) override {
        bytes_t f;
        for (const auto& p : parts) f.insert(f.end(), p.begin(), p.end());
        send(std::span<const std::byte>(f));
    }
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    /** @brief Send @p n READs of `/sensor/temp` up this link. */
    void reads(std::size_t n) {
        const bytes_t read = b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"back"}));
        for (std::size_t i = 0; i < n; ++i) inject(read);
    }
};

/** @brief A VALUE TLV carrying one byte. */
bytes_t b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief FIELD{ NAME "subscribers", VALUE u8 ELEMENT }: the `:subscribers[]` append. */
bytes_t b_field_subscribers_append() {
    bytes_t body;
    tr::wire::emit_name(body, "subscribers");
    const bytes_t mode = b_value_u8(1);
    body.insert(body.end(), mode.begin(), mode.end());
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief A SUBSCRIBER TLV whose target route is `/back`. */
bytes_t b_subscriber() {
    const bytes_t body = b_path({"back"});
    bytes_t out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
    return out;
}

/** @brief Register `/sensor/temp` on @p g holding a one-byte value. */
void seed(graph_t& g) {
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    const bytes_t v = b_value_u8(42);
    check(g.write(*path_t::parse("/sensor/temp"), tr::view::rope_t{*tr::view::over_bytes(v)})
              .has_value(),
          "/sensor/temp holds a value");
}

/** @brief The node: one value, two session links, a mount `c` a READ can be forwarded to. */
struct node_t {
    graph_t g;
    fwd_router_t router{g};
    session_link_t a;
    session_link_t b;
    session_link_t c;
    node_t() {
        seed(g);
        check(
            router.attach_link("a", a) && router.attach_link("b", b) && router.attach_link("c", c),
            "links a, b and the mount c attach");
    }
};

void census(std::size_t n) {
    std::printf("the contention census: N+1 READs from each of two sessions in one run\n");
    node_t node;
    node.a.reads(1);
    node.b.reads(1);
    node.a.reads(n);
    node.b.reads(n);
    check(node.a.got.result == n && node.a.got.backpressure == 1 && node.a.got.other == 0,
          "  a: N results, then exactly one BACKPRESSURE");
    check(node.b.got.result == n && node.b.got.backpressure == 1 && node.b.got.other == 0,
          "  b: N results, then exactly one BACKPRESSURE");
    node.router.close_read_run();
    node.a.reads(1);
    node.b.reads(1);
    node.a.reads(n - 1);
    node.b.reads(n - 1);
    check(node.a.got.backpressure == 1 && node.b.got.backpressure == 1,
          "  closing the run reset both counts: N more READs each, none refused");
    node.a.reads(1);
    node.b.reads(1);
    check(node.a.got.backpressure == 2 && node.b.got.backpressure == 2,
          "  and the (N+1)-th of the new run is refused again");
}

void sole_session(std::size_t n) {
    std::printf("a session alone in a run is never refused\n");
    node_t node;
    node.a.reads(3 * n);
    check(node.a.got.result == 3 * n && node.a.got.backpressure == 0,
          "  3N READs from a alone: every one answered");
    node.b.reads(1);
    check(node.b.got.result == 1, "  b's first READ is answered");
    node.a.reads(1);
    check(node.a.got.backpressure == 1, "  now two sessions read: a's surplus is refused");
    node.router.close_read_run();
    node.a.reads(3 * n);
    check(node.a.got.backpressure == 1, "  the next run, a alone again: none refused");
}

void never_refused(std::size_t n) {
    std::printf("under contention, only READs are refused\n");
    node_t node;
    node.a.reads(1);
    node.b.reads(1);
    node.a.reads(n);
    node.b.reads(n);
    check(node.a.got.backpressure == 1 && node.b.got.backpressure == 1,
          "  both sessions are over budget (the control)");
    const answers_t before = node.a.got;
    node.a.inject(
        b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"back"}), {}, b_value_u8(7)));
    check(node.a.got.result == before.result + 1 && node.a.got.backpressure == 1,
          "  a WRITE is acknowledged");
    node.a.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"back"}),
                        b_field_subscribers_append(), b_subscriber()));
    check(node.a.got.result == before.result + 2 && node.a.got.backpressure == 1,
          "  a SUBSCRIBE is acknowledged");
    std::size_t replies = 0;
    node.router.on_reply(
        [](void* ctx, const tr::view::rope_t&) { ++*static_cast<std::size_t*>(ctx); }, &replies);
    node.a.inject(
        tr::testing::b_fwd_reply(reply_kind_t::RESULT, b_path({}), b_path({"x"}), b_value_u8(1)));
    check(replies == 1 && node.a.got.backpressure == 1, "  a REPLY reaches the reply sink");
    const std::size_t c_before = node.c.sent;
    node.a.inject(b_fwd(fwd_op_t::READ, b_path({"c", "deep"}), b_path({"back"})));
    check(node.c.sent == c_before + 1 && node.a.got.backpressure == 1,
          "  a READ this node forwards goes on to the mount");
    node.a.reads(1);
    check(node.a.got.backpressure == 2, "  and a's next READ here is still refused (the control)");
    node.router.on_frame("a", b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"back"})));
    check(node.a.got.backpressure == 2,
          "  a READ through the public by-name door has no session and is not refused");
}

void concurrent_close() {
    std::printf("close_read_run from two other threads while two sessions READ\n");
    node_t node;
    constexpr std::size_t kReads = 4000;
    std::atomic<int> readers_left{2};
    std::atomic<std::size_t> closes{0};
    // Each session's READs come from its own thread, as each link's receive thread delivers
    // them; two more threads close runs without pause until both have finished, so closes
    // also race each other.
    const auto close_loop = [&] {
        do {
            node.router.close_read_run();
            closes.fetch_add(1, std::memory_order_relaxed);
        } while (readers_left.load(std::memory_order_acquire) != 0);
    };
    std::thread closer(close_loop);
    std::thread closer2(close_loop);
    std::thread ta([&] {
        node.a.reads(kReads);
        readers_left.fetch_sub(1, std::memory_order_release);
    });
    std::thread tb([&] {
        node.b.reads(kReads);
        readers_left.fetch_sub(1, std::memory_order_release);
    });
    ta.join();
    tb.join();
    closer.join();
    closer2.join();
    check(closes.load() >= 2, "  the closers ran");
    check(node.a.got.result + node.a.got.backpressure == kReads && node.a.got.other == 0,
          "  a: every READ answered once, with a result or BACKPRESSURE");
    check(node.b.got.result + node.b.got.backpressure == kReads && node.b.got.other == 0,
          "  b: every READ answered once, with a result or BACKPRESSURE");
    node.router.close_read_run();
    const answers_t before = node.a.got;
    node.a.reads(3);
    check(node.a.got.result == before.result + 3,
          "  after the threads stop, a fresh run serves a session alone");
}

/** @brief Poll @p f until it holds or the deadline passes; the final answer is returned. */
template <class Fn>
bool wait_until(Fn f, std::chrono::milliseconds timeout = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return f();
}

/** @brief A real TCP client of the listener, tallying the replies it receives. */
struct tcp_client_t {
    explicit tcp_client_t(std::uint16_t port)
        : link(std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", port,
                                                          tr::net::tcp_config_t{})) {
        link->set_receiver(sink);
        link->start_receiving();
    }
    /** @brief The receiver: one locked tally. */
    struct sink_t {
        std::mutex m;
        answers_t got;
        void operator()(std::span<const std::byte> f) {
            const std::lock_guard lk(m);
            got.add(f);
        }
    } sink;
    answers_t got() {
        const std::lock_guard lk(sink.m);
        return sink.got;
    }
    /** @brief Every reply this client has received. */
    std::size_t total() {
        const answers_t a = got();
        return a.result + a.backpressure + a.other;
    }
    /** @brief Send @p n READs of `/sensor/temp`. */
    void reads(std::size_t n) {
        const bytes_t read = b_fwd(fwd_op_t::READ, b_path({"sensor", "temp"}), b_path({"back"}));
        for (std::size_t i = 0; i < n; ++i) link->send(read);
    }
    std::unique_ptr<tr::net::tcp_transport_t> link;
};

void tcp_slots(std::size_t n, bool peer_named) {
    std::printf("two TCP clients of one %s listener are two sessions\n",
                peer_named ? "peer-named" : "flat");
    graph_t g;
    seed(g);
    fwd_router_t router(g);
    auto server = std::make_unique<tr::net::tcp_server_transport_t>(
        0, tr::net::tcp_server_config_t{.max_peers = 4, .peer_named = peer_named});
    check(server->ok() && router.attach_link("srv", *server), "  the listener mounts");
    auto p = std::make_unique<tcp_client_t>(server->local_port());
    auto q = std::make_unique<tcp_client_t>(server->local_port());
    // A flat listener has one routing identity, so its replies reach every client: each client
    // then sees both sessions' answers. A peer-named one answers the asking session only.
    std::size_t want_p = 0;
    std::size_t want_q = 0;
    const auto step = [&](tcp_client_t& from, std::size_t k) {
        from.reads(k);
        want_p += (&from == p.get() || !peer_named) ? k : 0;
        want_q += (&from == q.get() || !peer_named) ? k : 0;
        return wait_until([&] { return p->total() == want_p && q->total() == want_q; });
    };
    check(step(*p, 1) && step(*q, 1), "  each client's first READ is answered");
    check(step(*p, n) && step(*q, n), "  N more READs each are answered");
    const std::size_t seen = peer_named ? 1 : 2;  // the sessions whose replies a client sees
    const answers_t pa = p->got();
    const answers_t qa = q->got();
    check(pa.result == seen * n && pa.backpressure == seen && qa.result == seen * n &&
              qa.backpressure == seen,
          "  one BACKPRESSURE per session: each is counted in its own slot");
    router.close_read_run();
    check(step(*p, n) && p->got().backpressure == seen,
          "  after a close, N more from p: none refused");
    q.reset();
    p.reset();
    (void)router.remove_child("srv");
    server.reset();
}

}  // namespace

int main() {
    std::printf("#2774 per-session READ budget under contention\n\n");
    if constexpr (!tr::net::kReadBudgetOn) {
        std::printf("SKIP: this build's fragment leaves the READ budget off\n");
        return 77;
    } else {
        const std::size_t n = tr::graph::config_t::kReadBudget;
        census(n);
        sole_session(n);
        never_refused(n);
        concurrent_close();
        tcp_slots(n, /*peer_named=*/false);
        if constexpr (tr::net::kBusLinks) tcp_slots(n, /*peer_named=*/true);
        return tr::testing::summary("fwd_read_budget");
    }
}
