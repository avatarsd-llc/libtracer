/**
 * @file
 * @brief The leaf/branch fork of `graph_t::read`, and the state machine that now answers it
 *        without the map lock (#652).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `read` forks on "does this vertex have at least one REGISTERED direct child": a branch
 * serves the composed POINT tree of its descendants, a leaf serves its own last-known value.
 * That predicate used to be computed by walking the child list under `map_mutex_` shared —
 * one process-wide reader-writer lock on **every** read, which capped the whole process at
 * roughly 20 M reads/s no matter how disjoint the vertices were. It is now a bit on the
 * vertex, set when a child is filled and recomputed when one is retired.
 *
 * Setting is trivial and was already covered by `subtree_read_test`. **Clearing is not**, and
 * nothing in the suite exercised it: no existing test retires a vertex and then reads its
 * parent. That transition is the whole risk of this change — a stuck bit sends a leaf read
 * down the composed path (wrong shape, wrong bytes) or a branch read down the leaf path
 * (silently dropping every descendant). Each case below is one edge of that machine.
 *
 * The last test races readers against registration and retirement on the same vertex, which
 * is what makes the lock-free read carry its weight under ThreadSanitizer.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;

using tr::testing::check;

/** @brief A one-byte value living in its own heap segment. */
[[nodiscard]] tr::view::view_t val_u8(std::uint8_t b) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(1);
    seg->bytes[0] = std::byte{b};
    return tr::view::view_t::over(std::move(seg));
}

/**
 * @brief Whether a read of @p p came back as the vertex's own scalar rather than a composed
 *        POINT tree.
 *
 * The leaf read returns exactly the bytes that were stored — one byte here. The composed
 * branch read returns a POINT TLV, which is never one byte long. Length alone separates them,
 * which keeps this test independent of the composed encoding `subtree_read_test` owns.
 */
[[nodiscard]] bool reads_as_leaf_scalar(graph_t& g, const char* p, std::uint8_t expect) {
    const auto r = g.read(path_t(p));
    if (!r.has_value()) return false;
    std::vector<std::byte> flat;
    for (const tr::view::view_t& l : (*r)->links()) {
        const auto s = l.bytes();
        flat.insert(flat.end(), s.begin(), s.end());
    }
    return flat.size() == 1 && flat[0] == std::byte{expect};
}

/** @brief What one read of the forking vertex came back as. */
enum class shape_t : std::uint8_t {
    NONE,      /**< @brief The read returned no value at all. */
    LEAF,      /**< @brief The vertex's own one-byte scalar. */
    FORKED,    /**< @brief A composed reply that CARRIES the child. */
    ROOT_ONLY, /**< @brief The LEGAL retirement transient: a composed POINT wrapping the
                        root's scalar alone, zero child records — byte-identical to the
                        fully READ-ACL-pruned reply (RFC-0016 §B erratum 2026-08-13,
                        #1030). */
    OTHER,     /**< @brief Anything else — a malformed reply the race must never produce. */
};

/**
 * @brief Classify one read of @p p as leaf, forked, or neither, reusing @p scratch.
 *
 * The composed branch read emits, per included node below the root, that node's canonical
 * NAME record verbatim (ADR-0057), so the child's segment text appears literally in the
 * reply exactly when the subtree walk included the child. Reply SIZE cannot say that: the
 * retirement transient — the fork bit still set when the walk takes the map lock, the child
 * already gone — composes the ROOT ALONE, which is bigger than a scalar and carries no
 * child. Distinguishing the two is what lets the concurrent case assert that a reader saw
 * the vertex *forked* rather than merely saw the bit set — and what lets it ACCEPT the
 * root-only compose by name instead of lumping it with malformed replies: the shape is
 * specified legal (RFC-0016 §B erratum 2026-08-13, #1030), so it is matched byte-exactly
 * against the five bytes the erratum pins (POINT header wrapping the root's scalar).
 *
 * @p scratch is caller-owned so the reader loop keeps its capacity across millions of
 * reads instead of allocating inside the race.
 */
