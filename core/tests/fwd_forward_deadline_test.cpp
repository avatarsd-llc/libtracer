/**
 * @file
 * @brief RFC 0028 slice 2 (#1625) — a forwarded request always gets ONE answer in bounded
 *        time: the far end's reply, or the hop's own addressed error.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The defect: a hop that forwarded a READ toward another session kept nothing about it. A far
 * end that never answered — a connected client that serves no graph — left the requester with
 * no reply and no error, so a requester that sends one request at a time stalled behind the
 * silent one until its own timeout tore the session down.
 *
 * Every case runs a real router between two loopback links: `req`, over which a test client
 * sends the request and collects what comes back, and `far`, the session the request is
 * forwarded to. The five cases are the four clauses of #1625 plus the positive control:
 *
 *  1. **A silent far end** — the requester gets `tr::flow::timeout`, addressed to its own
 *     route, no sooner than the deadline and not long after it.
 *  2. **An answering far end** (the control) — the requester gets the real reply and nothing
 *     else; the entry is settled, so no timeout follows it.
 *  3. **A full table** — the forward past `kForwardPendingSlots` is refused AT ONCE with
 *     `tr::flow::backpressure` and never reaches the far end.
 *  4. **A far end that goes away** — its open forwards are answered with
 *     `tr::transport::down` at once, not at the deadline.
 *  5. **Not a READ** — a WRITE, even one asking for an ack, opens no entry and draws no
 *     timeout: a producer streaming acknowledged writes is never refused by a full table.
 *  6. **A late reply** — a requester sending one READ at a time, whose first reply arrives
 *     after the hop already answered it with a timeout: the late reply is dropped (one answer
 *     per request), and it does not settle the second READ, whose own deadline stays live.
 *  7. **`remove_child` of the far end** — the same `tr::transport::down` at once as
 *     `link_down`, through the removal path.
 *  8. **`remove_child` of the requester** — its open forwards are closed with no answer sent:
 *     nobody is left to answer.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/config.hpp"
#include "libtracer/error.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/loopback.hpp"
#include "libtracer/tlv.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::loopback_channel_t;
using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;
using tr::wire::err_t;
using tr::wire::type_t;
using clock_t_ = std::chrono::steady_clock;

/** @brief Every frame one loopback endpoint received, in arrival order. */
struct capture_t {
    std::mutex m;                               /**< @brief Guards @ref frames. */
    std::vector<std::vector<std::byte>> frames; /**< @brief What arrived. */

    /** @brief The receiver: keep a copy of @p f. */
    void operator()(std::span<const std::byte> f) {
        const std::lock_guard lock(m);
        frames.emplace_back(f.begin(), f.end());
    }
    /** @brief How many frames have arrived. */
    std::size_t count() {
        const std::lock_guard lock(m);
        return frames.size();
    }
    /** @brief Frame @p i (a copy). */
    std::vector<std::byte> at(std::size_t i) {
        const std::lock_guard lock(m);
        return frames.at(i);
    }
};

/** @brief What an arrived `FWD{REPLY}` says: its kind, its error code, its `dst`. */
struct reply_t {
    bool is_reply = false;           /**< @brief Decoded as a FWD with op=REPLY. */
    std::uint8_t kind = 0xFF;        /**< @brief `reply_kind_t` as a byte. */
    std::uint16_t code = 0;          /**< @brief The registered error code, if ERROR. */
    std::vector<std::byte> dst_body; /**< @brief The reply's `dst` PATH body. */
};

/** @brief Decode @p frame as a `FWD{REPLY}`. */
reply_t read_reply(std::span<const std::byte> frame) {
    reply_t out;
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->type != type_t::FWD || dec->children.size() < 4) return out;
    const auto& op = dec->children[0];
    out.is_reply = op.payload.size() == 1 &&
                   (std::to_integer<std::uint8_t>(op.payload[0]) & tr::graph::kFwdOpcodeMask) ==
                       static_cast<std::uint8_t>(fwd_op_t::REPLY);
    out.dst_body.assign(dec->children[1].payload.begin(), dec->children[1].payload.end());
    out.kind = tr::detail::load_le<std::uint8_t>(dec->children[3].payload);
    if (dec->children.size() >= 5) {
        const auto& status = dec->children[4];
        if (status.type == type_t::STATUS && !status.children.empty() &&
            status.children[0].type == type_t::ERROR && !status.children[0].children.empty())
            out.code = tr::detail::load_le<std::uint16_t>(status.children[0].children[0].payload);
    }
    return out;
}

/** @brief True when @p r is an addressed `kind=ERROR` reply carrying @p code, sent back to
 *         the client's own one-segment route `cli`. */
