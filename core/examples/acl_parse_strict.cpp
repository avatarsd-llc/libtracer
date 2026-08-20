/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — an ACL is a security document, so the parser REFUSES what it cannot
 *        read exactly.
 *
 * Everywhere else in this codebase a decoder is forgiving in one specific way: an unknown key is
 * skipped, because a newer peer legitimately sends more than the receiver understands
 * (`wire::config_reader_t`). `parse_acl` takes the OPPOSITE ruling on the same shape, and the
 * reason is that leniency here does not lose a field — it INVERTS or WIDENS a grant (#906,
 * docs/reference/05 §0x0A):
 *
 *  - a dropped `expires_ns` turns a time-limited grant permanent;
 *  - a dropped `flags` turns a vertex-local ACE into an inherited one;
 *  - a `type` sent big-endian as u16 `0x0001` truncates from DENY to ALLOW;
 *  - an unknown key is a restriction a newer writer meant to apply and this reader would ignore.
 *
 * So the rule is: any shape `encode_acl` would never emit is `TYPE_MISMATCH` at WRITE time,
 * where the operator finds out, rather than a quietly weaker policy at check time, where nobody
 * does. The one deliberate exception is a numeric payload NARROWER than its field —
 * little-endian zero-extension is exact, so it names the same integer.
 *
 * The examples below build their ACL bytes by hand, because the point is precisely the shapes
 * the typed builder cannot produce.
 *
 * Runs under ctest as `example_acl_parse_strict`; returns non-zero on any failed check.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::acl_right_t;
using tr::graph::status_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief Append a `NAME key` / `VALUE <@p width little-endian bytes of @p v>` pair. */
void put_uint(std::vector<std::byte>& entry, std::string_view key, std::uint64_t v,
              std::size_t width) {
    tr::wire::emit_name(entry, key);
    std::vector<std::byte> payload(width);
    tr::detail::store_le(payload, v, width);
    tr::wire::emit_tlv(entry, type_t::VALUE, opt_t{}, payload);
}

/** @brief Append a `NAME key` / `VALUE <@p text>` pair — how a subject token is spelled. */
void put_text(std::vector<std::byte>& entry, std::string_view key, std::string_view text) {
    tr::wire::emit_name(entry, key);
    tr::wire::emit_tlv(entry, type_t::VALUE, opt_t{},
                       std::as_bytes(std::span<const char>(text.data(), text.size())));
}

/** @brief Wrap one hand-built ACE body as the `ACL{ ACL{…} }` collection a `:acl` write carries. */
std::vector<std::byte> acl_of(std::span<const std::byte> entry) {
    std::vector<std::byte> body;
    tr::wire::emit_tlv(body, type_t::ACL, opt_t{.pl = true}, entry);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::ACL, opt_t{.pl = true}, body);
    return out;
}

/** @brief Decode @p bytes and parse them as an ACL under the target's bound policy. */
tr::graph::result_t<std::vector<ace_t>> parse(std::span<const std::byte> bytes) {
    const auto decoded = tr::wire::decode(bytes);
    if (!decoded) return std::unexpected(status_t::TYPE_MISMATCH);
    return tr::graph::parse_acl(*decoded);
}

/** @brief True iff @p r was refused as a malformed ACL. */
template <class T>
bool rejected(const tr::graph::result_t<T>& r) {
    return !r.has_value() && r.error() == status_t::TYPE_MISMATCH;
}

/** @brief @p right as the single `access_mask` bit it is. */
constexpr std::uint32_t bit(acl_right_t right) { return static_cast<std::uint32_t>(right); }

}  // namespace

