/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * effective_acl_t unit test (ADR-0050, the previously-untested half) — the pure
 * effective-ACL merge semantics driven directly with synthetic ACE lists (no
 * graph, no locks, no wall clock): own-before-ancestors ordering, nearest-first
 * ancestor order, INHERIT gating at merge time, any-present-ACE-closes (even
 * expired), open-by-default over an empty merge, and DENY first-match-per-bit
 * ordering under the full policy. Plus the graph-side cached-merge behavior the
 * pure tests cannot see: subtree-precise invalidation on :acl writes (ADR-0057
 * child links), placeholder skip semantics, and a TSan-facing dirty-flag race
 * (:acl rewrites vs concurrent gated ops — the recheck-after-publish protocol).
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::ace_type_t;
using tr::graph::acl_right_t;
using tr::graph::allow_only_policy_t;
using tr::graph::effective_acl_t;
using tr::graph::full_acl_policy_t;
using tr::graph::graph_t;
using tr::graph::kAceInherit;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;

int g_failures = 0;

/** @brief Print and count one PASS/FAIL check line. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief Copy a string's bytes into a std::byte vector (subject tokens). */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The requested right as its mask bit. */
constexpr std::uint32_t bit(acl_right_t r) { return static_cast<std::uint32_t>(r); }

/** @brief Build one synthetic ACE. */
ace_t ace(std::string_view subject, std::uint32_t mask, ace_type_t type = ace_type_t::ALLOW,
          std::uint8_t flags = 0, std::uint64_t expires_ns = 0) {
    return ace_t{.type = type,
                 .flags = flags,
                 .subject = as_bytes(subject),
                 .access_mask = mask,
                 .expires_ns = expires_ns};
}

/** @brief A check-time reference clock for the pure tests. */
constexpr std::uint64_t kNow = 1'000;

/** @brief Pure semantics: open-by-default vs any-present-ACE-closes (even expired). */
void test_open_default_and_closing() {
    std::printf("effective_acl_t — open by default / any present ACE closes:\n");
    const std::vector<std::byte> alice = as_bytes("alice");

    {
        const effective_acl_t eff;  // nothing appended
        check(eff.allows<allow_only_policy_t>(alice, bit(acl_right_t::READ), kNow),
              "empty merge => open (allowed) under allow_only");
        check(eff.allows<full_acl_policy_t>(alice, bit(acl_right_t::WRITE), kNow),
              "empty merge => open (allowed) under full");
    }
    {  // a non-matching ACE closes: present but granting someone else
        effective_acl_t eff;
        const std::vector<ace_t> own{ace("bob", bit(acl_right_t::READ))};
        eff.append_own(own);
        check(!eff.allows<allow_only_policy_t>(alice, bit(acl_right_t::READ), kNow),
              "one non-matching own ACE closes the vertex");
    }
    {  // an EXPIRED ACE still closes — presence, not applicability, is the switch
        effective_acl_t eff;
        const std::vector<ace_t> own{
            ace("alice", bit(acl_right_t::READ), ace_type_t::ALLOW, 0, /*expires=*/kNow - 1)};
        eff.append_own(own);
        check(!eff.allows<allow_only_policy_t>(alice, bit(acl_right_t::READ), kNow),
              "an expired ACE grants nothing AND still closes (denied)");
        check(eff.allows<allow_only_policy_t>(alice, bit(acl_right_t::READ), kNow - 2),
              "the same ACE grants before its expiry (merge caches no verdict)");
    }
}

