/**
 * @file
 * @brief GenMC harness over the SHIPPED `qsbr.hpp` (#2120): a reader inside a bracket never
 *        reads what a reclaimer has freed, under every execution the memory model allows.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Nothing here transcribes the protocol: every bracket, epoch bump, scan and drain is the
 * header's own code, driven against a domain this file owns (#2119) through its cells, so no
 * thread's record or `thread_local` exit release is involved. GenMC
 * reports a read of freed memory as an error, which is the use-after-free the grace period
 * exists to prevent. `tools/genmc_qsbr.sh` runs the matrix; the scenarios are:
 *
 * - `SCENARIO_HORIZON`: a reader opens `BRACKETS` brackets in a row and reads the published
 *   object in each; the reclaimer unpublishes it, closes the epoch and frees it once one
 *   `horizon` says the epoch is past (`graph_t::collect`'s shape).
 * - `SCENARIO_REUSE`: as above with one bracket, then the reader's thread exits and a second
 *   thread claims the same cell and brackets in it (`cell_release_t` then `claim`).
 * - `SCENARIO_RETIRE`: the reclaimer retires the object through the domain's own table with a
 *   hook that frees it, and the reader drains at its quiescent point (`reclaim_qsbr`'s shape).
 *
 * The FENCED preset is checked under RC11 and IMM. The COMPILER preset is checked under SC:
 * its argument is one hart's (`qsbr.hpp` §qsbr_single_hart), and one in-order hart with
 * preemption is exactly the set of SC interleavings. Under RC11 it would rightly fail.
 */
#include <genmc.h>
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>

// GenMC replaces `<stdlib.h>` with a shim that has no `rand`, which `<algorithm>` names in a
// template it never instantiates here; a declaration is all it needs.
namespace std {
int rand() noexcept;
}  // namespace std

#include "libtracer/qsbr.hpp"

#ifndef BRACKETS
#define BRACKETS 1
#endif

namespace q = tr::graph::detail_qsbr;

namespace {

/** @brief What the reclaimer frees and the reader reads. */
struct obj_t {
    int data; /**< @brief Read inside each bracket. */
};

/** @brief The domain under test: the header's own type, owned by this file. */
constinit q::registry_t g_domain{};
/** @brief The domain's retired slots, owned beside it. */
constinit q::retired_table_t g_table{};

std::atomic<obj_t*> g_link{nullptr}; /**< @brief The published object. */
obj_t* g_obj = nullptr;              /**< @brief The object, for the final free. */
std::atomic<int> g_freed{0};         /**< @brief Set once the object is freed. */

/** @brief The retire hook: frees the object. */
void free_hook(void* ctx) {
    std::free(ctx);
    g_freed.store(1, std::memory_order_relaxed);
}

/** @brief One bracket on cell @p idx: announce, read the published object, leave. */
void bracket(std::size_t idx) {
    const auto cell = static_cast<std::uint32_t>(idx + 1);  // the record's encoding
    q::go_online(g_domain, cell);
    const obj_t* const p = g_link.load(std::memory_order_acquire);
    volatile int r = p != nullptr ? p->data : 0;
    (void)r;
    q::go_offline(g_domain, cell);
}

void* reader(void*) {
    for (int b = 0; b < BRACKETS; ++b) bracket(0);
#if defined(SCENARIO_RETIRE)
    q::drain(g_domain, g_table);  // the quiescent point
#endif
#if defined(SCENARIO_REUSE)
    // `cell_release_t::~cell_release_t`: go quiescent, then give the cell back.
    q::cell_t& c = g_domain.cells[0];
    c.state.store(0, std::memory_order_seq_cst);
    c.claimed.store(false, std::memory_order_release);
#endif
    return nullptr;
}

#if defined(SCENARIO_REUSE)
void* successor(void*) {
    // `claim`: the next thread takes the same cell.
    bool expected = false;
    __VERIFIER_assume(g_domain.cells[0].claimed.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed));
    bracket(0);
    return nullptr;
}
#endif

void* reclaimer(void*) {
    g_link.store(nullptr, std::memory_order_relaxed);  // the unpublish
#if defined(SCENARIO_RETIRE)
    (void)q::retire(g_obj, free_hook, g_domain, g_table);
#else
    const std::uint32_t e = q::advance(g_domain);
    const auto past = [e](const q::horizon_t& h) { return h.past(e); };
    if (q::horizon(past, g_domain).past(e)) free_hook(g_obj);
#endif
    return nullptr;
}

}  // namespace

int main() {
    g_obj = static_cast<obj_t*>(std::malloc(sizeof(obj_t)));
    g_obj->data = 42;
    g_link.store(g_obj, std::memory_order_relaxed);
    g_domain.cells[0].claimed.store(true, std::memory_order_relaxed);  // the reader owns cell 0
    pthread_t r;
    pthread_t w;
    pthread_create(&r, nullptr, reader, nullptr);
#if defined(SCENARIO_REUSE)
    pthread_t s;
    pthread_create(&s, nullptr, successor, nullptr);
#endif
    pthread_create(&w, nullptr, reclaimer, nullptr);
    pthread_join(r, nullptr);
    pthread_join(w, nullptr);
#if defined(SCENARIO_REUSE)
    pthread_join(s, nullptr);
#endif
    // A deferred free is a later pass's: release it here so the run leaks nothing.
    if (g_freed.load(std::memory_order_relaxed) == 0 && g_domain.ctl.live.load() == 0)
        std::free(g_obj);
    return 0;
}