int main() {
    bool ok = true;

    // The canonical shape, as `encode_acl` emits it — the baseline every rejection below is one
    // edit away from, so nothing here is refused for an incidental reason.
    {
        std::vector<std::byte> entry;
        put_uint(entry, "type", 0, 1);
        put_uint(entry, "flags", 0, 1);
        put_text(entry, "subject", "alice");
        put_uint(entry, "access_mask", bit(acl_right_t::READ), 4);
        put_uint(entry, "expires_ns", 1'800'000'000'000'000'000ULL, 8);
        const auto parsed = parse(acl_of(entry));
        check(ok, parsed.has_value() && parsed->size() == 1, "the canonical ACE parses");
        check(ok,
              parsed->front().access_mask == bit(acl_right_t::READ) &&
                  parsed->front().expires_ns != 0,
              "…with its mask and its deadline intact");
    }

    // An UNKNOWN key. `config_reader_t` would skip the pair; this refuses the whole ACL.
    {
        std::vector<std::byte> entry;
        put_uint(entry, "type", 0, 1);
        put_text(entry, "subject", "alice");
        put_uint(entry, "access_mask", bit(acl_right_t::READ), 4);
        put_uint(entry, "only_on_tuesdays", 1, 1);
        check(ok, rejected(parse(acl_of(entry))),
              "an unknown key is rejected — skipping it would drop a restriction and widen the "
              "grant, which is the opposite ruling to config, deliberately");
    }

    // A numeric payload WIDER than its field. `load_le` reads the low bytes, so a big-endian u16
    // DENY (0x0001) would have read back as 0x00 — ALLOW. This is #906's motivating case.
    {
        std::vector<std::byte> entry;
        tr::wire::emit_name(entry, "type");
        const std::byte be_deny[2] = {std::byte{0x00}, std::byte{0x01}};  // DENY, big-endian u16
        tr::wire::emit_tlv(entry, type_t::VALUE, opt_t{}, be_deny);
        put_text(entry, "subject", "alice");
        put_uint(entry, "access_mask", bit(acl_right_t::READ), 4);
        check(ok, rejected(parse(acl_of(entry))),
              "a `type` payload wider than u8 is rejected, not truncated from DENY to ALLOW");
    }

    // NARROWER is fine, and is the one place leniency is safe: zero-extension names the same
    // integer, so a pre-RFC-0026 two-byte access_mask still reads exactly.
    {
        std::vector<std::byte> entry;
        put_uint(entry, "type", 0, 1);
        put_text(entry, "subject", "alice");
        put_uint(entry, "access_mask", bit(acl_right_t::READ), 2);
        const auto parsed = parse(acl_of(entry));
        check(ok, parsed.has_value() && parsed->front().access_mask == bit(acl_right_t::READ),
              "a NARROWER access_mask is accepted — little-endian zero-extension is exact");
    }

    // A missing required field. An absent `access_mask` is not "grants nothing"; it is an ACE
    // whose author believed they wrote one.
    {
        std::vector<std::byte> entry;
        put_uint(entry, "type", 0, 1);
        put_text(entry, "subject", "alice");
        check(ok, rejected(parse(acl_of(entry))), "a missing access_mask is rejected");
    }

    // An EMPTY numeric payload. It would load as 0 — `ALLOW` for `type`, "never expires" for
    // `expires_ns` — so an absent value must not read as the permissive one.
    {
        std::vector<std::byte> entry;
        put_uint(entry, "type", 0, 1);
        put_text(entry, "subject", "alice");
        put_uint(entry, "access_mask", bit(acl_right_t::READ), 4);
        tr::wire::emit_name(entry, "expires_ns");
        tr::wire::emit_tlv(entry, type_t::VALUE, opt_t{}, std::span<const std::byte>{});
        check(ok, rejected(parse(acl_of(entry))),
              "an empty expires_ns is rejected — absent must not read as 'never expires'");
    }

    // A flag bit the merge does not honour. INHERIT_ONLY / NO_PROPAGATE would be silently
    // mis-evaluated, so the subset refuses them rather than weakening them.
    {
        std::vector<std::byte> entry;
        put_uint(entry, "type", 0, 1);
        put_uint(entry, "flags", 0x02, 1);  // beyond kAceInherit (0x01)
        put_text(entry, "subject", "alice");
        put_uint(entry, "access_mask", bit(acl_right_t::READ), 4);
        check(ok, rejected(parse(acl_of(entry))),
              "a flag bit beyond kAceInherit is rejected — the merge cannot honour it");
    }

    // An odd child count: a trailing key whose value the sender believes it wrote.
    {
        std::vector<std::byte> entry;
        put_uint(entry, "type", 0, 1);
        put_text(entry, "subject", "alice");
        put_uint(entry, "access_mask", bit(acl_right_t::READ), 4);
        tr::wire::emit_name(entry, "expires_ns");  // key, no value
        check(ok, rejected(parse(acl_of(entry))), "an unpaired trailing key is rejected");
    }

    std::printf("one accepted shape, one safe leniency, six refusals — all at write time\n");
    return ok ? 0 : 1;
}
