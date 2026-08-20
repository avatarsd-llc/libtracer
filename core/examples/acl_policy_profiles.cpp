/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — two evaluators, and DENY exists only where one of them can see it.
 *
 * The wire layout is the full NFSv4-style model; the required-modules MCU profile enforces a
 * SUBSET of it (ADR-0020). ACE evaluation is therefore a pure per-target policy (ADR-0050) with
 * two adapters, both always compiled:
 *
 *  - `allow_only_policy_t` — the default. Any applicable ACE grants. Order is irrelevant,
 *    because DENY does not exist in this profile.
 *  - `full_acl_policy_t` — ordered first-match-per-bit: the FIRST applicable ACE decides, by its
 *    own type. This is the host profile, selected per target.
 *
 * Hand both the SAME two-ACE list — a DENY followed by an ALLOW for the same subject and the
 * same bit — and they answer the opposite thing. That is not a quirk to work around; it is the
 * reason `parse_acl` REFUSES to store a DENY ACE under the ALLOW-only profile. A stored DENY
 * that the running evaluator cannot see would be read as a grant, and a security document must
 * never be interpreted more broadly than it was written.
 *
 * Both policies are named EXPLICITLY below, as template arguments, so this example demonstrates
 * both arms in every build regardless of which one the target binds. The bound choice is
 * `tr::graph::acl_policy_t` (ADR-0068 — plain C++ in `config.hpp`, rebindable by a
 * `config_override.hpp` fragment); it is printed for information and nothing here is skipped
 * because of it.
 *
 * Runs under ctest as `example_acl_policy_profiles`; returns non-zero on any failed check.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/security_acl.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::ace_type_t;
using tr::graph::acl_right_t;
using tr::graph::acl_verdict_t;
using tr::graph::allow_only_policy_t;
using tr::graph::effective_acl_t;
using tr::graph::full_acl_policy_t;

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

/** @brief @p right as the single `access_mask` bit it is. */
constexpr std::uint32_t bit(acl_right_t right) { return static_cast<std::uint32_t>(right); }

/** @brief Check time — every ACE below is permanent, so the clock is not a variable here. */
constexpr std::uint64_t kNow = 1'800'000'000'000'000'000ULL;

}  // namespace

int main() {
    bool ok = true;
    const std::vector<std::byte> alice = as_bytes("alice");
    const std::uint32_t read = bit(acl_right_t::READ);

    // The discriminating list: refuse alice, then grant her. Same subject, same bit.
    const ace_t deny_then_allow[] = {
        {.type = ace_type_t::DENY, .subject = as_bytes("alice"), .access_mask = read},
        {.type = ace_type_t::ALLOW, .subject = as_bytes("alice"), .access_mask = read},
    };
    check(ok, full_acl_policy_t::allows(alice, read, deny_then_allow, kNow) == acl_verdict_t::DENY,
          "full policy: the FIRST applicable ACE decides, and it is a DENY");
    check(ok,
          allow_only_policy_t::allows(alice, read, deny_then_allow, kNow) == acl_verdict_t::ALLOW,
          "ALLOW-only policy: it never looks at the type, so the same bytes GRANT");

    // Which is exactly why that list may not be stored under the ALLOW-only profile. `parse_acl`
    // is where the refusal happens — the write door, not the evaluator (see acl_parse_strict).
    const auto encoded = tr::graph::encode_acl(deny_then_allow);
    const auto decoded = tr::wire::decode(encoded);
    check(ok, decoded.has_value(), "the ACL TLV itself is well-formed either way");
    check(ok, tr::graph::parse_acl<full_acl_policy_t>(*decoded).has_value(),
          "the full profile parses it: it can evaluate what it is about to store");
    const auto refused = tr::graph::parse_acl<allow_only_policy_t>(*decoded);
    check(ok, !refused && refused.error() == tr::graph::status_t::TYPE_MISMATCH,
          "the ALLOW-only profile refuses it with TYPE_MISMATCH, rather than storing a "
          "restriction it would go on to read as a grant");
    check(ok, !allow_only_policy_t::kAcceptsDeny && full_acl_policy_t::kAcceptsDeny,
          "kAcceptsDeny is the constant that decides that, and parse_acl is its only reader");

    // Order is a property of the full policy alone. Flip the two ACEs and it flips its answer;
    // an ALLOW-only list has nothing to order, which is why the MCU subset can skip the concept.
    const ace_t allow_then_deny[] = {deny_then_allow[1], deny_then_allow[0]};
    check(ok, full_acl_policy_t::allows(alice, read, allow_then_deny, kNow) == acl_verdict_t::ALLOW,
          "full policy: reversing the two ACEs reverses the verdict — stored order is semantic");

    // Above the policies, `effective_acl_t` adds the open-by-default rule and is where the
    // policy is chosen. Same merged list, both arms, in one build.
    effective_acl_t merged;
    merged.append_own(deny_then_allow);
    check(ok, !effective_acl_t::allows<full_acl_policy_t>(merged.merged(), alice, read, kNow),
          "through effective_acl_t, the full profile still denies alice");
    check(ok, effective_acl_t::allows<allow_only_policy_t>(merged.merged(), alice, read, kNow),
          "…and the ALLOW-only profile still grants her");
    check(ok,
          !effective_acl_t::allows<full_acl_policy_t>(merged.merged(), as_bytes("bob"), read, kNow),
          "and neither profile lets bob in: no ACE names him, and the list is not empty");

    std::printf("this build binds acl_policy_t with kAcceptsDeny=%s; both arms ran anyway\n",
                tr::graph::acl_policy_t::kAcceptsDeny ? "true (full)" : "false (ALLOW-only)");
    return ok ? 0 : 1;
}
