/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief The one teardown table (#2813): every link adapter runs the same four rows, so the
 *        `shut_down` contract is proved once, not per transport.
 *
 * `transport_t::shut_down` waits for nothing on any thread, and the destructor is the one join.
 * Each row builds a fresh rig (the link under test and a peer that sends it frames) and ends by
 * destroying the link off every callback thread:
 *
 *  1. **shut_down from a callback.** The link's own receive callback shuts it down: the call
 *     returns, on a thread that runs link callbacks, nothing is delivered after it, and the
 *     peer sees the connection close before the destructor runs.
 *  2. **shut_down from the owner.** The owner shuts the link down while its receive callback is
 *     held in flight: the call returns before the callback does (a join or a wait for the
 *     callback would hang here), nothing new is delivered after the callback leaves, and the
 *     peer sees the connection close before the destructor runs.
 *  3. **A send on the link's own receive thread across shut_down** (#2786). The held callback
 *     keeps sending on the link while the owner shuts it down, and after: every send returns,
 *     with nothing reported under TSan.
 *  4. **destroy.** The destructor waits out a callback still in flight after a shut_down: it
 *     does not return while the callback is held, and returns once it leaves.
 *  5. **shut_down while a callback is held before its read.** The peer closes the connection,
 *     the link's callback is held on its way into the read that sees that close, and the owner
 *     shuts the link down (it returns). Released, the callback reads the close and leaves; the
 *     link stays off the wait: no read or accept runs on that thread after it, and the
 *     destructor returns.
 *
 * A rig is a default-constructible type with:
 *
 * | Member | Does |
 * | --- | --- |
 * | `static constexpr const char* name` | The adapter's name in the report. |
 * | `bool ok()` | The link and its peer came up. |
 * | `tr::net::transport_t& link()` | The link under test. |
 * | `void poke()` | Make the peer send the link one frame (may be lost before the link is up). |
 * | `void destroy_link()` | Destroy the link under test; the peer stays. |
 *
 * A rig whose link has a connection also has `bool peer_saw_close()`: the peer has seen that
 * connection close. A link with none (UDP, a CAN bus) leaves it out.
 *
 * Row 5 runs for a rig that can hold the link's socket reads: `void drop_peer()` (the peer
 * closes its end), and the static `watch_io(std::thread::id, bool hold)`, `release_io()`,
 * `io_held()` and `io_calls()` (the reads and accepts made on the watched thread, which park
 * while held). The host socket rigs have them (`link_teardown_table_test`).
 *
 * Every wait is bounded: a row that hangs prints the step and exits at once, since the threads
 * it leaves stuck could not be joined.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <thread>

#include "libtracer/transport.hpp"
#include "libtracer/wait.hpp"

