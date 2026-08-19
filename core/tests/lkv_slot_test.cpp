/**
 * @file
 * @brief ADR-0069 — the LKV slot policies, both of them, in whatever build this is.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The policies are named here **explicitly**, not through `tr::graph::lkv_slot_t`, so one
 * ordinary CI leg exercises both — ADR-0069 §1 wanted "the host test build instantiates both
 * policies", and since slices 1–2 delivered the per-target seam with zero templates, naming
 * the types is all that takes. It matters most for the sanitizer legs: `hazard_slot_t` is
 * lock-free reclamation, where the failure mode is a use-after-free that only ASan and TSan
 * see, and where a passing single-threaded test proves close to nothing.
 *
 * What is asserted, per policy:
 *
 *   - the contract `vertex.hpp` calls: round-trip, empty load, replace, clear with `release`;
 *   - **ownership** — a handle taken before a replace stays valid and keeps its bytes, which
 *     is the property `read_subtree_folded` needs and the one #642 found the bench arms were
 *     not measuring;
 *   - **reclamation** — every published rope is freed by the time the slot is gone, checked
 *     with a counting deleter rather than by inspection;
 *   - **concurrency** — writers and readers on one slot, with each rope carrying a
 *     self-checking tag so a torn or recycled read is caught in a plain build too;
 *   - **the bound** — a lone writer's parked set stays inside ADR-0069 §2's "one batch",
 *     which is the RAM argument that chose hazard over epoch;
 *   - **exhaustion** — with every hazard index claimed, readers and writers still agree,
 *     which is the fallback ADR-0069 §3 promised (by a different mechanism; see the header);
 *   - **containment of an over-capacity thread** (#899, #1027) — claiming an index writes no
 *     byte of the announcement table, a thread that could not claim one stops re-probing every
 *     index on every operation, and taking the overflow spin lock writes no byte of the line
 *     `orphans` is read on; the three are what makes "costs the overflow threads and nothing
 *     else" a property rather than an aspiration;
 *   - **a declined publish is reported** — the whole point of `store` returning `bool`, driven
 *     here by replacing this binary's nothrow `operator new` rather than by inspection;
 *   - **the exit sweep spares live participants** (#898) — driven by an injected
 *     `final_sweep_t`, since a static-destruction object is otherwise unobservable, once as a
 *     deterministic snapshot of a blocked worker's lists and once as a race a sanitizer sees.
 */

#include "libtracer/lkv_slot.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>
#include <thread>
#include <vector>

#include "test_support.hpp"

