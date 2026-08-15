/**
 * @file
 * @brief route_handle_t unit test (Brick 4, ADR-0038 §3 / ADR-0039): the label state is per-
 *        connection — pmr-backed tables with a per-link mutex.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Covers: bind/lookup,
 * rebind-replaces, stale lookup, per-link label-space isolation, ensure_egress
 * reuse vs fresh, egress_route retention (NACK re-advertise), clear_link resets
 * one link only (allocator included), counts, and a whole-lifecycle run inside a
 * slab-backed monotonic resource with a null upstream (zero global heap for the
 * table state — the ADR-0039 host-owned-memory claim). #488: clear_link reclaims
 * the per-link shell (churn of N distinct names returns link_count() to steady
 * state) while staying UAF-free under a concurrent writer x clear_link race (the
 * shared_ptr pin — a TSan gate, run instrumented by the core-ci tsan job). #603: the label
 * allocator saturates at 65535 instead of wrapping through the reserved 0 back onto labels
 * that still alias live routes, and the per-link tables honour an injected binding bound —
 * refusing new flows rather than growing without limit, and never refusing an established one.
 */

#include "libtracer/route_handle.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory_resource>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "test_support.hpp"

namespace {

using namespace tr::net;

using tr::testing::check;

std::vector<std::byte> route_bytes(std::uint8_t tag) {
    return {std::byte{0x06}, std::byte{0x40}, std::byte{0x01}, std::byte{0x00}, std::byte{tag}};
}

/**
 * @brief Build a terminus binding field by field.
 *
 * A designated-initializer brace naming only some members trips
 * `-Werror=missing-field-initializers` on the ESP toolchain — the same reason `fwd_router.cpp`
 * builds its bindings field by field rather than with a brace.
 */
handle_binding_t terminus_binding(std::string_view down = {}) {
    handle_binding_t b;
    b.terminus = down.empty();
    b.down_link = down;
    return b;
}

/**
 * @brief Build a FORWARDING binding whose downstream half crosses @p down.
 *
 * Spelled separately from @ref terminus_binding because #716 turns on the difference: the
 * sweep keys on `down_link`, so a test that reads "terminus_binding(\"left\")" would name the
 * exact property under test as its opposite.
 */
handle_binding_t forward_binding(std::string_view down) { return terminus_binding(down); }

void exercise(route_handle_t& h) {
    // Per-link label spaces are independent and start at 1.
    check(h.alloc_label("a") == 1 && h.alloc_label("a") == 2 && h.alloc_label("b") == 1,
          "labels are per-link monotonic from 1");

    // Ingress: bind, lookup, rebind replaces, unknown label is stale.
    check(
        h.bind_ingress(
            "a", 7,
            handle_binding_t{
                .terminus = true, .down_link = {}, .out_label = 0, .local_route = route_bytes(1)}),
        "an unbounded table accepts every bind");
    auto b = h.lookup_ingress("a", 7);
    check(b && b->terminus && b->local_route == route_bytes(1), "ingress bind + lookup");
    check(
        h.bind_ingress("a", 7,
                       handle_binding_t{
                           .terminus = false, .down_link = "b", .out_label = 9, .local_route = {}}),
        "a rebind is accepted (it adds no entry)");
    b = h.lookup_ingress("a", 7);
    check(b && !b->terminus && b->down_link == "b" && b->out_label == 9,
          "rebinding a label replaces the binding");
    check(!h.lookup_ingress("a", 8) && !h.lookup_ingress("zz", 7),
          "unknown label / unknown link ⇒ stale (nullopt)");

    // Egress: record + retrieve (the NACK re-advertise path).
    check(h.record_egress("b", 3, route_bytes(2)), "an unbounded table accepts every record");
    const auto r = h.egress_route("b", 3);
    check(r && *r == route_bytes(2), "egress route retained for re-advertise");

    // ensure_egress: fresh once per (link, route), then reused; distinct per link.
    const auto [l1, fresh1] = h.ensure_egress("b", route_bytes(4));
    const auto [l2, fresh2] = h.ensure_egress("b", route_bytes(4));
    const auto [l3, fresh3] = h.ensure_egress("c", route_bytes(4));
    check(fresh1 && !fresh2 && l1 == l2, "ensure_egress mints once then reuses");
    check(fresh3 && l3 == 1, "the same route on another link is a separate flow");

    check(h.ingress_count() == 1 && h.egress_count() == 3, "counts see all links");

    // clear_link drops ONE link's state — bindings, egress, and the allocator.
    h.clear_link("a");
    check(!h.lookup_ingress("a", 7), "cleared link's binding is stale");
    check(h.alloc_label("a") == 1, "cleared link's allocator restarts at 1");
    check(h.egress_route("b", 3).has_value(), "other links untouched by clear_link");
    check(h.ingress_count() == 0 && h.egress_count() == 3, "counts after clear");
}

/**
 * @brief #488 — clear_link RECLAIMS a departed link's shell (was insert-only), and the
 *        erase-while-referenced path is use-after-free-free (the shared_ptr pin).
 *
 * Runs on the default (freeing) resource so a reclaimed shell is truly returned. The
 * concurrent section is a TSan gate: writers hammer a small shared name set while a
 * clearer erases the same names; the pin must keep every handed-out table alive.
 */
void churn_and_race() {
    std::printf(" #488 churn + erase-while-referenced:\n");
    route_handle_t h;
    check(h.link_count() == 0, "a fresh route_handle holds no link shells");

    // Reclamation: N distinct link names each mint one shell; clearing each reclaims it,
    // so link_count() returns to its steady state (it was insert-only before #488).
    constexpr int kNames = 64;
    for (int i = 0; i < kNames; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "peer%d", i);
        (void)h.ensure_egress(name, route_bytes(static_cast<std::uint8_t>(i)));
    }
    check(h.link_count() == kNames, "each distinct link name creates exactly one shell");
    for (int i = 0; i < kNames; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "peer%d", i);
        h.clear_link(name);
    }
    check(h.link_count() == 0, "clear_link reclaims the shell — links_ back to steady state");

    // Erase-while-referenced: writers churn ~2000 labels across 6 shared names while a
    // clearer erases those same names. A handed-out shared_ptr must pin its table so an
    // erase never frees a table mid-write (TSan/ASan would flag a UAF or race here).
    constexpr int kThreads = 4;
    constexpr int kIters = 1500;
    std::atomic<bool> go{false};
    auto writer = [&](int seed) {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kIters; ++i) {
            char name[8];
            std::snprintf(name, sizeof name, "L%d", (seed + i) % 6);
            (void)h.ensure_egress(name, route_bytes(static_cast<std::uint8_t>(i)));
            (void)h.bind_ingress(name, static_cast<std::uint16_t>((i & 0x7FFF) + 1),
                                 terminus_binding());
            (void)h.lookup_ingress(name, static_cast<std::uint16_t>((i & 0x7FFF) + 1));
        }
    };
    auto clearer = [&]() {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kIters; ++i) {
            char name[8];
            std::snprintf(name, sizeof name, "L%d", i % 6);
            h.clear_link(name);
        }
    };
    std::vector<std::thread> ts;
    ts.reserve(kThreads + 1);
    for (int t = 0; t < kThreads; ++t) ts.emplace_back(writer, t);
    ts.emplace_back(clearer);
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();
    // No crash / no sanitizer report IS the assertion; the registry stays bounded to the
    // live name set (<= 6), never the ~2000 distinct labels churned through it.
    check(h.link_count() <= 6, "concurrent churn + clear keeps shells bounded to live names");
}

