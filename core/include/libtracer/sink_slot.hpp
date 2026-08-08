/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * sink_slot — the ONE home of the "one observer pair, set from anywhere, read on
 * every frame" shape. `receiver_slot_t` already owns that discipline for the
 * transport delivery seam (mutex-guarded storage, snapshot under the lock,
 * dispatch outside it) but it also owns the delivery-TIER select, so a plain
 * observer cannot reuse it. `fwd_router_t` held its five sinks as ten plain
 * non-atomic members instead (#914): the setters stored `fn` then `ctx` while the
 * RX threads did check-then-call, which is a data race by the C++ memory model
 * and can hand a new `fn` the old `ctx`.
 *
 * The difference from `receiver_slot_t` is what a read costs. Its deliver runs
 * once per frame at the transport seam; a router sink is read two or three times
 * per frame on the forward hop, and the slot is measured there rather than
 * argued about. Two findings shaped this file, both from an interleaved A/B of
 * `bench_forward_demux` against the pre-slot tree:
 *
 *   - A read must not take a lock, and the slot must stay SMALL. A first cut
 *     carried a `std::mutex` per slot; at 64 bytes each that put the two sinks
 *     the FWD path reads on separate cache lines and cost a reproducible ~2% at
 *     64 registered links. Three words per slot puts them back on one line.
 *   - The no-sink case must cost exactly one load, because that is what the plain
 *     member it replaces cost and these sinks are unset in a production node.
 *
 * So the pair is published through a sequence counter and read with plain atomic
 * loads. Nothing spins and nothing locks on the read path: a reader that lands
 * inside a publish reports NO SINK for that frame rather than waiting, which is
 * also what keeps a single-core RTOS out of the unbounded-priority-inversion
 * corner ADR-0063 erratum 1 rejected a spinlock over.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace tr::net {

/**
 * @brief One `{function pointer, context}` observer pair, published coherently.
 *
 * Holds a sink in the ADR-0047 hot-path shape — a plain function pointer plus an
 * opaque `void*`, never a `std::function` — and makes the pair readable from a
 * transport receive thread while a control thread installs or clears it.
 *
 * **Thread contract.** @ref set may race every reader. @ref get returns a pair
 * that was installed by ONE @ref set call — never a new `fn` beside a stale `ctx`
 * — and the caller dispatches from that snapshot, so a concurrent clear can no
 * longer null the pointer between a caller's test and its call. Two @ref set
 * calls must NOT run concurrently with each other: the publish is serialized by
 * the owner (`fwd_router_t` holds one mutex for its five slots), not by the slot,
 * which is what keeps a slot three words wide.
 *
 * **What a set costs a frame in flight.** Installing or clearing leaves a window
 * of a few instructions in which the slot reads as EMPTY, so a frame that lands
 * exactly there is not dispatched to either the old sink or the new one. That is
 * deliberate: these sinks are configured, not swapped per frame, and skipping one
 * dispatch is strictly better than the alternative it replaces — calling the new
 * `fn` with the old `ctx`, which every sink then casts.
 *
 * Cost: with no sink installed a read is ONE atomic load, which is what the plain
 * member pair it replaces cost; with one installed it is three more loads of
 * adjacent words and a compare. No lock, no spin, on either path.
 *
 * The context pointer's lifetime is the caller's responsibility and must cover
 * every possible dispatch, exactly as it must for `receiver_slot_t`.
 *
 * @tparam Fn The sink's function-pointer type; its first parameter is the
 *            context (e.g. `void (*)(void* ctx, std::uint16_t label)`).
 */
template <typename Fn>
class sink_slot_t {
    // The design constraint this header can check locally: the sink is a bare function
    // pointer, so the published word is pointer-sized and the snapshot is two adjacent
    // words rather than an object needing a lock of its own.
    static_assert(sizeof(Fn) <= sizeof(void*) && std::is_pointer_v<Fn>,
                  "sink_slot_t publishes a pointer-sized function pointer");
    // NOT asserted: `std::atomic<Fn>::is_always_lock_free`. It is false on a target with no
    // atomic instructions at all — esp32c3 is rv32imc (no A extension), and Cortex-M0 has no
    // exclusives — and this header is on the fwd_router include path, so a hard assert there
    // breaks both esp32c3 CI legs at compile time. libtracer supports those targets, and on
    // them std::atomic falls back to libatomic: still CORRECT, just not lock-free. They are
    // single-core, so the contention the lock-free path exists to avoid does not arise. The
    // "no lock, no spin" claim above is therefore about hosts with real atomics; it is not a
    // portability promise, and it must not be spelled as one in a header every target parses.

   public:
    /** @brief One coherent read of the pair — both halves from the same @ref set. */
    struct snapshot_t {
        Fn fn;     /**< @brief The sink, or nullptr when none is installed. */
        void* ctx; /**< @brief The context to hand it back. */
    };

    /**
     * @brief Install (or clear, with a null @p fn) the sink.
     *
     * Safe to call while readers run; NOT safe to call concurrently with another
     * @ref set on the same slot — the owner serializes setters.
     *
     * @param fn  The sink; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible dispatch.
     */
    void set(Fn fn, void* ctx) noexcept {
        const std::uint32_t g = gen_.load(std::memory_order_relaxed);
        // Odd generation = publish in progress; a reader that sees it reports no sink.
        gen_.store(g + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        fn_.store(fn, std::memory_order_relaxed);
        ctx_.store(ctx, std::memory_order_relaxed);
        gen_.store(g + 2, std::memory_order_release);
    }

    /**
     * @brief Read the pair coherently; dispatch from the result, never from the members.
     *
     * @return The installed pair, or `{nullptr, nullptr}` when no sink is installed
     *         or a @ref set is in flight.
     */
    [[nodiscard]] snapshot_t get() const noexcept {
        // The no-sink case is ONE relaxed load. Sound as a filter and not merely an
        // optimization: with no function to call there is no pair to tear, nothing is
        // dereferenced, and observing the pre-state of a concurrent install is already
        // permitted (which of two installs a reader sees is unordered either way) — so
        // this needs no ordering, and an acquire here would be a barrier on a frame path
        // that has nothing to order. The acquire that matters is in @ref read_installed.
        if (fn_.load(std::memory_order_relaxed) == nullptr) return {nullptr, nullptr};
        return read_installed();
    }

   private:
    /**
     * @brief The coherent read, kept OUT of line so an unset slot inlines to a load
     *        and a branch.
     *
     * `fwd_router.cpp` has eight of these on its frame paths; letting the sequence
     * protocol inline with them grew `on_frame_impl` and bought nothing, since a node
     * that installs no observer never executes it.
     */
    [[gnu::noinline]] [[nodiscard]] snapshot_t read_installed() const noexcept {
        const std::uint32_t before = gen_.load(std::memory_order_acquire);
        if ((before & 1U) == 0U) {
            const Fn fn = fn_.load(std::memory_order_relaxed);
            void* const ctx = ctx_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (gen_.load(std::memory_order_relaxed) == before) return {fn, ctx};
        }
        return {nullptr, nullptr};
    }

    /** @brief Even = settled, odd = publishing. Bumped twice per @ref set. */
    std::atomic<std::uint32_t> gen_{0};
    std::atomic<Fn> fn_{nullptr};
    std::atomic<void*> ctx_{nullptr};
};

}  // namespace tr::net