bool is_error(const reply_t& r, err_t code) {
    const std::vector<std::byte> cli = b_path({"cli"});
    // The PATH body is everything after its 4-byte header.
    const std::vector<std::byte> cli_body(cli.begin() + 4, cli.end());
    return r.is_reply && r.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::ERROR) &&
           r.code == static_cast<std::uint16_t>(code) && r.dst_body == cli_body;
}

/** @brief Spin until @p pred holds or @p limit passes, calling @p tick each turn. */
template <class Pred, class Tick>
bool wait_for(Pred pred, std::chrono::milliseconds limit, Tick tick) {
    const auto end = clock_t_::now() + limit;
    while (clock_t_::now() < end) {
        tick();
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

/** @brief Milliseconds since @p t0. */
long long ms_since(clock_t_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_t_::now() - t0).count();
}

/**
 * @brief The hop under test: a router with a requester link `req` and a far link `far`.
 *
 * The client holds `req_ch.a()`, whose receiver collects every answer; the far side holds
 * `far_ch.b()`, whose receiver collects every forwarded frame. Neither side answers unless a
 * case makes it.
 */
struct hop_t {
    graph_t graph;
    fwd_router_t router{graph};
    loopback_channel_t req_ch;
    loopback_channel_t far_ch;
    capture_t client;  /**< @brief What came back to the requester. */
    capture_t far_end; /**< @brief What was forwarded to the far end. */

    hop_t() {
        req_ch.a().set_receiver(client);
        far_ch.b().set_receiver(far_end);
        check(router.add_child("req", req_ch.b()), "the requester link is a child");
        check(router.add_child("far", far_ch.a()), "the far link is a child");
    }
    ~hop_t() {
        req_ch.shutdown();
        far_ch.shutdown();
    }
    hop_t(const hop_t&) = delete;
    hop_t& operator=(const hop_t&) = delete;

    /** @brief The client sends a READ of `/far/x`, asking for a reply at `cli`. */
    void read_far() {
        req_ch.a().send(b_fwd(fwd_op_t::READ, b_path({"far", "x"}), b_path({"cli"})));
    }

    /**
     * @brief The far end answers forwarded frame @p i the way a terminus does: a `RESULT`
     *        `FWD{REPLY}` to the accumulated `src`, which this hop grew by the requester
     *        link's mount.
     * @retval false Frame @p i did not decode.
     */
    bool far_answers(std::size_t i) {
        // Kept alive for the decode: the decoded tree's payload spans view these bytes.
        const std::vector<std::byte> forwarded = far_end.at(i);
        const auto fwd = tr::wire::decode(forwarded);
        if (!fwd || fwd->children.size() < 3) return false;
        std::vector<std::byte> reply_dst;
        tr::wire::emit_tlv(reply_dst, type_t::PATH, tr::wire::opt_t{}, fwd->children[2].payload);
        std::vector<std::byte> kind;
        const std::byte ok{static_cast<std::uint8_t>(tr::graph::reply_kind_t::RESULT)};
        tr::wire::emit_tlv(kind, type_t::VALUE, tr::wire::opt_t{},
                           std::span<const std::byte>(&ok, 1));
        far_ch.b().send(b_fwd(fwd_op_t::REPLY, reply_dst, b_path({"x"}), {}, kind));
        return true;
    }
};

/** @brief Case 1: a silent far end costs the requester one deadline, then an answer. */
void test_silent_far_end_times_out() {
    std::printf("a silent far end: the requester gets tr::flow::timeout after the deadline:\n");
    hop_t h;
    const auto t0 = clock_t_::now();
    h.read_far();
    check(wait_for([&] { return h.far_end.count() == 1; }, 2s, [] {}),
          "  the READ was forwarded to the far end");
    check(h.router.forward_stats().in_use == 1, "  and the hop holds one open entry for it");

    // The node's periodic loop: nothing else flows, so the embedder's tick is the timer.
    const bool answered =
        wait_for([&] { return h.client.count() >= 1; }, tr::net::kForwardDeadline + 2s,
                 [&] { (void)h.router.expire_forwards(); });
    const long long took = ms_since(t0);
    check(answered, "  the requester got an answer instead of silence");
    if (!answered) return;
    check(is_error(read_reply(h.client.at(0)), err_t::FLOW_TIMEOUT),
          "  it is an addressed kind=ERROR tr::flow::timeout, routed to the requester's src");
    check(took >= tr::net::kForwardDeadline.count(), "  no sooner than the deadline");
    check(took < tr::net::kForwardDeadline.count() + 500, "  and not long after it");
    const auto st = h.router.forward_stats();
    check(st.in_use == 0 && st.expired == 1, "  the entry is closed and counted as expired");
    std::printf("  answered after %lld ms (deadline %lld ms)\n", took,
                static_cast<long long>(tr::net::kForwardDeadline.count()));
}

/** @brief Case 2 (control): an answering far end settles the entry; no timeout follows. */
void test_answered_forward_is_settled() {
    std::printf("an answering far end: the real reply, and no timeout after it:\n");
    hop_t h;
    h.read_far();
    check(wait_for([&] { return h.far_end.count() == 1; }, 2s, [] {}),
          "  the READ was forwarded to the far end");
    check(h.far_answers(0), "  the forwarded frame decodes and the far end answers it");

    check(wait_for([&] { return h.client.count() >= 1; }, 2s, [] {}),
          "  the reply reached the requester");
    check(h.router.forward_stats().in_use == 0, "  and settled the hop's entry");
    // Past the deadline, the tick finds nothing to answer.
    std::this_thread::sleep_for(tr::net::kForwardDeadline + 100ms);
    check(h.router.expire_forwards() == 0, "  so the deadline passes without a timeout");
    std::this_thread::sleep_for(50ms);
    check(h.client.count() == 1, "  the requester got exactly one answer");
    const reply_t r = read_reply(h.client.at(0));
    check(r.is_reply && r.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::RESULT),
          "  and it is the far end's own RESULT");
}