/** @brief Pure semantics: INHERIT gating — append_ancestor drops non-INHERIT ACEs. */
void test_inherit_gating() {
    std::printf("effective_acl_t — ancestor ACEs are INHERIT-gated at merge time:\n");
    const std::vector<std::byte> alice = as_bytes("alice");

    const std::vector<ace_t> ancestor{
        ace("alice", bit(acl_right_t::READ)),  // no INHERIT — that vertex only
        ace("alice", bit(acl_right_t::WRITE), ace_type_t::ALLOW, kAceInherit)};
    effective_acl_t eff;
    eff.append_ancestor(ancestor);
    check(eff.merged().size() == 1 && eff.merged()[0].access_mask == bit(acl_right_t::WRITE),
          "append_ancestor keeps only the INHERIT-flagged ACE");
    check(!eff.allows<allow_only_policy_t>(alice, bit(acl_right_t::READ), kNow),
          "a non-INHERIT ancestor READ grant does not travel (denied — the "
          "inherited WRITE ACE closes)");
    check(eff.allows<allow_only_policy_t>(alice, bit(acl_right_t::WRITE), kNow),
          "the INHERIT-flagged ancestor WRITE grant travels");

    effective_acl_t none;
    const std::vector<ace_t> only_local{ace("alice", bit(acl_right_t::READ))};
    none.append_ancestor(only_local);
    check(none.merged().empty() &&
              none.allows<allow_only_policy_t>(alice, bit(acl_right_t::READ), kNow),
          "an ancestor with ONLY non-INHERIT ACEs contributes nothing — stays open");
    check(none.allows<full_acl_policy_t>(alice, bit(acl_right_t::READ), kNow),
          "same under the full policy (open default is policy-independent)");
}

/** @brief Pure semantics: own-before-ancestors and nearest-ancestor-first ordering. */
void test_ordering() {
    std::printf("effective_acl_t — evaluation order: own first, ancestors nearest-first:\n");
    const std::vector<std::byte> alice = as_bytes("alice");

    {  // own ALLOW beats a nearer-than-nothing ancestor DENY appended after it
        effective_acl_t eff;
        const std::vector<ace_t> own{ace("alice", bit(acl_right_t::WRITE))};
        const std::vector<ace_t> parent{
            ace("alice", bit(acl_right_t::WRITE), ace_type_t::DENY, kAceInherit)};
        eff.append_own(own);
        eff.append_ancestor(parent);
        check(eff.allows<full_acl_policy_t>(alice, bit(acl_right_t::WRITE), kNow),
              "full: own ALLOW evaluates before an inherited DENY");
    }
    {  // own DENY beats an inherited ALLOW
        effective_acl_t eff;
        const std::vector<ace_t> own{ace("alice", bit(acl_right_t::WRITE), ace_type_t::DENY)};
        const std::vector<ace_t> parent{
            ace("alice", bit(acl_right_t::WRITE), ace_type_t::ALLOW, kAceInherit)};
        eff.append_own(own);
        eff.append_ancestor(parent);
        check(!eff.allows<full_acl_policy_t>(alice, bit(acl_right_t::WRITE), kNow),
              "full: own DENY evaluates before an inherited ALLOW");
    }
    {  // nearest ancestor first: parent DENY vs grandparent ALLOW
        effective_acl_t eff;
        const std::vector<ace_t> parent{
            ace("alice", bit(acl_right_t::WRITE), ace_type_t::DENY, kAceInherit)};
        const std::vector<ace_t> grandparent{
            ace("alice", bit(acl_right_t::WRITE), ace_type_t::ALLOW, kAceInherit)};
        eff.append_ancestor(parent);       // nearest first
        eff.append_ancestor(grandparent);  // then farther
        check(!eff.allows<full_acl_policy_t>(alice, bit(acl_right_t::WRITE), kNow),
              "full: the NEARER ancestor's DENY wins over a farther ALLOW");
    }
    {  // per-bit: a DENY on WRITE leaves READ to fall through to a later ALLOW
        effective_acl_t eff;
        const std::vector<ace_t> own{ace("alice", bit(acl_right_t::WRITE), ace_type_t::DENY)};
        const std::vector<ace_t> parent{
            ace("EVERYONE@", bit(acl_right_t::READ) | bit(acl_right_t::WRITE), ace_type_t::ALLOW,
                kAceInherit)};
        eff.append_own(own);
        eff.append_ancestor(parent);
        check(eff.allows<full_acl_policy_t>(alice, bit(acl_right_t::READ), kNow),
              "full: first-match-per-BIT — the WRITE DENY does not decide READ");
        check(!eff.allows<full_acl_policy_t>(alice, bit(acl_right_t::WRITE), kNow),
              "full: ... while WRITE stays denied");
    }
    {  // allow_only: order is irrelevant — any applicable ACE grants
        effective_acl_t eff;
        const std::vector<ace_t> own{ace("bob", bit(acl_right_t::READ))};
        const std::vector<ace_t> parent{
            ace("alice", bit(acl_right_t::READ), ace_type_t::ALLOW, kAceInherit)};
        eff.append_own(own);
        eff.append_ancestor(parent);
        check(eff.allows<allow_only_policy_t>(as_bytes("alice"), bit(acl_right_t::READ), kNow),
              "allow_only: an inherited grant applies regardless of position");
    }
}