/**
 * @brief #603 defect 3: the label allocator saturates instead of wrapping through the
 *        reserved 0 and back onto labels that still alias live routes.
 *
 * The pre-fix failure is a MISROUTE, not a drop: `next_label` was a bare `uint16_t`
 * incremented unchecked, so allocation 65536 handed out 0 ("none") and 65537 handed out 1
 * — while label 1's egress entry still named the first flow's route. A COMPACT on the
 * reused label then resolved the wrong route, silently.
 *
 * Verified against a build with the two saturation guards deleted: FOUR assertions flip to
 * FAIL there — "stays exhausted" (x2), "a new flow is refused", and "records no egress
 * binding". The rest PASS on the broken build too and are corroborating, not separating:
 *   - "returns the reserved 0" passes because the wrap lands ON 0 exactly once, which is
 *     precisely the bug — the very next call returns 1;
 *   - "label 1 still aliases the first route" passes because `egress_route` returns the
 *     FIRST match and the colliding entry is appended after it. The duplicate is invisible
 *     through that accessor; `egress_count` is what sees it.
 * Read the four, not the eleven.
 */
void label_space_exhaustion() {
    std::printf(" #603 label-space exhaustion (saturate, never wrap):\n");
    route_handle_t h;

    // A real flow holds label 1 for the whole test — this is what a wrapped allocator
    // would collide with.
    const auto [first, first_fresh] = h.ensure_egress("x", route_bytes(1));
    check(first == 1 && first_fresh, "the first compact flow takes label 1");

    // Drain the rest of the 16-bit space. alloc_label does not touch the egress table, so
    // this is O(1) per call rather than a 65k linear rescan.
    std::uint16_t last = 0;
    for (std::uint32_t i = 2; i <= 65535; ++i) last = h.alloc_label("x");
    check(last == 65535, "the allocator issues the whole space, 1..65535, in order");

    // Exhausted, and STICKY. Pre-fix this returned 0 once (the wrap) and then 1, 2, ... —
    // so the second call is the assertion that actually separates the builds.
    check(h.alloc_label("x") == 0, "an exhausted allocator returns the reserved 0");
    check(h.alloc_label("x") == 0, "and stays exhausted rather than walking back to 1");
    check(h.alloc_label("x") == 0, "and stays exhausted indefinitely");

    // The misroute itself: a NEW flow must be refused, never handed label 1, which still
    // aliases the first flow's route.
    const std::size_t egress_before = h.egress_count();
    const auto [second, second_fresh] = h.ensure_egress("x", route_bytes(2));
    check(second == 0 && !second_fresh, "a new flow is refused, not given a live label");
    check(h.egress_count() == egress_before, "a refused flow records no egress binding");
    check(h.egress_route("x", 1) == route_bytes(1), "label 1 still aliases the first route");

    // Exhaustion is per link (the label space is per-link by design, ADR-0038 §3) ...
    check(h.alloc_label("y") == 1, "a different link's space is untouched");
    // ... and clear_link — the (re)connect self-heal — restores it.
    h.clear_link("x");
    check(h.alloc_label("x") == 1, "clear_link restores the exhausted link's space");
}

