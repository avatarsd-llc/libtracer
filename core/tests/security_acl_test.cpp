/**
 * @file
 * @brief security_acl unit test (ADR-0050) — drives the PURE ACL policy seam directly, no graph, no
 *        locks, no wall clock: both adapters (allow_only / full), the ACE edge cases (expiry,
 *        EVERYONE@, per-bit matching, INHERIT-flag filtering, first-match-per-bit DENY ordering),
 *        and the typed parse/build round-trip that replaces the per-test byte builders.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#include "libtracer/security_acl.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/tlv_emit.hpp"

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

/** @brief The little-endian bytes of @p v in exactly @p width bytes (`width <= 8`). */
std::vector<std::byte> le(std::uint64_t v, std::size_t width) {
    std::vector<std::byte> out(width);
    tr::detail::store_le(std::span<std::byte>(out), v, width);
    return out;
}

/** @brief Append one `(NAME key, value)` pair to an ACE field body. */
void add_pair(std::vector<std::byte>& entry, std::string_view key, tr::wire::type_t vt,
              std::span<const std::byte> payload) {
    tr::wire::emit_name(entry, key);
    tr::wire::emit_tlv(entry, vt, tr::wire::opt_t{}, payload);
}

/** @brief Append a bare NAME key with no value — the trailing-unpaired-key shape. */
void add_key(std::vector<std::byte>& entry, std::string_view key) {
    tr::wire::emit_name(entry, key);
}

/** @brief Append a bare value with no key — the shape that desynchronizes the pair stream. */
void add_val(std::vector<std::byte>& entry, tr::wire::type_t vt,
             std::span<const std::byte> payload) {
    tr::wire::emit_tlv(entry, vt, tr::wire::opt_t{}, payload);
}

/** @brief Wrap one ACE field body as the `ACL{ ACL{…} }` blob an `:acl` write carries. */
std::vector<std::byte> one_ace(std::span<const std::byte> entry) {
    std::vector<std::byte> body;
    tr::wire::emit_tlv(body, tr::wire::type_t::ACL, tr::wire::opt_t{.pl = true}, entry);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::ACL, tr::wire::opt_t{.pl = true}, body);
    return out;
}

/**
 * @brief Assert that @p wire is REJECTED with `TYPE_MISMATCH` under `Policy`.
 *
 * @note `wire::decode` borrows its input, so @p wire must outlive the call — passing a
 *       temporary is fine (it lives to the end of the full expression).
 */
template <class Policy>
void rejects(std::span<const std::byte> wire, std::string_view what) {
    const auto acl = tr::wire::decode(wire);
    if (!acl.has_value()) {
        check(false, what);  // the blob must be structurally decodable to test the parse gate
        return;
    }
    const auto parsed = parse_acl<Policy>(*acl);
    check(!parsed.has_value() && parsed.error() == status_t::TYPE_MISMATCH, what);
}

