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
 * storage with no destructor and no initialization guard (which is what lets
 * @ref reclaim_local_t's per-thread parking allocate nothing, ever).
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
 * **Re-entrant unsubscribe is FORBIDDEN**, not merely discouraged: unsubscribing from
 * inside a delivery would retire a pair the running fan-out's snapshot still names. A debug
 * build asserts on it (see `%graph.cpp`); an `NDEBUG` build cannot see it, which is the
 * trade this policy exists to make. Select it only where the deployment can show that
 * re-entrant unsubscribe does not occur — otherwise take the default, which supports it.
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

    /** @brief Re-entrant `unsubscribe()` (from inside a delivery) is not supported. */
    static constexpr bool kReentrantUnsubscribe = false;

    /**
     * @brief Whether the grace point is stated over EVERY thread rather than one.
     *
     * False here: there is no grace period at all, so there is nothing for a second thread to
     * be inside. See @ref reclaim_qsbr_t for the policy that answers true and what changes.
     */
    static constexpr bool kGraceSpansThreads = false;
};

/**
 * @brief **`reclaim_local`** (the DEFAULT) — the grace point is the moment this thread's
 *        dispatch stack unwinds to depth 0.
 *
 * It is the default because it makes the MCU and the host build behave IDENTICALLY
 * (ADR-0080 §Decision 1). `reclaim_strict` would make the same application code legal on a
 * host that tolerates re-entrant unsubscribe and illegal on the constrained target — a
 * portability bug that surfaces only where it is hardest to debug.
 *
 * @section reclaim_local_guarantee What a caller gets
 *
 * Exactly one of two things, and the library decides which — the caller never asks:
 *
 * 1. **`unsubscribe()` was called from outside any delivery** (dispatch depth 0 — the
 *    ordinary case). No delivery to that context can be in flight on this thread, so the
 *    release hook runs INLINE and `unsubscribe()` returns already quiescent. This is
 *    `reclaim_strict`'s guarantee, delivered at `reclaim_strict`'s cost, for the case that
 *    dominates.
 * 2. **`unsubscribe()` was called from INSIDE a delivery** (a subscriber callback
 *    unsubscribing itself or a sibling). The running fan-out is walking a snapshot that
 *    still names the retired pair, so the pair is PARKED and the hook runs when the
 *    outermost delivery on this thread returns — i.e. before the `write()` / `propagate()`
 *    that started it hands control back.
 *
 * In both cases the signal is the hook. There is no poll, no wait, and no verb the embedder
 * must remember to call.
 *
 * @section reclaim_local_scope The scope of the guarantee
 *
 * It is stated over **one thread's dispatch domain**, which is the WIDE / MCU target this
 * policy is for: a single-threaded node, where publish and `unsubscribe()` cannot overlap
 * because there is no second thread to overlap with. An embedder that dispatches from
 * several threads concurrently and unsubscribes from another needs a grace period spanning
 * every thread — that is @ref reclaim_qsbr_t, ADR-0080's third policy
 * ([#894](https://github.com/avatarsd-llc/libtracer/issues/894),
 * [#1376](https://github.com/avatarsd-llc/libtracer/issues/1376)), not this one.
 *
 * @section reclaim_local_cost What it costs
 *
 * One non-atomic increment, decrement and branch per `fan_out` — regardless of subscriber
 * count — on a per-thread counter, so no cache line is ever shared and no atomic is
 * involved. Parking allocates nothing: the retired pairs sit in a bounded per-thread array
 * sized by @ref tr::graph::default_config_t::kDeferredReleaseSlots.
 */
struct reclaim_local_t {
    /** @brief The policy's name, for a diagnostic that must say which one is bound. */
    static constexpr std::string_view kName = "reclaim_local";

    /**
     * @brief Whether a retired pair may be DEFERRED past `unsubscribe()`'s return.
     *
     * True here: a re-entrant unsubscribe parks its pair and the outermost dispatch's exit
     * releases it. The deferral happens ONLY at depth > 0 — at depth 0 the release is inline.
     */
    static constexpr bool kDefersToDispatchExit = true;

    /** @brief Re-entrant `unsubscribe()` (from inside a delivery) is supported. */
    static constexpr bool kReentrantUnsubscribe = true;

    /**
     * @brief Whether the grace point is stated over EVERY thread rather than one.
     *
     * False here, and it is the ONE limitation of this policy: the grace point is this
     * thread's dispatch stack, so a sibling thread's in-flight fan-out is invisible to it. See
     * @ref reclaim_local_scope.
     */
    static constexpr bool kGraceSpansThreads = false;
};