/**
 * @brief #603 defect 2: the per-link tables honour an INJECTED bound, refusing new bindings
 *        rather than growing without limit — and never refusing an established flow.
 *
 * The bound is a constructor argument, not a constant (CONTEXT.md §Resource bound); `0`
 * stays unbounded, so every existing caller is unchanged. Refusal, not eviction: a label
 * binding exists precisely because a flow is long-running, so evict-oldest would
 * preferentially kill the longest-lived stream and make it re-advertise forever.
 *
 * Every assertion here fails against an unbounded build EXCEPT the two `unbounded` ones,
 * which are the control — they pin that `0` really does mean "no bound" and so must pass
 * both ways.
 */
void bounded_tables() {
    std::printf(" #603 injected per-link binding bound:\n");
    constexpr std::size_t kMax = 4;
    route_handle_t h(std::pmr::get_default_resource(), kMax);

    // Fill the ingress table to the bound.
    for (std::uint16_t i = 1; i <= kMax; ++i)
        check(h.bind_ingress("a", i, terminus_binding()), "a bind below the bound is accepted");
    check(h.ingress_count() == kMax, "the ingress table stops exactly at the bound");
    check(h.refused_bindings() == 0, "nothing refused while there was room");

    // A NEW label is refused ...
    check(!h.bind_ingress("a", 99, terminus_binding()),
          "a new label is refused once the table is full");
    check(h.ingress_count() == kMax, "a refused bind grows the table by nothing");
    check(h.refused_bindings() == 1, "the refusal is counted, not silent");

    // ... but an ESTABLISHED flow is not. This is the whole point of refusing over
    // evicting: a full table degrades new flows and leaves running ones alone.
    check(h.bind_ingress("a", 2, terminus_binding("z")),
          "an already-bound label still rebinds when the table is full");
    const auto b = h.lookup_ingress("a", 2);
    check(b && !b->terminus && b->down_link == "z", "and the rebind actually took effect");
    check(h.refused_bindings() == 1, "a rebind is not a refusal");

    // The bound is per link, not per node.
    check(h.bind_ingress("b", 1, terminus_binding()), "a different link has its own budget");

    // Egress is bounded independently, and reports exhaustion the same way a drained label
    // space does — so the caller's degrade to the full-route form is one path, not two.
    for (std::uint16_t i = 0; i < kMax; ++i)
        check(h.ensure_egress("c", route_bytes(static_cast<std::uint8_t>(i))).second,
              "an egress flow below the bound is minted");
    const auto [label, fresh] = h.ensure_egress("c", route_bytes(200));
    check(label == 0 && !fresh, "a full egress table refuses like an exhausted label space");
    check(h.egress_count() == kMax, "and records nothing");
    // Reuse is checked BEFORE the bound, so an established egress flow still resolves.
    const auto [reused, reused_fresh] = h.ensure_egress("c", route_bytes(0));
    check(reused != 0 && !reused_fresh, "an established egress flow is reused, not refused");

    // clear_link releases the whole budget — the (re)connect self-heal.
    h.clear_link("a");
    check(h.bind_ingress("a", 42, terminus_binding()), "clear_link frees the link's budget");

    // Control: 0 means unbounded. These two must pass on BOTH builds.
    route_handle_t u;
    for (std::uint16_t i = 1; i <= 64; ++i) (void)u.bind_ingress("a", i, terminus_binding());
    check(u.ingress_count() == 64, "unbounded: the default bound of 0 refuses nothing");
    check(u.refused_bindings() == 0, "unbounded: nothing counted");
}

}  // namespace

