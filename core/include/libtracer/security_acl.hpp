/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * security_acl — the ACL policy seam (ADR-0050). ACE evaluation is a PURE
 * per-target policy over typed ACEs: no graph access, no locks, no clock reads
 * of its own — the graph owns the effective-ACE walk (own + INHERIT-flagged
 * ancestor ACEs) and hands each list to the policy with the check-time `now`.
 * Two adapters realize the ADR-0020 split, selected at build time through the
 * target's module set (ADR-0047 §1 — the choice is per-target configuration and
 * the check runs on the data plane, so compile-time selection applies):
 *
 *   - allow_only_policy_t — the required-modules MCU profile (ALLOW-only,
 *     single INHERIT flag); the default.
 *   - full_acl_policy_t   — the security_acl host module (ordered
 *     first-match-per-bit with DENY), selected by LIBTRACER_ACL_FULL.
 *
 * The typed ACE parse/build (`ace_t` ↔ wire ACL TLV, docs/reference/05 §0x0A)
 * lives here too, so ACE edge cases (expiry, INHERIT, ordering) are
 * unit-testable without a live graph and tests need no hand-rolled byte
 * builders.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/status.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/vertex.hpp"

/**
 * @file
 * @brief `tr::graph` ACL policy seam (ADR-0050): pure ACE evaluation + typed
 *        ACE parse/build.
 */

namespace tr::graph {

/**
 * @brief A pure policy's answer for one ACE list (ADR-0050).
 *
 * `NO_MATCH` means no applicable ACE decided the bit — the caller keeps walking
 * (ancestor lists) and finally applies the open-by-default rule itself.
 */
enum class acl_verdict_t : std::uint8_t {
    ALLOW,    /**< @brief A matching ACE grants the right. */
    DENY,     /**< @brief A matching DENY ACE refuses it (full policy only). */
    NO_MATCH, /**< @brief No applicable ACE decided — keep walking / default. */
};

namespace detail_acl {

/** @brief True iff @p ace applies to @p subject right @p now for @p bit under @p required_flags. */
[[nodiscard]] inline bool ace_applies(const ace_t& ace, std::span<const std::byte> subject,
                                      std::uint32_t bit, std::uint64_t now,
                                      std::uint8_t required_flags) noexcept {
    if ((ace.flags & required_flags) != required_flags) return false;
    if ((ace.access_mask & bit) == 0) return false;
    if (ace.expires_ns != 0 && ace.expires_ns <= now) return false;
    static constexpr std::string_view kEveryone = "EVERYONE@";
    if (tr::detail::as_string_view(ace.subject) == kEveryone) return true;
    return std::ranges::equal(ace.subject, subject);
}

}  // namespace detail_acl

/**
 * @brief The required-modules MCU profile policy (ADR-0020 core subset): ALLOW-only.
 *
 * Any applicable ACE grants — order is irrelevant because DENY does not exist in
 * this profile (a `:acl` write carrying one is rejected at parse time).
 */
struct allow_only_policy_t {
    /** @brief This profile rejects DENY ACEs at parse time. */
    static constexpr bool kAcceptsDeny = false;

    /**
     * @brief Evaluate one ACE list — pure: no locks, no clock, no graph access.
     *
     * @param subject        The resolved subject token bytes (ADR-0018).
     * @param bit            The requested right (one `acl_right_t` bit).
     * @param aces           One vertex's stored ACEs, in stored order.
     * @param now            Check-time wall clock, ns since the UNIX epoch.
     * @param required_flags ACEs lacking these flag bits are skipped — `0` for the
     *                       target's own list, `kAceInherit` for an ancestor's.
     * @return `ALLOW` or `NO_MATCH` (this profile never returns `DENY`).
     */
    [[nodiscard]] static acl_verdict_t allows(std::span<const std::byte> subject, std::uint32_t bit,
                                              std::span<const ace_t> aces, std::uint64_t now,
                                              std::uint8_t required_flags = 0) noexcept {
        for (const ace_t& ace : aces) {
            if (detail_acl::ace_applies(ace, subject, bit, now, required_flags))
                return acl_verdict_t::ALLOW;
        }
        return acl_verdict_t::NO_MATCH;
    }
};

/**
 * @brief The `security_acl` host policy (ADR-0020 full model): ordered
 *        first-match-per-bit with DENY.
 *
 * For the requested bit, the FIRST applicable ACE in stored order decides —
 * `ALLOW` or `DENY` per its type (NFSv4 evaluation). The graph calls this per
 * effective-ACE list, own list before ancestors, so cross-list ordering follows
 * the effective-ACL definition of ADR-0020.
 */
struct full_acl_policy_t {
    /** @brief The full model stores and evaluates DENY ACEs. */
    static constexpr bool kAcceptsDeny = true;

