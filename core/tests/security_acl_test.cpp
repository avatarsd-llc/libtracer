/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * security_acl unit test (ADR-0050) — drives the PURE ACL policy seam directly,
 * no graph, no locks, no wall clock: both adapters (allow_only / full), the ACE
 * edge cases (expiry, EVERYONE@, per-bit matching, INHERIT-flag filtering,
 * first-match-per-bit DENY ordering), and the typed parse/build round-trip that
 * replaces the per-test byte builders.
 */
#include "libtracer/security_acl.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include "libtracer/frame.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::ace_type_t;
using tr::graph::acl_right_t;
using tr::graph::acl_verdict_t;
using tr::graph::allow_only_policy_t;
using tr::graph::encode_acl;
using tr::graph::full_acl_policy_t;
using tr::graph::kAceInherit;
using tr::graph::parse_acl;
using tr::graph::status_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

constexpr std::uint32_t bit(acl_right_t r) { return static_cast<std::uint32_t>(r); }

ace_t ace(std::string_view subject, std::uint32_t mask, ace_type_t type = ace_type_t::ALLOW,
          std::uint8_t flags = 0, std::uint64_t expires_ns = 0) {
    return ace_t{.type = type,
                 .flags = flags,
                 .subject = as_bytes(subject),
                 .access_mask = mask,
                 .expires_ns = expires_ns};
}

}  // namespace

