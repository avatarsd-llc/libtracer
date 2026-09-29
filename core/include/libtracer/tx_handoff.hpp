/**
 * @file
 * @brief Enqueue-then-write for a link that must serialize its writes (RFC 0028 §4.7, #1619).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A stream link has to put one record on the wire at a time, and its `send` runs on whatever
 * thread published: delivery is in-call. The shape this replaces held the link's write mutex
 * across the blocking transport write, so a second publisher to the same link waited on the
 * mutex for as long as the first one's write took — up to the link's whole write budget on a
 * peer that had stopped reading. One stalled peer therefore stalled every unrelated task that
 * happened to publish toward it.
 *
 * `tr::net::tx_handoff_t` splits the two jobs. Its lock guards a small bounded queue and a
 * "writer in flight" flag, and it is never held across I/O. A publisher that finds no writer
 * in flight becomes the writer and puts its own record on the wire directly (no copy). A
 * publisher that finds a writer in flight copies its record into a free slot and returns at
 * once; the writer drains that slot after its own record. A publisher that finds every slot
 * taken drops the record and the link counts it — the same answer the async server link
 * already gives on an empty TX pool. So no publisher ever waits on another publisher's write.
 *
 * The writer itself still waits on I/O: on its own record and then on each record it drains
 * for the others. Each write is bounded by the link's write budget, and a stream link tears the
 * peer down after `kMaxConsecutiveStalls` (3) stalled writes in a row, so the writer pays up to
 * three write windows before the link gives up — never more, and nobody queues behind it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "libtracer/mem_source.hpp"

namespace tr::net {

/**
 * @brief A bounded enqueue-then-write queue: one writer at a time, the rest enqueue or drop.
 *
 * Usage, on a link's `send`:
 * @code
 *   switch (tx_.admit([&](mem::block_array_t<std::byte>& slot) { return fill(slot); })) {
 *       case tx_handoff_t::admit_t::REFUSED: ++dropped; return;   // queue full: counted
 *       case tx_handoff_t::admit_t::QUEUED: return;               // the writer will send it
 *       case tx_handoff_t::admit_t::WRITE: break;                 // this thread is the writer
 *   }
 *   write(own_record);
 *   for (auto rec = tx_.next(); !rec.empty(); rec = tx_.next()) write(rec);
 * @endcode
 *
 * A thread that got `WRITE` MUST call @ref next until it returns an empty span: that call is
 * what hands the writer role back. Records leave in admission order.
 *
 * Slot storage comes from a failable @ref mem::block_source_t (ADR-0065): a slot that cannot
 * grow refuses its record by value, never throws, so the send path is safe on a
 * `-fno-exceptions` profile. The array of slot headers itself is a `std::vector` sized once,
 * on the global heap, at construction — never at send time.
 *
 * @note Thread-safe. The internal mutex is held only for a slot fill (a copy) or an index
 *       step, never across the caller's I/O.
 */
class tx_handoff_t {
   public:
    /** @brief What @ref admit decided for one record. */
    enum class admit_t : std::uint8_t {
        WRITE,   /**< @brief No writer was in flight: the caller is now the writer. */
        QUEUED,  /**< @brief A writer is in flight: the record was copied into a slot. */
        REFUSED, /**< @brief A writer is in flight and every slot is taken, or the fill
                              refused: the record is dropped and the caller counts it. */
    };

    /**
     * @brief Constructs a queue of @p depth slots behind the in-flight writer.
     *
     * @param depth      Records that may wait behind the writer. `0` makes the link
     *                   drop-and-count whenever a write is in flight — the zero-RAM form.
     * @param src        Where slot storage is drawn from.
     * @param slot_bytes Bytes reserved in each slot up front, so a fill up to that size never
     *                   allocates at send time. `0` reserves nothing; a slot grows on first
     *                   use and keeps its capacity afterwards.
     */
    tx_handoff_t(std::size_t depth, mem::block_source_t& src, std::size_t slot_bytes = 0) {
        slots_.reserve(depth);
        for (std::size_t i = 0; i < depth; ++i) {
            slots_.emplace_back(src);
            if (slot_bytes > 0) (void)slots_.back().buf.reserve(slot_bytes);
        }
    }

    tx_handoff_t(const tx_handoff_t&) = delete;
    tx_handoff_t& operator=(const tx_handoff_t&) = delete;

    /**
     * @brief Become the writer, or queue a copy of the record, or refuse it.
     *
     * @p fill is called only on the queue path, under the internal lock, with a slot to write
     * the record into (reserve, then write through `data()`). It returns the byte count it
     * wrote; `0` refuses (the slot could not grow, or the record cannot be encoded), and the
     * slot stays free.
     *
     * @tparam Fill Callable `std::size_t(mem::block_array_t<std::byte>&)`.
     */
    template <class Fill>
    [[nodiscard]] admit_t admit(Fill&& fill) {
        const std::lock_guard lock(m_);
        if (!busy_) {
            busy_ = true;
            return admit_t::WRITE;
        }
        if (count_ == slots_.size()) {
            ++refused_;
            return admit_t::REFUSED;
        }
        slot_t& slot = slots_[(head_ + count_) % slots_.size()];
        slot.len = std::forward<Fill>(fill)(slot.buf);
        if (slot.len == 0) {
            ++refused_;
            return admit_t::REFUSED;
        }
        ++count_;
        if (count_ > peak_) peak_ = count_;
        return admit_t::QUEUED;
    }

    /**
     * @brief The writer's step: release the record handed out last, hand out the next one.
     *
     * @return The next queued record, valid until the following call; or an empty span, in
     *         which case the queue was empty and the caller is no longer the writer.
     */
    [[nodiscard]] std::span<std::byte> next() {
        const std::lock_guard lock(m_);
        if (handed_) {
            head_ = (head_ + 1) % slots_.size();
            --count_;
            handed_ = false;
        }
        if (count_ == 0) {
            busy_ = false;
            return {};
        }
        handed_ = true;
        slot_t& slot = slots_[head_];
        return {slot.buf.data(), slot.len};
    }

    /** @brief The slot count this queue was built with — its effective ceiling. */
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    /** @brief Records refused since construction (queue full, or the fill refused). */
    [[nodiscard]] std::size_t refused() const {
        const std::lock_guard lock(m_);
        return refused_;
    }

    /** @brief The most records ever waiting at once behind the writer. */
    [[nodiscard]] std::size_t peak() const {
        const std::lock_guard lock(m_);
        return peak_;
    }

   private:
    /** @brief One queued record: its storage and the bytes the fill wrote into it. */
    struct slot_t {
        /** @brief An empty slot drawing from @p src. */
        explicit slot_t(mem::block_source_t& src) noexcept : buf(src) {}
        mem::block_array_t<std::byte> buf; /**< @brief The record's storage. */
        std::size_t len = 0;               /**< @brief Bytes of `buf` that are the record. */
    };

    mutable std::mutex m_;      /**< @brief Guards every field below; never held across the
                                            caller's I/O. */
    std::vector<slot_t> slots_; /**< @brief The ring of queued records. */
    std::size_t head_ = 0;      /**< @brief Oldest queued record's slot. */
    std::size_t count_ = 0;     /**< @brief Queued records, the handed-out one included
                                            until the next @ref next. */
    std::size_t refused_ = 0;   /**< @brief Records refused since construction. */
    std::size_t peak_ = 0;      /**< @brief High-water mark of `count_`. */
    bool busy_ = false;         /**< @brief A writer is in flight. */
    bool handed_ = false;       /**< @brief `slots_[head_]` is being written. */
};

}  // namespace tr::net