/** @brief Case 3: a full table refuses the next forward at once, by value. */
void test_full_table_refuses_at_once() {
    std::printf("a full table refuses the next forward at once with tr::flow::backpressure:\n");
    hop_t h;
    constexpr std::size_t slots = tr::net::fwd_pending_t::kSlots;
    for (std::size_t i = 0; i < slots; ++i) h.read_far();
    check(wait_for([&] { return h.far_end.count() == slots; }, 2s, [] {}),
          "  every forward that fits reached the far end");
    check(h.router.forward_stats().in_use == slots, "  and the table is full");

    const auto t0 = clock_t_::now();
    h.read_far();
    check(wait_for([&] { return h.client.count() >= 1; }, 1s, [] {}),
          "  the next one was answered");
    const long long took = ms_since(t0);
    if (h.client.count() == 0) return;
    check(is_error(read_reply(h.client.at(0)), err_t::FLOW_BACKPRESSURE),
          "  with an addressed tr::flow::backpressure");
    check(took < tr::net::kForwardDeadline.count(), "  at once, not at a deadline");
    check(h.far_end.count() == slots, "  and it never reached the far end");
    check(h.router.forward_stats().refused == 1, "  the refusal is counted");
}

/** @brief Case 4: a far end that goes away resolves its open forwards at once. */
void test_departed_far_end_resolves_at_once() {
    std::printf("a far end that goes away: tr::transport::down at once, not at the deadline:\n");
    hop_t h;
    h.read_far();
    check(wait_for([&] { return h.far_end.count() == 1; }, 2s, [] {}),
          "  the READ was forwarded to the far end");
    const auto t0 = clock_t_::now();
    h.router.link_down("far");
    check(wait_for([&] { return h.client.count() >= 1; }, 1s, [] {}),
          "  the requester was answered");
    const long long took = ms_since(t0);
    if (h.client.count() == 0) return;
    check(is_error(read_reply(h.client.at(0)), err_t::TRANSPORT_DOWN),
          "  with an addressed tr::transport::down");
    check(took < tr::net::kForwardDeadline.count(), "  at once, not at the deadline");
    check(h.router.forward_stats().in_use == 0, "  and the entry is closed");
}

/** @brief Case 5: a WRITE opens no entry, so an acknowledged write stream is never refused. */
void test_write_opens_nothing() {
    std::printf("a WRITE is not bounded, so a write stream never fills the table:\n");
    hop_t h;
    std::vector<std::byte> value;
    const std::byte one{1};
    tr::wire::emit_tlv(value, type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(&one, 1));
    constexpr std::size_t writes = tr::net::fwd_pending_t::kSlots + 4;
    for (std::size_t i = 0; i < writes; ++i)
        h.req_ch.a().send(b_fwd(fwd_op_t::WRITE, b_path({"far", "x"}), b_path({"cli"}), {}, value));
    check(wait_for([&] { return h.far_end.count() == writes; }, 2s, [] {}),
          "  every WRITE was forwarded, past the table's size");
    check(h.router.forward_stats().in_use == 0, "  and none opened an entry");
    std::this_thread::sleep_for(tr::net::kForwardDeadline + 100ms);
    check(h.router.expire_forwards() == 0 && h.client.count() == 0,
          "  so nothing is ever answered on their behalf");
}