int main() {
    std::printf("security_acl — the pure ACL policy seam (ADR-0050):\n");
    const std::vector<std::byte> alice = as_bytes("alice");
    const std::vector<std::byte> bob = as_bytes("bob");
    constexpr std::uint64_t kNow = 1'000;

    // 1. allow_only: a matching ALLOW grants; a non-matching subject/bit does not.
    {
        const std::vector<ace_t> aces{ace("alice", bit(acl_right_t::READ))};
        check(allow_only_policy_t::allows(alice, bit(acl_right_t::READ), aces, kNow) ==
                  acl_verdict_t::ALLOW,
              "allow_only: matching ALLOW => ALLOW");
        check(allow_only_policy_t::allows(bob, bit(acl_right_t::READ), aces, kNow) ==
                  acl_verdict_t::NO_MATCH,
              "allow_only: wrong subject => NO_MATCH");
        check(allow_only_policy_t::allows(alice, bit(acl_right_t::WRITE), aces, kNow) ==
                  acl_verdict_t::NO_MATCH,
              "allow_only: right bit not in mask => NO_MATCH");
    }

    // 2. EVERYONE@ matches any subject; expiry is evaluated against the CALLER's now.
    {
        const std::vector<ace_t> aces{
            ace("EVERYONE@", bit(acl_right_t::READ), ace_type_t::ALLOW, 0, /*expires=*/500)};
        check(allow_only_policy_t::allows(bob, bit(acl_right_t::READ), aces, /*now=*/499) ==
                  acl_verdict_t::ALLOW,
              "EVERYONE@ matches any subject before expiry");
        check(allow_only_policy_t::allows(bob, bit(acl_right_t::READ), aces, /*now=*/500) ==
                  acl_verdict_t::NO_MATCH,
              "an expired ACE grants nothing (now == expires_ns)");
    }

    // 3. required_flags: an ancestor list only contributes INHERIT-flagged ACEs.
    {
        const std::vector<ace_t> aces{
            ace("alice", bit(acl_right_t::READ)),  // no INHERIT
            ace("alice", bit(acl_right_t::WRITE), ace_type_t::ALLOW, kAceInherit)};
        check(allow_only_policy_t::allows(alice, bit(acl_right_t::READ), aces, kNow, kAceInherit) ==
                  acl_verdict_t::NO_MATCH,
              "required_flags=INHERIT skips a non-INHERIT ACE");
        check(allow_only_policy_t::allows(alice, bit(acl_right_t::WRITE), aces, kNow,
                                          kAceInherit) == acl_verdict_t::ALLOW,
              "required_flags=INHERIT admits an INHERIT ACE");
    }

    // 4. full policy: ordered first-match-per-bit — the FIRST applicable ACE decides.
    {
        const std::vector<ace_t> deny_first{
            ace("alice", bit(acl_right_t::WRITE), ace_type_t::DENY),
            ace("EVERYONE@", bit(acl_right_t::WRITE) | bit(acl_right_t::READ))};
        check(full_acl_policy_t::allows(alice, bit(acl_right_t::WRITE), deny_first, kNow) ==
                  acl_verdict_t::DENY,
              "full: DENY first => DENY for the denied subject");
        check(full_acl_policy_t::allows(bob, bit(acl_right_t::WRITE), deny_first, kNow) ==
                  acl_verdict_t::ALLOW,
              "full: another subject falls through to the EVERYONE@ ALLOW");
        check(full_acl_policy_t::allows(alice, bit(acl_right_t::READ), deny_first, kNow) ==
                  acl_verdict_t::ALLOW,
              "full: per-bit — the DENY carries WRITE only, READ falls through");

        const std::vector<ace_t> allow_first{
            ace("alice", bit(acl_right_t::WRITE)),
            ace("alice", bit(acl_right_t::WRITE), ace_type_t::DENY)};
        check(full_acl_policy_t::allows(alice, bit(acl_right_t::WRITE), allow_first, kNow) ==
                  acl_verdict_t::ALLOW,
              "full: ALLOW first wins over a later DENY (stored order)");
    }

    // 5. Typed build → parse round-trip (the byte-builder replacement).
    {
        const std::vector<ace_t> in{ace("alice", bit(acl_right_t::READ) | bit(acl_right_t::WRITE),
                                        ace_type_t::ALLOW, kAceInherit, /*expires=*/42),
                                    ace("EVERYONE@", bit(acl_right_t::SUBSCRIBE))};
        const std::vector<std::byte> wire = encode_acl(in);
        const auto acl = tr::wire::decode(wire);
        check(acl.has_value() && acl->type == tr::wire::type_t::ACL, "encode_acl yields ACL{...}");
        const auto out = parse_acl<allow_only_policy_t>(*acl);
        check(out.has_value() && out->size() == 2, "parse_acl round-trips 2 ACEs");
        check(out && (*out)[0].subject == in[0].subject &&
                  (*out)[0].access_mask == in[0].access_mask && (*out)[0].flags == kAceInherit &&
                  (*out)[0].expires_ns == 42,
              "ACE 0 fields survive the round-trip");
        check(out && (*out)[1].subject == in[1].subject && (*out)[1].expires_ns == 0,
              "ACE 1 fields survive (expires omitted => 0)");
    }

    // 6. Parse strictness follows the policy: DENY parses under full, rejects under
    //    allow_only (never store semantics the evaluator would silently weaken).
    {
        const std::vector<ace_t> in{ace("alice", bit(acl_right_t::WRITE), ace_type_t::DENY)};
        const std::vector<std::byte> wire = encode_acl(in);
        const auto acl = tr::wire::decode(wire);
        check(acl.has_value(), "a DENY ACL encodes/decodes structurally");
        const auto strict = parse_acl<allow_only_policy_t>(*acl);
        check(!strict && strict.error() == status_t::TYPE_MISMATCH,
              "allow_only parse rejects a DENY ACE with TYPE_MISMATCH");
        const auto full = parse_acl<full_acl_policy_t>(*acl);
        check(full.has_value() && full->size() == 1 && (*full)[0].type == ace_type_t::DENY,
              "full parse stores the DENY ACE");
    }

    // 7. Flag strictness: any bit beyond INHERIT is rejected by both adapters (the
    //    graph merge does not honor richer NFSv4 flags yet — reject, never weaken).
    {
        const std::vector<ace_t> in{
            ace("alice", bit(acl_right_t::READ), ace_type_t::ALLOW, /*flags=*/0x2)};
        // Bind the encoded bytes: decode() is zero-copy (the tlv borrows the input),
        // so the buffer must outlive parse_acl below.
        const std::vector<std::byte> wire = encode_acl(in);
        const auto acl = tr::wire::decode(wire);
        check(acl.has_value() && !parse_acl<full_acl_policy_t>(*acl).has_value(),
              "a flag bit beyond INHERIT is TYPE_MISMATCH even under full");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
