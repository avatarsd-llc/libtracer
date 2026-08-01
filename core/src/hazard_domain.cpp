/**
 * @file
 * @brief The reclamation domain's asymmetric barrier: where the hazard protocol's
 *        StoreLoad ordering is PAID (ADR-0072 erratum 1).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A hazard-pointer reader must order "I announce p" before "is p still published?". That
 * is a StoreLoad, the one reordering x86-TSO permits, so the classical protocol spends an
 * `mfence`-class instruction on EVERY protected read — and on the value-seam path that
 * fence was the whole of the measured latency regression (#576).
 *
 * The cost is asymmetric by nature: the reader needs the ordering only because the
 * reclaimer might look at its announcement, and the reclaimer is cold. Linux can move the
 * whole of it to that side — `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED)` interrupts
 * every CPU running a thread of this process and serializes it, which is a fence inserted
 * into every other thread's instruction stream on demand. The reader then announces with a
 * plain store and a compiler barrier, and the argument becomes: a reader either announced
 * before the reclaimer's barrier (so the reclaimer sees the announcement) or issues its
 * re-read after it (so it sees the displacement) — never neither.
 *
 * Where that is unavailable — every non-Linux target, an old kernel, a seccomp policy that
 * denies the call, or a ThreadSanitizer build whose happens-before model cannot represent
 * a barrier delivered by an IPI — the pair degrades to the classical protocol: readers
 * announce `seq_cst`, this file's heavy barrier is a plain `seq_cst` fence, and nothing
 * about correctness changes. The mode is resolved ONCE per process and cached in each
 * reader thread's own storage (`detail_hz::t_light`) when it claims its participant — a
 * thread announcing light implies the probe succeeded, and the heavy barrier below issues
 * the syscall whenever the probe succeeded, so a reader and a scan can never end up on
 * opposite halves of the protocol.
 */
#include "libtracer/hazard_domain.hpp"

#include <atomic>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#if __has_include(<linux/membarrier.h>)
#include <linux/membarrier.h>
#define LIBTRACER_HAVE_MEMBARRIER_H 1
#endif
#endif

/** @brief 1 when this translation unit is compiled under ThreadSanitizer. */
#if defined(__SANITIZE_THREAD__)
#define LIBTRACER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LIBTRACER_TSAN 1
#endif
#endif
#ifndef LIBTRACER_TSAN
#define LIBTRACER_TSAN 0
#endif

namespace tr::mem {
namespace {

#if defined(LIBTRACER_HAVE_MEMBARRIER_H) && !LIBTRACER_TSAN
/** @brief Query the kernel, register for the expedited command, and report usability. */
[[nodiscard]] bool probe_membarrier() noexcept {
    const long q = ::syscall(__NR_membarrier, MEMBARRIER_CMD_QUERY, 0, 0);
    if (q < 0) return false;
    if ((q & MEMBARRIER_CMD_PRIVATE_EXPEDITED) == 0) return false;
    // Registration is mandatory before the expedited command may be issued, and it is
    // per-process (not per-thread), so doing it here — once — is enough.
    if (::syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) != 0)
        return false;
    return true;
}
#else
/** @brief No asymmetric barrier on this platform / build: the classical protocol it is. */
[[nodiscard]] bool probe_membarrier() noexcept { return false; }
#endif

/**
 * @brief The resolved mode. A function-local static so it is initialized on first use
 *        (never during static initialization of another translation unit) exactly once;
 *        every hot-path read of it goes through the domain's own `light_` member instead.
 */
[[nodiscard]] bool light_mode() noexcept {
    static const bool kLight = probe_membarrier();
    return kLight;
}

}  // namespace

bool hazard_light_announce_available() noexcept { return light_mode(); }

void hazard_heavy_barrier() noexcept {
    // Always at least a full fence for THIS thread — that half is what orders the
    // reclaimer's own displacement before its reads of the announcement table, and it is
    // all the classical protocol needs.
    std::atomic_thread_fence(std::memory_order_seq_cst);
#if defined(LIBTRACER_HAVE_MEMBARRIER_H) && !LIBTRACER_TSAN
    // And, in light mode, the other half: serialize every other CPU running a thread of
    // this process, so a reader's plain-store announcement cannot still be sitting in a
    // store buffer when the loop below reads the table.
    if (light_mode()) (void)::syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0);
#endif
}

}  // namespace tr::mem
