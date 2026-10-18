/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The RECLAMATION POLICY seam (ADR-0080) — when the library may free the memory behind a
 * user-code seam after the user asks to release it. Selected per target on the ADR-0079
 * axis and closed at build time, exactly like `acl_policy_t` and `lkv_slot_t`: a policy is
 * a TYPE named by `tr::graph::default_config_t::reclaim_policy_t`, never a runtime flag
 * and never a template parameter threaded through `graph_t` (ADR-0070 §Decision 2).
 */
#pragma once

#include <cstddef>
#include <string_view>
#include <type_traits>

#include "libtracer/config.hpp"
#include "libtracer/qsbr.hpp"

/**
 * @file
 * @brief `tr::graph` reclamation policies (ADR-0080): the build-time-closed grace point at
 *        which a retired subscription's `{fn, ctx}` pair becomes safe to free.
 */

namespace tr::graph {

/**
 * @brief The hook a caller hands @ref graph_t::unsubscribe so the LIBRARY can signal it that
 *        the retired subscription's context is dead.
 *
 * The direction is the whole point of ADR-0080 §Decision 4: the embedder never polls
 * in-flight state and never waits. It registers this, and libtracer calls it exactly once,
 * on the caller's own thread, outside every graph lock, at the policy's grace point. A
 * contract of the form "the callback may still be invoked until you call X" is what that
 * decision rejects.
 *
 * @param ctx The `callback_ctx` the subscription was admitted with, handed straight back.
 */
using subscriber_release_fn_t = void (*)(void* ctx);

/**
 * @brief One retired subscription's release obligation — the `{ctx, hook}` pair a policy
 *        that defers must hold until its grace point.
 *
 * Trivially copyable and free of any owning member, so a bounded array of these is plain
 * storage with no destructor and no initialization guard (which is what lets the QSBR
 * domain's retired table allocate nothing, ever).
 */
struct retired_callback_t {
    void* ctx = nullptr;                       /**< @brief The subscriber's own context. */
    subscriber_release_fn_t release = nullptr; /**< @brief What to call on it, exactly once. */
};

/**
 * @brief **`reclaim_strict`** — the grace point is the moment `unsubscribe()` returns.
 *
 * The opt-in ZERO-COST mode (ADR-0080 §Decision 2), for an MCU deployment that provably
 * never unsubscribes from inside a dispatch. Nothing is tracked on the dispatch path, no
 * state is held anywhere, and `unsubscribe()` runs the release hook inline before it
 * returns — so the caller may free its context on that return with no further ceremony.
 *
 * **Re-entrant and cross-thread unsubscribe are FORBIDDEN and NOT DETECTED**: unsubscribing
 * while any delivery is live — inside one, or on another thread — would retire a pair a
 * running fan-out's snapshot still names. This policy keeps no dispatch state, so no build can
 * see it; that is the trade it exists to make (ADR-0080 Amendment 1 §1). Select it only where
 * the deployment can show that no dispatch is live when it unsubscribes, for example a
 * thread-affine context that must be released on its own thread — otherwise take the default.
 *
 * Selecting it is one line in `libtracer/config_override.hpp`:
 * `using reclaim_policy_t = reclaim_strict_t;`
 */
struct reclaim_strict_t {
    /** @brief The policy's name, for a diagnostic that must say which one is bound. */
    static constexpr std::string_view kName = "reclaim_strict";