/**
 * @brief **`reclaim_qsbr`** — the grace point is the moment EVERY dispatching thread has
 *        passed a quiescent state (#1376).
 *
 * ADR-0080's third policy and the MID / NARROW many-core one: bind it when this node
 * dispatches from several threads at once and may unsubscribe from a thread other than the one
 * delivering. That is the single case neither shipped policy covers —
 * @ref reclaim_local_t's grace point is one thread's dispatch stack, so a sibling thread's live
 * snapshot is invisible to it, and @ref reclaim_strict_t has no grace period at all.
 *
 * Selecting it is one line in `libtracer/config_override.hpp`:
 * `using reclaim_policy_t = reclaim_qsbr_t;`
 *
 * @section reclaim_qsbr_guarantee What a caller gets
 *
 * A retired `{fn, ctx}` pair is released once no thread can still be walking a snapshot that
 * names it. As under @ref reclaim_local_t the caller never polls and never waits; it registers
 * a @ref subscriber_release_fn_t and is told. Two cases, and again the library picks:
 *
 * 1. **No participant is mid-dispatch** — every ordinary unsubscribe on a node that is not
 *    concurrently publishing. The scan concludes immediately, the hook runs INLINE, and
 *    `unsubscribe()` returns already quiescent. This is the same property (a) the other two
 *    policies deliver, and it still dominates.
 * 2. **Some participant IS mid-dispatch.** The pair is deferred and released by whichever
 *    participant next completes the grace period.
 *
 * @section reclaim_qsbr_thread The one API difference, stated rather than hidden
 *
 * In case 2 the hook runs **on a thread other than the `unsubscribe()` caller** — specifically
 * on whichever participant's quiescence completed the grace period, or on the caller's own
 * next dispatch exit, whichever comes first. Both other policies promise the caller's own
 * thread; a grace period that spans threads structurally cannot, because the only alternatives
 * are to block the caller (ADR-0080 §Decision 4 rejects waiting outright) or to leak the pair.
 *
 * So a release hook under this policy must be **thread-safe with respect to its own context**.
 * That is a real widening of the contract and it is why this is an opt-in policy rather than
 * the default: on the single-threaded target @ref reclaim_local_t serves, the distinction does
 * not exist, and ADR-0080 §Decision 1's parity argument keeps the default where it is.
 *
 * @section reclaim_qsbr_cost What it costs
 *
 * Per OUTERMOST `fan_out` — never per edge, and nothing at all on a publish nobody subscribed
 * to, because the bracket sits below `fan_out`'s no-subscriber gate:
 *
 * - entry: one RELAXED load of a read-mostly shared line (the epoch, bumped only on the
 *   control plane) and one `seq_cst` store to this thread's own cache-line-isolated cell. No
 *   atomic read-modify-write;
 * - exit: one `release` store to that same cell, plus one relaxed load of a read-mostly count
 *   that is 0 on any node not mid-teardown, and the predictable branch it guards.
 *
 * The `O(kQsbrParticipants)` scan is on the RECLAIM path only — that is the precise difference
 * from the shape [#635](https://github.com/avatarsd-llc/libtracer/issues/635) rejected, which
 * put a hazard scan on the READ path. Storage is
 * @ref tr::graph::default_config_t::kQsbrParticipants cache-line-isolated cells plus one
 * shared table of @ref tr::graph::default_config_t::kDeferredReleaseSlots retired pairs, all
 * `.bss`, and **none of it emitted into a build that binds a different policy**.
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
     * True: this policy needs the very same dispatch bracket @ref reclaim_local_t does — the
     * transition to depth 0 IS the quiescent state it announces — and defers whenever the scan
     * finds a participant that has not yet reached one.
     */
    static constexpr bool kDefersToDispatchExit = true;

    /** @brief Re-entrant `unsubscribe()` is supported — subsumed by the grace period. */
    static constexpr bool kReentrantUnsubscribe = true;

    /** @brief The grace point is stated over EVERY dispatching thread. See
     *         @ref reclaim_qsbr_thread for what that changes for a release hook. */
    static constexpr bool kGraceSpansThreads = true;
};

// The seam is a build-time-closed type set (ADR-0047 §1) and the three policies above are its
// whole membership — ADR-0080 §Decision names exactly these and no fourth is contemplated.
// `%config.hpp` derives the loose `reclaim_policy_t` / `kDeferredReleaseSlots` /
// `kQsbrParticipants` spellings from `config_t`, exactly as it does for every other knob — this
// header only DEFINES the types a fragment may bind to.
static_assert(reclaim_strict_t::kDefersToDispatchExit != reclaim_local_t::kDefersToDispatchExit,
              "two policies must differ in the one property graph_t branches on; if they ever "
              "agree, that branch is dead and one of them is redundant");
static_assert(reclaim_local_t::kGraceSpansThreads != reclaim_qsbr_t::kGraceSpansThreads,
              "reclaim_qsbr exists precisely because its grace point spans threads and "
              "reclaim_local's does not; if that ever stops being true, so does the policy");
static_assert(!reclaim_policy_t::kDefersToDispatchExit || kDeferredReleaseSlots > 0,
              "this build binds a DEFERRING reclamation policy but gave it nowhere to park: "
              "0 slots would drop and leak every re-entrant unsubscribe. Raise "
              "kDeferredReleaseSlots, or bind reclaim_strict_t if you want no parking at all.");
static_assert(!reclaim_policy_t::kGraceSpansThreads || kQsbrParticipants > 0,
              "this build binds reclaim_qsbr but left it no participant cells: with 0 cells "
              "every dispatching thread lands in the overflow tally and no retired pair could "
              "ever be reclaimed. Raise kQsbrParticipants.");
static_assert(!reclaim_policy_t::kGraceSpansThreads || reclaim_policy_t::kDefersToDispatchExit,
              "a cross-thread grace period is announced BY the dispatch bracket, so a policy "
              "that spans threads must also take that bracket");
static_assert(std::is_same_v<subscriber_release_fn_t, void (*)(void*)>,
              "the QSBR domain types its retired hooks as a bare void(*)(void*) so that "
              "`%qsbr.hpp` need not include this header — the two spellings must stay one type");

}  // namespace tr::graph