/**
 * @brief `egress_route`'s copy-out soft-fails instead of aborting (#603 defect 1).
 *
 * This runs on a transport receive thread — `fwd_router_t::on_nack` reaches it from an
 * inbound HANDLE_NACK — so a throwing allocation there is an `abort()` under the shipping
 * `-fno-exceptions` profile. Exhaustion must take the SAME `nullopt` the "no route bound"
 * case already takes, so no caller learns a new shape.
 *
 * The hook gates `tr::detail::probe_bytes` only, never real `operator new`. That is what
 * makes this test discriminating rather than decorative: the previous
 * `std::vector(begin, end)` never consulted the probe, so under this same hook it would
 * allocate successfully and hand back the route — the middle assertion is what fails.
 */
void egress_route_soft_fails_on_exhaustion() {
    std::printf(" #603 egress_route copy-out is nothrow:\n");
    route_handle_t h;
    const std::array<std::byte, 8> route{};
    check(h.record_egress("up", 7, std::vector<std::byte>(route.begin(), route.end())),
          "the egress route binds");
    check(h.egress_route("up", 7).has_value(), "and reads back while memory is available");

    {
        struct hook_guard_t {
            hook_guard_t() {
                tr::detail::probe_fail_hook = [](std::size_t) noexcept { return false; };
            }
            ~hook_guard_t() { tr::detail::probe_fail_hook = nullptr; }
        } const starve;
        check(!h.egress_route("up", 7).has_value(),
              "under exhaustion it answers nullopt — the same answer as an unbound label, "
              "never an abort");
    }

    check(h.egress_route("up", 7).has_value(), "and reads back again once memory returns");
    check(h.egress_route("up", 8) == std::nullopt, "an unbound label still answers nullopt");
}

/**
 * @brief `cache_resolution` on a link that departed between resolve and cache must be a
 *        no-op, not a null dereference.
 *
 * A mutation sweep found `route_handle.cpp:103` — `if (!t) return;` — undefended: it is the
 * only thing between a departed link and `t->m`. The window is real and routine, not
 * exotic: `on_compact` resolves a binding, calls `deliver_local`, and only THEN caches the
 * resolution, while `clear_link` runs on every reconnect (RFC-0004 §E.1 self-heal).
 *
 * The rest of this file already covers the label allocator, the per-link tables, and the
 * `clear_link` teardown itself. This is the one gap the sweep left: the seven other guards
 * in `route_handle.cpp` were caught, and the remaining two are `try_reserve` OOM paths that
 * need an allocation-injection seam (#730) to reach.
 */
void cache_after_teardown() {
    std::printf(" cache_resolution after clear_link (the teardown window):\n");
    route_handle_t h;

    check(h.bind_ingress("gone", 5, terminus_binding()), "a binding exists on the link");
    check(h.resolved("gone", 5).found, "and resolves before teardown");

    h.clear_link("gone");
    check(!h.resolved("gone", 5).found, "clear_link dropped the binding");

    // The window: a resolution computed BEFORE the teardown, cached after it. Without the
    // guard this dereferences the departed link's table.
    resolved_binding_t late;
    late.found = true;
    late.terminus = true;
    late.warm = true;
    h.cache_resolution("gone", 5, late);

    // Asserted positively: the departed link must still be departed. A cache that
    // resurrected the table would be worse than a crash — it would route to a dead link.
    check(!h.resolved("gone", 5).found,
          "caching against a departed link neither crashes NOR resurrects the binding");
    check(h.link_count() == 0, "and no link shell was re-created by the late cache");

    // The positive control: caching still WORKS on a live link, so the guard is not
    // swallowing every call.
    check(h.bind_ingress("live", 9, terminus_binding()), "a live link binds");
    check(!h.resolved("live", 9).warm, "its binding starts cold");
    resolved_binding_t warm;
    warm.found = true;
    warm.terminus = true;
    warm.warm = true;
    h.cache_resolution("live", 9, warm);
    check(h.resolved("live", 9).warm, "and cache_resolution warms a LIVE link's binding");
}

