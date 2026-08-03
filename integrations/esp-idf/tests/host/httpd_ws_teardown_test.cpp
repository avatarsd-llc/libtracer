/**
 * @file
 * @brief #816 — adopted-mode teardown of `httpd_ws_link_t`, on the host.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The REAL chip translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`)
 * is compiled against a host fake of the one ESP-IDF facility the teardown contract
 * rests on — the session table plus the control work queue (see fake_httpd.hpp). What
 * the fake reproduces is exactly what makes #816 a use-after-free rather than a
 * cosmetic leak: `free_ctx` is stored by the server, runs LATER, and runs on the
 * server's task, not the destroying one.
 *
 * The four states of the teardown, each pinned here:
 *   1. the ordinary case — the destroyer is some other task, the server task is live:
 *      the detach is queued, runs there, and the destructor does not return until every
 *      session's ctx/free_ctx is retired. Closing the sessions afterwards (the peer
 *      hanging up, or the adopting server stopping) then calls NOTHING;
 *   2. the #814 case — the destructor runs ON the server task: waiting for queued work
 *      would deadlock by construction, so the detach happens inline and teardown is
 *      prompt AND complete;
 *   3. the wedged case — the server task never drains the queue: the destructor gives
 *      up on its bound and neutralises the sessions instead of freeing them, so the
 *      late `free_ctx` (delivered here, after the link is gone) is inert. Under ASan
 *      this test IS the use-after-free detector;
 *   4. the racing frame — a frame already in the handler when teardown starts must not
 *      ARM a new session behind the detach snapshot.
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

/** @brief The URI registration the link installs — captured so a test can deliver a
 *         frame the way the server task would, including after the unregister. */
httpd_uri_t ws_uri(httpd_ws_link_t* link) {
    httpd_uri_t uri = {};
    uri.uri = "/ws";
    uri.method = HTTP_GET;
    uri.handler = nullptr;  // filled by claim_session via the link's own registration
    uri.user_ctx = link;
    uri.is_websocket = true;
    uri.handle_ws_control_frames = false;
    return uri;
}

/**
 * @brief The link's URI handler, recovered the only way a test legitimately can: the
 *        fake records what `httpd_register_uri_handler` was given.
 *
 * Kept as a free function so every test claims its peers the same way — through the
 * real handler, so the real `httpd_sess_set_ctx` arming runs.
 */
esp_err_t deliver(httpd_ws_link_t* link, int fd, std::span<const std::byte> body) {
    httpd_uri_t uri = ws_uri(link);
    uri.handler = fake_httpd::registered_handler();
    return fake_httpd::instance().deliver_frame(uri, fd, body);
}

/** @brief A minimal frame body — the link only has to accept and route it. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/** @brief Admit @p fd and claim it as a peer, on the server task. */
void claim_session(server_task_t& task, httpd_ws_link_t* link, int fd) {
    fake_httpd::instance().open_session(fd);
    task.run_on_task([&, fd] { (void)deliver(link, fd, kBody); });
}

// ---------------------------------------------------------------------------
// State 1 — destroyed from another task while the server task is live.
// ---------------------------------------------------------------------------
void test_queued_detach() {
    std::printf("adopted dtor, server task live:\n");
    server_task_t task;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    check(link->ok(), "the adopting link registered its URI");

    claim_session(task, link.get(), 100);
    claim_session(task, link.get(), 101);
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
// State 2 — the destructor runs ON the server task (#814: a drain would deadlock).
// ---------------------------------------------------------------------------
void test_dtor_on_server_task() {
    std::printf("adopted dtor, running ON the server task:\n");
    server_task_t task;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim_session(task, link.get(), 200);

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
    claim_session(task, link.get(), 300);

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
void test_frame_racing_teardown() {
    std::printf("frame racing the teardown:\n");
    server_task_t task;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim_session(task, link.get(), 400);

    std::atomic<bool> teardown_started{false};
    std::atomic<int> late_verdict{ESP_OK};
    // The raw pointer, taken BEFORE the reset: unique_ptr::reset nulls itself before it
    // runs the deleter, and the racing frame must reach the still-living object.
    httpd_ws_link_t* const racing = link.get();
    fake_httpd::instance().open_session(401);
    // Queued FIRST, so it is drained before the destructor's detach work: it stands in
    // for a frame the server task had already dispatched into the handler when the
    // destructor began.
    fake_httpd::instance().post([&] {
        while (!teardown_started.load()) std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(20ms);  // let the destructor reach its detach
        late_verdict.store(deliver(racing, 401, kBody));
    });

    std::thread destroyer([&] {
        teardown_started.store(true);
        link.reset();
    });
    destroyer.join();

    check(late_verdict.load() == ESP_FAIL,
          "a frame racing the teardown is REFUSED (the socket is closed), not admitted");
    check(fake_httpd::instance().session_ctx(401) == nullptr,
          "and it arms no session behind the detach snapshot");
    task.run_on_task([] { fake_httpd::instance().close_all(); });
}

}  // namespace

int main() {
    std::printf("httpd_ws_link adopted-mode teardown host suite (#816):\n");
    test_queued_detach();
    test_dtor_on_server_task();
    test_wedged_server_task();
    test_frame_racing_teardown();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
