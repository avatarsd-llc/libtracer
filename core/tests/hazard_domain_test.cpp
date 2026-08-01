/**
 * @file
 * @brief `tr::mem::hazard_domain_t` on its own terms — the paths the tenant-level suite
 *        cannot reach (ADR-0072 + errata 1 and 2).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `seam_reclaim_test` exercises the domain THROUGH `graph_t`, which is the right place to
 * assert the tenant's behavior and the wrong place to assert the domain's edges: the
 * degradation paths (guards nested deeper than a participant reaches, more long-lived
 * readers than a fixed table would hold, announcement storage that cannot be obtained at
 * all, a backend that can allocate nothing) are exactly the ones a tenant cannot drive on
 * demand — and the repo's own rule is that a guard is vacuous until it is exercised. Both
 * blocking defects this design has had lived on one of them.
 *
 * Every case below is written so that its pre-fix behavior is a HANG, a use-after-free, or
 * unbounded growth, not a soft failure.
 */

#include "libtracer/hazard_domain.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
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

/**
 * @brief The TENANT CONTRACT in one helper: displace, park, and collect where no lock is
 *        held (ADR-0072 erratum 2).
 *
 * `retire` deliberately frees nothing — freeing runs the block's destructor, which for the
 * real tenant is embedder code — so a tenant pairs it with a `reclaim_due()`-gated
 * `collect()` at a point where it holds nothing. `graph_t::retire` does exactly this, past
 * `map_mutex_`.
 */
block_t* displace_and_retire(hazard_domain_t& d, std::atomic<block_t*>& slot) {
    block_t* old = slot.exchange(nullptr, std::memory_order_seq_cst);
    if (old != nullptr) d.retire(old->link);
    if (d.reclaim_due()) d.collect();
    return old;
}

/** @brief How many of the nested guards below actually protected something. */
int g_nested_protects = 0;

/**
 * @brief Take @p depth nested guards on this thread — each protecting one of @p held — and
 *        run @p body at the bottom of the stack.
 *
 * Recursion rather than a container of guards, because that is the shape the real nesting
 * has: a seam read whose user callback re-enters the graph and reads another seam.
 */
template <typename F>
void nest_guards(hazard_domain_t& d, std::vector<std::atomic<block_t*>>& held, std::size_t depth,
                 F&& body) {
    if (depth == 0) {
        body();
        return;
    }
    hazard_domain_t::guard_t g{d};
    if (g.protect(held[depth - 1]) != nullptr) ++g_nested_protects;
    nest_guards(d, held, depth - 1, static_cast<F&&>(body));
}