/**
 * @brief #716 — `clear_link(L)` also sweeps every ingress binding, on ANY link, whose
 *        downstream half crossed `L`.
 *
 * The store-under-inbound / point-at-outbound asymmetry ADR-0062's erratum named: a
 * forwarding binding lives under the link it ARRIVES on while `down_link` names the link it
 * LEAVES by, so clearing `L`'s own tables left a binding elsewhere aimed at an out-label that
 * died with them. Whether that mattered is a routing question the unit cannot see; what the
 * unit CAN pin is the state, which is the whole input to the routing decision.
 *
 * The three-way discrimination is the point — a sweep that took too much would be as wrong as
 * one that took too little: the crossing binding goes, the sibling forwarding binding through
 * another link stays, and a TERMINUS binding (no downstream half at all) stays.
 */
void cross_link_sweep() {
    std::printf(
        " clear_link sweeps CROSS-LINK bindings whose downstream half crossed it (#716):\n");
    route_handle_t h;

    check(h.bind_ingress("up", 1, forward_binding("left")), "a binding on \"up\" forwards to left");
    check(h.bind_ingress("up", 2, forward_binding("right")), "another forwards to right");
    check(h.bind_ingress("up", 3, terminus_binding()), "and a third terminates locally");
    check(h.bind_ingress("left", 4, forward_binding("right")),
          "a binding on the doomed link itself forwards elsewhere");
    check(h.record_egress("left", 8, route_bytes(3)), "left holds an egress route too");
    check(h.ingress_count() == 4, "four ingress bindings before the clear");

    h.clear_link("left");

    check(!h.lookup_ingress("up", 1),
          "the CROSS-LINK binding pointing at \"left\" is swept — it is as stale as the erased "
          "table it aimed into");
    check(h.lookup_ingress("up", 2).has_value(),
          "the sibling binding through \"right\" is untouched (the sweep is scoped)");
    check(h.lookup_ingress("up", 3).has_value(),
          "the TERMINUS binding is untouched — it has no downstream half to cross anything");
    check(!h.lookup_ingress("left", 4), "the cleared link's own bindings go with its table");
    check(!h.egress_route("left", 8).has_value(), "as does its egress table");
    check(h.ingress_count() == 2, "exactly two bindings survive");

    // The sweep must not resurrect or invent a link shell for a name it merely scanned.
    check(h.link_count() == 1, "only the surviving link's shell remains");
}

/**
 * @brief Model `fwd_router_t::on_advertise`'s FORWARDING leg, with @p mid run in the window
 *        between taking the out-label and binding the ingress swap.
 *
 * The calls and their order are the router's, not the test's: sample the downstream epoch,
 * take this hop's out-label on the downstream link (one per (link, route) since #913, which
 * retains the stripped egress route in the same critical section), bind the inbound label to
 * that swap, and — since #833 — hand the take back when the bind refuses. @p mid is where a
 * reconnect on the DOWNSTREAM link lands: the #827 window. A no-op models the uncontended path.
 *
 * It used to spell the take as `alloc_label` + `record_egress`; #913 replaced that pair in the
 * router, and this model followed it so the window under test stays the shipped one.
 *
 * @return `{out_label, bound}` — the label taken downstream, and whether the swap was bound.
 */
struct advertise_leg_t {
    std::uint16_t out_label = 0; /**< @brief The downstream label this hop took (0 ⇒ none). */
    bool bound = false;          /**< @brief Whether the ingress swap was actually recorded. */
};

advertise_leg_t advertise_forwarding_leg(route_handle_t& h, std::string_view in_link,
                                         std::uint16_t in_label, std::string_view down,
                                         const std::vector<std::byte>& route,
                                         const std::function<void()>& mid) {
    const std::uint32_t epoch = h.link_epoch(down);
    const std::uint16_t out = h.ensure_egress(down, route).first;
    if (out == 0) return {};
    mid();  // a (re)connect on `down` lands HERE — after the take, before the bind
    handle_binding_t fwd = forward_binding(down);
    fwd.out_label = out;
    if (h.bind_ingress_forward(in_link, in_label, std::move(fwd), epoch)) return {out, true};
    h.release_egress(down, out, route);
    return {out, false};
}

