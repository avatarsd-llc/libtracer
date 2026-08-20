/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — `expires_ns` is evaluated against the CALLER's `now`, at check time.
 *
 * An ACE may carry an absolute deadline: `expires_ns`, nanoseconds since the UNIX epoch, with
 * `0` meaning "never". It is not a timer and nothing sweeps it — the policy compares it to the
 * `now` its caller passed in, on every single check. That is why the graph caches the merged
 * effective-ACE list but never a verdict: a merge stays valid as the clock moves, so expiry
 * needs no invalidation mechanism at all (ADR-0050).
 *
 * The consequence to hold on to is the one that catches people out: an expired ACE grants
 * nothing AND still closes the vertex. Presence is what closes (see acl_open_by_default), and
 * an expired entry is present. A temporary grant that lapses therefore does not restore the
 * open state it replaced — it leaves the vertex shut to everyone.
 *
 * This runs against the PURE surface — `effective_acl_t` takes `now` as a parameter, so the
 * example can step the clock without sleeping — and then confirms the same rule at the real
 * `graph_t` door, which reads its own clock.
 *
 * Runs under ctest as `example_acl_expiry`; returns non-zero on any failed check.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/security_acl.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::acl_right_t;
using tr::graph::effective_acl_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief @p s as opaque subject-token bytes. */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The caller context IS the subject token. */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
}

/** @brief @p right as the single `access_mask` bit it is. */
constexpr std::uint32_t bit(acl_right_t right) { return static_cast<std::uint32_t>(right); }

/** @brief A one-byte VALUE. */
tr::view::view_t some_value() {
    const std::byte one[1] = {std::byte{0x01}};
    return *tr::view::over_bytes(one);
}

/** @brief True iff @p r was refused by an ACL gate. */
template <class T>
bool denied(const tr::graph::result_t<T>& r) {
    return !r.has_value() && r.error() == status_t::PERMISSION_DENIED;
}

/** @brief A visitor's badge, valid until @ref kDeadline. */
constexpr std::uint64_t kDeadline = 1'800'000'000'000'000'000ULL;  // ~2027-01-15, in ns

}  // namespace

int main() {
    bool ok = true;

    // The pure surface: build one effective-ACE list ONCE, then ask it about several `now`s.
    // Nothing below rebuilds or invalidates it — that is the property being shown.
    const ace_t issued[] = {{.subject = as_bytes("visitor"),
                             .access_mask = bit(acl_right_t::READ),
                             .expires_ns = kDeadline}};
    effective_acl_t merged;
    merged.append_own(issued);
    const std::span<const std::byte> visitor_bytes = issued[0].subject;
    const std::vector<std::byte> other = as_bytes("resident");

    check(ok, merged.allows(visitor_bytes, bit(acl_right_t::READ), kDeadline - 1),
          "one nanosecond before the deadline, the badge works");
    check(ok, !merged.allows(visitor_bytes, bit(acl_right_t::READ), kDeadline),
          "AT the deadline it does not — expiry is `expires_ns <= now`, not `<`");
    check(ok, !merged.allows(visitor_bytes, bit(acl_right_t::READ), kDeadline + 1),
          "and after it, of course, it does not");
    check(ok, merged.allows(visitor_bytes, bit(acl_right_t::READ), kDeadline - 1),
          "the SAME list answers yes again for an earlier `now`: no state changed, nothing "
          "was invalidated, and nothing had to be");

    // The expired entry is still an entry, so the vertex it guards stays shut.
    check(ok, !merged.allows(other, bit(acl_right_t::READ), kDeadline - 1),
          "the resident was never granted anything, and a present ACE closes the vertex");
    check(ok, !merged.allows(other, bit(acl_right_t::READ), kDeadline + 1),
          "…and the badge lapsing does not reopen it — this is the trap worth remembering");

    // `0` is not a deadline in the distant past; it means the ACE never expires.
    const ace_t permanent[] = {
        {.subject = as_bytes("resident"), .access_mask = bit(acl_right_t::READ)}};
    effective_acl_t forever;
    forever.append_own(permanent);
    check(ok, forever.allows(other, bit(acl_right_t::READ), kDeadline + 1),
          "expires_ns == 0 means never expires, not expired at the epoch");

    // The same predicate at the real door, where `now` comes from the graph's own clock.
    graph_t g;
    g.configure_subject_resolver(caller_is_subject, nullptr);
    const vertex_handle_t v = g.register_vertex(path_t("/room"), role_t::STORED_VALUE);
    (void)g.write(v, some_value());
    const ace_t both[] = {
        {.subject = as_bytes("live"),
         .access_mask = bit(acl_right_t::READ),
         .expires_ns = ~0ULL >> 1},
        {.subject = as_bytes("stale"), .access_mask = bit(acl_right_t::READ), .expires_ns = 1},
    };
    (void)g.write(path_t("/room:acl"), *tr::view::over_bytes(tr::graph::encode_acl(both)));
    check(ok, g.read(v, "live").has_value(), "a grant expiring in the year 2262 still reads");
    check(ok, denied(g.read(v, "stale")), "a grant that expired 1 ns after the epoch does not");

    std::printf("one ACE list, four verdicts, zero invalidations — deadline %llu ns\n",
                static_cast<unsigned long long>(kDeadline));
    return ok ? 0 : 1;
}