/** @brief Retire @p n fresh blocks through @p d, the tenant way. */
void churn(hazard_domain_t& d, std::size_t n) {
    std::atomic<block_t*> slot{nullptr};
    for (std::size_t i = 0; i < n; ++i) {
        slot.store(new block_t{}, std::memory_order_release);
        (void)displace_and_retire(d, slot);
    }
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
//     PRE-ERRATUM-1: this is the case that reached `alloc_record() == nullptr` and blocked.
void test_starved_backend_still_reclaims() {
    std::printf("erratum 1: a backend that allocates NOTHING still reclaims (and returns):\n");
    const int before = g_live.load();
    starved_backend_t starved;
    {
        hazard_domain_t d{starved, &reclaim_block};
        churn(d, 3 * hazard_domain_t::kRetireBatch);
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
    churn(d, 3 * hazard_domain_t::kRetireBatch);
    check(g_live.load() > before, "the announced block is still alive after several scans");

    release.store(true, std::memory_order_release);
    reader.join();
    churn(d, 2 * hazard_domain_t::kRetireBatch);
    d.collect();
    check(g_live.load() == before, "and a scan AFTER the guard cleared freed it");
}

// ---------------------------------------------------------------------------
// (4) Guards nest FAR past a participant's inline words. The thread extends its own
//     announcement chain; nothing stalls, and reclamation keeps running domain-wide.
//
//     PRE-ERRATUM-1 the inner claim took a NON-RECURSIVE spin lock on a shared overflow
//     cell, which a thread already holding it self-deadlocks on. Between erratum 1 and
//     this pass the third level instead set the per-DOMAIN stall counter, which froze
//     reclamation for every tenant — one thread's nesting depth stopping everyone else's
//     reclamation is the #576 defect wearing a different hat, so it is asserted against
//     here rather than asserted FOR (as the previous revision of this case did).
void test_deep_nesting_does_not_stall_the_domain() {
    std::printf("erratum 2: guards nest to any depth WITHOUT pausing reclamation:\n");
    const int before = g_live.load();
    hazard_domain_t d{tr::mem::heap_backend(), &reclaim_block};
    constexpr std::size_t kDepth = 4 * tr::mem::detail_hz::kAnnouncePerThread + 1;

    std::vector<std::atomic<block_t*>> held(kDepth);
    for (std::atomic<block_t*>& s : held) s.store(new block_t{}, std::memory_order_release);

    nest_guards(d, held, kDepth, [&] {
        check(g_nested_protects == static_cast<int>(kDepth),
              "nine nested guards on one thread all protect (no hang, no null, no stall)");

        // With that stack of guards live, retire blocks NOBODY announces. They must still
        // be freed: one thread's nesting depth is not the domain's business.
        const int with_guards = g_live.load();
        churn(d, 6 * hazard_domain_t::kRetireBatch);
        check(g_live.load() <= with_guards + static_cast<int>(hazard_domain_t::kRetireBatch),
              "unannounced blocks are still reclaimed while nine guards are nested");

        // And the deeply announced ones are NOT freed.
        for (std::atomic<block_t*>& s : held) (void)displace_and_retire(d, s);
        churn(d, 6 * hazard_domain_t::kRetireBatch);
        check(g_live.load() - before >= static_cast<int>(kDepth),
              "every block a nested guard announced survived (including past the inline words)");
    });
    d.collect();
    d.collect();
    check(g_live.load() == before, "and all of them are reclaimed once the guards unwind");
}

// ---------------------------------------------------------------------------
// (5) THE STARVATION SHAPE. More long-lived reader threads than a fixed participant table
//     would have held, each taking guards continuously, while the control plane retires
//     20 000 blocks.
//
//     This is the case round 2 measured as broken: with a fixed 64-participant table the
//     65th thread got no word on EVERY guard, set the per-domain stall, and reclamation
//     froze — 12 159 live blocks after 20 000 retires and still climbing, with each retire
//     re-walking the parked set (0.062 µs/retire unstalled vs 105 µs/retire at 15 000
//     parked). Participants are unbounded now, so there is no 65th thread.
//
//     The live-block bound is also the O(1) proof for `retire`: a scan walks at most the
//     parked set, and the collect threshold is twice what a scan could not free, so
//     bounding the parked set bounds both the growth AND the per-retire work.
void test_more_long_lived_readers_than_a_table_would_hold() {
    std::printf("round 2: 65 long-lived readers + 20k retires — bounded, and not O(parked):\n");
    const int before = g_live.load();
    hazard_domain_t d{tr::mem::heap_backend(), &reclaim_block};
    constexpr std::size_t kReaders = 65;  // one past the fixed table this design replaced
    constexpr std::size_t kRetires = 20000;

    std::vector<std::atomic<block_t*>> slots(kReaders);
    for (std::atomic<block_t*>& s : slots) s.store(new block_t{}, std::memory_order_release);

    std::atomic<std::size_t> running{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (std::size_t i = 0; i < kReaders; ++i) {
        readers.emplace_back([&, i] {
            running.fetch_add(1, std::memory_order_acq_rel);
            while (!stop.load(std::memory_order_relaxed)) {
                hazard_domain_t::guard_t g{d};
                block_t* p = g.protect(slots[i]);
                // NEST, the way a user callback re-entering the graph does.
                hazard_domain_t::guard_t inner{d};
                block_t* q = inner.protect(slots[(i + 1) % kReaders]);
                if (p == nullptr || q == nullptr) ++g_failures;
                std::this_thread::yield();
            }
        });
    }
    while (running.load(std::memory_order_acquire) < kReaders) std::this_thread::yield();

    std::atomic<block_t*> slot{nullptr};
    int peak = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto half = t0;
    for (std::size_t i = 0; i < kRetires; ++i) {
        slot.store(new block_t{}, std::memory_order_release);
        (void)displace_and_retire(d, slot);
        peak = std::max(peak, g_live.load() - before);
        if (i + 1 == kRetires / 2) half = std::chrono::steady_clock::now();
    }
    const auto t1 = std::chrono::steady_clock::now();
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& t : readers) t.join();

    // The readers hold their OWN blocks, never the retired stream's, so nothing in that
    // stream is announced and the parked set can only be what the threshold allows.
    const int bound = static_cast<int>(kReaders + 4 * hazard_domain_t::kRetireBatch);
    check(peak <= bound,
          "peak live blocks stayed bounded across 20k retires (was: 12 159 and climbing)");
    const double first_us =
        std::chrono::duration<double, std::micro>(half - t0).count() / (kRetires / 2);
    const double second_us =
        std::chrono::duration<double, std::micro>(t1 - half).count() / (kRetires / 2);
    // Reported, not asserted: a wall-clock ratio is not a stable gate. The BOUND above is
    // the gate, and it is what makes the cost flat.
    std::printf(
        "  [note] %.3f us/retire over the first half, %.3f over the second (peak live %d)\n",
        first_us, second_us, peak);

    for (std::atomic<block_t*>& s : slots) (void)displace_and_retire(d, s);
    d.collect();
    d.collect();
    check(g_live.load() == before, "and everything is reclaimed once the readers leave");
}

// ---------------------------------------------------------------------------
// (6) The stall path itself — now reachable ONLY through an allocation failure, injected
//     here because a branch nothing exercises is a vacuous guard.
//
//     Two properties: a stalled guard is SAFE (nothing it could hold is freed), and a
//     stalled domain is CHEAP (a scan declines in O(1) and raises its own threshold, so
//     `retire` does not become O(parked) for as long as the stall lasts — the round-2
//     defect, whose measured shape was every retire re-walking and re-parking the batch).
void test_stall_is_safe_and_cheap() {
    std::printf("erratum 2: an unservable guard stalls — safely, and in O(1) per retire:\n");
    const int before = g_live.load();
    hazard_domain_t d{tr::mem::heap_backend(), &reclaim_block};
    std::atomic<block_t*> watched{new block_t{}};
    std::atomic<bool> stalled{false};
    std::atomic<bool> release{false};

    // A FRESH thread, so it has to claim a participant — which the injected failure denies.
    tr::mem::detail_hz::g_deny_announce_storage.store(true, std::memory_order_relaxed);
    std::thread reader([&] {
        hazard_domain_t::guard_t g{d};
        block_t* p = g.protect(watched);
        if (p == nullptr) ++g_failures;
        stalled.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        if (p != nullptr && p->payload != 0) ++g_failures;  // ASan carries the UAF half
    });
    while (!stalled.load(std::memory_order_acquire)) std::this_thread::yield();
    tr::mem::detail_hz::g_deny_announce_storage.store(false, std::memory_order_relaxed);

    (void)displace_and_retire(d, watched);
    churn(d, 4 * hazard_domain_t::kRetireBatch);
    check(g_live.load() - before >= 1,
          "a stalled guard's block is not freed (it announced no address to check)");
    // The O(1) half, structurally: after a scan under the stall the domain must NOT be
    // asking to be collected again — that is the raised threshold, and it is the whole
    // difference between amortized O(1) and the measured 105 us/retire.
    d.collect();
    check(!d.reclaim_due(),
          "a scan under a stall raises the threshold, so the next retire does not re-scan");

    release.store(true, std::memory_order_release);
    reader.join();
    d.collect();
    d.collect();
    check(g_live.load() == before, "and reclamation resumes the moment the stall clears");
}

}  // namespace

int main() {
    test_retire_frees_and_destructor_sweeps();
    test_starved_backend_still_reclaims();
    test_announced_block_survives_scans();
    test_deep_nesting_does_not_stall_the_domain();
    test_more_long_lived_readers_than_a_table_would_hold();
    test_stall_is_safe_and_cheap();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
