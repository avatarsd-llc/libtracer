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

#include "libtracer/config.hpp"

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
 * every thread — that is `reclaim_qsbr`, ADR-0080's third policy
 * ([#894](https://github.com/avatarsd-llc/libtracer/issues/894) follow-up), not this one.
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
};

// The seam is a build-time-closed type set (ADR-0047 §1) and this is its whole membership
// today. `reclaim_qsbr` — the MID / NARROW many-core policy whose grace point is a quiescent
// state on every thread — is deliberately NOT declared here: an alias a target can bind to
// nothing is worse than an absent one, so it arrives with its implementation.
// `%config.hpp` derives the loose `reclaim_policy_t` / `kDeferredReleaseSlots` spellings from
// `config_t`, exactly as it does for every other knob — this header only DEFINES the types a
// fragment may bind to.
static_assert(reclaim_strict_t::kDefersToDispatchExit != reclaim_local_t::kDefersToDispatchExit,
              "the two shipped policies must differ in the one property graph_t branches on; "
              "if they ever agree, that branch is dead and one of them is redundant");
static_assert(!reclaim_policy_t::kDefersToDispatchExit || kDeferredReleaseSlots > 0,
              "this build binds a DEFERRING reclamation policy but gave it nowhere to park: "
              "0 slots would drop and leak every re-entrant unsubscribe. Raise "
              "kDeferredReleaseSlots, or bind reclaim_strict_t if you want no parking at all.");

}  // namespace tr::graph
