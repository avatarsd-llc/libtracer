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
 *   - **a declined publish is reported** — the whole point of `store` returning `bool`, driven
 *     here by replacing this binary's nothrow `operator new` rather than by inspection.
 */

#include "libtracer/lkv_slot.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using tr::graph::hazard_slot_t;
using tr::graph::sp_atomic_slot_t;
using tr::view::rope_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

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
 * @brief Writers replacing one slot while readers read it.
 *
 * The readers' job is to fail loudly on anything that is not a value some writer published:
 * a null where one was published, or an identity that does not re-derive.
 */
template <typename slot_t>
void concurrent(const char* name, std::size_t writers, std::size_t readers, std::size_t rounds) {
    std::printf("%s — %zu writers / %zu readers on one slot:\n", name, writers, readers);
    slot_t slot;
    (void)slot.store(make_tagged(0));

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> bad{0};
    std::atomic<std::size_t> reads{0};
    std::vector<std::thread> pool;

    for (std::size_t r = 0; r < readers; ++r) {
        pool.emplace_back([&] {
            std::size_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                // Hold two handles at once: the N-simultaneous-pins property
                // read_subtree_folded needs, which one hazard slot per thread cannot express
                // and which is exactly why the read has to promote (ADR-0069 §5).
                const auto a = slot.load();
                const auto b = slot.load();
                if (!intact(a) || !intact(b)) bad.fetch_add(1, std::memory_order_relaxed);
                ++n;
            }
            reads.fetch_add(n, std::memory_order_relaxed);
        });
    }
    for (std::size_t w = 0; w < writers; ++w) {
        pool.emplace_back([&, w] {
            for (std::size_t i = 0; i < rounds; ++i) {
                if (!slot.store(make_tagged(w * rounds + i + 1))) bad.fetch_add(1, relaxed_);
            }
        });
    }
    for (std::size_t i = readers; i < pool.size(); ++i) pool[i].join();
    stop.store(true, std::memory_order_relaxed);
    for (std::size_t i = 0; i < readers; ++i) pool[i].join();

    check(bad.load() == 0, "every concurrent read returned an intact published value");
    check(reads.load() > 0, "the readers actually ran");
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
        bool expected = false;
        if (reg.cells[i].claimed.compare_exchange_strong(expected, true)) taken.push_back(i);
    }
    check(taken.size() + 1 >= tr::graph::kHazardReaderSlots,
          "the registry is full (this thread may already hold one index)");

    concurrent<hazard_slot_t>("  overflow", 1, 2, 500);

    for (std::size_t i : taken) reg.cells[i].claimed.store(false);
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

    exhausted_registry();
    check(g_live.load() == 0, "the overflow run freed every rope too");

    sp_atomic_never_declines();
    declined_publish();
    check(g_live.load() == 0, "the starvation probes freed every rope too");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
