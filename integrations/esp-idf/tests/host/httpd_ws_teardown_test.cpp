/**
 * @file
 * @brief #816 — adopted-mode teardown of `httpd_ws_link_t`, on the host.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The REAL chip translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`)
 * is compiled against a host fake of the ESP-IDF facilities the teardown contract rests
 * on — the session table, the latched WebSocket route, the request scope, and the
 * control work queue (see fake_httpd.hpp). What the fake reproduces is exactly what
 * makes #816 a use-after-free rather than a cosmetic leak: the server stores pointers
 * into this link, runs them LATER, and runs them on its own task.
 *
 * The states of the teardown, each pinned here:
 *   1. the ordinary case — the destroyer is some other task, the server task is live:
 *      the detach is queued, runs there, and the destructor does not return until every
 *      session's ctx/free_ctx is retired. Closing the sessions afterwards (the peer
 *      hanging up, or the adopting server stopping) then calls NOTHING;
 *   2. the destructor runs ON the server task from a work item: waiting for queued work
 *      would deadlock by construction, so the detach happens inline and is complete;
 *   3. the wedged case — the server task never drains the queue: the destructor gives
 *      up on its bound and neutralises the sessions instead of freeing them, so the
 *      late `free_ctx` (delivered here, after the link is gone) is inert. Under ASan
 *      this test IS the use-after-free detector;
 *   4. the handler barrier — a frame already INSIDE the handler when teardown starts:
 *      the destructor must join it before freeing anything, and the session that frame
 *      arms behind the destructor's back must still be detached;
 *   5. the latched route — a frame dispatched after the destructor returned. Neither
 *      the URI unregister nor the ctx detach can revoke `sock_db::ws_handler`, so this
 *      dispatch happens on silicon and must land somewhere safe;
 *   6. the in-call teardown (#814) — the app callback destroys the link from inside the
 *      handler. `httpd_sess_set_ctx` cannot detach the session being serviced, and
 *      `httpd_req_cleanup` runs its `free_ctx` after the destructor has returned;
 *   7. descriptor reuse — a detach item that drains late must not touch the CO-TENANT
 *      that has since been accepted onto the recycled descriptor.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include "fake_httpd.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"

namespace {

using namespace std::chrono_literals;
using tr::net::httpd_ws_link_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/**
 * @brief A thread standing in for the `esp_http_server` task: it, and only it, drains
 *        the control queue — so anything queued to it runs there, as on silicon.
 */
class server_task_t {
   public:
    server_task_t() : th_([this] { loop(); }) {}
    ~server_task_t() {
        run_.store(false);
        th_.join();
    }
    /** @brief Stop draining the queue — the wedged-task case (state 3). */
    void park() { parked_.store(true); }
    /** @brief Resume draining. */
    void unpark() { parked_.store(false); }
    /** @brief Run @p action on this task and block until it has finished. */
    void run_on_task(std::function<void()> action) {
        std::atomic<bool> done{false};
        fake_httpd::instance().post([&] {
            action();
            done.store(true);
        });
        while (!done.load()) std::this_thread::sleep_for(1ms);
    }

   private:
    void loop() {
        while (run_.load()) {
            if (!parked_.load()) (void)fake_httpd::instance().run_pending();
            std::this_thread::sleep_for(1ms);
        }
    }
    std::atomic<bool> run_{true};
    std::atomic<bool> parked_{false};
    std::thread th_;
};

/** @brief A minimal frame body — the link only has to accept and route it. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/**
 * @brief Deliver one frame to @p fd through the route its session latched.
 *
 * The only way a test legitimately reaches the handler, and deliberately NOT through a
 * pointer the test kept: the fake dispatches from the session, so a delivery after the
 * URI was unregistered — or after the link died — is the same call the server makes.
 */
esp_err_t deliver(int fd, std::span<const std::byte> body, bool final = true,
                  httpd_ws_type_t type = HTTPD_WS_TYPE_BINARY) {
    return fake_httpd::instance().deliver_frame(fd, body, final, type);
}

/** @brief Admit @p fd and claim it as a peer, on the server task. */
void claim_session(server_task_t& task, int fd) {
    fake_httpd::instance().open_session(fd);
    task.run_on_task([fd] { (void)deliver(fd, kBody); });
}

/** @brief A co-tenant's session context — a stand-in for the SPA component's, on the
 *         shared `:80` server this link only adopts. */
struct co_tenant_t {
    bool freed = false; /**< @brief Set if its free_ctx ever ran. */
};

/** @brief The co-tenant's `free_ctx`, as httpd would store it. */
void free_co_tenant(void* ctx) { static_cast<co_tenant_t*>(ctx)->freed = true; }