namespace {

using tr::graph::hazard_slot_t;
using tr::graph::sp_atomic_slot_t;
using tr::view::rope_t;

using tr::testing::check;

/** @brief Shorthand for the order the counters in this test use. */
constexpr std::memory_order relaxed_ = std::memory_order_relaxed;

/** @brief How many ropes this test has published and not yet seen freed. */
std::atomic<std::size_t> g_live{0};

/**
 * @brief A rope carrying a self-checking identity, so a reader can tell a live value from
 *        recycled memory without a sanitizer's help.
 */
struct tagged_rope_t : rope_t {
    std::uint64_t tag = 0;     /**< @brief Which publish produced this rope. */
    std::uint64_t inverse = 0; /**< @brief `~tag`, re-derived on every read. */
};

/** @brief Publish-ready rope number @p tag, counted into @ref g_live until it is freed. */
[[nodiscard]] std::shared_ptr<const rope_t> make_tagged(std::uint64_t tag) {
    auto* raw = new tagged_rope_t;
    raw->tag = tag;
    raw->inverse = ~tag;
    g_live.fetch_add(1, std::memory_order_relaxed);
    return std::shared_ptr<const tagged_rope_t>(raw, [](const tagged_rope_t* p) {
        g_live.fetch_sub(1, std::memory_order_relaxed);
        delete p;
    });
}

/** @brief Whether @p sp is a rope this test published and still holds its own identity. */
[[nodiscard]] bool intact(const std::shared_ptr<const rope_t>& sp) {
    if (!sp) return false;
    const auto* t = static_cast<const tagged_rope_t*>(sp.get());
    return t->inverse == ~t->tag;
}

/** @brief The identity @ref make_tagged stamped into @p sp. */
[[nodiscard]] std::uint64_t tag_of(const std::shared_ptr<const rope_t>& sp) {
    return static_cast<const tagged_rope_t*>(sp.get())->tag;
}

/** @brief The `vertex.hpp` call shape, on one policy, single-threaded. */
template <typename slot_t>
void contract(const char* name) {
    std::printf("%s — the contract vertex.hpp calls:\n", name);
    {
        slot_t slot;
        check(slot.load() == nullptr, "a slot nobody wrote reads as empty");

        check(slot.store(make_tagged(1)), "a publish onto a fresh slot reports success");
        const auto first = slot.load();
        check(intact(first) && tag_of(first) == 1, "the published value reads back");

        check(slot.store(make_tagged(2)), "and so does a publish that replaces a value");
        check(intact(first) && tag_of(first) == 1,
              "a handle taken before the replace still owns its value");
        const auto second = slot.load();
        check(intact(second) && tag_of(second) == 2, "and the replacement is what reads back");

        slot.clear(std::memory_order_release);  // revert_to_placeholder's clear
        check(slot.load() == nullptr, "a release-ordered clear empties the slot");
        check(intact(second) && tag_of(second) == 2, "the cleared value is still the reader's");
    }
    check(g_live.load() == 0, "every published rope was freed by the time the slot was gone");
}

/**
 * @brief How long a writer waits for the readers to be inside their loop (#1378).
 *
 * A bound, not a timing assumption. The writers' work is bounded, so without a start latch
 * they can finish and set `stop` before a reader's first iteration on an oversubscribed
 * host — which is what made "the readers actually ran" a ~0.7 %-per-run flake rather than a
 * statement about the slot. The latch removes that; the deadline keeps a host that genuinely
 * cannot schedule a reader from hanging the suite, and the non-vacuity check below is what
 * reports it.
 */
constexpr auto kReaderLatchWait = std::chrono::seconds(2);

/**
 * @brief Writers replacing one slot while readers read it.
 *
 * The readers' job is to fail loudly on anything that is not a value some writer published:
 * a null where one was published, or an identity that does not re-derive.
 *
 * Every reader runs one UNCONDITIONAL pass before it ever consults `stop`, and the writers
 * wait (bounded) for all of them to announce that pass before storming. Both halves are
 * about the same thing: the overlap this function is named for is established by the test
 * rather than hoped for from the scheduler (#1378).
 */
template <typename slot_t>
void concurrent(const char* name, std::size_t writers, std::size_t readers, std::size_t rounds) {
    std::printf("%s — %zu writers / %zu readers on one slot:\n", name, writers, readers);
    slot_t slot;
    (void)slot.store(make_tagged(0));

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> bad{0};
    std::atomic<std::size_t> reads{0};
    std::atomic<std::size_t> started{0};
    std::vector<std::thread> pool;

    for (std::size_t r = 0; r < readers; ++r) {
        pool.emplace_back([&] {
            std::size_t n = 0;
            // Hold two handles at once: the N-simultaneous-pins property
            // read_subtree_folded needs, which one hazard slot per thread cannot express
            // and which is exactly why the read has to promote (ADR-0069 §5).
            const auto pass = [&] {
                const auto a = slot.load();
                const auto b = slot.load();
                if (!intact(a) || !intact(b)) bad.fetch_add(1, std::memory_order_relaxed);
                ++n;
            };
            pass();
            started.fetch_add(1, std::memory_order_release);
            while (!stop.load(std::memory_order_relaxed)) pass();
            reads.fetch_add(n, std::memory_order_relaxed);
        });
    }
    for (std::size_t w = 0; w < writers; ++w) {
        pool.emplace_back([&, w] {
            const auto deadline = std::chrono::steady_clock::now() + kReaderLatchWait;
            while (started.load(std::memory_order_acquire) < readers &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            for (std::size_t i = 0; i < rounds; ++i) {
                if (!slot.store(make_tagged(w * rounds + i + 1))) bad.fetch_add(1, relaxed_);
            }
        });
    }
    for (std::size_t i = readers; i < pool.size(); ++i) pool[i].join();
    stop.store(true, std::memory_order_relaxed);
    for (std::size_t i = 0; i < readers; ++i) pool[i].join();

    check(bad.load() == 0, "every concurrent read returned an intact published value");
    // Non-vacuity, and now structural: the readers are joined above, and each of them ran a
    // pass that no `stop` could cut short. A shortfall here means a reader died, not that it
    // lost a race.
    check(reads.load() >= readers, "every reader ran at least its unconditional pass");
    check(intact(slot.load()), "the slot still holds a good value afterwards");
}

/** @brief Publishing hard from one thread must not park an unbounded set (ADR-0069 §2). */
template <typename slot_t>
void bounded_parking(const char* name, std::size_t publishes) {
    std::printf("%s — a lone writer's parked set stays bounded:\n", name);
    std::size_t peak = 0;
    {
        slot_t slot;
        for (std::size_t i = 0; i < publishes; ++i) {
            (void)slot.store(make_tagged(i + 1));
            peak = std::max(peak, g_live.load(std::memory_order_relaxed));
        }
    }
    // One batch of parked nodes, plus the value the slot itself holds. `sp_atomic_slot_t`
    // reclaims on the spot and sits at 1; the point is that neither grows with `publishes`.
    const std::size_t bound = tr::graph::detail_hp::kRetireBatch + 2;
    std::printf("    peak live ropes over %zu publishes = %zu (bound %zu)\n", publishes, peak,
                bound);
    check(peak <= bound, "the parked set is one batch, not a function of the write count");
    check(g_live.load() == 0, "and it drains completely when the slot dies");
}

/**
 * @brief With every hazard index claimed, the overflow path has to carry the whole load.
 *
 * White-box on purpose: claiming the registry directly costs one atomic store per index,
 * where reproducing it honestly would need `kHazardReaderSlots + 1` live threads — too heavy
 * to run under TSan, which is the leg this test most needs to be in.
 */
void exhausted_registry() {
    std::printf("hazard_slot_t — every index claimed, so readers and writers overflow:\n");
    auto& reg = tr::graph::detail_hp::registry();
    std::vector<std::size_t> taken;
    for (std::size_t i = 0; i < tr::graph::kHazardReaderSlots; ++i) {
        if (tr::graph::detail_hp::try_claim(reg, i)) taken.push_back(i);
    }
    check(taken.size() + 1 >= tr::graph::kHazardReaderSlots,
          "the registry is full (this thread may already hold one index)");

    concurrent<hazard_slot_t>("  overflow", 1, 2, 500);

    for (std::size_t i : taken) tr::graph::detail_hp::release_claim(reg, i);
}

/**
 * @brief Claiming an index must not write into the announcement table (#899).
 *
 * Stated as bytes on purpose. The defect was storage: the claim flag lived inside `cell_t`,
 * so every probe of an index was a read-modify-write on the line whose owner announces its
 * pin there — and nothing in the abstract machine distinguishes that from a probe elsewhere,
 * only the machine running it, which invalidates the reader's hot line for one and not the
 * other. What IS observable here is that a claim leaves the announcement table's object
 * representation untouched; the header's `alignas` assertions carry the other half (the table
 * starts on a line of its own).
 *
 * Single-threaded by construction — the comparison reads the announcement bytes directly, so
 * it must run where no other participant can be storing a pin.
 */
void claiming_writes_nothing_into_the_announcement_table() {
    namespace hp = tr::graph::detail_hp;
    std::printf("hazard_slot_t — claiming an index leaves every announcement byte alone:\n");
    auto& reg = hp::registry();

    std::size_t victim = hp::kNoIndex;
    std::array<unsigned char, sizeof(reg.cells)> before{};
    std::memcpy(before.data(), &reg.cells, sizeof(reg.cells));
    for (std::size_t i = 0; i < tr::graph::kHazardReaderSlots; ++i) {
        if (hp::try_claim(reg, i)) {
            victim = i;
            break;
        }
    }
    std::array<unsigned char, sizeof(reg.cells)> after{};
    std::memcpy(after.data(), &reg.cells, sizeof(reg.cells));
    if (victim != hp::kNoIndex) hp::release_claim(reg, victim);

    check(victim != hp::kNoIndex, "an index was actually claimed, so there was something to see");
    check(std::memcmp(before.data(), after.data(), before.size()) == 0,
          "claiming an index modified no byte of the announcement table");
}

/**
 * @brief An over-capacity thread must not re-probe the whole claim table per operation (#899).
 *
 * Counted rather than timed: the defect is a *number* of read-modify-writes, and a wall-clock
 * reading of a contention effect on a shared runner is not evidence of anything. The bound is
 * relative to the work done — "fewer probes than operations" — because an absolute one would
 * pass vacuously on a thread that happened to claim an index.
 */
void overflow_thread_stops_sweeping_the_claim_table() {
    namespace hp = tr::graph::detail_hp;
    std::printf("hazard_slot_t — an over-capacity thread does not re-probe every index:\n");
    auto& reg = hp::registry();
    std::vector<std::size_t> taken;
    for (std::size_t i = 0; i < tr::graph::kHazardReaderSlots; ++i) {
        if (hp::try_claim(reg, i)) taken.push_back(i);
    }

    constexpr std::size_t kOps = 500;
    std::size_t probes = 0;
    std::size_t idx = 0;
    std::size_t good = 0;
    {
        hazard_slot_t slot;
        (void)slot.store(make_tagged(7));
        std::thread over([&] {
            const std::size_t before = hp::self().claim_probes();
            for (std::size_t i = 0; i < kOps; ++i) {
                if (intact(slot.load())) ++good;
            }
            probes = hp::self().claim_probes() - before;
            idx = hp::self().index();
        });
        over.join();
    }
    for (std::size_t i : taken) hp::release_claim(reg, i);

    check(idx == hp::kNoIndex, "the thread really was over capacity (it claimed no index)");
    check(good == kOps, "and every one of its reads returned an intact value");
    std::printf("    %zu claim-table RMWs over %zu operations (budget < %zu)\n", probes, kOps,
                kOps);
    check(probes < kOps, "an over-capacity thread issues fewer claim RMWs than it does reads");
}

/**
 * @brief The overflow spin lock must not write the line `orphans` lives on (#1027).
 *
 * The sibling of the claim-table finding, against a different reader. A thread that could not
 * claim an index takes and drops this flag once per operation — two read-modify-writes, one at
 * each end of its ticket — while threads inside the budget LOAD `orphans`, in `scan` and in
 * `retire_and_flush` on every slot destruction. Unpadded, the flag sat eight bytes past
 * `orphans`, so an over-capacity thread took a line in-capacity threads read exclusive.
 *
 * Asserted as bytes rather than as addresses, because the address arithmetic alone would not
 * say the lock ever WRITES anything: the line is snapshotted, the flag is taken and left held,
 * and the snapshot is compared. The "the lock was free" check ahead of it is what stops the
 * comparison passing vacuously — a `test_and_set` on an already-set flag writes the same value
 * back and would dirty nothing even with the two on one line. The line-disjointness check that
 * follows is the layout half, and the header's `alignas` assertion carries it into every build.
 *
 * Single-threaded by construction: it reads the registry's bytes directly.
 */
void the_overflow_lock_does_not_dirty_the_orphan_line() {
    namespace hp = tr::graph::detail_hp;
    std::printf("hazard_slot_t — taking the overflow lock leaves the orphan line alone:\n");
    if constexpr (tr::graph::kCacheLineBytes == 0) {
        std::printf("    single-core profile: no cache lines to share, nothing to assert\n");
    } else {
        constexpr std::size_t kLine = tr::graph::kCacheLineBytes;
        auto& reg = hp::registry();
        const auto* base = reinterpret_cast<const unsigned char*>(&reg);
        check(reinterpret_cast<std::uintptr_t>(base) % kLine == 0,
              "the registry itself starts on a line, so its offsets are line offsets");

        // The whole line `orphans` sits on, clipped to the registry so nothing past it is read.
        const auto orphans_off =
            static_cast<std::size_t>(reinterpret_cast<const unsigned char*>(&reg.orphans) - base);
        const std::size_t lo = orphans_off - orphans_off % kLine;
        const std::size_t hi = std::min(lo + kLine, sizeof(hp::registry_t));

        std::vector<unsigned char> before(hi - lo);
        std::vector<unsigned char> after(hi - lo);
        std::memcpy(before.data(), base + lo, before.size());
        const bool was_free = !reg.overflow_lock.flag.test_and_set(std::memory_order_acquire);
        std::memcpy(after.data(), base + lo, after.size());
        if (was_free) reg.overflow_lock.flag.clear(std::memory_order_release);

        const auto flag_off = static_cast<std::size_t>(
            reinterpret_cast<const unsigned char*>(&reg.overflow_lock.flag) - base);
        std::printf("    orphans at +%zu (line %zu), overflow lock flag at +%zu (line %zu)\n",
                    orphans_off, orphans_off / kLine, flag_off, flag_off / kLine);

        check(was_free, "the lock was free, so this really did write it (never a no-op compare)");
        check(std::memcmp(before.data(), after.data(), before.size()) == 0,
              "taking the overflow lock modified no byte of the line orphans is read on");
        check(flag_off / kLine != orphans_off / kLine,
              "and the flag the lock writes is not on the line orphans is read on");
    }
}

/**
 * @brief What the orphan path actually guarantees: released at slot death **or at the next
 *        domain scan** (#1037).
 *
 * `retire_and_flush` used to claim the stronger "released when its slot dies", which its own
 * `orphans` probe cannot deliver — the probe is a relaxed check-then-act, and no ordering inside
 * that function closes the window (the adopting `scan` races the same push). The claim was
 * weakened; this pins what replaced it, from the outside, on the shape that produces an orphan
 * in the first place: a writer thread that exits while the slot it published into is still alive.
 *
 * **Non-vacuity is the point.** A test that only asserted "everything was freed at the end" would
 * pass against a build with no orphan path at all. So the orphan is asserted **present** — both
 * as a non-null domain list and as a rope still counted live — *before* the scan that drains it,
 * and the drain is then attributed to that scan rather than to teardown.
 *
 * Single-threaded at every point it reads the registry: the writer is joined first, and joining
 * orders `~participant_t`'s push before these loads.
 */
void orphans_drain_at_the_next_scan() {
    namespace hp = tr::graph::detail_hp;
    std::printf("hazard_slot_t — an exited writer's parked value is released by the next scan:\n");
    auto& reg = hp::registry();

    // Adopt anything an earlier probe left parked, so what this test observes is its own doing.
    hp::retire_and_flush(nullptr);
    const std::size_t base = g_live.load(relaxed_);

    // The slot outlives the writer on purpose — that is what leaves the node on the writer's
    // list at exit instead of flushing it through `~hazard_slot_t` on the writer's own thread.
    hazard_slot_t slot;
    std::thread writer([&slot] {
        check(slot.store(make_tagged(101)), "the writer published onto the shared slot");
        check(slot.store(make_tagged(102)), "and displaced it, parking the first on its own list");
    });
    writer.join();

    const bool orphaned = reg.orphans.load(std::memory_order_relaxed) != nullptr;
    const std::size_t after_exit = g_live.load(relaxed_);
    std::printf("    after the writer exited: orphans=%s, live ropes=%zu (was %zu)\n",
                orphaned ? "present" : "none", after_exit, base);
    check(orphaned, "the exited writer really did orphan its retired list (non-vacuous)");
    check(after_exit == base + 2,
          "and both ropes are still allocated — the parked one and the slot's own");

    // The next scan on any thread. This is the whole of the weakened guarantee.
    hp::retire_and_flush(nullptr);

    const std::size_t after_scan = g_live.load(relaxed_);
    std::printf("    after the next scan:      orphans=%s, live ropes=%zu\n",
                reg.orphans.load(std::memory_order_relaxed) != nullptr ? "present" : "none",
                after_scan);
    check(reg.orphans.load(std::memory_order_relaxed) == nullptr,
          "the scan adopted the orphan list");
    check(after_scan == base + 1, "and released the parked rope, leaving only the slot's value");

    slot.clear();
    hp::retire_and_flush(nullptr);
    check(g_live.load(relaxed_) == base, "clearing the slot released the rest");
}

/**
 * @brief The exit sweep must not free the lists of a participant that is still live (#898).
 *
 * The sweep is a static-destruction object, so the honest instrument is an **injected
 * trigger**: `final_sweep_t`'s destructor *is* the whole of the behaviour under test, and
 * running one on the stack runs exactly the production path at a moment where a worker thread
 * is provably still claimed. The worker parks nodes, announces that it has, and then blocks —
 * so the comparison below reads lists nobody is touching, which makes the check deterministic
 * rather than a race the test hopes to win.
 */
void sweep_spares_a_live_participant() {
    namespace hp = tr::graph::detail_hp;
    std::printf("hazard_slot_t — the exit sweep leaves a live participant's lists alone:\n");

    std::atomic<bool> parked{false};
    std::atomic<bool> resume{false};
    std::atomic<std::size_t> widx{hp::kNoIndex};
    std::atomic<bool> reread_ok{false};
    {
        hazard_slot_t slot;
        std::thread worker([&] {
            for (std::uint64_t i = 0; i < 4; ++i) (void)slot.store(make_tagged(i + 1));
            widx.store(hp::self().index());
            parked.store(true);
            while (!resume.load()) std::this_thread::yield();
            for (std::uint64_t i = 0; i < 64; ++i) (void)slot.store(make_tagged(i + 100));
            reread_ok.store(intact(slot.load()));
        });

        while (!parked.load()) std::this_thread::yield();
        auto& reg = hp::registry();
        const std::size_t idx = widx.load();
        check(idx != hp::kNoIndex, "the worker claimed a domain index");
        const hp::lists_t& l = reg.lists[idx];
        const hp::node_t* const retired = l.retired;
        const hp::node_t* const freelist = l.freelist;
        const std::size_t retired_n = l.retired_n;
        const std::size_t freelist_n = l.freelist_n;
        check(retired != nullptr, "and parked nodes on it, so the sweep has something to take");

        {
            hp::final_sweep_t sweep;  // exactly what exit runs, while the worker is still live
            (void)sweep;
        }

        // Asked through the same operation the sweep uses: an index a live participant owns
        // must be unclaimable. Handed straight back if it somehow was not, so a failure here
        // reports one defect instead of cascading into the worker's lists.
        const bool stolen = hp::try_claim(reg, idx);
        if (stolen) hp::release_claim(reg, idx);
        check(!stolen, "the worker still owns its index after the sweep");
        check(l.retired == retired && l.freelist == freelist && l.retired_n == retired_n &&
                  l.freelist_n == freelist_n,
              "and the sweep freed none of the nodes that index owns");

        resume.store(true);
        worker.join();
        check(reread_ok.load(), "the worker publishes and reads correctly on the far side");
    }
    check(g_live.load() == 0, "and every rope is still reclaimed once the slot dies");
}

/**
 * @brief The same defect as a race rather than a snapshot: sweeps against a thread that is
 *        inside `store()` / `load()`, which is the shape ASan and TSan see.
 *
 * A plain build can only report a value that came back wrong; the sanitizer legs are what
 * name the use-after-free on `l.freelist` between `acquire_node`'s read and its use.
 */
void sweep_races_a_live_writer(std::size_t sweeps) {
    namespace hp = tr::graph::detail_hp;
    std::printf("hazard_slot_t — %zu exit sweeps against a thread still publishing:\n", sweeps);
    std::atomic<std::size_t> bad{0};
    {
        hazard_slot_t slot;
        std::atomic<bool> stop{false};
        std::thread worker([&] {
            for (std::uint64_t i = 1; !stop.load(relaxed_); ++i) {
                if (!slot.store(make_tagged(i))) bad.fetch_add(1, relaxed_);
                if (!intact(slot.load())) bad.fetch_add(1, relaxed_);
            }
        });
        for (std::size_t i = 0; i < sweeps; ++i) {
            hp::final_sweep_t sweep;
            (void)sweep;
        }
        stop.store(true, relaxed_);
        worker.join();
    }
    check(bad.load() == 0, "every publish took and every read returned an intact value");
    check(g_live.load() == 0, "and the run freed every rope");
}

/**
 * @brief Whether nothrow allocation in this binary is currently rigged to fail.
 *
 * Only `hazard_slot_t::store`'s node allocation uses the nothrow form on the paths this test
 * exercises, so flipping this starves exactly the allocation under test.
 */
std::atomic<bool> g_starve{false};

}  // namespace

/**
 * @brief Replacement nothrow `operator new` — the only way to reach a declined publish.
 *
 * Forwards to the throwing form on success, so every pointer handed out is still a real
 * `::operator new` pointer that the default `operator delete` (and a sanitizer's replacement
 * of it) pairs with correctly. Replacing the whole new/delete family with `malloc`/`free`
 * would have been simpler and would have blinded ASan for this translation unit, which is the
 * one leg this test most needs.
 */
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (g_starve.load(std::memory_order_relaxed)) return nullptr;
    try {
        return ::operator new(n);
    } catch (...) {
        return nullptr;
    }
}

