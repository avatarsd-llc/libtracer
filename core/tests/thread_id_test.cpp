/**
 * @file
 * @brief #1532 — `tr::detail::thread_id_t`: the ownership-stamp identity, and the one property
 *        the ESP-IDF boot regression turned on.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * v0.15.0 abort-looped at boot on ESP-IDF because `transport_vertex_t`'s RFC-0014 S6 ownership
 * stamp called `std::this_thread::get_id()` from a native FreeRTOS task, and IDF's
 * `pthread_self()` asserts for any task it did not register — the IDF main task included. With
 * IDF's assert compiled out the same call answers `NULL`, which made the stamp compare equal to
 * the unowned sentinel and reported **every** native task as the owner of an **unowned** mutex.
 *
 * So the contract has two halves and this file pins both, on the host arm:
 *
 * 1. **An identity distinguishes contexts** — two running threads never share one, which is what
 *    makes the S6 self-check mean anything at all.
 * 2. **The sentinel is distinguishable from every live value** — `this_thread_id()` never
 *    answers `unowned_thread_id()`. This is the half that was violated on the failing platform,
 *    and it is asserted here as an invariant of the abstraction rather than inherited from
 *    whatever the platform happens to return.
 *
 * The FreeRTOS arm cannot be exercised on the host — it is compiled and linked by the esp-idf
 * CI's `full_node` build for every target, which is where its gate lives. The S6 behaviour
 * itself is unchanged and stays bound by `core/tests/net_lock_order_test.cpp`.
 */

#include "libtracer/thread_id.hpp"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "test_support.hpp"

namespace {

using tr::detail::thread_id_t;
using tr::testing::check;

/** @brief Claim 1: distinct running contexts carry distinct identities. */
void identities_distinguish_contexts() {
    std::printf("thread_id — an identity distinguishes running contexts:\n");
    const thread_id_t here = tr::detail::this_thread_id();
    check(here == tr::detail::this_thread_id(),
          "the same context answers the same identity twice (it is a stamp, not a nonce)");

    std::atomic<bool> differed{false};
    std::atomic<bool> stable{false};
    std::thread other([&] {
        const thread_id_t there = tr::detail::this_thread_id();
        differed.store(there != here);
        stable.store(there == tr::detail::this_thread_id());
    });
    other.join();
    check(differed.load(), "another thread answers a DIFFERENT identity");
    check(stable.load(), "... and its own is stable within it");
}

/**
 * @brief Claim 2 — the invariant #1532 was about: a live identity is never the sentinel.
 *
 * Checked on several threads at once rather than one, because the failing platform's inversion
 * was universal (every unregistered task collapsed onto the sentinel) and a single-thread check
 * would have caught it just as well — but a reader should see that the claim is about *all*
 * contexts, not the lucky one the test happened to run on.
 */
void the_sentinel_is_never_a_live_identity() {
    std::printf("thread_id — the unowned sentinel is distinguishable from every live value:\n");
    check(tr::detail::this_thread_id() != tr::detail::unowned_thread_id(),
          "a running context's identity is NEVER the unowned sentinel (#1532's inversion)");
    check(tr::detail::unowned_thread_id() == tr::detail::unowned_thread_id(),
          "and the sentinel is itself — a stamp cleared twice stays cleared");

    std::atomic<int> live{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < 4; ++i) {
        pool.emplace_back([&live] {
            if (tr::detail::this_thread_id() != tr::detail::unowned_thread_id()) live.fetch_add(1);
        });
    }
    for (std::thread& t : pool) t.join();
    check(live.load() == 4, "... on every context, not merely the one that asked first");
}

/**
 * @brief The stamp/clear cycle the S6 seam runs, in miniature.
 *
 * `ctl_owner_` is an `std::atomic<thread_id_t>`, so the type has to BE atomically storable —
 * which is a real constraint on the abstraction and is exactly what a `struct` identity would
 * have failed. The cycle below is the one `ctl_txn_t` performs: stamp on acquire, compare on
 * re-entry, clear on release, and compare again.
 */
void the_stamp_cycle_reads_back() {
    std::printf("thread_id — the S6 stamp/check/clear cycle:\n");
    std::atomic<thread_id_t> owner{};
    check(owner.load() == tr::detail::unowned_thread_id(),
          "a default-constructed stamp is UNOWNED (what `ctl_owner_{}` relies on)");
    check(owner.load() != tr::detail::this_thread_id(),
          "so an unowned mutex is not reported as held by this thread — the inverted case");

    owner.store(tr::detail::this_thread_id());
    check(owner.load() == tr::detail::this_thread_id(), "stamped: this thread holds it");

    std::atomic<bool> other_sees_held{true};
    std::thread other([&] { other_sees_held.store(owner.load() == tr::detail::this_thread_id()); });
    other.join();
    check(!other_sees_held.load(), "... and ANOTHER thread does not read the stamp as its own");

    owner.store(tr::detail::unowned_thread_id());
    check(owner.load() != tr::detail::this_thread_id(), "cleared: nobody holds it again");
}

}  // namespace

int main() {
    std::printf("#1532 thread-identity primitive\n\n");
    identities_distinguish_contexts();
    the_sentinel_is_never_a_live_identity();
    the_stamp_cycle_reads_back();
    return tr::testing::summary("thread_id");
}
