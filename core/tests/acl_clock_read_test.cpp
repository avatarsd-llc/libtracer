/**
 * @file
 * @brief #2308 — the ACL gate reads no clock: ACE expiry compares against the `now` the
 *        application's subject-lookup hook supplies (RFC-0037).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `graph_t::acl_allows` used to read `system_clock::now()` for a leased ACE (#1665 and #1685
 * had already narrowed it to that case). The reference clock is now the application's: the
 * hook writes its `now` beside the subject token, and a leased ACE grants while
 * `now < expires_ns`. A hook that supplies no `now` leaves `kAceNowUnknown`, and a leased
 * grant then grants nothing (fail closed); a leased DENY still refuses. Standing grants never
 * consult `now`.
 *
 * The fake clock is the hook's context, so the test steps time without sleeping. The library's
 * clock reads are counted by interposing libstdc++'s out-of-line `system_clock::now()` in this
 * executable; the count must stay zero. Where that symbol cannot be interposed (another
 * standard library), the self-check in `main` skips that half rather than pass vacuously.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <expected>
#include <string_view>
#include <vector>

#include "graph_sinks.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

/** @brief `system_clock::now()` calls observed since process start. */
std::atomic<unsigned> g_clock_reads{0};

}  // namespace

#if defined(__GLIBCXX__) && defined(__linux__)
namespace std::chrono::_V2 {
/** @brief Counting interposer for libstdc++'s `system_clock::now()` — same result, plus a tally. */
system_clock::time_point system_clock::now() noexcept {
    g_clock_reads.fetch_add(1, std::memory_order_relaxed);
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return time_point(std::chrono::duration_cast<duration>(std::chrono::seconds(ts.tv_sec) +
                                                           std::chrono::nanoseconds(ts.tv_nsec)));
}
}  // namespace std::chrono::_V2
#endif

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;
using tr::testing::make_value;

/** @brief Copy @p s into an owned byte vector. */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The application's clock, as the hook's context: a value the test steps. */
struct app_clock_t {
    bool supplies = true;  /**< @brief False: the hook leaves `now` unset, as a clockless app. */
    std::uint64_t now = 0; /**< @brief The `now` the hook hands the gate, in the app's ns. */
};

/** @brief The test hook: the caller context IS the subject token, and the app's `now` rides
 *         beside it when the app has one. */
std::expected<void, tr::wire::err_t> caller_is_subject(void* ctx, std::string_view caller,
                                                       tr::mem::bytes_t& out,
                                                       std::uint64_t& now_ns) {
    const auto& clock = *static_cast<const app_clock_t*>(ctx);
    if (clock.supplies) now_ns = clock.now;
    return tr::testing::subject_into(out, caller);
}

/**
 * @brief An `:acl` value of one ALLOW ACE granting @p subject READ and WRITE.
 * @param expires_ns The ACE's lease, absolute ns since the UNIX epoch; `0` is a standing grant.
 */
std::vector<std::byte> allow_all(std::string_view subject, std::uint64_t expires_ns = 0) {
    const auto mask = static_cast<std::uint32_t>(tr::graph::acl_right_t::READ) |
                      static_cast<std::uint32_t>(tr::graph::acl_right_t::WRITE);
    const std::vector<tr::graph::ace_t> aces{tr::graph::ace_t{
        .subject = tr::graph::subject_of(subject), .access_mask = mask, .expires_ns = expires_ns}};
    return tr::graph::encode_acl(aces);
}

/** @brief An ACE list of @p type for @p subject, every right, at lease @p expires_ns. */
tr::graph::ace_t ace(tr::graph::ace_type_t type, std::string_view subject,
                     std::uint64_t expires_ns) {
    return tr::graph::ace_t{
        .type = type,
        .subject = tr::graph::subject_of(subject),
        .access_mask = static_cast<std::uint32_t>(tr::graph::acl_right_t::READ) |
                       static_cast<std::uint32_t>(tr::graph::acl_right_t::WRITE),
        .expires_ns = expires_ns};
}

