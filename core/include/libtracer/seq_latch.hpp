/**
 * @file
 * @brief `tr::seq_latch_t` — a two-sided sequence latch: one writer republishes a record IN
 *        PLACE while readers copy it out lock-free and never see two publishes mixed.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The graph's declaration nodes (#2032) are immortal and keyed by vertex address, so a
 * re-registration must REPLACE what a node holds rather than prepend a new node, and must do
 * it under readers that take no lock: the write gate copies an admission hook, or scans
 * payload-right rows, on every write to a declaring vertex. What a replacement must never do
 * is let a reader assemble one record from two registrations — a hook's `fn` from one and its
 * `ctx` from the next, or one occupant's row count over another's rows.
 *
 * **Why two sides.** The latch holds the record twice. Readers read the side the sequence's
 * low bit names; the writer rewrites only the OTHER side, then advances the sequence to point
 * readers at it. A reader whose side was rewritten under it sees the sequence move and copies
 * again. So a reader retries only when a publish COMPLETED during its copy — progress — and
 * never waits on a writer: a writer preempted mid-rewrite is rewriting the side no new reader
 * reads. A one-sided seqlock would let a high-priority reader that preempts the writer on one
 * core spin on it; `tr::sink_slot_t` avoids that by answering "no sink" to a reader that
 * overlaps a publish, which for a FILTER would be a bypass, so it cannot serve here.
 *
 * **Ordering.** The writer (one at a time — the caller serializes writers): a release fence,
 * the relaxed stores to the inactive side, then a release store of the advanced sequence. The
 * reader: an acquire load of the sequence, the relaxed loads, an acquire fence, then a relaxed
 * re-load that must match. A reader that read any word the writer stored after its fence
 * synchronizes with that fence, so its re-load sees the sequence the writer had already
 * published, and it retries. A reader that read none of them read exactly the publish its
 * acquire load named, whole.
 *
 * **What a side may hold.** Only atomics, read RELAXED by the projection a reader passes and
 * written relaxed by the fill a writer passes — never a plain field, which a rewrite would race.
 * A side may point at storage of its own (the payload-right tables do); then that storage must
 * outlive every reader that can still hold the pointer, and a projection must bound every index
 * it derives from words it read, since a torn copy is discarded only AFTER it was made.
 *
 * Layer-neutral (`tr`, beside `tr::sink_slot_t`): a pure publication primitive over
 * `<atomic>` that names no libtracer layer.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace tr {

/**
 * @brief A record of type @p Side held twice and republished in place by one writer at a time,
 *        read lock-free by any number of readers.
 *
 * @tparam Side A default-constructible aggregate of atomics: what one publish writes and one
 *         read copies out. See the file comment for what it may hold.
 */
template <class Side>
class seq_latch_t {
   public:
    /** @brief Both sides default-constructed; the first read answers side 0's defaults. */
    seq_latch_t() noexcept = default;
    seq_latch_t(const seq_latch_t&) = delete;
    seq_latch_t& operator=(const seq_latch_t&) = delete;

    /**
     * @brief Copy the current record out through @p project — lock-free, never torn.
     *
     * @p project is called on the current side and must only LOAD (relaxed) from it; it may be
     * called more than once, and only the result of the last call is returned, so it must have
     * no effect a retry could duplicate beyond what its own result carries.
     */
    template <class F>
    [[nodiscard]] auto read(F&& project) const {
        for (;;) {
            const std::uint32_t s = seq_.load(std::memory_order_acquire);
            auto copy = project(sides_[s & 1U]);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s) return copy;
        }
    }

    /** @brief The current side, for the WRITER only: while no other publish can run, nothing
     *         rewrites it, so the writer may read it without the retry. */
    [[nodiscard]] Side& current() noexcept {
        return sides_[seq_.load(std::memory_order_relaxed) & 1U];
    }

    /** @brief The side the next publish rewrites, for the WRITER only. */
    [[nodiscard]] Side& next() noexcept {
        return sides_[(seq_.load(std::memory_order_relaxed) + 1U) & 1U];
    }

    /** @brief Both sides, for an owner tearing the record down when no reader can remain. */
    [[nodiscard]] std::array<Side, 2>& sides() noexcept { return sides_; }

    /**
     * @brief Publish: @p fill rewrites the inactive side (relaxed stores only), then readers
     *        are pointed at it. One writer at a time; the caller serializes them.
     *
     * @p fill may fail by returning `false` — then the sequence does not move and readers keep
     * the current record. Anything it changed on the inactive side is unseen by new readers.
     *
     * @return Whatever @p fill returned.
     */
    template <class F>
    bool publish(F&& fill) {
        const std::uint32_t s = seq_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        if (!fill(sides_[(s + 1U) & 1U])) return false;
        seq_.store(s + 1U, std::memory_order_release);
        return true;
    }

   private:
    std::atomic<std::uint32_t> seq_{0}; /**< @brief Its low bit names the side readers read. */
    std::array<Side, 2> sides_{};       /**< @brief The record, twice. */
};

}  // namespace tr
