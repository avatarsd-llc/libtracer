/**
 * @file
 * @brief A global heap-allocation counter — the "measured, not asserted" instrument the 16KB-RAM
 *        zero-heap gate requires (ADR-0038 §16KB-RAM feasibility gate: "zero heap allocations and
 *        zero heap growth, measured (an allocator counter / __wrap_malloc)").
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The bench replaces the global `operator new`/`delete` (all variants, incl. the
 * aligned-nothrow form `heap_alloc` uses and the plain form STL containers use) with
 * a thin counting wrapper over malloc/free. When ARMED, every C++ allocation on the
 * calling thread's path is counted; DISARMED, it is pass-through. So a caller can
 * bracket exactly one forward hop (or one terminus resolve) and read back the count
 * and bytes it cost — the number the Stage-2 flip must drive to zero on the forward
 * path (ADR-0038 invariants #1/#2/#5).
 *
 * The override lives in exactly one TU (bench_forward_heap.cpp); this header only
 * declares the arm/reset/snapshot controls. Counting is process-global and not
 * thread-scoped, so the bench measures on ONE thread (the synchronous-substrate model:
 * a 16KB CAN node forwards inline on its receive, no async handoff — ADR-0038).
 */
#pragma once

#include <atomic>
#include <cstddef>

namespace probe {

/** @brief The live counters (defined in bench_forward_heap.cpp alongside the operator news). */
inline std::atomic<bool> g_armed{false};
inline std::atomic<std::size_t> g_allocs{0};
inline std::atomic<std::size_t> g_frees{0};
inline std::atomic<std::size_t> g_bytes{0};

/** @brief One measurement window's result. */
struct counts_t {
    std::size_t allocs = 0; /**< number of operator-new calls while armed */
    std::size_t frees = 0;  /**< number of operator-delete calls while armed */
    std::size_t bytes = 0;  /**< total bytes requested by those allocs */
};

/** @brief Zero the counters (call before arming a fresh window). */
inline void reset() {
    g_allocs.store(0, std::memory_order_relaxed);
    g_frees.store(0, std::memory_order_relaxed);
    g_bytes.store(0, std::memory_order_relaxed);
}

/**
 * @brief Begin / end counting.
 *
 * seq_cst so the arm strictly precedes the measured region.
 */
inline void arm() { g_armed.store(true, std::memory_order_seq_cst); }
inline void disarm() { g_armed.store(false, std::memory_order_seq_cst); }

/** @brief Read the counters (typically after disarm()). */
[[nodiscard]] inline counts_t snapshot() {
    return counts_t{g_allocs.load(std::memory_order_relaxed),
                    g_frees.load(std::memory_order_relaxed),
                    g_bytes.load(std::memory_order_relaxed)};
}

/** @brief RAII window: reset + arm on construction, disarm on scope exit; read via .result(). */
class window_t {
   public:
    window_t() {
        reset();
        arm();
    }
    ~window_t() { disarm(); }
    window_t(const window_t&) = delete;
    window_t& operator=(const window_t&) = delete;
    [[nodiscard]] counts_t result() const { return snapshot(); }
};

}  // namespace probe