// --- graph-side cache behavior -----------------------------------------------

/** @brief The test resolver (ADR-0018): caller bytes are the subject; empty = trusted. */
std::optional<subject_token_t> caller_is_subject(std::string_view caller) {
    if (caller.empty()) return std::nullopt;
    return as_bytes(caller);
}

/** @brief Heap-copy bytes into a value view. */
tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief A one-byte VALUE TLV write under a caller context. */
tr::graph::result_t<void> write_u8(graph_t& g, vertex_handle_t v, std::uint8_t x,
                                   std::string_view caller = {}) {
    std::vector<std::byte> out;
    const std::byte payload[1] = {std::byte{x}};
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{}, payload);
    return g.write(v, make_value(out), caller);
}

/** @brief true iff the result is PERMISSION_DENIED. */
template <class T>
bool denied(const tr::graph::result_t<T>& r) {
    return !r.has_value() && r.error() == status_t::PERMISSION_DENIED;
}

/** @brief Subtree-precise invalidation: a :acl rewrite flips cached descendant verdicts. */
void test_cache_invalidation() {
    std::printf("cached merge — a :acl write re-marks the WRITTEN vertex's subtree:\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/dev/temp"), role_t::STORED_VALUE);
    vertex_handle_t leaf = g.register_vertex(path_t("/dev/temp/raw"), role_t::STORED_VALUE);
    vertex_handle_t outside = g.register_vertex(path_t("/other"), role_t::STORED_VALUE);
    (void)write_u8(g, leaf, 7);
    (void)write_u8(g, outside, 7);

    // Warm every cache while everything is open.
    check(g.read(leaf, "peer-i").has_value(), "no ACL anywhere => leaf open (cache warmed)");
    check(g.read(outside, "peer-i").has_value(), "sibling subtree open (cache warmed)");

    // Install an INHERIT READ grant on the ancestor — the leaf's warmed-open cache
    // must be invalidated through the subtree walk, not sit stale.
    const std::vector<ace_t> grant{
        ace("peer-i", bit(acl_right_t::READ), ace_type_t::ALLOW, kAceInherit)};
    check(g.write(path_t("/dev:acl"), make_value(tr::graph::encode_acl(grant))).has_value(),
          "install INHERIT READ for peer-i on /dev");
    check(g.read(leaf, "peer-i").has_value(), "leaf READ allowed via the fresh inherited ACE");
    check(denied(g.read(leaf, "peer-x")), "leaf now CLOSED to others (stale-open would allow)");
    check(g.read(outside, "peer-x").has_value(),
          "the sibling subtree's cache was NOT invalidated (subtree-precise)");

    // Rewrite the ancestor ACL to a different subject: the leaf's cached grant for
    // peer-i must die with it.
    const std::vector<ace_t> regrant{
        ace("peer-j", bit(acl_right_t::READ), ace_type_t::ALLOW, kAceInherit)};
    check(g.write(path_t("/dev:acl"), make_value(tr::graph::encode_acl(regrant))).has_value(),
          "rewrite /dev:acl to grant peer-j instead");
    check(denied(g.read(leaf, "peer-i")), "peer-i's cached inherited grant is gone");
    check(g.read(leaf, "peer-j").has_value(), "peer-j's new inherited grant applies");

    // Clearing to an empty ACL reopens the subtree (storing replaces).
    const std::vector<ace_t> empty;
    check(g.write(path_t("/dev:acl"), make_value(tr::graph::encode_acl(empty))).has_value(),
          "replace /dev:acl with an empty ACL");
    check(g.read(leaf, "peer-x").has_value(), "empty effective ACL => leaf open again");
}