[[nodiscard]] shape_t classify_read(graph_t& g, const char* p, std::uint8_t leaf_byte,
                                    std::string_view child_name, std::vector<std::byte>& scratch) {
    const auto r = g.read(path_t(p));
    if (!r.has_value()) return shape_t::NONE;
    scratch.clear();
    for (const tr::view::view_t& l : (*r)->links()) {
        const auto s = l.bytes();
        scratch.insert(scratch.end(), s.begin(), s.end());
    }
    if (scratch.size() == 1)
        return scratch[0] == std::byte{leaf_byte} ? shape_t::LEAF : shape_t::OTHER;
    const auto hit =
        std::search(scratch.begin(), scratch.end(), child_name.begin(), child_name.end(),
                    [](std::byte b, char c) { return b == static_cast<std::byte>(c); });
    if (hit != scratch.end()) return shape_t::FORKED;
    // No child record: legal only as the exact root-only compose — POINT (0x07, opt.PL,
    // 16-bit length 1) wrapping the root's stored scalar, the erratum's canonical bytes.
    const std::array<std::byte, 5> root_only{std::byte{0x07}, std::byte{0x40}, std::byte{0x01},
                                             std::byte{0x00}, std::byte{leaf_byte}};
    return std::ranges::equal(scratch, root_only) ? shape_t::ROOT_ONLY : shape_t::OTHER;
}

/** @brief Whether a read of @p p came back as something bigger than a bare scalar. */
[[nodiscard]] bool reads_as_composed(graph_t& g, const char* p) {
    const auto r = g.read(path_t(p));
    if (!r.has_value()) return false;
    std::size_t n = 0;
    for (const tr::view::view_t& l : (*r)->links()) n += l.bytes().size();
    return n > 1;
}

/** @brief The fork follows registration, in both directions. */
void test_fork_tracks_registration() {
    std::printf("the fork follows registration and retirement:\n");
    graph_t g;
    const auto dev = g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    (void)g.write(dev, val_u8(0x11));
    check(reads_as_leaf_scalar(g, "/dev", 0x11), "a vertex with no children reads as a leaf");

    const auto a = g.register_vertex(path_t("/dev/a"), role_t::STORED_VALUE);
    (void)g.write(a, val_u8(0x22));
    check(reads_as_composed(g, "/dev"), "registering a child turns the parent into a branch");

    const auto b = g.register_vertex(path_t("/dev/b"), role_t::STORED_VALUE);
    (void)g.write(b, val_u8(0x33));
    check(reads_as_composed(g, "/dev"), "a second child keeps it a branch");

    check(g.retire(a).has_value(), "retire the first child");
    check(reads_as_composed(g, "/dev"),
          "with one child left the parent is STILL a branch (the recompute must see the "
          "surviving sibling, not just the departing one)");

    check(g.retire(b).has_value(), "retire the last child");
    check(reads_as_leaf_scalar(g, "/dev", 0x11),
          "the parent is a LEAF again and serves its own value byte-identically");

    const auto c = g.register_vertex(path_t("/dev/c"), role_t::STORED_VALUE);
    (void)g.write(c, val_u8(0x44));
    check(reads_as_composed(g, "/dev"), "re-registering under it makes it a branch once more");
}

/** @brief Only DIRECT children count — a placeholder between does not promote a leaf. */
void test_placeholders_do_not_count() {
    std::printf("placeholders are not registered children:\n");
    graph_t g;
    const auto top = g.register_vertex(path_t("/top"), role_t::STORED_VALUE);
    (void)g.write(top, val_u8(0x55));
    // `/top/mid` is created as an unregistered PLACEHOLDER on the way to `/top/mid/leaf`.
    const auto leaf = g.register_vertex(path_t("/top/mid/leaf"), role_t::STORED_VALUE);
    (void)g.write(leaf, val_u8(0x66));
    check(reads_as_leaf_scalar(g, "/top", 0x55),
          "a registered GRANDchild behind a placeholder leaves the vertex a leaf");
    // The placeholder is invisible to `find`, so it never reaches the fork at all — the
    // bit it carries for its registered child is simply unobservable through `read`.
    check(!g.read(path_t("/top/mid")).has_value(),
          "the placeholder itself stays invisible to read, fork bit or no fork bit");
    check(reads_as_leaf_scalar(g, "/top/mid/leaf", 0x66),
          "and the registered grandchild reads as its own leaf");
}

