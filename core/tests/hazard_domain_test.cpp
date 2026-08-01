/**
 * @file
 * @brief `tr::mem::hazard_domain_t` on its own terms — the paths the tenant-level suite
 *        cannot reach (ADR-0072 + erratum 1).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `seam_reclaim_test` exercises the domain THROUGH `graph_t`, which is the right place to
 * assert the tenant's behavior and the wrong place to assert the domain's edges: the
 * degradation paths (no announcement word left, guards nested past a participant's words,
 * a backend that can allocate nothing) are exactly the ones a tenant cannot drive on
 * demand — and the repo's own rule is that a guard is vacuous until it is exercised. The
 * deadlock this design replaces lived on one of them.
 *
 * Every case below is written so that its pre-fix behavior is a HANG or a use-after-free,
 * not a soft failure: `retire` used to allocate a record from the injected backend and, on
 * failure, scan and then SPIN until the announcing reader cleared — which, when the retirer
 * holds a lock that reader's callback needs, is forever.
 */

#include "libtracer/hazard_domain.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/mem_heap.hpp"

namespace {

using tr::mem::hazard_domain_t;
using tr::mem::retire_link_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief Live count of @ref block_t — the instrument every case below reads. */
std::atomic<int> g_live{0};

/** @brief A tenant block carrying its own retire record FIRST, the way `value_handlers_t`
 *         must: the domain identifies a parked block by its link's address. */
struct block_t {
    retire_link_t link;
    int payload = 0;
    block_t() noexcept { g_live.fetch_add(1, std::memory_order_relaxed); }
    ~block_t() { g_live.fetch_sub(1, std::memory_order_relaxed); }
};
static_assert(offsetof(block_t, link) == 0, "the link IS the block address");

/** @brief This domain's ONE reclaimer (ADR-0072 erratum 1 — type erasure moved from the
 *         record to the domain's constructor). The block came from the global heap, so the
 *         backend argument goes unused. */
void reclaim_block(void* p, tr::mem::mem_backend_t&) noexcept { delete static_cast<block_t*>(p); }

/** @brief A backend that can allocate NOTHING — every `alloc` answers backpressure.
 *
 *  The domain must not care: since erratum 1 it allocates nothing at all. Before it, this
 *  is the backend that drove `retire` onto its blocking branch. */
struct starved_backend_t : tr::mem::mem_backend_t {
    starved_backend_t() noexcept : mem_backend_t("starved") {}
    /** @brief Nothing was ever handed out, so nothing can come back. */
    void destroy(tr::view::segment_t*) noexcept override {}
};

/** @brief Displace whatever @p slot publishes and retire it; returns the retired block. */
block_t* displace_and_retire(hazard_domain_t& d, std::atomic<block_t*>& slot) {
    block_t* old = slot.exchange(nullptr, std::memory_order_seq_cst);
    if (old != nullptr) d.retire(old->link);
    return old;
}

// ---------------------------------------------------------------------------
// (1) The domain frees what nobody announces, and the destructor sweeps the rest.
void test_retire_frees_and_destructor_sweeps() {
    std::printf("hazard_domain_t: unannounced blocks are freed; ~domain sweeps the rest:\n");
    const int before = g_live.load();
    {
        hazard_domain_t d{tr::mem::heap_backend(), &reclaim_block};
        std::atomic<block_t*> slot{nullptr};
        int peak = 0;
        for (std::size_t i = 0; i < 6 * hazard_domain_t::kRetireBatch; ++i) {
            slot.store(new block_t{}, std::memory_order_release);
            (void)displace_and_retire(d, slot);
            peak = std::max(peak, g_live.load() - before);
        }
        check(peak <= static_cast<int>(hazard_domain_t::kRetireBatch) + 1,
              "live blocks stay within one scan batch across many retires");
        slot.store(new block_t{}, std::memory_order_release);
        (void)displace_and_retire(d, slot);
    }
    check(g_live.load() == before, "~hazard_domain_t freed every parked block");
}

// ---------------------------------------------------------------------------
// (2) A backend that can allocate nothing changes nothing: retire is allocation-free.
//     PRE-FIX: this is the case that reached `alloc_record() == nullptr` and blocked.
void test_starved_backend_still_reclaims() {
    std::printf("erratum 1: a backend that allocates NOTHING still reclaims (and returns):\n");
    const int before = g_live.load();
    starved_backend_t starved;
    {
        hazard_domain_t d{starved, &reclaim_block};
        std::atomic<block_t*> slot{nullptr};
        for (std::size_t i = 0; i < 3 * hazard_domain_t::kRetireBatch; ++i) {
            slot.store(new block_t{}, std::memory_order_release);
            (void)displace_and_retire(d, slot);
        }
        check(g_live.load() - before <= static_cast<int>(hazard_domain_t::kRetireBatch),
              "retire under a starved backend still parks, scans and frees");
    }
    check(g_live.load() == before, "and its final sweep freed the remainder");
}

// ---------------------------------------------------------------------------
// (3) An announced block survives every scan until its guard clears — and only then.
void test_announced_block_survives_scans() {
    std::printf("hazard_domain_t: an announced block outlives concurrent retire + scans:\n");
    const int before = g_live.load();
    hazard_domain_t d{tr::mem::heap_backend(), &reclaim_block};
    std::atomic<block_t*> slot{new block_t{}};
    std::atomic<bool> announced{false};
    std::atomic<bool> release{false};
    block_t* watched = slot.load();

    std::thread reader([&] {
        hazard_domain_t::guard_t g{d};
        block_t* p = g.protect(slot);
        if (p == nullptr) ++g_failures;
        announced.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        // Touch the block INSIDE the guard: ASan/TSan carry the use-after-free half.
        if (p != nullptr && p->payload != 0) ++g_failures;
    });
    while (!announced.load(std::memory_order_acquire)) std::this_thread::yield();

    (void)displace_and_retire(d, slot);
    std::atomic<block_t*> churn{nullptr};
    for (std::size_t i = 0; i < 3 * hazard_domain_t::kRetireBatch; ++i) {
        churn.store(new block_t{}, std::memory_order_release);
        (void)displace_and_retire(d, churn);
    }
    check(g_live.load() > before, "the announced block is still alive after several scans");
    (void)watched;

    release.store(true, std::memory_order_release);
    reader.join();
    for (std::size_t i = 0; i < 2 * hazard_domain_t::kRetireBatch; ++i) {
        churn.store(new block_t{}, std::memory_order_release);
        (void)displace_and_retire(d, churn);
    }
    d.collect();
    check(g_live.load() == before, "and a scan AFTER the guard cleared freed it");
}

// ---------------------------------------------------------------------------
// (4) Guards nest past a participant's words: the deepest one stalls reclamation instead
//     of waiting for anything. PRE-FIX the inner claim took a NON-RECURSIVE spin lock on
//     the shared overflow cell, which a thread already holding it self-deadlocks on.
void test_nested_guards_do_not_deadlock() {
    std::printf("erratum 1: guards nest past kAnnouncePerThread — stall, never wait:\n");
    const int before = g_live.load();
    hazard_domain_t d{tr::mem::heap_backend(), &reclaim_block};
    std::atomic<block_t*> a{new block_t{}};
    std::atomic<block_t*> b{new block_t{}};
    std::atomic<block_t*> c{new block_t{}};

    {
        hazard_domain_t::guard_t g1{d};
        hazard_domain_t::guard_t g2{d};
        hazard_domain_t::guard_t g3{d};  // one past kAnnouncePerThread — the stall path
        check(g1.protect(a) != nullptr && g2.protect(b) != nullptr && g3.protect(c) != nullptr,
              "three nested guards on one thread all protect (no hang, no null)");

        // Retire all three and churn hard: the stall must hold EVERYTHING back, including
        // the two blocks that are announced by address anyway.
        (void)displace_and_retire(d, a);
        (void)displace_and_retire(d, b);
        (void)displace_and_retire(d, c);
        std::atomic<block_t*> churn{nullptr};
        for (std::size_t i = 0; i < 3 * hazard_domain_t::kRetireBatch; ++i) {
            churn.store(new block_t{}, std::memory_order_release);
            (void)displace_and_retire(d, churn);
        }
        check(g_live.load() - before >= 3, "a stalling guard freezes reclamation (nothing freed)");
    }
    d.collect();
    check(g_live.load() == before, "and reclamation resumes the moment the guards are gone");
}

// ---------------------------------------------------------------------------
// (5) More concurrent reader threads than the participant table holds. The overflow
//     threads take the stall path; nobody blocks, nothing announced is freed.
void test_more_readers_than_participants() {
    std::printf("erratum 1: kParticipants + 1 concurrent readers — the last one stalls:\n");
    const int before = g_live.load();
    hazard_domain_t d{tr::mem::heap_backend(), &reclaim_block};
    constexpr std::size_t kReaders = tr::mem::detail_hz::kParticipants + 1;

    std::vector<std::atomic<block_t*>> slots(kReaders);
    for (std::atomic<block_t*>& s : slots) s.store(new block_t{}, std::memory_order_release);

    std::atomic<std::size_t> parked{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (std::size_t i = 0; i < kReaders; ++i) {
        readers.emplace_back([&, i] {
            hazard_domain_t::guard_t g{d};
            block_t* p = g.protect(slots[i]);
            // NEST inside the parked guard, the way a user callback re-entering the graph
            // does — with the table already oversubscribed. This is the combination the
            // pre-erratum domain self-deadlocked on: an overflow reader holding the shared
            // cell's NON-RECURSIVE spin lock, then claiming again.
            hazard_domain_t::guard_t inner{d};
            block_t* q = inner.protect(slots[(i + 1) % kReaders]);
            if (p == nullptr || q == nullptr) ++g_failures;
            parked.fetch_add(1, std::memory_order_acq_rel);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
            if (p != nullptr && p->payload != 0) ++g_failures;
        });
    }
    while (parked.load(std::memory_order_acquire) < kReaders) std::this_thread::yield();

    for (std::atomic<block_t*>& s : slots) (void)displace_and_retire(d, s);
    std::atomic<block_t*> churn{nullptr};
    for (std::size_t i = 0; i < 3 * hazard_domain_t::kRetireBatch; ++i) {
        churn.store(new block_t{}, std::memory_order_release);
        (void)displace_and_retire(d, churn);
    }
    check(g_live.load() - before >= static_cast<int>(kReaders),
          "every block a parked reader holds survived, including the overflow reader's");

    release.store(true, std::memory_order_release);
    for (std::thread& t : readers) t.join();
    d.collect();
    d.collect();  // the first scan re-parks what the second one frees
    check(g_live.load() == before, "all reclaimed once every reader left");
}

}  // namespace

int main() {
    test_retire_frees_and_destructor_sweeps();
    test_starved_backend_still_reclaims();
    test_announced_block_survives_scans();
    test_nested_guards_do_not_deadlock();
    test_more_readers_than_participants();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