/**
 * @brief #827 — a reconnect landing INSIDE an in-flight advertise must not leave a stale swap.
 *
 * #716's sweep closed the steady-state gap, but it can only erase bindings that already
 * exist. An `on_advertise` running on another link's rx thread mints its out-label and
 * retains its egress route against the PRE-clear downstream table, and then binds the swap —
 * and if `clear_link` runs between those two steps, the sweep scans the inbound link before
 * the binding is there and the bind lands after the table it aims into is gone. The result is
 * byte-for-byte the #716 state the sweep exists to prevent: an ingress swap naming an
 * out-label whose egress route no longer exists, so the downstream's NACKs die in an empty
 * lookup and the upstream is never told. A microsecond window, but a permanent outcome.
 *
 * Run against a build with the epoch check deleted, the first two assertions FAIL: the swap
 * binds and the stale binding is readable. The third PASSES either way — the egress route is
 * gone in both builds, which is precisely why it cannot be the separating assertion.
 */
void reconnect_inside_advertise() {
    std::printf(" a reconnect INSIDE an in-flight advertise binds no stale swap (#827):\n");
    const auto nop = []() {};

    {
        route_handle_t h;
        const advertise_leg_t r = advertise_forwarding_leg(h, "up", 5, "down", route_bytes(1), nop);
        check(r.bound && h.lookup_ingress("up", 5).has_value(),
              "control: with no reconnect in the window the swap binds normally");
    }
    {
        // The window. `clear_link("down")` runs after the egress route is retained and before
        // the swap is bound — exactly the interleaving the issue describes.
        route_handle_t h;
        const advertise_leg_t r = advertise_forwarding_leg(h, "up", 5, "down", route_bytes(1),
                                                           [&h]() { h.clear_link("down"); });
        check(!r.bound, "the swap minted against the PRE-clear table is refused");
        check(!h.lookup_ingress("up", 5),
              "no ingress swap survives aiming at the erased egress table");
        check(!h.egress_route("down", r.out_label).has_value(),
              "corroborating: the egress route it aimed at is indeed gone (true either way)");
    }
    {
        // Scope: the guard keys on the DOWNSTREAM link. A reconnect of some unrelated link in
        // the same window must not refuse the swap — a guard that refuses everything is as
        // wrong as one that refuses nothing. This holds once the downstream tables EXIST
        // (they do here: the leg's alloc_label runs before the hook); in the narrower
        // first-contact sub-window (no tables at epoch-sample time) an unrelated clear DOES
        // refuse spuriously — the documented, safe-direction liveness nit in link_epoch's doc.
        route_handle_t h;
        (void)h.ensure_egress("other", route_bytes(7));
        const advertise_leg_t r = advertise_forwarding_leg(h, "up", 6, "down", route_bytes(2),
                                                           [&h]() { h.clear_link("other"); });
        check(r.bound && h.lookup_ingress("up", 6).has_value(),
              "an unrelated link's reconnect in the same window does NOT refuse the swap");
    }
    {
        // And the inbound link's own reconnect: the swap may or may not be retained (it may
        // land in a detached table), but it must never be readable AND stale.
        route_handle_t h;
        (void)advertise_forwarding_leg(h, "up", 7, "down", route_bytes(3),
                                       [&h]() { h.clear_link("up"); });
        const auto b = h.lookup_ingress("up", 7);
        check(!b || h.egress_route("down", b->out_label).has_value(),
              "an inbound reconnect leaves no readable swap without its egress route");
    }
}

/**
 * @brief The same window, driven CONCURRENTLY — the shape the race actually has in the field.
 *
 * One thread runs the forwarding leg with the window held open (a yield spin, the harness's
 * only lever — nothing in the shipped path knows this test exists); the other reconnects the
 * downstream link. The invariant is the #716 state itself: this node must never end a round
 * holding a readable forwarding swap whose out-label has no egress route behind it.
 *
 * Also the TSan gate for the new guard — the epoch read and the bind must be one critical
 * section against `clear_link`'s bump-erase-sweep, or the sweep and the bind interleave and
 * the guard is decorative.
 */
