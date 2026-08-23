/**
 * @file
 * @brief #1494 — a reply serviced IN-CALL leaves by the socket, not by the control queue.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #949 TX-pool, #835 send-stall and #954 session-identity suites:
 * the REAL chip translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`)
 * compiled against the host fake of `esp_http_server` (fake_httpd.hpp).
 *
 * The reported defect was a WS session that stopped answering PERMANENTLY once a client
 * pipelined writes past the link's TX depth: throughput degraded, then the session went
 * silent and a later READ on it timed out while the node stayed healthy. The mechanism
 * that survived audit is the reply leg's dependence on the httpd control mbox: the in-call
 * reserve (`tx_reply_reserve`) is claimable once per DRAIN, not once per request — the
 * httpd task takes one control message per server pass — so a pipelining client is
 * serviced again before the previous reply's slot comes back, its reply falls into the
 * general pool where it competes with subscription pushes, and an in-call send never waits
 * for the pool (it would be waiting on its own stack frame, the #814 deadlock). Every
 * reply past that point is dropped.
 *
 * The fix takes the reply leg off the queue entirely: a send issued from inside this
 * link's own handler frame writes straight to the socket, which is the task it was going
 * to be marshalled onto anyway. What that has to buy, and what is pinned here:
 *   1. the reply reaches the wire DURING the handler call — before anything drains — and
 *      the control queue is offered nothing;
 *   2. it survives the exact conditions that used to lose it: a fully claimed TX pool AND
 *      a control queue that refuses every enqueue. This is the case that fails against the
 *      pre-fix code, and it is the issue's headline;
 *   3. a ROPE reply still crosses as ONE WebSocket frame, gathered without a second
 *      buffer, and the slot it borrows for the gather is back in the pool on return;
 *   4. an off-task producer is UNCHANGED — it still marshals through the queue, which is
 *      what the queue is for. Without this the fix would be "stop using the queue", which
 *      is not safe from a task that does not own the descriptor.
 */

#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_httpd.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"

namespace {

using fake_httpd::send_result_t;
using tr::net::httpd_ws_link_t;
using tr::net::peer_handle_t;

int g_failures = 0;

/** @brief Record one assertion's verdict. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief Record a verdict that also reports the number that decided it. */
void check_eq(std::size_t got, std::size_t want, std::string_view what) {
    const bool ok = got == want;
    std::printf("  [%s] %.*s (got %zu, want %zu)\n", ok ? "PASS" : "FAIL",
                static_cast<int>(what.size()), what.data(), got, want);
    if (!ok) ++g_failures;
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief The request body every case delivers — the link only has to route it. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/** @brief The first half of a two-segment reply rope. */
const std::byte kHead[] = {std::byte{0xA0}, std::byte{0xA1}};

/** @brief The second half of it. */
const std::byte kTail[] = {std::byte{0xB0}, std::byte{0xB1}, std::byte{0xB2}};

/** @brief The one socket every case here serves. */
constexpr int kFd = 700;

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief A peer-named link that adopts the fake server (the directed-reply mode). */
std::unique_ptr<httpd_ws_link_t> make_link() {
    return std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
}

/**
 * @brief The application, standing in for `fwd_router`: on every inbound frame, answer the
 *        peer that sent it — through the endpoint resolved BEFORE the call, exactly as a
 *        forwarding hop holds a resolution across its dispatch.
 *
 * Installed with @ref tr::net::transport_t::set_peer_receiver, so it runs INSIDE the link's
 * WebSocket handler frame. That is the whole instrument: nothing else about a host test
 * distinguishes "the httpd task" from "the thread the test runs on".
 */
class replier_t {
   public:
    /** @brief Answer the next @p n inbound frames with @p parts (borrowed, must outlive). */
    void arm(tr::net::transport_t* to, std::span<const std::span<const std::byte>> parts) {
        to_ = to;
        parts_ = parts;
    }
    /** @brief Stop answering (the claim frame must not draw a reply of its own). */
    void disarm() { to_ = nullptr; }
    /** @brief The inbound sink. */
    void operator()(peer_handle_t, std::span<const std::byte>) {
        if (to_ != nullptr) to_->send(parts_);
    }

   private:
    tr::net::transport_t* to_ = nullptr;                /**< @brief Where the reply goes. */
    std::span<const std::span<const std::byte>> parts_; /**< @brief What it says. */
};

/** @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim). */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief The directed endpoint of the single peer currently open. */
tr::net::transport_t* only_peer(httpd_ws_link_t& link) {
    std::string name;
    link.enumerate_peers([&name](std::string_view p) { name = std::string(p); });
    return name.empty() ? nullptr : link.peer_link(name);
}

/** @brief Retire the link, the fake's sessions and its queue settings between cases. */
void reset(std::unique_ptr<httpd_ws_link_t>& link) {
    link.reset();
    fake_httpd::instance().set_queue_refusing(false);
    fake_httpd::instance().close_all();
    fake_httpd::instance().clear_sent_frames();
    drain();
}

/** @brief True when @p got is @p want, byte for byte. */
bool same_bytes(std::span<const std::byte> got, std::span<const std::byte> want) {
    if (got.size() != want.size()) return false;
    for (std::size_t i = 0; i < got.size(); ++i)
        if (got[i] != want[i]) return false;
    return true;
}

// ---------------------------------------------------------------------------
// 1 — the reply is on the wire before anything drains, and nothing was queued.
// ---------------------------------------------------------------------------
/**
 * @brief The routing decision itself, measured on both of its effects.
 *
 * A queued reply is invisible until the httpd task's next pass, so "sent before any drain"
 * is exactly the statement "it did not go through the queue" — and the queue's own depth
 * says the same thing from the other side. The claimed-slot count is the third: the pooled
 * path claims one for the gather and holds it until the item drains, so a reply that
 * borrowed nothing leaves the pool reading zero all the way through.
 */
void test_an_in_call_reply_bypasses_the_queue() {
    std::printf("a reply issued inside the handler frame:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");
    replier_t app;
    link->set_peer_receiver(app);
    claim(kFd);
    drain();
    tr::net::transport_t* const to = only_peer(*link);
    check(to != nullptr, "the peer resolved to a directed endpoint");
    if (to == nullptr) return;
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});
    fake_httpd::instance().clear_sent_frames();

