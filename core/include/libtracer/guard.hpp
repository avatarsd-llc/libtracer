/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * guard — the one critical-section vocabulary (RFC-0028 §5.5, slice 10), freestanding. The LKV
 * slot, both memory-layer pools and the write-sequence bump all bind it.
 */
#pragma once

#include <atomic>
#include <concepts>
#include <type_traits>

/**
 * @file
 * @brief The critical-section concepts (`tr::lockable`, `tr::guard`), the no-op guard, the RAII
 *        scope and `rmw_counter_t` — the freestanding half of the guard vocabulary.
 *
 * This header lives in the LAYER-NEUTRAL `tr` namespace (#1703), as `%sink_slot.hpp` does
 * (#1049): the memory layer (L0) and the graph layer (L4) both bind a guard, so a home in
 * `tr::graph` made L0 reach up a layer to name its own lock. It includes only `<atomic>`,
 * `<concepts>` and `<type_traits>` — no `<thread>`, no `<chrono>` — so a target with no thread
 * support (the Cortex-M0 footprint sentinel) can include it. The host's address-striped guard
 * sleeps when it spins out, so it needs `<thread>` and lives apart, in `%guard_mutex.hpp`.
 */

namespace tr {

/**
 * @brief The lock-object half of the contract: `lock()` and `unlock()`, both `noexcept`.
 *
 * What a pool that owns its one lock asks of it (`tr::mem::pool_source_t<Sync>`, ADR-0067: the
 * seam "asks only for `lock()`/`unlock()`"). A `tr::guard` is always one.
 */
template <class L>
concept lockable = requires(L& l) {
    { l.lock() } noexcept;
    { l.unlock() } noexcept;
};

/**
 * @brief The one critical-section contract a build binds per target (RFC-0028 §5.5, §5.6).
 *
 * A guard is a LOCK OBJECT: `lock()` / `unlock()` open and close one short section. The same
 * type serves every critical section the library opens —
 *
 * - the single-writer LKV slot takes the guard that covers the slot's address,
 *   `G::for_address(slot)`, around its pointer swap and its handle copy;
 * - `tr::mem::synchronized_pool_t<G>` owns one `G` and takes it around its free-list edit;
 * - `tr::rmw_counter_t` takes it around its bump on a core with no atomic read-modify-write.
 *
 * The build binds it once, as `tr::graph::config_t::guard_t`. It was spelled `reader_guard_t`
 * until #1703, a name that undersold it: the guard serializes writers, the pool and the
 * write-sequence bump, not only readers.
 *
 * The traits are what a caller reasons with, in place of prose:
 * - `is_isr_safe` — the section may be entered from an interrupt;
 * - `is_nonblocking` — entering never waits on the OS (no heap, no syscall, no sleep);
 * - `may_spin` — a contender can SPIN-wait on the holder, unboundedly. Refused where
 *   `kSpinWaitSafe` is `false` (#1158, #1618): a spinner that outranks the holder never yields
 *   the CPU the holder needs.
 * - `name` — a stable name, which a pool built over the guard reports as its own.
 */
template <class G>
concept guard = lockable<G> && requires(const void* at) {
    { G::for_address(at) } noexcept -> std::same_as<G&>;
    { G::is_isr_safe } -> std::convertible_to<bool>;
    { G::is_nonblocking } -> std::convertible_to<bool>;
    { G::may_spin } -> std::convertible_to<bool>;
    { G::name } -> std::convertible_to<const char*>;
};

/**
 * @brief The guard that guards nothing — for a build that is single-threaded by contract, and
 *        the default policy of a pool one thread owns.
 *
 * A build that binds `hazard_slot_t` never opens a guard, so this is never taken there. A
 * build that binds `single_writer_slot_t` (or a pool) over this guard is asserting that no
 * other thread ever touches the guarded state concurrently — true of a single-threaded program,
 * and of a `tr::mem::pool_source_t` whose one owner is the only thread that draws from it
 * (ADR-0067). It is empty, so `[[no_unique_address]]` erases it.
 */
struct no_guard_t {
    static constexpr bool is_isr_safe = false;   /**< @brief Guards nothing, so promises nothing. */
    static constexpr bool is_nonblocking = true; /**< @brief Does nothing, so never waits. */
    static constexpr bool may_spin = false;      /**< @brief Never waits at all. */
    static constexpr const char* name = "no_guard"; /**< @brief Census name. */