/** @brief Placeholder intermediates hold no ACEs and never close a chain. */
void test_placeholder_and_new_vertices() {
    std::printf("cached merge — placeholders contribute nothing; newborns start dirty:\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    (void)g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    // /a/b is a PLACEHOLDER (never registered); /a/b/c is real.
    vertex_handle_t c = g.register_vertex(path_t("/a/b/c"), role_t::STORED_VALUE);
    (void)write_u8(g, c, 7);

    const std::vector<ace_t> grant{
        ace("peer-i", bit(acl_right_t::READ), ace_type_t::ALLOW, kAceInherit)};
    (void)g.write(path_t("/a:acl"), make_value(tr::graph::encode_acl(grant)));
    check(g.read(c, "peer-i").has_value(),
          "the INHERIT grant crosses the placeholder level (empty list = no-op)");
    check(denied(g.read(c, "peer-x")), "…and the chain still closes for other subjects");

    // A vertex registered AFTER the ancestor's :acl write starts dirty and merges
    // the existing inherited ACEs on its first check.
    vertex_handle_t d = g.register_vertex(path_t("/a/b/c/d"), role_t::STORED_VALUE);
    (void)write_u8(g, d, 7);
    check(g.read(d, "peer-i").has_value(), "a newborn under a closed subtree inherits the grant");
    check(denied(g.read(d, "peer-x")), "…and the closure");
}

/** @brief TSan target: :acl rewrites racing gated ops never wedge a stale cache. */
void test_concurrent_rewrite_race() {
    std::printf("dirty-flag protocol — concurrent :acl rewrites vs gated reads:\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    (void)g.register_vertex(path_t("/r"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/r/m"), role_t::STORED_VALUE);
    vertex_handle_t leaf = g.register_vertex(path_t("/r/m/leaf"), role_t::STORED_VALUE);
    (void)write_u8(g, leaf, 7);

    const std::vector<ace_t> grant{
        ace("reader", bit(acl_right_t::READ), ace_type_t::ALLOW, kAceInherit)};
    const std::vector<std::byte> grant_wire = tr::graph::encode_acl(grant);
    (void)g.write(path_t("/r:acl"), make_value(grant_wire));

    constexpr int kWriterLoops = 400;
    std::atomic<bool> stop{false};
    // Writer: keeps REPLACING /r:acl with the SAME grant — every write re-marks the
    // subtree dirty, forcing rebuilds to race the marks (the verdict is constant, so
    // any allowed/denied flicker would be a real protocol bug, not scheduling).
    std::thread writer([&] {
        for (int i = 0; i < kWriterLoops; ++i)
            (void)g.write(path_t("/r:acl"), make_value(grant_wire));
        stop.store(true, std::memory_order_release);
    });
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> wrong{0};
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                if (!g.read(leaf, "reader").has_value()) wrong.fetch_add(1);
                if (g.read(leaf, "intruder").has_value()) wrong.fetch_add(1);
                reads.fetch_add(2, std::memory_order_relaxed);
            }
        });
    }
    writer.join();
    for (std::thread& th : readers) th.join();
    check(wrong.load() == 0, "no verdict flicker across " + std::to_string(reads.load()) +
                                 " gated reads racing " + std::to_string(kWriterLoops) +
                                 " :acl rewrites");

    // After the storm, a real change still lands (no stale-forever cache).
    const std::vector<ace_t> regrant{
        ace("other", bit(acl_right_t::READ), ace_type_t::ALLOW, kAceInherit)};
    (void)g.write(path_t("/r:acl"), make_value(tr::graph::encode_acl(regrant)));
    check(denied(g.read(leaf, "reader")) && g.read(leaf, "other").has_value(),
          "the post-race rewrite is observed (dirty flag survived every race)");
}

}  // namespace

int main() {
    test_open_default_and_closing();
    test_inherit_gating();
    test_ordering();
    test_cache_invalidation();
    test_placeholder_and_new_vertices();
    test_concurrent_rewrite_race();
    std::printf(g_failures == 0 ? "\neffective_acl: PASS\n" : "\neffective_acl: FAIL (%d)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