    /** @copydoc allow_only_policy_t::allows */
    [[nodiscard]] static acl_verdict_t allows(std::span<const std::byte> subject, std::uint32_t bit,
                                              std::span<const ace_t> aces, std::uint64_t now,
                                              std::uint8_t required_flags = 0) noexcept {
        for (const ace_t& ace : aces) {
            if (!detail_acl::ace_applies(ace, subject, bit, now, required_flags)) continue;
            return ace.type == ace_type_t::DENY ? acl_verdict_t::DENY : acl_verdict_t::ALLOW;
        }
        return acl_verdict_t::NO_MATCH;
    }
};

/**
 * @brief The target's selected ACL policy (ADR-0047 §1 build-time module set).
 *
 * Default: the ALLOW-only MCU profile. Defining `LIBTRACER_ACL_FULL` (the CMake
 * option of the same name) swaps in the full `security_acl` host policy — a
 * target-configuration change, never an edit to `graph.cpp`.
 */
#if defined(LIBTRACER_ACL_FULL)
using acl_policy_t = full_acl_policy_t;
#else
using acl_policy_t = allow_only_policy_t;
#endif

/**
 * @brief Parse a decoded `:acl` ACL TLV into typed ACEs (docs/reference/05 §0x0A).
 *
 * STRICT per the selected @p Policy: an ACE whose semantics the policy would
 * silently weaken is rejected with `TYPE_MISMATCH` at write time — a DENY ACE
 * under `allow_only_policy_t`, any flag bit beyond `kAceInherit` (the
 * inheritance-only subset both adapters honor today; richer NFSv4 flags gate on
 * the graph's merge honoring them first), or a missing type/subject/access_mask.
 *
 * @tparam Policy The accepting policy (defaults to the target's selection).
 * @param acl A decoded ACL @ref wire::tlv_t (`ACL{ ACL{NAME/VALUE…}* }`).
 * @return The typed ACE list, in wire order, or `TYPE_MISMATCH`.
 */
template <class Policy = acl_policy_t>
[[nodiscard]] result_t<std::vector<ace_t>> parse_acl(const wire::tlv_t& acl) {
    using wire::tlv_t;
    using wire::type_t;
    std::vector<ace_t> out;
    out.reserve(acl.children.size());
    for (const tlv_t& entry : acl.children) {
        if (entry.type != type_t::ACL || !entry.opt.pl)
            return std::unexpected(status_t::TYPE_MISMATCH);
        ace_t ace;
        bool has_type = false;
        bool has_subject = false;
        bool has_mask = false;
        const std::vector<tlv_t>& ch = entry.children;
        for (std::size_t i = 0; i + 1 < ch.size(); ++i) {
            if (ch[i].type != type_t::NAME) continue;
            const std::string_view key = tr::detail::as_string_view(ch[i].payload);
            const tlv_t& val = ch[i + 1];
            if (key == "type" && val.type == type_t::VALUE) {
                const std::uint8_t t = tr::detail::load_le<std::uint8_t>(val.payload);
                // ALLOW=0 / DENY=1; DENY only where the policy evaluates it —
                // never store semantics the evaluator would silently weaken.
                if (t > 1 || (t == 1 && !Policy::kAcceptsDeny))
                    return std::unexpected(status_t::TYPE_MISMATCH);
                ace.type = static_cast<ace_type_t>(t);
                has_type = true;
            } else if (key == "flags" && val.type == type_t::VALUE) {
                ace.flags = tr::detail::load_le<std::uint8_t>(val.payload);
                // Single INHERIT bit only: INHERIT_ONLY/NO_PROPAGATE/GROUP would
                // be silently mis-evaluated by the merge, so reject, not weaken.
                if ((ace.flags & static_cast<std::uint8_t>(~kAceInherit)) != 0)
                    return std::unexpected(status_t::TYPE_MISMATCH);
            } else if (key == "subject") {
                // The subject token is opaque bytes (ADR-0018) — accept any opaque
                // TLV's payload (VALUE recommended; NAME for "OWNER@"/"EVERYONE@").
                ace.subject.assign(val.payload.begin(), val.payload.end());
                has_subject = ace.subject.size() > 0;
            } else if (key == "access_mask" && val.type == type_t::VALUE) {
                ace.access_mask = static_cast<std::uint32_t>(tr::detail::load_le(val.payload));
                has_mask = true;
            } else if (key == "expires_ns" && val.type == type_t::VALUE) {
                ace.expires_ns = tr::detail::load_le<std::uint64_t>(val.payload);
            }
        }
        if (!has_type || !has_subject || !has_mask) return std::unexpected(status_t::TYPE_MISMATCH);
        out.push_back(std::move(ace));
    }
    return out;
}

/**
 * @brief Encode typed ACEs as the wire `ACL{ ACL{…}* }` TLV bytes — the typed
 *        builder (the inverse of @ref parse_acl; kills per-test byte builders).
 *
 * Emits NAME-tagged `type`(u8) / `flags`(u8) / `subject`(opaque VALUE) /
 * `access_mask`(u32) children, plus `expires_ns`(u64) when non-zero, per
 * docs/reference/05 §0x0A. Encoding is unvalidated by design (tests build
 * deliberately-rejectable ACLs with it); @ref parse_acl is the gate.
 */
[[nodiscard]] inline std::vector<std::byte> encode_acl(std::span<const ace_t> aces) {
    using wire::opt_t;
    using wire::type_t;
    const auto emit_u = [](std::vector<std::byte>& out, std::string_view name, std::uint64_t v,
                           std::size_t width) {
        wire::emit_name(out, name);
        std::vector<std::byte> payload(width);
        tr::detail::store_le(payload, v, width);
        wire::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
    };
    std::vector<std::byte> body;
    for (const ace_t& ace : aces) {
        std::vector<std::byte> entry;
        emit_u(entry, "type", static_cast<std::uint8_t>(ace.type), 1);
        emit_u(entry, "flags", ace.flags, 1);
        wire::emit_name(entry, "subject");
        wire::emit_tlv(entry, type_t::VALUE, opt_t{}, ace.subject);
        emit_u(entry, "access_mask", ace.access_mask, 4);
        if (ace.expires_ns != 0) emit_u(entry, "expires_ns", ace.expires_ns, 8);
        wire::emit_tlv(body, type_t::ACL, opt_t{.pl = true}, entry);
    }
    std::vector<std::byte> out;
    wire::emit_tlv(out, type_t::ACL, opt_t{.pl = true}, body);
    return out;
}

}  // namespace tr::graph