// ---------------------------------------------------------------------------
// State 1 — destroyed from another task while the server task is live.
// ---------------------------------------------------------------------------
void test_queued_detach() {
    std::printf("adopted dtor, server task live:\n");
    server_task_t task;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    check(link->ok(), "the adopting link registered its URI");

    claim_session(task, 100);
    claim_session(task, 101);
    check(fake_httpd::instance().session_ctx(100) != nullptr,
          "an admitted peer arms a session ctx (the callback #816 is about)");

    const std::size_t before = fake_httpd::instance().free_ctx_calls();
    const auto t0 = std::chrono::steady_clock::now();
    link.reset();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    check(elapsed < 900ms, "the destructor does not sit out the drain bound");
    check(fake_httpd::instance().session_ctx(100) == nullptr &&
              fake_httpd::instance().session_ctx(101) == nullptr,
          "every session's ctx is cleared BEFORE the destructor returns");
    check(fake_httpd::instance().free_ctx_calls() == before + 2,
          "each session's free_ctx ran exactly once, while the link was still alive");

    // The peers now hang up (or the adopting server stops): with ctx/free_ctx retired,
    // this must reach no code at all. Under ASan a missed detach faults here.
    const std::size_t after_detach = fake_httpd::instance().free_ctx_calls();
    task.run_on_task([] { fake_httpd::instance().close_all(); });
    check(fake_httpd::instance().free_ctx_calls() == after_detach,
          "a post-teardown disconnect calls back into NOTHING");
}

// ---------------------------------------------------------------------------
// State 2 — the destructor runs ON the server task (a drain would deadlock).
// ---------------------------------------------------------------------------
void test_dtor_on_server_task() {
    std::printf("adopted dtor, running ON the server task:\n");
    server_task_t task;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim_session(task, 200);

    const std::size_t before = fake_httpd::instance().free_ctx_calls();
    // Both observations are taken INSIDE the work item, the instant the destructor
    // returns: once the item finishes, the server task would drain any work an
    // abandoning teardown had left behind and paper over the difference.
    std::atomic<void*> ctx_at_return{nullptr};
    std::atomic<std::size_t> calls_at_return{0};
    const auto t0 = std::chrono::steady_clock::now();
    task.run_on_task([&] {
        link.reset();
        ctx_at_return.store(fake_httpd::instance().session_ctx(200));
        calls_at_return.store(fake_httpd::instance().free_ctx_calls());
    });
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    check(elapsed < 500ms, "no self-deadlock: the destructor does not wait on its own task");
    check(ctx_at_return.load() == nullptr,
          "the detach still HAPPENED (inline), not skipped or abandoned");
    check(calls_at_return.load() == before + 1,
          "the inline detach ran the session's free_ctx exactly once");
    task.run_on_task([] { fake_httpd::instance().close_all(); });
}

// ---------------------------------------------------------------------------
// State 3 — the server task never drains: expiry must leave memory VALID.
// ---------------------------------------------------------------------------
void test_wedged_server_task() {
    std::printf("adopted dtor, server task wedged:\n");
    server_task_t task;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim_session(task, 300);

    task.park();  // the queue stops being drained: the detach can never run
    const auto t0 = std::chrono::steady_clock::now();
    link.reset();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    check(elapsed >= 900ms, "the destructor spent its bound waiting for the wedged task");
    check(fake_httpd::instance().session_ctx(300) != nullptr,
          "the session is still armed — this IS the abandoned case");

    // Now let the server task run again: the queued detach work executes with the link
    // long gone (it must touch only the server handle), and the peer then hangs up,
    // firing the free_ctx the teardown could not retire. Both must be inert; under ASan
    // a link-touching work item or a freed session slot faults here.
    task.unpark();
    task.run_on_task([] { fake_httpd::instance().close_all(); });
    check(!fake_httpd::instance().has_session(300),
          "the late close ran to completion on the leaked, inert session shell");
}

// ---------------------------------------------------------------------------
// State 4 — a frame already inside the handler when teardown starts.
// ---------------------------------------------------------------------------
void test_handler_barrier() {
    std::printf("frame inside the handler when the teardown starts:\n");
    server_task_t task;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    // NOT claimed: this is the peer whose FIRST frame is in flight, so the link holds no
    // slot for it at all — the population the destructor's session snapshot cannot see.
    fake_httpd::instance().open_session(400);

    std::atomic<bool> in_handler{false};
    std::atomic<bool> teardown_started{false};
    std::atomic<bool> handler_done{false};
    std::atomic<bool> dtor_returned{false};
    std::atomic<bool> returned_before_handler{false};
    // Fires between the handler's header pass and its payload pass — the frame is inside
    // the link and has claimed nothing yet (the exact window a bare fd snapshot misses).
    fake_httpd::instance().set_frame_hook([&] {
        in_handler.store(true);
        while (!teardown_started.load()) std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(50ms);  // hold the frame open across the barrier
        if (dtor_returned.load()) returned_before_handler.store(true);
    });
    fake_httpd::instance().post([&] {
        (void)deliver(400, kBody);
        handler_done.store(true);
    });
    while (!in_handler.load()) std::this_thread::sleep_for(1ms);

    teardown_started.store(true);
    link.reset();
    dtor_returned.store(true);
    fake_httpd::instance().set_frame_hook(nullptr);

    check(handler_done.load(),
          "the destructor JOINED the in-flight handler instead of freeing under it");
    check(!returned_before_handler.load(), "and it did not return while the frame was live");
    // The frame claimed its session while the destructor was blocked in that join. The
    // snapshot is taken afterwards, so the late arrival is still detached — the guarantee
    // that replaces refusing the frame outright.
    task.run_on_task([] {});  // let the queued detach drain
    check(fake_httpd::instance().session_ctx(400) == nullptr,
          "a session armed DURING the barrier is still in the detach snapshot");
    task.run_on_task([] { fake_httpd::instance().close_all(); });
}

