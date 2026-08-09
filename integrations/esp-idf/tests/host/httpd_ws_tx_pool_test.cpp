/**
 * @file
 * @brief #949 — the TX slot pool IS the link's outstanding-send bound: exceeding it drops
 *        and counts, and every enqueue failure gives the slot straight back.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 teardown, #835 send-stall and #954 session-identity
 * suites: the REAL chip translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`)
 * compiled against the host fake of `esp_http_server` (fake_httpd.hpp).
 *
 * This replaces the #944 strand suite, which staged an enqueue that `httpd_queue_work`
 * accepted and lwIP then binned in silence. Above the ESP-IDF floor the component now
 * requires (`idf_component.yml`, `idf: ">=5.5.5"`) that event cannot occur —
 * `httpd_queue_work` reserves its mbox slot through a counting semaphore before the
 * `sendto` and returns `ESP_FAIL` when it cannot — so the compensation it justified (a
 * four-state slot lifetime, an age stamp, a sweep, a `tx_strands()` counter and an
 * unbounded heap work-item fallback) is gone, and what has to be pinned instead is the
 * behaviour that replaced it.
 *
 * The four properties pinned here:
 *   1. a full control queue never costs a slot — the refusal recycles it, so a link can
 *      absorb any number of refusals and still be serving afterwards. This is the case the
 *      strand suite's headline becomes above the floor;
 *   2. a burst past the pool is DROPPED and counted, and it allocates nothing. This is the
 *      deleted fallback's replacement, and the one that fails loudest against the old code,
 *      which posted a heap work item instead and counted nothing;
 *   3. the drop is a bound, not a cliff: the pooled frames of that same burst are all
 *      delivered, so property 2 is not satisfied by a link that drops everything;
 *   4. claims, refusals and drains racing for real wedge no slot — the transitions are
 *      lock-free, and this is the arrangement the tsan/asan lanes can falsify.
 */

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
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

int g_failures = 0;

/** @brief Global-heap allocation count — the instrument property 2 is measured with. */
std::atomic<std::size_t> g_allocs{0};

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

/** @brief A minimal frame body — the link only has to accept and route it. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/** @brief The one socket every case here serves. */
constexpr int kFd = 500;

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief A link that adopts the fake server. */
std::unique_ptr<httpd_ws_link_t> make_link() {
    return std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
}

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
    fake_httpd::instance().set_queue_capacity(0);
    fake_httpd::instance().close_all();
    drain();
}

// ---------------------------------------------------------------------------
// 1 — a refused enqueue costs no slot, however many times it happens.
// ---------------------------------------------------------------------------
/**
 * @brief The strand headline, restated for a control queue that fails fast.
 *
 * Every enqueue below is refused, which is the only way an offered frame can fail to be
 * queued above the floor. The slot each one claimed must come straight back — the item was
 * never posted, so nothing else can be reading it — and the count of refusals must be far
 * past the pool's capacity so that a link leaking one slot per refusal would show up as a
 * dead pool rather than as an arithmetic slip.
 */
