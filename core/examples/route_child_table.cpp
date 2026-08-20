/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the routing table is one NAME→link map with one slot per name, and
 *        `add_child`'s `[[nodiscard]] bool` is the only thing between you and a child that is
 *        published UP but reachable by no `dst`.
 *
 * `fwd_router_t` has exactly one table: `child_registry_t`, keyed on this node's own local name
 * for each link (ADR-0037). Everything the routing plane does — forward, reply, advertise —
 * resolves through it. There is no second table, and no destination ever appears in it.
 *
 * Two properties are worth knowing before you wire a node:
 *
 *  1. **A refusal registers NOTHING.** `add_child` returns false for a name no address could
 *     ever spell (empty, containing an empty route segment, wider than `graph::kMaxSegments`) and
 *     for a registry that could not grow. Discarding that `bool` is how #930 shipped: a
 *     connection reported UP whose every forward missed and fell through to the terminus with
 *     no error anywhere. The attribute does not forbid ignoring the result — it makes ignoring
 *     it a deliberate, greppable `(void)`.
 *  2. **A NAME owns exactly one slot, for the router's life** (#884). Re-adding a live name
 *     REBINDS its slot rather than appending a second one, and `remove_child` TOMBSTONES the
 *     slot instead of erasing it, so a later re-add revives it in place. A second slot would
 *     shadow the first on every name-keyed lookup — returning the DEAD one — and churn on a
 *     stable name set would grow the chain every lookup walks without bound.
 *
 * Hence the two counters this example watches: `live_size()` is what routes, `size()` is what
 * lookups walk. Registration/removal churn moves the first and must not grow the second.
 *
 * No frames are routed here — this example is about the table alone. Runs under ctest as
 * `example_route_child_table`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief A `transport_t` that keeps every frame handed to it — an identity for the table. */
struct recording_link_t : tr::net::transport_t {
    std::vector<std::vector<std::byte>> sent; /**< @brief Frames emitted on this link, in order. */
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::vector<std::byte> flat;
        for (const auto part : iov) flat.insert(flat.end(), part.begin(), part.end());
        sent.push_back(std::move(flat));
    }
};

}  // namespace

int main() {
    bool ok = true;
    graph_t g;
    tr::net::fwd_router_t router(g);
    recording_link_t first, second;

    // Every mutation is its own statement, and every observation is a separate one. Mixing
    // them into one printf() argument list would leave the order unspecified — this example
    // is about counters that a call CHANGES, so the sequencing has to be explicit.
    const bool added = router.add_child("up", first);
    check(ok, added, "add_child returns true and the child is registered");
    check(ok, router.registry().live_size() == 1 && router.registry().size() == 1,
          "one live slot, one slot walked");

    // A name no `dst` could ever spell is refused ALWAYS (not just in debug builds), and the
    // table is exactly as it was.
    const bool empty_name = router.add_child("", second);
    const bool empty_segment = router.add_child("a//b", second);
    check(ok, !empty_name && !empty_segment, "an unaddressable name is refused, both spellings");
    check(ok, router.registry().size() == 1, "and a refusal registered NOTHING — no ghost slot");

    // Re-adding a LIVE name rebinds its one slot. The link the name resolves to changes; the
    // number of slots does not.
    const bool rebound = router.add_child("up", second);
    check(ok, rebound, "re-adding a live name succeeds");
    check(ok, router.registry().by_name("up") == &second,
          "and it REBINDS: the name now resolves to the new link");
    check(ok, router.registry().size() == 1 && router.receiver_ctx_count() == 1,
          "one name still owns exactly one slot and one receiver context");

    // Removal is a tombstone, not an erase: nothing routes through the name any more, but the
    // slot a lock-free reader may be walking right now stays put.
    const bool removed = router.remove_child("up");
    check(ok, removed, "remove_child reports that it named a live child");
    check(ok, router.registry().by_name("up") == nullptr, "the name resolves to nothing now");
    check(ok, router.registry().live_size() == 0 && router.registry().size() == 1,
          "live_size() fell to zero; size() did not — the slot is tombstoned, not erased");
    check(ok, !router.remove_child("up"), "removing it again names no live child");

    // The re-add revives the tombstone in place. This is the churn bound: N add/remove cycles
    // on a stable name set leave the chain at the number of distinct NAMES, not of calls.
    const bool revived = router.add_child("up", first);
    check(ok, revived, "a re-add after removal succeeds");
    check(ok, router.registry().live_size() == 1 && router.registry().size() == 1,
          "and revives the SAME slot — churn on a stable name set grows nothing");
    check(ok, router.receiver_ctx_count() == 1, "the receiver-context chain is likewise unchanged");

    std::printf("one name, one slot: %zu live of %zu walked, %zu receiver context(s)\n",
                router.registry().live_size(), router.registry().size(),
                router.receiver_ctx_count());
    return ok ? 0 : 1;
}