// ---------------------------------------------------------------------------
// State 5 — the WS route the session latched outlives both URI and link.
// ---------------------------------------------------------------------------
void test_dispatch_after_teardown() {
    std::printf("frame dispatched after the destructor returned:\n");
    server_task_t task;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    // Upgraded, never heard from: httpd answers the handshake itself and (IDF v6) does
    // not call the handler for it, so this session holds the route while being INVISIBLE
    // to the link. No slot is open, so the destructor's session work is a no-op — and
    // the frame below is still dispatched into it.
    fake_httpd::instance().open_session(500);

    link.reset();

    std::atomic<int> verdict{ESP_OK};
    task.run_on_task([&] { verdict.store(deliver(500, kBody)); });
    check(verdict.load() == ESP_FAIL,
          "a frame on the latched route is REFUSED once the link is gone (httpd closes it)");
    check(fake_httpd::instance().session_ctx(500) == nullptr,
          "and it arms nothing: the refused frame never reaches the link");
    task.run_on_task([] { fake_httpd::instance().close_all(); });
}

// ---------------------------------------------------------------------------
// State 6 — the app tears the link down from INSIDE the handler (#814).
// ---------------------------------------------------------------------------
void test_teardown_inside_handler() {
    std::printf("app destroys the link from inside the handler:\n");
    server_task_t task;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    // The delivery sink IS the teardown — a graph command that retires this transport,
    // serviced in-call on the server task, which is the #814 shape. Armed only after the
    // peer is established, so the teardown lands on a session that is already claimed.
    bool teardown_now = false;
    auto sink = [&](std::string_view, std::span<const std::byte>) {
        if (teardown_now) link.reset();
    };
    link->set_peer_receiver(sink);
    claim_session(task, 600);
    check(link != nullptr, "the claiming frame was delivered without tearing anything down");

    const std::size_t before = fake_httpd::instance().free_ctx_calls();
    teardown_now = true;
    task.run_on_task([] {
        // Fragmented, so the completed message is delivered out of the slot's reassembly
        // buffer: the destructor frees that slot mid-delivery, and anything the RX path
        // wrote back into it afterwards would be a use-after-free.
        (void)deliver(600, kBody, false, HTTPD_WS_TYPE_BINARY);
        (void)deliver(600, kBody, true, HTTPD_WS_TYPE_CONTINUE);
    });
    check(link == nullptr, "the link was destroyed in-call, from the handler's own frame");
    // httpd_req_cleanup has now run on the server task. It is the one call the destructor
    // could not be present for; the session's stored callback must land on memory the
    // teardown deliberately left valid. Under ASan this check is the detector.
    check(fake_httpd::instance().free_ctx_calls() == before,
          "the post-return httpd_req_cleanup freed nothing it should not have");
    task.run_on_task([] { fake_httpd::instance().close_all(); });
    check(!fake_httpd::instance().has_session(600),
          "the later close lands on the leaked, inert slot rather than freed memory");
}

// ---------------------------------------------------------------------------
// State 7 — the detach drains late, onto a RECYCLED descriptor.
// ---------------------------------------------------------------------------
void test_fd_reuse_before_detach() {
    std::printf("detach draining onto a reused descriptor:\n");
    server_task_t task;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim_session(task, 700);

    // Wedge the server task so the destructor abandons its detach with the work item
    // still queued — state 3, which is precisely when the item drains arbitrarily late.
    task.park();
    link.reset();
    check(fake_httpd::instance().session_ctx(700) != nullptr, "the detach item is still queued");

    // Our peer hangs up and the SHARED server accepts an unrelated client onto the same
    // descriptor, with its own context and its own destructor.
    co_tenant_t stranger;
    fake_httpd::instance().close_session(700);
    fake_httpd::instance().open_session(700, &stranger, &free_co_tenant);

    task.unpark();
    task.run_on_task([] {});  // drain the abandoned detach against the reused descriptor

    check(!stranger.freed, "the co-tenant's free_ctx was NOT run by our detach");
    check(fake_httpd::instance().session_ctx(700) == &stranger,
          "the co-tenant's context is untouched");
    check(fake_httpd::instance().has_session(700), "and its live session was not force-closed");
    task.run_on_task([] { fake_httpd::instance().close_all(); });
}

}  // namespace

int main() {
    std::printf("httpd_ws_link adopted-mode teardown host suite (#816):\n");
    test_queued_detach();
    test_dtor_on_server_task();
    test_wedged_server_task();
    test_handler_barrier();
    test_dispatch_after_teardown();
    test_teardown_inside_handler();
    test_fd_reuse_before_detach();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