/** @brief Case 6: a late reply is dropped, and does not settle the requester's next READ. */
void test_late_reply_is_dropped_not_settling_the_next() {
    std::printf("a late reply is dropped and leaves the next READ's deadline live:\n");
    hop_t h;
    const auto tick = [&] { (void)h.router.expire_forwards(); };
    // READ A: the far end is slow, so the hop answers A itself at the deadline.
    h.read_far();
    check(wait_for([&] { return h.far_end.count() == 1; }, 2s, [] {}),
          "  READ A was forwarded to the far end");
    check(wait_for([&] { return h.client.count() >= 1; }, tr::net::kForwardDeadline + 2s, tick),
          "  A was answered by the hop");
    if (h.client.count() == 0) return;
    check(is_error(read_reply(h.client.at(0)), err_t::FLOW_TIMEOUT), "  with tr::flow::timeout");

    // READ B, one at a time: same link, same return route, so the same key as A.
    const auto t_b = clock_t_::now();
    h.read_far();
    check(wait_for([&] { return h.far_end.count() == 2; }, 2s, [] {}),
          "  READ B was forwarded to the far end");
    // A's reply finally arrives. It byte-matches B's stored route too.
    check(h.far_answers(0), "  the far end answers A, late");
    std::this_thread::sleep_for(50ms);
    check(h.client.count() == 1, "  the late reply was NOT forwarded: A has exactly one answer");
    const auto st = h.router.forward_stats();
    check(st.late == 1, "  it is counted as late");
    check(st.in_use == 1, "  and B's entry is still open — the late reply did not settle it");

    // B's far end stays silent: B still gets its own bounded answer.
    check(wait_for([&] { return h.client.count() >= 2; }, tr::net::kForwardDeadline + 2s, tick),
          "  B was answered too");
    const long long took = ms_since(t_b);
    if (h.client.count() < 2) return;
    check(is_error(read_reply(h.client.at(1)), err_t::FLOW_TIMEOUT),
          "  with its own tr::flow::timeout");
    check(took >= tr::net::kForwardDeadline.count(), "  at B's own deadline");
    std::this_thread::sleep_for(50ms);
    check(h.client.count() == 2, "  two requests, two answers");
}

/** @brief Case 7: removing the far child answers its open forwards at once. */
void test_removed_far_end_resolves_at_once() {
    std::printf("remove_child of the far end: tr::transport::down at once:\n");
    hop_t h;
    h.read_far();
    check(wait_for([&] { return h.far_end.count() == 1; }, 2s, [] {}),
          "  the READ was forwarded to the far end");
    const auto t0 = clock_t_::now();
    check(h.router.remove_child("far"), "  the far child was removed");
    check(wait_for([&] { return h.client.count() >= 1; }, 1s, [] {}),
          "  the requester was answered");
    const long long took = ms_since(t0);
    if (h.client.count() == 0) return;
    check(is_error(read_reply(h.client.at(0)), err_t::TRANSPORT_DOWN),
          "  with an addressed tr::transport::down");
    check(took < tr::net::kForwardDeadline.count(), "  at once, not at the deadline");
    check(h.router.forward_stats().in_use == 0, "  and the entry is closed");
}

/** @brief Case 8: removing the requester closes its open forwards with no answer. */
void test_removed_requester_is_forgotten() {
    std::printf("remove_child of the requester: its forwards close, nothing is sent:\n");
    hop_t h;
    h.read_far();
    check(wait_for([&] { return h.far_end.count() == 1; }, 2s, [] {}),
          "  the READ was forwarded to the far end");
    check(h.router.forward_stats().in_use == 1, "  one entry is open");
    check(h.router.remove_child("req"), "  the requester child was removed");
    check(h.router.forward_stats().in_use == 0, "  its entry is closed at once");
    std::this_thread::sleep_for(tr::net::kForwardDeadline + 100ms);
    check(h.router.expire_forwards() == 0, "  so the deadline finds nothing to answer");
    check(h.client.count() == 0, "  and nothing was sent to the departed requester");
}

}  // namespace

int main() {
    if constexpr (tr::net::fwd_pending_t::kSlots == 0)
        return tr::testing::skipped("fwd_forward_deadline",
                                    "this build sets kForwardPendingSlots = 0: forwards are "
                                    "unbounded by configuration");
    std::printf("== RFC 0028 slice 2 / #1625: a forwarded request always gets one answer ==\n");
    test_silent_far_end_times_out();
    test_answered_forward_is_settled();
    test_full_table_refuses_at_once();
    test_departed_far_end_resolves_at_once();
    test_write_opens_nothing();
    test_late_reply_is_dropped_not_settling_the_next();
    test_removed_far_end_resolves_at_once();
    test_removed_requester_is_forgotten();
    return tr::testing::summary("fwd_forward_deadline_test");
}