/** @brief Retiring a subtree in one call must leave every surviving parent's bit right. */
void test_subtree_retire() {
    std::printf("a subtree retire clears the bit at every level it emptied:\n");
    graph_t g;
    const auto root = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    (void)g.write(root, val_u8(0x77));
    const auto mid = g.register_vertex(path_t("/s/mid"), role_t::STORED_VALUE);
    (void)g.write(mid, val_u8(0x88));
    const auto deep = g.register_vertex(path_t("/s/mid/deep"), role_t::STORED_VALUE);
    (void)g.write(deep, val_u8(0x99));
    check(reads_as_composed(g, "/s"), "before: /s is a branch");

    check(g.retire(mid).has_value(), "retire /s/mid, which takes /s/mid/deep with it");
    check(reads_as_leaf_scalar(g, "/s", 0x77),
          "/s is a leaf again — the subtree walk cleared its bit, not just the leaf's");
}

/** @brief Retiring an already-retired vertex must not corrupt the parent's bit. */
void test_idempotent_retire() {
    std::printf("a repeated retire does not disturb a surviving sibling:\n");
    graph_t g;
    const auto p = g.register_vertex(path_t("/i"), role_t::STORED_VALUE);
    (void)g.write(p, val_u8(0xAA));
    const auto x = g.register_vertex(path_t("/i/x"), role_t::STORED_VALUE);
    const auto y = g.register_vertex(path_t("/i/y"), role_t::STORED_VALUE);
    (void)g.write(x, val_u8(0xBB));
    (void)g.write(y, val_u8(0xCC));

    check(g.retire(x).has_value(), "retire /i/x");
    check(g.retire(x).has_value(), "retire /i/x again (idempotent per RFC-0009 §B.4)");
    check(reads_as_composed(g, "/i"),
          "/i is still a branch — a second retire of an already-retired child must not clear "
          "the bit that /i/y is holding up");
}

/**
 * @brief Readers racing registration and retirement on the vertex they are forking on.
 *
 * The fork read is lock-free while `fill` and `mark_unregistered` mutate the bit under the
 * graph's unique lock. Which side of a concurrent registration a reader lands on was never
 * ordered by anything a caller can observe, so the assertion over the free-running reads is
 * that every one is a shape the contract admits — leaf, forked, or the root-only
 * retirement transient the RFC-0016 §B erratum (2026-08-13, #1030) specifies as legal —
 * never that it saw a particular side. The transient is COUNTED, not asserted present:
 * whether the race window is hit is a scheduler artifact no portable test may demand.
 *
 * The overlap is asserted, not hoped for, and it is asserted by OBSERVATION rather than by
 * rate. Two earlier versions got that wrong. The first ran 300 rounds against four readers
 * and asserted only `reads > 0`, which four freshly-spawned threads satisfy before the writer
 * has finished starting them — it passed while racing almost nothing. Its replacement
 * asserted `reads >= kRounds * 2`, a wall-clock throughput floor: the reader loop spun for
 * however long the writer took, so the count measured how much CPU a shared runner handed the
 * readers, not anything about the code. It went red on CI for a branch that touched nothing
 * on this path (#1021), and it would have gone GREEN on a fast runner whose millions of reads
 * all landed on the same side of the fork.
 *
 * What replaces it makes the interleaving structural. The writer publishes a phase counter
 * AFTER each mutation and does not advance until every reader has acknowledged that phase; a
 * reader acknowledges by classifying its first read taken in that phase. Because the writer
 * cannot move on before the acknowledgement lands, that read both started and finished while
 * the phase held, so its shape is DETERMINED — forked while the child is registered, leaf
 * after it is retired — on any machine, at any speed. Between the handshakes the readers keep
 * hammering unsynchronised reads straight across the register/write/retire, so the race TSan
 * is here for is unchanged; only the evidence that it happened is.
 */