void reconnect_race_invariant() {
    std::printf(" the same window under two threads (#827):\n");
    constexpr int kRounds = 400;
    int violations = 0;
    for (int i = 0; i < kRounds; ++i) {
        route_handle_t h;
        (void)h.ensure_egress("down", route_bytes(9));  // the pre-clear table already exists
        std::atomic<bool> go{false};
        advertise_leg_t r;
        std::thread adv([&]() {
            while (!go.load(std::memory_order_acquire)) {
            }
            r = advertise_forwarding_leg(h, "up", 5, "down", route_bytes(1), []() {
                for (int k = 0; k < 64; ++k) std::this_thread::yield();
            });
        });
        std::thread rec([&]() {
            while (!go.load(std::memory_order_acquire)) {
            }
            h.clear_link("down");
        });
        go.store(true, std::memory_order_release);
        adv.join();
        rec.join();
        const auto b = h.lookup_ingress("up", 5);
        if (b && !b->terminus && b->down_link == "down" && !h.egress_route("down", b->out_label))
            ++violations;
    }
    check(violations == 0, "no round left a readable swap pointing at a dead egress table");
    if (violations != 0) std::printf("       (%d of %d rounds)\n", violations, kRounds);
}

/**
 * @brief #833 — a REFUSED forwarding bind leaves no label and no route behind, and takes
 *        nothing away from the flows that share the entry.
 *
 * The order is forced: the swap binding names the out-label, so the label must be taken
 * before the bind can be attempted. When the bind then refuses — a full ingress table here,
 * the #827 epoch guard in the field — the hop returns WITHOUT advertising, so what it took
 * aliases a route no binding aims at and no peer has ever seen. It stayed in the live
 * downstream table until that link's next `clear_link`.
 *
 * The bound is 2 per table, and the two takes ahead of the refusal name the SAME route: that
 * is what fills the INGRESS table (one binding per inbound label) while leaving the EGRESS
 * table at one entry (#913 dedups by route), which is the only arrangement in which the bind
 * is the step that refuses. With distinct routes the egress table fills in step and
 * `ensure_egress` refuses first — a path that mints nothing and was never the leak.
 */
void refused_bind_unwinds_the_take() {
    std::printf(" #833 a refused forwarding bind strands neither label nor route:\n");
    const auto nop = []() {};

    {
        route_handle_t h(std::pmr::get_default_resource(), 2);
        // Two established flows through the same stripped route: ingress "up" is now AT the
        // bound with a single egress entry behind both.
        const advertise_leg_t a =
            advertise_forwarding_leg(h, "up", 10, "down", route_bytes(1), nop);
        const advertise_leg_t b =
            advertise_forwarding_leg(h, "up", 11, "down", route_bytes(1), nop);
        check(a.bound && b.bound && a.out_label == b.out_label,
              "setup: two inbound labels share ONE downstream label (#913)");
        check(h.ingress_count() == 2 && h.egress_count() == 1, "setup: ingress full, egress at 1");

        // The refusal. A NEW route takes a fresh downstream label, then the bind refuses
        // because "up" is at its bound.
        const advertise_leg_t r =
            advertise_forwarding_leg(h, "up", 12, "down", route_bytes(2), nop);
        std::printf("    census: taken label %u | egress %zu | ingress %zu\n", r.out_label,
                    h.egress_count(), h.ingress_count());
        check(!r.bound && r.out_label != 0, "the bind is refused after the label was taken");
        check(h.egress_count() == 1, "the refused take left NO egress entry behind");
        check(!h.egress_route("down", r.out_label).has_value(),
              "and no retained route under the label it took");
        check(h.ingress_count() == 2, "and no ingress binding, which is the refusal itself");

        // The label SPACE came back too, not just the table slot: the next new flow on this
        // link — driven from a second inbound link, whose own ingress table is empty — is
        // handed the very label the refusal gave up.
        const advertise_leg_t n =
            advertise_forwarding_leg(h, "up2", 1, "down", route_bytes(3), nop);
        check(n.bound && n.out_label == r.out_label,
              "the next new flow is handed the label the refusal gave back");
    }
    {
        // The established-flow half of the gate: a flow that SHARES an entry must not be
        // unwound by a newcomer's refusal. "up2" reuses "down"'s existing label for the same
        // route and is refused (its own ingress table is full through another downstream
        // link), so the release lands on an entry that is not its own mint.
        route_handle_t h(std::pmr::get_default_resource(), 1);
        const advertise_leg_t est =
            advertise_forwarding_leg(h, "up", 10, "down", route_bytes(1), nop);
        const advertise_leg_t fill =
            advertise_forwarding_leg(h, "up2", 20, "down2", route_bytes(9), nop);
        check(est.bound && fill.bound, "setup: one established flow, and \"up2\" at its bound");

        const advertise_leg_t r =
            advertise_forwarding_leg(h, "up2", 21, "down", route_bytes(1), nop);
        check(!r.bound && r.out_label == est.out_label,
              "the newcomer REUSED the established label and was then refused");
        check(h.egress_route("down", est.out_label) == route_bytes(1),
              "the established flow keeps its label and its retained route");
        check(h.lookup_ingress("up", 10).has_value(), "and keeps its ingress binding");
        check(h.egress_count() == 2, "no entry was erased on either link");
    }
    {
        // A release must not RESURRECT a link shell. The reconnect case is the common
        // companion of a refusal, and it has already erased the whole downstream table.
        route_handle_t h;
        const advertise_leg_t r = advertise_forwarding_leg(h, "up", 5, "down", route_bytes(1),
                                                           [&h]() { h.clear_link("down"); });
        check(!r.bound, "the epoch guard refuses the swap (#827)");
        check(h.link_count() == 1, "the release created no empty shell for the cleared link");
    }
}