    /**
     * @brief Whether a retired pair may be DEFERRED past `unsubscribe()`'s return.
     *
     * False here: there is no grace period to defer into, so `unsubscribe()` releases
     * inline and the dispatch path carries nothing at all.
     */
    static constexpr bool kDefersToDispatchExit = false;
};

/**
 * @brief **`reclaim_qsbr`** (the DEFAULT on every target) — the grace point is the moment
 *        EVERY dispatching thread has passed a quiescent state (#1376, #2127).
 *
 * The default on WIDE, MID and NARROW alike (ADR-0080 Amendment 1 §1). A grace point stated
 * over one thread cannot be safe: an app thread that unsubscribes at depth 0 would release a
 * pair another thread's fan-out snapshotted a moment before, and preemption opens that window
 * on one CPU too. @ref reclaim_strict_t is the only opt-out.
 *
 * @section reclaim_qsbr_guarantee What a caller gets
 *
 * A retired `{fn, ctx}` pair is released once no thread can still be walking a snapshot that
 * names it. The caller never polls and never waits; it registers a
 * @ref subscriber_release_fn_t and is told. Two cases, and the library picks:
 *
 * 1. **No participant is mid-dispatch** — every ordinary unsubscribe on a node that is not
 *    concurrently publishing. The scan concludes immediately, the hook runs INLINE, and
 *    `unsubscribe()` returns already quiescent. This is @ref reclaim_strict_t's guarantee,
 *    and it dominates.
 * 2. **Some participant IS mid-dispatch.** The pair is deferred and released by whichever
 *    participant next completes the grace period.
 *
 * @section reclaim_qsbr_thread Where the hook runs
 *
 * In case 2 the hook runs **on a thread other than the `unsubscribe()` caller** — on whichever
 * participant's quiescence completed the grace period, or on the caller's own next dispatch
 * exit, whichever comes first. A grace period that spans threads structurally cannot promise
 * the caller's thread: the only alternatives are to block the caller (ADR-0080 §Decision 4
 * rejects waiting outright) or to leak the pair. The parity this keeps is the one Amendment 1
 * defines: the hook runs once, after every snapshot that could name the pair is gone, the same
 * on every target.
 *
 * So a release hook must be **short, thread-safe with respect to its own context,
 * non-blocking and `noexcept`**: it may run on a receiving thread, at that thread's priority
 * and on its stack. Posting the context to an app-owned queue is the ideal shape. A context
 * that must be released on its own thread takes @ref reclaim_strict_t instead.
 *
 * @section reclaim_qsbr_cost What it costs
 *
 * Per OUTERMOST `fan_out` — never per edge, and nothing at all on a publish nobody subscribed
 * to, because the bracket sits below `fan_out`'s no-subscriber gate:
 *
 * - entry: one RELAXED load of a read-mostly shared line (the epoch, bumped only on the
 *   control plane), one release store to this thread's own cache-line-isolated cell and one
 *   `seq_cst` fence after it (`qsbr.hpp` §qsbr_ordering). No atomic read-modify-write on a
 *   shared line;
 * - exit: one `release` store to that same cell, plus one relaxed load of a read-mostly count
 *   that is 0 on any node not mid-teardown, and the predictable branch it guards.
 *
 * The `O(kQsbrParticipants)` scan is on the RECLAIM path only — that is the precise difference
 * from the shape [#635](https://github.com/avatarsd-llc/libtracer/issues/635) rejected, which
 * put a hazard scan on the READ path. Storage is
 * @ref tr::graph::default_config_t::kQsbrParticipants cache-line-isolated cells plus one
 * shared table of @ref tr::graph::default_config_t::kDeferredReleaseSlots retired pairs, all
 * `.bss`. The cells and the epoch are also the transport plane's grace in every build
 * (`fwd_router_t`'s frame bracket), so a net-plane build carries them under either policy;
 * the retired table is its own object, named only by this policy's paths, so a
 * `reclaim_strict` build does not link it (#2111).
 *
 * @section reclaim_qsbr_897 What it discharges for #897
 *
 * ADR-0080 §"#897 maps onto the same seam" asks that each thread self-drain its own retired
 * LKV list at its own quiescent point, so `~hazard_slot_t` never has to reach across a live
 * thread's private list. Under this policy that is exactly what happens, and it costs
 * `%lkv_slot.hpp` no code at all: the quiescent point calls the already-shipped
 * `%tr::graph::detail_hp::retire_and_flush(nullptr)`, whose cheap early-out makes it free on a
 * thread that parked nothing. No `store()`-path atomic is added.
 */
struct reclaim_qsbr_t {
    /** @brief The policy's name, for a diagnostic that must say which one is bound. */
    static constexpr std::string_view kName = "reclaim_qsbr";

    /**
     * @brief Whether a retired pair may be DEFERRED past `unsubscribe()`'s return.
     *
     * True: the dispatch bracket's transition to depth 0 IS the quiescent state this policy
     * announces, and it defers whenever the scan finds a participant that has not yet reached
     * one.
     */
    static constexpr bool kDefersToDispatchExit = true;
};

// The seam is a build-time-closed type set (ADR-0047 §1) of exactly two tags (ADR-0080
// Amendment 1 §1): `reclaim_local_t` is withdrawn, and `%config.hpp` refuses a build that still
// names it. `%config.hpp` derives the loose `reclaim_policy_t` / `kDeferredReleaseSlots` /
// `kQsbrParticipants` spellings from `config_t`, exactly as it does for every other knob — this
// header only DEFINES the types a fragment may bind to.
static_assert(reclaim_strict_t::kDefersToDispatchExit != reclaim_qsbr_t::kDefersToDispatchExit,
              "the two policies must differ in the one property graph_t branches on; if they "
              "ever agree, that branch is dead and one of them is redundant");
static_assert(!reclaim_policy_t::kDefersToDispatchExit || kDeferredReleaseSlots > 0,
              "this build binds a DEFERRING reclamation policy but gave it nowhere to park: "
              "0 slots would refuse every unsubscribe made while a dispatch is live. Raise "
              "kDeferredReleaseSlots, or bind reclaim_strict_t if you want no parking at all.");
static_assert(!reclaim_policy_t::kDefersToDispatchExit || kQsbrParticipants > 0,
              "this build binds reclaim_qsbr but left it no participant cells: with 0 cells "
              "every dispatching thread lands in the overflow tally and no retired pair could "
              "ever be reclaimed. Raise kQsbrParticipants.");
static_assert(std::is_same_v<subscriber_release_fn_t, void (*)(void*)>,
              "the QSBR domain types its retired hooks as a bare void(*)(void*) so that "
              "`%qsbr.hpp` need not include this header — the two spellings must stay one type");

}  // namespace tr::graph
