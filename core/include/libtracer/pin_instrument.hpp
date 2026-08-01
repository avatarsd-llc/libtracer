/**
 * @file
 * @brief The RFC-0022 §6 reachability instrument: how many WRITE stores actually took the pinned
 *        subview branch versus the one-copy branch.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * §6 asks whether turning pinning on by default is a latency win. Every arm of that
 * measurement is worthless unless the arm that intends to pin *reached the pin branch* —
 * pinning needs an OWNING, view-delivered frame AND a trailer-less opt byte, and a bench
 * that satisfies neither reports a clean "no regression" on nothing (the `fold-b4` lesson:
 * a gate leg whose reachability was never checked reported failures against inert code).
 * So the decision site ticks a counter per branch and every published cell carries it.
 *
 * **Off unless asked for.** Without `LIBTRACER_PIN_INSTRUMENT` the counters do not exist and
 * the calls compile to nothing, so the measured binaries the library ships are unchanged.
 * The bench target defines it for the library and for itself, which is why it lives in a
 * public header rather than beside the decision site in `core/src`.
 *
 * @section pin_instr_tls Why these are thread_local and NOT atomic
 *
 * They were `std::atomic<std::uint64_t>` for exactly one run. RFC-0022 §6's arm B — the
 * same-binary sentinel, whose whole job is to prove the implementing build is
 * indistinguishable from untouched main — came out **~20 ns per store slower** than the
 * control (185 ns against 161 ns p50, the same sign in 12 of 12 interleaved rounds). Rebuilding
 * arm B with the instrument off closed the gap to zero. One relaxed `fetch_add` on a shared
 * cache line, on the hot store path, was a fifth of the effect the bench was trying to
 * resolve: the instrument was measuring itself.
 *
 * `thread_local` plain integers cost a TLS-relative load-add-store, touch no shared line, and
 * restored arm B to parity. The price is that a counter must be READ on the thread that ticked
 * it — true for the microbench (single-threaded) and irrelevant to `bench_pin_net`, which
 * counts deliveries in the receiver's graph and never reads these.
 */
#pragma once

#ifdef LIBTRACER_PIN_INSTRUMENT

#include <cstdint>

namespace tr::graph::instrument {

/** @brief Stores that took the ADR-0042 §3 pinned-subview branch (this thread's). */
inline thread_local std::uint64_t g_pin_hits = 0;
/** @brief Stores that took the ADR-0041 §2 one-copy branch (this thread's). */
inline thread_local std::uint64_t g_copy_hits = 0;
/**
 * @brief Stores whose ratio predicate said PIN but whose reader could not (`pin_wire` returned
 *        `nullopt` — a borrowed, span-delivered frame with no owning segment to subview).
 *
 * Separated from @ref g_copy_hits because the two mean opposite things: a copy here is the
 * transport being definitionally inert for pinning, not the predicate declining.
 */
inline thread_local std::uint64_t g_pin_refused = 0;

inline void tick_pin() noexcept { ++g_pin_hits; }
inline void tick_copy() noexcept { ++g_copy_hits; }
inline void tick_refused() noexcept { ++g_pin_refused; }

/** @brief Zero this thread's three counters — call between interleaved arms. */
inline void reset() noexcept {
    g_pin_hits = 0;
    g_copy_hits = 0;
    g_pin_refused = 0;
}

}  // namespace tr::graph::instrument

#define LIBTRACER_TICK_PIN() ::tr::graph::instrument::tick_pin()
#define LIBTRACER_TICK_COPY() ::tr::graph::instrument::tick_copy()
#define LIBTRACER_TICK_PIN_REFUSED() ::tr::graph::instrument::tick_refused()

#else

#define LIBTRACER_TICK_PIN() ((void)0)
#define LIBTRACER_TICK_COPY() ((void)0)
#define LIBTRACER_TICK_PIN_REFUSED() ((void)0)

#endif