/** @brief The no-`now` rule on the pure surface, under the full policy where DENY exists. */
void test_no_now_fails_closed_both_ways() {
    std::printf("no now supplied: a lease is proved neither live nor lapsed (RFC-0037):\n");
    using tr::graph::ace_type_t;
    using tr::graph::acl_verdict_t;
    using tr::graph::full_acl_policy_t;
    using tr::graph::kAceNowUnknown;
    const auto bit = static_cast<std::uint32_t>(tr::graph::acl_right_t::READ);
    const std::vector<std::byte> a = as_bytes("peer-a");
    const tr::graph::ace_t allow_leased[] = {ace(ace_type_t::ALLOW, "peer-a", 1000)};
    check(full_acl_policy_t::allows(a, bit, allow_leased, 999) == acl_verdict_t::ALLOW,
          "a leased ALLOW grants while now < expires_ns");
    check(
        full_acl_policy_t::allows(a, bit, allow_leased, kAceNowUnknown) == acl_verdict_t::NO_MATCH,
        "with no now it does not grant");
    // A leased DENY in front of a standing ALLOW: skipping the DENY for want of a clock would
    // grant what a live clock refuses, so the DENY still applies.
    const tr::graph::ace_t deny_first[] = {ace(ace_type_t::DENY, "peer-a", 1000),
                                           ace(ace_type_t::ALLOW, "peer-a", 0)};
    check(full_acl_policy_t::allows(a, bit, deny_first, 999) == acl_verdict_t::DENY,
          "a live leased DENY refuses");
    check(full_acl_policy_t::allows(a, bit, deny_first, 1000) == acl_verdict_t::ALLOW,
          "a lapsed leased DENY steps aside for the standing ALLOW");
    check(full_acl_policy_t::allows(a, bit, deny_first, kAceNowUnknown) == acl_verdict_t::DENY,
          "with no now the leased DENY still refuses: fail closed");
    const tr::graph::ace_t standing[] = {ace(ace_type_t::ALLOW, "peer-a", 0)};
    check(full_acl_policy_t::allows(a, bit, standing, kAceNowUnknown) == acl_verdict_t::ALLOW,
          "a standing grant does not care whether now was supplied");
}

}  // namespace

int main() {
    // Self-check: the interposer is the symbol this executable resolves `system_clock::now()`
    // to. The library is linked into it statically, so a library read would land here too.
    const unsigned before_self = g_clock_reads.load();
    (void)std::chrono::system_clock::now();
    const bool counting = g_clock_reads.load() != before_self;
    const unsigned base = g_clock_reads.load();

    test_no_now_fails_closed_both_ways();

    app_clock_t clock;
    graph_t g;
    g.set_hook(&tr::graph::graph_hooks_t::subject_lookup, {caller_is_subject, &clock});
    const vertex_handle_t open = g.register_vertex(path_t("/open"), role_t::STORED_VALUE);
    const vertex_handle_t standing = g.register_vertex(path_t("/standing"), role_t::STORED_VALUE);
    const vertex_handle_t leased = g.register_vertex(path_t("/leased"), role_t::STORED_VALUE);
    check(g.write(path_t("/standing:acl"), make_value(allow_all("peer-a"))).has_value(),
          "trusted local caller installs the standing :acl");
    constexpr std::uint64_t kLease = 5'000;
    check(g.write(path_t("/leased:acl"), make_value(allow_all("peer-a", kLease))).has_value(),
          "trusted local caller installs the leased :acl");

    std::printf("a lease follows the app's now through the real hook (#2308):\n");
    clock.now = kLease - 1;
    check(g.write(leased, make_value(as_bytes("v")), "peer-a").has_value(),
          "app now < expires_ns => WRITE allowed");
    check(g.read(leased, "peer-a").has_value(), "and READ allowed");
    clock.now = kLease;
    check(!g.write(leased, make_value(as_bytes("v")), "peer-a").has_value(),
          "app now == expires_ns => WRITE refused (expiry is <=)");
    clock.now = kLease + 1;
    check(!g.read(leased, "peer-a").has_value(), "app now past expires_ns => READ refused");
    clock.now = kLease - 1;
    check(g.read(leased, "peer-a").has_value(),
          "the app's clock is the reference: an earlier now grants again, nothing was cached");

    std::printf("a hook that supplies no now fails a lease closed (RFC-0037):\n");
    clock.supplies = false;
    check(!g.write(leased, make_value(as_bytes("v")), "peer-a").has_value(),
          "no now => the leased grant does not grant");
    check(g.write(standing, make_value(as_bytes("v")), "peer-a").has_value(),
          "no now => a standing grant still grants");
    check(!g.write(standing, make_value(as_bytes("v")), "peer-b").has_value(),
          "and still refuses a subject it does not name");
    check(g.write(open, make_value(as_bytes("v")), "peer-a").has_value(),
          "no ACL anywhere => open by default, now or not");

    if (counting)
        check(g_clock_reads.load() == base, "the library read no system_clock across every gate");
    else
        std::printf("  [SKIP] system_clock::now() is not interposable here\n");
    return tr::testing::summary("acl_clock_read");
}