/**
 * @brief `copy_local_route` — the allocation-free route read the warm COMPACT arm uses (#917).
 *
 * Four outcomes, and the reason all four are asserted is that the accessor reports them
 * through ONE `std::size_t`: the caller distinguishes "gone" from "too long for my buffer"
 * only by comparing the return against its own capacity. A test that checked the fitting case
 * alone would let the too-long case silently degrade into "no observation at all", which is
 * indistinguishable from a delivery that never happened.
 */
void copy_local_route_outcomes() {
    std::printf(" copy_local_route (the warm arm's allocation-free route read):\n");
    route_handle_t h;

    const std::vector<std::byte> want = route_bytes(7);
    handle_binding_t term;
    term.terminus = true;
    term.local_route = want;
    check(h.bind_ingress("in", 3, std::move(term)), "a terminus binding with a route exists");
    check(h.bind_ingress("in", 4, forward_binding("down")),
          "and a forwarding swap, which has none");

    // 1) It fits: the full route is written and its size returned.
    std::array<std::byte, 16> buf{};
    const std::size_t n = h.copy_local_route("in", 3, buf);
    check(n == want.size(), "a fitting route reports its own size");
    check(std::equal(want.begin(), want.end(), buf.begin()),
          "and the bytes land byte-exact in the caller's buffer");

    // 2) It does NOT fit: the size is still reported, and nothing is written. The untouched
    //    buffer is the load-bearing half — the caller falls back to the owning form, so a
    //    partial copy here would be a TRUNCATED route reported as a whole one.
    std::array<std::byte, 2> tiny{};
    const std::size_t big = h.copy_local_route("in", 3, tiny);
    check(big == want.size(), "a route too long for the buffer still reports its size");
    check(tiny[0] == std::byte{0} && tiny[1] == std::byte{0},
          "and writes NOTHING — a short buffer is never partially filled");

    // 3) A forwarding swap has no local route, and 4) an absent binding/link is the same 0.
    check(h.copy_local_route("in", 4, buf) == 0, "a forwarding swap reports 0 (no local route)");
    check(h.copy_local_route("in", 99, buf) == 0, "an unbound label reports 0");
    check(h.copy_local_route("nope", 3, buf) == 0, "an unknown link reports 0, not a crash");

    // The teardown window `cache_resolution` already guards, on this door too.
    h.clear_link("in");
    check(h.copy_local_route("in", 3, buf) == 0, "a departed link reports 0");
}

int main() {
    std::printf("route_handle_t (Brick 4 — per-connection pmr label tables):\n");

    {
        std::printf(" default resource:\n");
        route_handle_t h;
        exercise(h);
    }
    {
        // The ADR-0039 claim: the whole label lifecycle draws from the host slab —
        // a null upstream would throw on ANY global-heap fallback for table state.
        std::printf(" slab resource (null upstream — zero global heap):\n");
        alignas(std::max_align_t) static std::array<std::byte, 16384> slab;
        std::pmr::monotonic_buffer_resource mr(slab.data(), slab.size(),
                                               std::pmr::null_memory_resource());
        route_handle_t h(&mr);
        exercise(h);
    }

    churn_and_race();
    label_space_exhaustion();
    bounded_tables();
    egress_route_soft_fails_on_exhaustion();

    cache_after_teardown();
    cross_link_sweep();
    reconnect_inside_advertise();
    reconnect_race_invariant();
    refused_bind_unwinds_the_take();
    copy_local_route_outcomes();

    return tr::testing::summary("route_handle");
}