    const std::span<const std::byte> reply[] = {std::span<const std::byte>(kHead)};
    app.arm(to, reply);
    const std::uint32_t drops_before = link->enqueue_drops();
    (void)fake_httpd::instance().deliver_frame(kFd, kBody);

    const auto sent = fake_httpd::instance().sent_frames();
    check_eq(sent.size(), 1, "the reply was on the wire before anything drained");
    if (sent.size() == 1) {
        check(sent[0].fd == kFd, "to the socket that asked");
        check(sent[0].type == HTTPD_WS_TYPE_BINARY, "as a BINARY frame");
        check(same_bytes(sent[0].payload, kHead), "carrying the reply bytes");
    }
    check_eq(fake_httpd::instance().queue_depth(), 0, "the control queue was offered nothing");
    check_eq(link->tx_slots_busy(), 0, "and no TX slot was claimed for it");
    check_eq(link->enqueue_drops() - drops_before, 0, "nothing was dropped");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 — the wedge: a full pool AND a refusing control queue cannot lose the reply.
// ---------------------------------------------------------------------------
/**
 * @brief #1494's headline, staged in one call.
 *
 * The two conditions the reported session met before it went silent, imposed together and
 * deliberately harder than the field case: every pool slot is claimed by pushes that have
 * not drained, and the control socket refuses every further enqueue. Against the pre-fix
 * code the reply reaches `queue_send`, finds the pool exhausted or its enqueue refused,
 * and is dropped and counted — the session answers nothing and the client's read times
 * out. The reply leg no longer consults either resource, so it goes out regardless, and
 * the pushes' slots are still theirs afterwards.
 */
void test_a_full_pool_and_a_refused_queue_do_not_wedge_the_reply() {
    std::printf("a reply against a full pool and a refusing control queue:\n");
    auto link = make_link();
    replier_t app;
    link->set_peer_receiver(app);
    claim(kFd);
    drain();
    tr::net::transport_t* const to = only_peer(*link);
    check(to != nullptr, "the peer resolved to a directed endpoint");
    if (to == nullptr) return;
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    // Fill the pool from ANOTHER task, which is the only producer that can: these are
    // queued and left undrained, so every slot they claimed stays claimed.
    const std::size_t capacity = httpd_ws_link_t::kDefaultTxPoolSlots;
    std::thread producer([to, capacity] {
        for (std::size_t i = 0; i < capacity; ++i) to->send(std::span<const std::byte>(kBody));
    });
    producer.join();
    check_eq(link->tx_slots_busy(), capacity, "the pool is fully claimed by the pushes");

    fake_httpd::instance().set_queue_refusing(true);
    fake_httpd::instance().clear_sent_frames();
    const std::span<const std::byte> reply[] = {std::span<const std::byte>(kHead)};
    app.arm(to, reply);
    const std::uint32_t drops_before = link->enqueue_drops();
    (void)fake_httpd::instance().deliver_frame(kFd, kBody);

    const auto sent = fake_httpd::instance().sent_frames();
    check_eq(sent.size(), 1, "the reply went out anyway");
    if (sent.size() == 1) check(same_bytes(sent[0].payload, kHead), "and it is the reply");
    check_eq(link->enqueue_drops() - drops_before, 0, "it was not dropped");
    check_eq(link->tx_slots_busy(), capacity, "and it took none of the pushes' slots");

    // The session is not merely answered once: with the congestion gone it keeps serving,
    // which is what "permanent wedge" would deny.
    fake_httpd::instance().set_queue_refusing(false);
    drain();
    check_eq(link->tx_slots_busy(), 0, "the pushes drained and the pool is idle again");
    fake_httpd::instance().clear_sent_frames();
    (void)fake_httpd::instance().deliver_frame(kFd, kBody);
    check_eq(fake_httpd::instance().sent_frames().size(), 1, "and the next request is answered");

    app.disarm();
    reset(link);
}

// ---------------------------------------------------------------------------
// 3 — a rope reply crosses as one frame, and gives its scratch slot straight back.
// ---------------------------------------------------------------------------
/**
 * @brief The gather arm, which is the one that still touches the pool.
 *
 * A multi-segment reply cannot be written from the caller's memory — a WebSocket frame is
 * contiguous — so it is gathered through a pool slot used as scratch. The slot is claimed
 * and released inside the call and posted nowhere, so the pool must read idle on return;
 * a shape that leaked it would fail the last check here rather than three thousand frames
 * later on silicon.
 */
void test_a_rope_reply_is_one_frame_and_strands_no_slot() {
    std::printf("a two-segment reply rope:\n");
    auto link = make_link();
    replier_t app;
    link->set_peer_receiver(app);
    claim(kFd);
    drain();
    tr::net::transport_t* const to = only_peer(*link);
    check(to != nullptr, "the peer resolved to a directed endpoint");
    if (to == nullptr) return;
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});
    fake_httpd::instance().clear_sent_frames();

    const std::span<const std::byte> rope[] = {std::span<const std::byte>(kHead),
                                               std::span<const std::byte>(kTail)};
    app.arm(to, rope);
    (void)fake_httpd::instance().deliver_frame(kFd, kBody);

    const auto sent = fake_httpd::instance().sent_frames();
    check_eq(sent.size(), 1, "the rope crossed as ONE WebSocket frame");
    if (sent.size() == 1) {
        const std::byte want[] = {kHead[0], kHead[1], kTail[0], kTail[1], kTail[2]};
        check(same_bytes(sent[0].payload, want), "carrying both segments, in order");
    }
    check_eq(fake_httpd::instance().queue_depth(), 0, "the control queue was offered nothing");
    check_eq(link->tx_slots_busy(), 0, "and the scratch slot went straight back to the pool");

    app.disarm();
    reset(link);
}