/** @brief The ACEs @p wire parses to under `Policy`, or an empty list if it was rejected. */
template <class Policy>
std::vector<ace_t> parsed_aces(std::span<const std::byte> wire) {
    const auto acl = tr::wire::decode(wire);
    if (!acl.has_value()) return {};
    const auto out = parse_acl<Policy>(*acl);
    if (!out.has_value()) return {};
    return *out;
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

    // 8. The #906 rejection-vector table: one blob per lenient arm parse_acl used to
    //    admit. Each deviates from encode_acl's shape in EXACTLY ONE way, so a failing
    //    row names the arm it covers rather than "some bad ACL is rejected".
    {
        using tr::wire::type_t;
        const std::vector<std::byte> subject = as_bytes("alice");
        const std::vector<std::byte> mask4 = le(bit(acl_right_t::READ), 4);

        // 8a. A big-endian u16 `type` of 0x0001 (DENY): the low byte is 0x00, so a
        //     width-tolerant load read it as ALLOW=0 and it passed the `t > 1` gate —
        //     the DENY-to-ALLOW inversion, under the policy that evaluates DENY.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE,
                     std::vector<std::byte>{std::byte{0x00}, std::byte{0x01}});
            add_pair(e, "flags", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::VALUE, subject);
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            rejects<full_acl_policy_t>(one_ace(e),
                                       "BE-u16 `type` 0x0001 is not truncated to ALLOW");
            rejects<allow_only_policy_t>(one_ace(e), "the same blob is rejected under allow_only");
        }

        // 8b. An EMPTY `type` payload loads as 0 = ALLOW: an absent value must not read
        //     as the permissive one.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, std::span<const std::byte>{});
            add_pair(e, "subject", type_t::VALUE, subject);
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            rejects<allow_only_policy_t>(one_ace(e), "an empty `type` payload is not ALLOW");
        }

        // 8c. A u64 `access_mask` was loaded and TRUNCATED to u32 — the high bytes
        //     dropped silently while `has_mask` still counted the field as present.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::VALUE, subject);
            add_pair(e, "access_mask", type_t::VALUE, le(0x0000'0001'0000'0001ull, 8));
            rejects<allow_only_policy_t>(one_ace(e), "a u64 `access_mask` is not truncated to u32");
        }

        // 8d. A two-byte `flags`: the u8 field's declared width is one.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "flags", type_t::VALUE, le(kAceInherit, 2));
            add_pair(e, "subject", type_t::VALUE, subject);
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            rejects<allow_only_policy_t>(one_ace(e), "an over-wide `flags` payload is rejected");
        }

        // 8e. An over-wide `expires_ns` whose low eight bytes are zero: the truncating
        //     load read 0, and 0 means "never expires" — a time-limited grant made
        //     permanent by a width the builder never emits.
        {
            std::vector<std::byte> wide(16);
            wide[8] = std::byte{0x01};
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::VALUE, subject);
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            add_pair(e, "expires_ns", type_t::VALUE, wide);
            rejects<allow_only_policy_t>(one_ace(e),
                                         "an over-wide `expires_ns` does not truncate to never");
        }

        // 8f. `expires_ns` paired with a NON-VALUE TLV fell through the else-if chain and
        //     left expires_ns = 0 — the same permanent grant, by a wrong value type.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::VALUE, subject);
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            add_pair(e, "expires_ns", type_t::NAME, as_bytes("1000"));
            rejects<allow_only_policy_t>(one_ace(e),
                                         "a NAME-typed `expires_ns` is rejected, not skipped");
        }

        // 8g. An unknown NAME key was dropped, so a restrictive attribute a newer writer
        //     meant to apply would evaluate more broadly than written.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::VALUE, subject);
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            add_pair(e, "audit", type_t::VALUE, le(1, 1));
            rejects<allow_only_policy_t>(one_ace(e), "an unknown ACE key is rejected, not ignored");
        }

        // 8h. A trailing unpaired NAME: the old `i + 1 < size` bound simply never reached
        //     it, so a key whose value the sender believes it wrote vanished.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::VALUE, subject);
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            add_key(e, "expires_ns");
            rejects<allow_only_policy_t>(one_ace(e), "a trailing unpaired NAME key is rejected");
        }

        // 8i. A non-NAME where a key belongs: the every-offset scan resynchronized onto
        //     the next NAME and parsed a body whose pairing is lost.
        {
            std::vector<std::byte> e;
            add_val(e, type_t::VALUE, le(0, 1));
            add_val(e, type_t::VALUE, le(0, 1));
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::VALUE, subject);
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            rejects<allow_only_policy_t>(one_ace(e), "a non-NAME in a key slot is rejected");
        }

        // 8j. A duplicate `type`, DENY then ALLOW: last-wins silently dropped the DENY —
        //     the inversion again, this time by repetition rather than by width.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(1, 1));
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::VALUE, subject);
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            rejects<full_acl_policy_t>(one_ace(e),
                                       "a duplicate `type` does not last-wins to ALLOW");
        }

        // 8k. A duplicate `access_mask`, narrow then wide: last-wins WIDENED the grant.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::VALUE, subject);
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            add_pair(e, "access_mask", type_t::VALUE, le(0xFFFFFFFFull, 4));
            rejects<allow_only_policy_t>(one_ace(e), "a duplicate `access_mask` is rejected");
        }

        // 8l. An empty `subject` token — rejected before, and still rejected at the pair
        //     rather than by the end-of-ACE required-field check.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::VALUE, std::span<const std::byte>{});
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            rejects<allow_only_policy_t>(one_ace(e), "an empty `subject` token is rejected");
        }
    }

    // 9. What the strict walk must still ACCEPT — the shapes the wire genuinely carries.
    {
        using tr::wire::type_t;
        const std::vector<std::byte> mask4 = le(bit(acl_right_t::READ), 4);

        // 9a. The `acl/acl-aces` conformance vector (and the Rust core's builder, and
        //     reference/05 §0x0A's own layout) spell `access_mask` as u16 where
        //     encode_acl spells it u32. A narrower payload zero-extends exactly, so both
        //     name the same rights and both parse — rejecting the narrow one would make
        //     this core reject its own published vector.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "flags", type_t::VALUE, le(kAceInherit, 1));
            add_pair(e, "subject", type_t::VALUE, as_bytes("peer-a"));
            add_pair(e, "access_mask", type_t::VALUE, le(0x0003, 2));
            const std::vector<ace_t> got = parsed_aces<allow_only_policy_t>(one_ace(e));
            check(got.size() == 1 && got[0].access_mask == 0x0003 && got[0].flags == kAceInherit,
                  "a u16 `access_mask` (the conformance-vector spelling) still parses");
        }

        // 9b. A NAME-typed `subject` is the OWNER@/EVERYONE@ spelling and stays legal —
        //     and, being pair-consumed, is no longer re-read as the next key. Before the
        //     fix this ACE bound `subject` to the FOLLOWING key's name ("access_mask")
        //     instead of the token actually written.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::NAME, as_bytes("subject"));
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            const std::vector<ace_t> got = parsed_aces<allow_only_policy_t>(one_ace(e));
            check(got.size() == 1 && got[0].subject == as_bytes("subject"),
                  "a NAME-typed subject binds the token written, not the next key");
        }

        // 9c. EVERYONE@ as a NAME-typed subject, the round-trip gate's named shape.
        {
            std::vector<std::byte> e;
            add_pair(e, "type", type_t::VALUE, le(0, 1));
            add_pair(e, "flags", type_t::VALUE, le(0, 1));
            add_pair(e, "subject", type_t::NAME, as_bytes("EVERYONE@"));
            add_pair(e, "access_mask", type_t::VALUE, mask4);
            add_pair(e, "expires_ns", type_t::VALUE, le(42, 8));
            const std::vector<ace_t> got = parsed_aces<allow_only_policy_t>(one_ace(e));
            check(got.size() == 1 && got[0].subject == as_bytes("EVERYONE@") &&
                      got[0].expires_ns == 42,
                  "a NAME-typed EVERYONE@ subject with an expiry parses");
        }
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