    /** @brief No section to open. */
    void lock() noexcept {}
    /** @brief No section to close. */
    void unlock() noexcept {}
    /** @brief The one instance every address shares; it holds nothing. */
    static no_guard_t& for_address(const void*) noexcept {
        static no_guard_t g;
        return g;
    }
};

/**
 * @brief One section of the guard @p G that covers @p at, for the scope's lifetime — how the
 *        LKV slot and `tr::rmw_counter_t` open their guard.
 */
template <class G>
class guard_scope_t {
   public:
    /** @brief Take the guard covering the address @p at. */
    explicit guard_scope_t(const void* at) noexcept : g_(G::for_address(at)) { g_.lock(); }
    /** @brief Release it. */
    ~guard_scope_t() { g_.unlock(); }
    guard_scope_t(const guard_scope_t&) = delete;
    guard_scope_t& operator=(const guard_scope_t&) = delete;

   private:
    G& g_; /**< @brief The instance taken; outlives the scope (static or pool-owned). */
};

/**
 * @brief A wrapping counter, bumped by many writers and read without a lock, whose bump is
 *        chosen at compile time from what the target's hardware can do (#1621, RFC-0028 D6).
 *
 * - **Native** (`std::atomic<T>::is_always_lock_free`): the bump is one `fetch_add` — `lock
 *   xadd` on x86-64, `amoadd.w` on rv32imac (ESP32-C6), `ldrex`/`strex` on Cortex-M3/M4/M7
 *   (both cores of an STM32H7), `s32c1i` on Xtensa. No call, no masked interrupt.
 * - **Guarded** (no atomic RMW: rv32imc such as the ESP32-C3, Cortex-M0/M0+): the bump is a
 *   load and a store inside one section of @p G, the build's ONE critical-section type
 *   (`config_t::guard_t`, RFC-0028 §5.5) — an interrupt mask on a single-core chip, and on a
 *   dual-core one the cross-core lock `G` already is for the LKV slot. Plain aligned loads and
 *   stores stay single instructions on these targets (rv32imc compiles them to
 *   `fence; lw; fence`), so only the bump pays, and it pays the same section libatomic would
 *   open for it, without the call.
 *
 * A guarded bump that skipped the guard would be unsound with two writers: one that loaded
 * `n` and was preempted can store `n + 1` after a later writer's `n + 2`, rewinding the counter
 * to a value a reader already snapshotted, and that reader then misses the later change. The
 * guard serializes the writers; the reader needs no guard, because every store is a whole
 * aligned word.
 *
 * Both bindings give the bump `seq_cst` ordering, so a caller's Dekker pair (bump, then read a
 * flag; set the flag, then read the counter) holds on either. The counter only ever moves by
 * one and wraps at `T`'s width, so it is for EQUALITY tests (`now != then`), never `<`.
 *
 * @tparam T       An unsigned integer, at most a machine word wide.
 * @tparam G       The build's critical-section guard (a `tr::guard`), taken only by the guarded
 *                 binding.
 * @tparam kNative Which binding. Defaults to what the target supports; a test names it to
 *                 drive the guarded binding on a host that has atomic RMW.
 */
template <class T, class G, bool kNative = std::atomic<T>::is_always_lock_free>
class rmw_counter_t {
    static_assert(std::is_unsigned_v<T>, "the counter wraps, so it must be unsigned");

   public:
    /** @brief Whether the bump is one hardware RMW (`true`) or a guarded load + store. */
    static constexpr bool is_native = kNative;

    /** @brief Move the counter on by one, `seq_cst`, wrapping at `T`'s width. */
    void bump() noexcept {
        if constexpr (kNative) {
            value_.fetch_add(1, std::memory_order_seq_cst);
        } else {
            static_assert(guard<G>, "the guarded bump needs the build's guard (tr::guard)");
            const guard_scope_t<G> section(this);
            value_.store(static_cast<T>(value_.load(std::memory_order_relaxed) + 1u),
                         std::memory_order_seq_cst);
        }
    }

    /** @brief The current count, `seq_cst`. Lock-free on both bindings. */
    [[nodiscard]] T load() const noexcept { return value_.load(std::memory_order_seq_cst); }

    /**
     * @brief Set the count outright. For a test that must reach the wrap without 2^32 bumps,
     *        and for nothing else: a store races every bump.
     */
    void preset(T value) noexcept { value_.store(value, std::memory_order_seq_cst); }

   private:
    std::atomic<T> value_{0}; /**< @brief The count; stores and loads are whole words. */
};

}  // namespace tr