void test_refused_enqueues_never_cost_a_slot() {
    std::printf("thirty-two refused enqueues against a four-slot pool:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();

    const std::size_t capacity = httpd_ws_link_t::tx_slot_capacity();
    const std::size_t rounds = 8 * capacity;
    fake_httpd::instance().set_queue_refusing(true);
    for (std::size_t i = 0; i < rounds; ++i) peer->send(std::span<const std::byte>(kBody));

    check_eq(link->enqueue_drops(), rounds, "every refusal was counted at the link");
    check_eq(link->tx_slots_busy(), 0, "and not one of them left a slot claimed");

    // The congestion passes: the pool must be serving, not merely un-leaked.
    fake_httpd::instance().set_queue_refusing(false);
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    for (std::size_t i = 0; i < capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    check_eq(link->tx_slots_busy(), capacity, "a full pool's worth of frames claimed slots again");
    drain();
    check_eq(fake_httpd::instance().frames_sent() - sent_before, capacity,
             "and all of them were delivered");
    check_eq(link->tx_slots_busy(), 0, "the pool drained back to idle");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 — a burst past the pool is dropped, counted, and allocates NOTHING.
// ---------------------------------------------------------------------------
/**
 * @brief The deleted fallback, pinned by its absence.
 *
 * Before #949 a send that found no free slot allocated a work item and a payload buffer on
 * the global heap and posted them anyway: the outstanding-send count was bounded by the
 * heap rather than by the control queue behind it, and nothing counted the event. Here the
 * pool is filled with frames that are genuinely queued (nothing drains in between), and
 * four more sends are offered on top.
 *
 * Two measurements, and both are needed. The counter says the drop happened; the
 * allocation count says it was a DROP and not a quieter fallback — a link that still heap
 * fell back would satisfy neither, and one that fell back without counting would satisfy
 * only the second.
 */
void test_a_burst_past_the_pool_drops_and_allocates_nothing() {
    std::printf("four sends offered to a pool that is already full:\n");
    auto link = make_link();
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    const std::size_t capacity = httpd_ws_link_t::tx_slot_capacity();
    // Warm up on the ordinary path and drain, so anything first-use (the fake's deque node,
    // a lazy init in the log) is spent OUTSIDE the window below.
    peer->send(std::span<const std::byte>(kBody));
    drain();
    check_eq(link->tx_slots_busy(), 0, "the warm-up drained: the pool is idle again");

    for (std::size_t i = 0; i < capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    check_eq(link->tx_slots_busy(), capacity, "the pool is fully claimed, all of it live");

    const std::uint32_t drops_before = link->enqueue_drops();
    const std::size_t allocs_before = g_allocs.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    const std::size_t allocs_after = g_allocs.load(std::memory_order_relaxed);

    check_eq(link->enqueue_drops() - drops_before, capacity,
             "every send past the pool was counted as a drop");
    check_eq(allocs_after - allocs_before, 0,
             "and none of them touched the global heap: there is no fallback left");
    check_eq(fake_httpd::instance().queue_depth(), capacity,
             "the control queue holds the pooled frames only — the drops were never offered");

    reset(link);
}

// ---------------------------------------------------------------------------
// 3 — the bound is a bound, not a cliff.
// ---------------------------------------------------------------------------
/**
 * @brief The guard without which case 2 is satisfied by a link that drops everything.
 *
 * Same burst as case 2, drained afterwards: exactly the pooled frames must reach the wire,
 * and the link must keep serving after the burst. A link whose pool never recovers from an
 * over-offer — the shape the old strand was — fails the second half here.
 */
void test_the_pooled_frames_of_an_over_offer_all_go_out() {
    std::printf("the same burst, drained:\n");
    auto link = make_link();
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    const std::size_t capacity = httpd_ws_link_t::tx_slot_capacity();
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    for (std::size_t i = 0; i < 2 * capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    drain();

    check_eq(fake_httpd::instance().frames_sent() - sent_before, capacity,
             "the pooled half of the burst was delivered in full");
    check_eq(link->tx_slots_busy(), 0, "and the pool came back to idle");

    const std::size_t sent_mid = fake_httpd::instance().frames_sent();
    for (std::size_t i = 0; i < capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    drain();
    check_eq(fake_httpd::instance().frames_sent() - sent_mid, capacity,
             "and the link still serves a full pool's worth afterwards");

    reset(link);
}

// ---------------------------------------------------------------------------
// 4 — claims, refusals and drains racing for real.
// ---------------------------------------------------------------------------
/**
 * @brief Several sending tasks and the httpd task, concurrently, over a control queue
 *        small enough that a large share of the enqueues is refused.
 *
 * The cases above are single-threaded: they pin the outcomes but give a sanitizer nothing
 * to look at, and the claim/release pair is lock-free. This one puts a claim, a refusal's
 * release and a drained item's release on different threads at the same time.
 *
 * The post-condition is the strongest DETERMINISTIC one available, checked after everything
 * has quiesced: not a single slot may still be claimed. A slot leaked by a lost race — a
 * refusal path that failed to release, a double release that let two senders hold one slot
 * — shows up here as a pool that never comes back to zero.
 */
void test_concurrent_claims_refusals_and_drains_wedge_nothing() {
    std::printf("senders, refusals and the drain racing over a starved control queue:\n");
    auto link = make_link();
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    const std::size_t capacity = httpd_ws_link_t::tx_slot_capacity();
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    const std::size_t refused_before = fake_httpd::instance().queue_drops();
    // Two entries deep: enough that work really drains, small enough that a great many
    // enqueues meet a full mbox and are refused.
    fake_httpd::instance().set_queue_capacity(2);

    std::atomic<bool> stop{false};
    std::vector<std::thread> senders;
    for (int t = 0; t < 3; ++t) {
        senders.emplace_back([peer] {
            for (int i = 0; i < 300; ++i) peer->send(std::span<const std::byte>(kBody));
        });
    }
    std::thread drainer([&stop] {
        while (!stop.load(std::memory_order_relaxed)) (void)fake_httpd::instance().run_pending();
    });
    for (auto& s : senders) s.join();
    stop.store(true, std::memory_order_relaxed);
    drainer.join();
    drain();

    check(fake_httpd::instance().queue_drops() > refused_before,
          "the storm really did meet a full mbox (otherwise this case proves nothing)");
    check(fake_httpd::instance().frames_sent() > sent_before, "and frames really did go out");

    fake_httpd::instance().set_queue_capacity(0);
    for (std::size_t i = 0; i < capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    drain();
    check_eq(link->tx_slots_busy(), 0, "every slot came back: nothing was wedged by the race");

    reset(link);
}

}  // namespace

/** @brief Counting `operator new` — the global-heap instrument case 2 reads. */
void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* const p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) std::abort();  // this suite is built without exceptions
    return p;
}

/** @brief The matching `operator delete`. */
void operator delete(void* p) noexcept { std::free(p); }

/** @brief Sized `operator delete` — C++14 onwards emits this form. */
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main() {
    std::printf("httpd_ws_link TX slot-pool host suite (#949):\n");
    test_refused_enqueues_never_cost_a_slot();
    test_a_burst_past_the_pool_drops_and_allocates_nothing();
    test_the_pooled_frames_of_an_over_offer_all_go_out();
    test_concurrent_claims_refusals_and_drains_wedge_nothing();
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
