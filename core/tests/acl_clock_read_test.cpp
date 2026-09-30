/**
 * @file
 * @brief #1665 — an attributed op reads the ACE-expiry clock only when an ACL is evaluated.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `graph_t::acl_allows` used to read `system_clock::now()` on every op that carried a caller,
 * before its bearing-ancestor walk found out whether any ACL applied at all. The open-by-default
 * arm (resolver installed, no ACE anywhere up the chain) now returns without a clock read; a
 * guarded vertex still reads it, where an ACE's `expires_ns` may need it.
 *
 * The count comes from interposing libstdc++'s out-of-line `system_clock::now()` in this test
 * executable — the one clock the library reads (`now_ns()` in graph.cpp). Where that symbol is
 * not the one the library calls (another standard library), the self-check in `main` proves the
 * interposer is not reached and the test skips rather than passing vacuously.
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
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;
using tr::testing::make_value;

/** @brief Copy @p s into an owned byte vector. */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The test resolver: the caller context IS the subject token. */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
}

/** @brief An `:acl` value of one ALLOW ACE granting @p subject READ and WRITE. */
std::vector<std::byte> allow_all(std::string_view subject) {
    const auto mask = static_cast<std::uint32_t>(tr::graph::acl_right_t::READ) |
                      static_cast<std::uint32_t>(tr::graph::acl_right_t::WRITE);
    const std::vector<tr::graph::ace_t> aces{
        tr::graph::ace_t{.subject = as_bytes(subject), .access_mask = mask}};
    return tr::graph::encode_acl(aces);
}

}  // namespace

int main() {
    graph_t g;
    {
        auto hooks = g.hooks();
        hooks.subject_resolver = {caller_is_subject, nullptr};
        g.set_hooks(hooks);
    }
    const vertex_handle_t open = g.register_vertex(path_t("/open"), role_t::STORED_VALUE);
    const vertex_handle_t guarded = g.register_vertex(path_t("/guarded"), role_t::STORED_VALUE);
    check(g.write(path_t("/guarded:acl"), make_value(allow_all("peer-a"))).has_value(),
          "trusted local caller installs the :acl");

    // Self-check: a guarded op must reach the interposer, or this platform cannot observe it.
    const unsigned before_guarded = g_clock_reads.load();
    check(g.write(guarded, make_value(as_bytes("v")), "peer-a").has_value(),
          "guarded WRITE by a granted caller is allowed");
    if (g_clock_reads.load() == before_guarded) {
        std::printf("SKIP: system_clock::now() is not interposable here\n");
        return 77;
    }

    std::printf("attributed ops on an unguarded subtree read no clock (#1665):\n");
    const unsigned before_open = g_clock_reads.load();
    check(g.write(open, make_value(as_bytes("v")), "peer-a").has_value(),
          "resolver + no ACL => WRITE allowed");
    check(g.read(open, "peer-a").has_value(), "resolver + no ACL => READ allowed");
    check(g_clock_reads.load() == before_open,
          "no system_clock::now() on the open-by-default arm of acl_allows");
    return tr::testing::summary("acl_clock_read");
}