void test_concurrent_fork() {
    constexpr std::size_t kRounds = 4000;
    constexpr std::size_t kReaders = 4;
    constexpr const char* kRoot = "/r";
    constexpr const char* kChildPath = "/r/kid";
    constexpr std::string_view kChildName = "kid";
    constexpr std::uint8_t kRootByte = 0xDD;
    constexpr std::uint8_t kChildByte = 0xEE;

    std::printf("readers forking while a writer registers and retires:\n");
    graph_t g;
    const auto p = g.register_vertex(path_t(kRoot), role_t::STORED_VALUE);
    (void)g.write(p, val_u8(kRootByte));

    // Prove the classifier can tell the two shapes apart BEFORE the race leans on it. A
    // both-shapes-seen guard built on a classifier that answers the same thing either way
    // would be vacuous in exactly the manner this case is meant to stop being.
    std::vector<std::byte> probe;
    check(classify_read(g, kRoot, kRootByte, kChildName, probe) == shape_t::LEAF,
          "the shape classifier calls the childless vertex a LEAF");
    const auto seed = g.register_vertex(path_t(kChildPath), role_t::STORED_VALUE);
    (void)g.write(seed, val_u8(kChildByte));
    check(classify_read(g, kRoot, kRootByte, kChildName, probe) == shape_t::FORKED,
          "and calls it FORKED once the child is registered and written — the reply carries "
          "the child's name record, which a root-only compose does not");
    check(g.retire(seed).has_value(), "retire the seed child");
    check(classify_read(g, kRoot, kRootByte, kChildName, probe) == shape_t::LEAF,
          "and a LEAF again once the child is gone");

    /** @brief One reader's private tally, plus the slot the writer's handshake waits on. */
    struct alignas(64) reader_tally_t {
        std::atomic<std::size_t> acked{0}; /**< @brief Highest phase this reader has answered. */
        std::size_t reads = 0;             /**< @brief Reads completed, free-running included. */
        std::size_t bad = 0;               /**< @brief Reads that returned no value. */
        std::size_t root_only = 0;         /**< @brief Free-running reads that saw the LEGAL
                                                     root-only retirement transient. */
        std::size_t malformed = 0;         /**< @brief Reads of a shape the contract does not
                                                     admit — neither settled shape nor the
                                                     root-only transient. */
        std::size_t forked = 0;            /**< @brief Phase-contained reads that saw the child. */
        std::size_t leaf = 0;              /**< @brief Phase-contained reads that saw the scalar. */
        std::size_t wrong = 0;             /**< @brief Phase-contained reads of the wrong shape. */
    };
    std::vector<reader_tally_t> tally(kReaders);

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> ready{0};
    // Odd phases are "the child is registered and written", even ones "the child is retired".
    // Phase 0 is the pre-start state and is never acknowledged.
    std::atomic<std::size_t> phase{0};
    std::vector<std::thread> pool;
    for (std::size_t i = 0; i < kReaders; ++i) {
        pool.emplace_back([&, i] {
            reader_tally_t& t = tally[i];
            std::vector<std::byte> scratch;
            std::size_t answered = 0;
            ready.fetch_add(1, std::memory_order_release);
            while (!stop.load(std::memory_order_relaxed)) {
                const std::size_t seq = phase.load(std::memory_order_acquire);
                const shape_t s = classify_read(g, kRoot, kRootByte, kChildName, scratch);
                ++t.reads;
                // EVERY read is classified, free-running included (#1030): the two settled
                // shapes and the root-only retirement transient are the contract's whole
                // vocabulary (RFC-0016 §B + erratum 2026-08-13); anything else is a torn
                // reply. The earlier arm counted only "a value came back", which is
                // exactly how the transient went unseen.
                if (s == shape_t::NONE) ++t.bad;
                if (s == shape_t::ROOT_ONLY) ++t.root_only;
                if (s == shape_t::OTHER) ++t.malformed;
                if (seq != answered) {
                    // This read STARTED after `seq` was published and finished before the
                    // acknowledgement below, which the writer is still waiting on — so the
                    // whole read ran inside phase `seq` and its shape is the phase's, not a
                    // transient's. Acknowledge once per phase: the store is the only shared
                    // write the reader makes, and keeping it off the per-read path leaves the
                    // free-running reads uncontended.
                    const shape_t want = (seq % 2 == 1) ? shape_t::FORKED : shape_t::LEAF;
                    if (s != want) {
                        ++t.wrong;
                    } else if (want == shape_t::FORKED) {
                        ++t.forked;
                    } else {
                        ++t.leaf;
                    }
                    answered = seq;
                    t.acked.store(seq, std::memory_order_release);
                }
                // A scheduling hint, not a knob any assertion reads. The loop is otherwise a
                // pure spin, so on ONE core a reader would hold the CPU for a whole time slice
                // and the handshake would crawl: measured 27.7 s under TSan on `taskset -c 0`
                // without this, 0.30 s with it, at an unchanged read count on a free machine.
                std::this_thread::yield();
            }
        });
    }
    // Do not start the writer until every reader is in its loop, or the early rounds race
    // nothing but thread start-up.
    while (ready.load(std::memory_order_acquire) < kReaders) { /* spin to a common start */
    }
    /** @brief Block until every reader has answered phase @p want. */
    const auto await_phase = [&](std::size_t want) {
        for (reader_tally_t& t : tally)
            while (t.acked.load(std::memory_order_acquire) < want) std::this_thread::yield();
    };
    std::size_t seq = 0;
    for (std::size_t round = 0; round < kRounds; ++round) {
        const auto kid = g.register_vertex(path_t(kChildPath), role_t::STORED_VALUE);
        (void)g.write(kid, val_u8(kChildByte));
        phase.store(++seq, std::memory_order_release);
        await_phase(seq);
        (void)g.retire(kid);
        phase.store(++seq, std::memory_order_release);
        await_phase(seq);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : pool) t.join();

    std::size_t reads = 0, bad = 0, root_only = 0, malformed = 0, forked = 0, leaf = 0, wrong = 0;
    for (const reader_tally_t& t : tally) {
        reads += t.reads;
        bad += t.bad;
        root_only += t.root_only;
        malformed += t.malformed;
        forked += t.forked;
        leaf += t.leaf;
        wrong += t.wrong;
    }
    // A reader cannot skip a phase (the writer will not publish the next one until this reader
    // has answered) and cannot answer one twice, so each of the two counts below is exactly one
    // observation per reader per round when the fork behaves.
    const std::size_t want_each = kRounds * kReaders;

    std::printf(
        "    %zu rounds x %zu readers -> %zu reads, %zu no-value, %zu malformed, %zu "
        "root-only transients; %zu forked + %zu leaf phase observations, %zu wrong\n",
        kRounds, kReaders, reads, bad, malformed, root_only, forked, leaf, wrong);
    check(bad == 0, "every concurrent read of the forking vertex returned a value");
    check(malformed == 0,
          "every free-running read was one of the three specified shapes — leaf, forked, or "
          "the root-only retirement transient (RFC-0016 §B erratum, #1030) — never torn");
    check(wrong == 0, "every read the handshake pinned inside a phase saw that phase's shape");
    check(forked == want_each,
          "each reader observed the vertex FORKED — child in the reply — once per round");
    check(leaf == want_each, "and observed it back as a bare LEAF once per round");
    check(reads_as_leaf_scalar(g, kRoot, kRootByte), "and the vertex settles back to a leaf");
}

}  // namespace

/** @brief Run the fork state-machine probes. */
int main() {
    std::printf("graph_t::read leaf/branch fork (#652):\n");
    test_fork_tracks_registration();
    test_placeholders_do_not_count();
    test_subtree_retire();
    test_idempotent_retire();
    test_concurrent_fork();
    return tr::testing::summary("read_fork");
}