/** @brief The paired deallocation, for a constructor that throws out of the nothrow form. */
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }

namespace {

/**
 * @brief A publish that cannot get a node must SAY SO, and must leave the old value standing.
 *
 * Runs on a fresh thread on purpose: a participant that has already published owns a recycled
 * node and never allocates again, so the only reachable allocation is a cold participant's
 * first one. That is also precisely the window the header claims is the whole exposure.
 */
void declined_publish() {
    std::printf("hazard_slot_t — a publish that cannot allocate is reported, not swallowed:\n");
    hazard_slot_t slot;
    check(slot.store(make_tagged(1)), "the main thread publishes normally");

    bool declined = false;
    bool preserved = false;
    bool recovered = false;
    std::thread cold([&] {
        g_starve.store(true, std::memory_order_relaxed);
        declined = !slot.store(make_tagged(2));
        const auto still = slot.load();
        preserved = intact(still) && tag_of(still) == 1;
        g_starve.store(false, std::memory_order_relaxed);
        recovered = slot.store(make_tagged(3));
    });
    cold.join();

    check(declined, "a cold participant with no memory reports the publish as declined");
    check(preserved, "and the value that was already published is still the one that reads back");
    check(recovered, "the same participant publishes fine once memory is back");

    // A clear needs no node, so it is the one operation starvation cannot touch.
    std::thread cold2([&] {
        g_starve.store(true, std::memory_order_relaxed);
        slot.clear();
        g_starve.store(false, std::memory_order_relaxed);
    });
    cold2.join();
    check(slot.load() == nullptr, "a clear succeeds even with no memory at all");
}

/** @brief The refcount slot allocates nothing to publish, so starvation cannot reach it. */
void sp_atomic_never_declines() {
    std::printf("sp_atomic_slot_t — nothing to allocate, so nothing to decline:\n");
    sp_atomic_slot_t slot;
    bool ok = false;
    std::thread cold([&] {
        g_starve.store(true, std::memory_order_relaxed);
        ok = slot.store(make_tagged(1));
        g_starve.store(false, std::memory_order_relaxed);
    });
    cold.join();
    check(ok, "a publish succeeds with nothrow allocation rigged to fail");
    slot.clear();
}

}  // namespace