namespace tr::testing::teardown {

using namespace std::chrono_literals;

/** @brief How long a step may take before the row is called stuck. */
inline constexpr std::chrono::milliseconds kStuck = 5s;
/** @brief How long the table waits for a delivery that must not come. */
inline constexpr std::chrono::milliseconds kQuiet = 150ms;

/** @brief Poll @p cond until it holds or @p budget passes; answer its last value. */
template <class Fn>
bool eventually(Fn cond, std::chrono::milliseconds budget = kStuck) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!cond()) {
        if (std::chrono::steady_clock::now() >= deadline) return cond();
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/** @brief Exit at once when a step is stuck: its threads could not be joined. */
inline void stuck_unless(bool ok, std::string_view rig, std::string_view step) {
    if (ok) return;
    std::printf("  [FAIL] %.*s: stuck at %.*s; exiting\n", static_cast<int>(rig.size()), rig.data(),
                static_cast<int>(step.size()), step.data());
    std::fflush(stdout);
    std::_Exit(1);
}

/** @brief Poke the peer until the link's callback has been entered at least once. */
template <class Rig>
bool poke_until_entered(Rig& rig, const std::atomic<int>& entries) {
    return eventually([&] {
        if (entries.load() != 0) return true;
        rig.poke();
        std::this_thread::sleep_for(5ms);
        return entries.load() != 0;
    });
}

/** @brief Run @p fn on a thread of its own and answer whether it returned within @p budget;
 *         a call that did not is left running and the row is stuck. */
template <class Fn>
bool returns_within(Fn fn, std::atomic<bool>& done, std::chrono::milliseconds budget = kStuck) {
    std::thread t([&] {
        fn();
        done.store(true);
    });
    const bool ok = eventually([&] { return done.load(); }, budget);
    if (ok) t.join();
    if (!ok) t.detach();
    return ok;
}

/** @brief Poke a few more frames, then answer whether the link's count stayed at @p seen. */
template <class Rig>
bool nothing_delivered_after(Rig& rig, const std::atomic<int>& entries, int seen) {
    for (int i = 0; i < 5; ++i) rig.poke();
    std::this_thread::sleep_for(kQuiet);
    return entries.load() == seen;
}

/** @brief Whether the peer sees the link's connection close within the stuck bound, before the
 *         row destroys the link: shut_down shuts it down, the destructor only frees (#2813).
 *         True for a rig with no connection to close. */
template <class Rig>
bool peer_sees_close(Rig& rig) {
    if constexpr (requires { rig.peer_saw_close(); })
        return eventually([&] { return rig.peer_saw_close(); });
    return true;
}

/** @brief Row 1: the link's own callback shuts it down. */
template <class Rig, class Check>
void row_shut_down_from_a_callback(Check check) {
    Rig rig;
    std::atomic<int> entries{0};
    std::atomic<bool> on_callback{false};
    std::atomic<bool> returned{false};
    auto rx = [&](std::span<const std::byte>) {
        if (entries.fetch_add(1) != 0) return;
        on_callback.store(tr::net::on_callback_thread());
        rig.link().shut_down();  // the removal, delivered on the link's own callback
        returned.store(true);
    };
    check(rig.ok(), "1. the link and its peer came up");
    rig.link().set_receiver(rx);
    (void)rig.link().start_receiving();
    stuck_unless(poke_until_entered(rig, entries), Rig::name, "row 1, the first delivery");
    stuck_unless(eventually([&] { return returned.load(); }), Rig::name,
                 "row 1, shut_down on its own callback");
    check(on_callback.load(), "1. shut_down from a callback ran on a callback thread and returned");
    check(nothing_delivered_after(rig, entries, entries.load()),
          "1. nothing is delivered after it");
    check(peer_sees_close(rig), "1. the peer sees the connection close before the destructor");
    std::atomic<bool> gone{false};
    stuck_unless(returns_within([&] { rig.destroy_link(); }, gone), Rig::name, "row 1, destroy");
    check(gone.load(), "1. the destructor joins and returns");
}

/** @brief The shared body of rows 2 and 3: the owner shuts the link down while its callback
 *         is held in flight, sending on the link when @p send_across . */
template <class Rig, class Check>
void row_shut_down_from_the_owner(Check check, bool send_across) {
    Rig rig;
    std::atomic<int> entries{0};
    std::atomic<bool> release{false};
    std::atomic<bool> left{false};
    std::atomic<int> sends{0};
    const std::byte frame[4] = {std::byte{0x2a}, std::byte{0x28}, std::byte{0x13}, std::byte{0}};
    auto rx = [&](std::span<const std::byte>) {
        if (entries.fetch_add(1) != 0) return;
        // Held in flight until the owner's shut_down has returned; with send_across, sending on
        // the link from its own receive thread the whole time, and a few times after.
        while (!release.load()) {
            if (send_across) {
                rig.link().send(std::span<const std::byte>(frame));
                sends.fetch_add(1);
            }
            std::this_thread::sleep_for(1ms);
        }
        for (int i = 0; send_across && i < 3; ++i) {
            rig.link().send(std::span<const std::byte>(frame));
            sends.fetch_add(1);
        }
        left.store(true);
    };
    check(rig.ok(), std::string_view(send_across ? "3. the link and its peer came up"
                                                 : "2. the link and its peer came up"));
    rig.link().set_receiver(rx);
    (void)rig.link().start_receiving();
    stuck_unless(poke_until_entered(rig, entries), Rig::name, "rows 2/3, the first delivery");
    std::atomic<bool> shut{false};
    const bool returned = returns_within([&] { rig.link().shut_down(); }, shut);
    const bool held = !left.load();
    release.store(true);
    stuck_unless(returned, Rig::name, "rows 2/3, shut_down from the owner (it waited)");
    stuck_unless(eventually([&] { return left.load(); }), Rig::name, "rows 2/3, the callback");
    if (send_across) {
        check(sends.load() > 3, "3. the callback sent on the link across shut_down, and after");
    } else {
        check(held, "2. shut_down from the owner returned while its callback was in flight");
        check(nothing_delivered_after(rig, entries, entries.load()),
              "2. nothing new is delivered once that callback has left");
    }
    check(peer_sees_close(rig),
          send_across ? "3. the peer sees the connection close before the destructor"
                      : "2. the peer sees the connection close before the destructor");
    std::atomic<bool> gone{false};
    stuck_unless(returns_within([&] { rig.destroy_link(); }, gone), Rig::name, "rows 2/3, destroy");
    check(gone.load(), send_across ? "3. the destructor joins and returns"
                                   : "2. the destructor joins and returns");
}

/** @brief Row 4: the destructor waits out a callback still in flight after a shut_down. */
template <class Rig, class Check>
void row_destroy(Check check) {
    Rig rig;
    std::atomic<int> entries{0};
    std::atomic<bool> release{false};
    std::atomic<bool> left{false};
    auto rx = [&](std::span<const std::byte>) {
        if (entries.fetch_add(1) != 0) return;
        while (!release.load()) std::this_thread::sleep_for(1ms);
        left.store(true);
    };
    check(rig.ok(), "4. the link and its peer came up");
    rig.link().set_receiver(rx);
    (void)rig.link().start_receiving();
    stuck_unless(poke_until_entered(rig, entries), Rig::name, "row 4, the first delivery");
    std::atomic<bool> shut{false};
    stuck_unless(returns_within([&] { rig.link().shut_down(); }, shut), Rig::name,
                 "row 4, shut_down");
    std::atomic<bool> gone{false};
    std::thread destroyer([&] {
        rig.destroy_link();
        gone.store(true);
    });
    std::this_thread::sleep_for(kQuiet);
    const bool waited = !gone.load();
    release.store(true);
    stuck_unless(eventually([&] { return gone.load(); }), Rig::name, "row 4, destroy");
    destroyer.join();
    check(waited && left.load(),
          "4. the destructor waited out the callback in flight, then returned");
}

/** @brief Row 5: a callback held on its way into the read that sees the peer's close, across
 *         the owner's shut_down. */
template <class Rig, class Check>
void row_held_before_its_read(Check check) {
    if constexpr (requires(Rig& r) {
                      r.drop_peer();
                      Rig::watch_io(std::thread::id{}, true);
                  }) {
        Rig rig;
        std::atomic<int> entries{0};
        std::atomic<std::thread::id> on{};
        auto rx = [&](std::span<const std::byte>) {
            if (entries.fetch_add(1) == 0) on.store(std::this_thread::get_id());
        };
        check(rig.ok(), "5. the link and its peer came up");
        rig.link().set_receiver(rx);
        (void)rig.link().start_receiving();
        stuck_unless(poke_until_entered(rig, entries), Rig::name, "row 5, the first delivery");
        std::this_thread::sleep_for(kQuiet);  // the pokes still on their way land first
        Rig::watch_io(on.load(), true);
        rig.drop_peer();  // the link's callback runs for the close, and parks before its read
        stuck_unless(eventually([] { return Rig::io_held(); }), Rig::name,
                     "row 5, held before its read");
        std::atomic<bool> shut{false};
        const bool returned = returns_within([&] { rig.link().shut_down(); }, shut);
        Rig::release_io();
        stuck_unless(returned, Rig::name, "row 5, shut_down (it waited)");
        std::this_thread::sleep_for(kQuiet);  // the released callback reads the close and leaves
        const int calls = Rig::io_calls();
        std::this_thread::sleep_for(kQuiet);
        check(Rig::io_calls() == calls,
              "5. the link stays off the wait: no read or accept runs after the held one");
        std::atomic<bool> gone{false};
        stuck_unless(returns_within([&] { rig.destroy_link(); }, gone), Rig::name,
                     "row 5, destroy");
        check(gone.load(), "5. the destructor joins and returns");
        Rig::watch_io(std::thread::id{}, false);
    }
}

/**
 * @brief Run every row against @p Rig , reporting each check through @p check , a callable
 *        taking `(bool ok, std::string_view what)`.
 */
template <class Rig, class Check>
void run_table(Check check) {
    std::printf("  teardown table: %s\n", Rig::name);
    row_shut_down_from_a_callback<Rig>(check);
    row_shut_down_from_the_owner<Rig>(check, false);
    row_shut_down_from_the_owner<Rig>(check, true);
    row_destroy<Rig>(check);
    row_held_before_its_read<Rig>(check);
}

}  // namespace tr::testing::teardown