// ---------------------------------------------------------------------------
// 4 — the off-task producer is untouched.
// ---------------------------------------------------------------------------
/**
 * @brief The control without which the fix reads as "stop using the queue".
 *
 * A send from a task that does not own the descriptor MUST still be marshalled: writing
 * the socket from it is the #954 hazard the whole async-send pattern exists to avoid. So
 * the same endpoint, called off-task, has to behave exactly as it always did — nothing on
 * the wire until the httpd task drains, one slot held until then, and the frame delivered
 * when it does.
 */
void test_an_off_task_send_still_takes_the_queue() {
    std::printf("the same endpoint, called from a producer task:\n");
    auto link = make_link();
    replier_t app;
    link->set_peer_receiver(app);
    claim(kFd);
    drain();
    tr::net::transport_t* const to = only_peer(*link);
    check(to != nullptr, "the peer resolved to a directed endpoint");
    if (to == nullptr) return;
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});
    fake_httpd::instance().clear_sent_frames();

    std::thread producer([to] { to->send(std::span<const std::byte>(kHead)); });
    producer.join();

    check_eq(fake_httpd::instance().sent_frames().size(), 0, "nothing reached the wire yet");
    check_eq(fake_httpd::instance().queue_depth(), 1, "it was posted to the control queue");
    check_eq(link->tx_slots_busy(), 1, "holding the slot it gathered into");
    drain();
    const auto sent = fake_httpd::instance().sent_frames();
    check_eq(sent.size(), 1, "the drain delivered it");
    if (sent.size() == 1) check(same_bytes(sent[0].payload, kHead), "intact");
    check_eq(link->tx_slots_busy(), 0, "and gave the slot back");

    reset(link);
}

}  // namespace

/** @brief Run every case; non-zero exit on the first failed assertion anywhere. */
int main() {
    std::printf("#1494 - the in-call reply leaves by the socket, not the control queue\n");
    test_an_in_call_reply_bypasses_the_queue();
    test_a_full_pool_and_a_refused_queue_do_not_wedge_the_reply();
    test_a_rope_reply_is_one_frame_and_strands_no_slot();
    test_an_off_task_send_still_takes_the_queue();
    std::printf(g_failures == 0 ? "OK\n" : "FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