/** @brief Run every slot-policy probe. */
int main() {
    std::printf("LKV slot policies (ADR-0069): sp_atomic_slot_t and hazard_slot_t\n\n");

    contract<sp_atomic_slot_t>("sp_atomic_slot_t");
    contract<hazard_slot_t>("hazard_slot_t");

    const std::size_t hw = std::max<std::size_t>(std::thread::hardware_concurrency(), 2);
    const std::size_t readers = std::min<std::size_t>(hw, 8);
    concurrent<sp_atomic_slot_t>("sp_atomic_slot_t", 2, readers, 5000);
    check(g_live.load() == 0, "sp_atomic_slot_t: the concurrent run freed every rope");
    concurrent<hazard_slot_t>("hazard_slot_t", 2, readers, 5000);
    check(g_live.load() == 0, "hazard_slot_t: the concurrent run freed every rope");

    bounded_parking<sp_atomic_slot_t>("sp_atomic_slot_t", 20000);
    bounded_parking<hazard_slot_t>("hazard_slot_t", 20000);

    claiming_writes_nothing_into_the_announcement_table();
    exhausted_registry();
    check(g_live.load() == 0, "the overflow run freed every rope too");
    overflow_thread_stops_sweeping_the_claim_table();
    check(g_live.load() == 0, "the over-capacity probe freed every rope too");
    the_overflow_lock_does_not_dirty_the_orphan_line();
    orphans_drain_at_the_next_scan();
    check(g_live.load() == 0, "the orphan-drain probe freed every rope too");

    sweep_spares_a_live_participant();
    sweep_races_a_live_writer(200);

    sp_atomic_never_declines();
    declined_publish();
    check(g_live.load() == 0, "the starvation probes freed every rope too");

    return tr::testing::summary("lkv_slot");
}
