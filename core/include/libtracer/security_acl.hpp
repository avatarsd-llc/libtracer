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
 *     first-match-per-bit with DENY), bound by an override fragment.
 *
 * The typed ACE parse/build (`ace_t` ↔ wire ACL TLV, docs/reference/05 §0x0A) lives
 * here too, so ACE edge cases (expiry, INHERIT, ordering) are unit-testable without a
 * live graph and tests need no hand-rolled byte builders.
 *
 * The ACE RECORDS themselves are one header up, in `libtracer/acl_ace.hpp` (#868). They
 * used to be declared in `vertex.hpp` while their evaluation lived here, so this header
 * pulled a 3400-line vertex hub to name four types it owns the meaning of — the straddle
 * #868 set out to end. It is ended by giving the records their own home rather than by
 * folding them in beside the evaluator, because the two halves sit on OPPOSITE sides of
 * the vertex: `vertex_ext_t` stores a `std::vector<ace_t>`, so the records are compiled by
 * every net-plane TU, while nothing in the vertex core calls the policies or the codec.
 * Declaring the records here would drag this header along with them — measured at 15 -> 100
 * dependent TUs — and an ACL evaluation edit would rebuild the tree. See `acl_ace.hpp`.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/acl_ace.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/config.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/status.hpp"
#include "libtracer/tlv_emit.hpp"

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

/**
 * @brief The wildcard subject spelling (ADR-0020): an ACE carrying exactly these bytes as its
 *        subject applies to every resolved subject.
 *
 * It is RESERVED, not merely magic (#908). The wire has one spelling for a subject token —
 * the `acl/acl-aces` conformance vector sends `peer-a` and this string as the same opaque
 * VALUE — so a principal that could BE these bytes would be indistinguishable from the
 * wildcard, and the deployments at risk are exactly the ones whose resolver passes a
 * caller-supplied identity through (usernames, cert CNs, peer names). The core therefore
 * refuses to let a resolved subject spell it (see @ref is_reserved_subject), rather than
 * leaving every integrator to know to blacklist it.
 *
 * @note This is the ONLY special subject. ADR-0020 originally named `OWNER@` alongside it, but
 *       no evaluator here ever special-cased that string, so an `OWNER@` ACE matched nobody —
 *       and since any present ACE closes an otherwise-open vertex, such an ACE LOCKED the
 *       vertex it was written to delegate. ADR-0020's erratum (#1033) withdraws the name
 *       rather than reserving it: with no document telling an operator to write that ACE,
 *       there is nothing for an impersonated `OWNER@` principal to match either. It is an
 *       ordinary opaque token. Real owner semantics need a per-vertex owner identity the graph
 *       does not hold and would change how a STORED ACE evaluates — an amendment, not this.
 */
inline constexpr std::string_view kEveryoneSubject = "EVERYONE@";

/**
 * @brief True iff @p subject spells a RESERVED subject token — today exactly @ref
 *        kEveryoneSubject — and so may never be a resolved principal (#908).
 *
 * Checked in the two places a subject reaches an ACE comparison in `core/`: once per policy
 * evaluation (`allow_only_policy_t::allows` / `full_acl_policy_t::allows` — between them the
 * only callers of `%detail_acl::ace_applies`, and what `effective_acl_t::allows` drives),
 * where a reserved subject matches nothing; and once per resolver return in
 * `graph_t::acl_allows` — the only site that invokes a resolver — which is decisive: that
 * caller is refused at every gate, like the resolver's own error arm (#905). Public so an
 * integrator's resolver can reject the token at its own door too.
 */
[[nodiscard]] inline bool is_reserved_subject(std::span<const std::byte> subject) noexcept {
    return tr::detail::as_string_view(subject) == kEveryoneSubject;
}

namespace detail_acl {

/** @brief True iff @p ace applies to @p subject right @p now for @p bit under @p required_flags. */
[[nodiscard]] inline bool ace_applies(const ace_t& ace, std::span<const std::byte> subject,
                                      std::uint32_t bit, std::uint64_t now,
                                      std::uint8_t required_flags) noexcept {
    if ((ace.flags & required_flags) != required_flags) return false;
    if ((ace.access_mask & bit) == 0) return false;
    if (ace.expires_ns != 0 && ace.expires_ns <= now) return false;
    if (is_reserved_subject(ace.subject)) return true;  // the wildcard ACE matches everyone
    return std::ranges::equal(ace.subject, subject);
}

/**
 * @brief True iff @p payload is a canonical little-endian encoding of an ACE numeric
 *        field @p width bytes wide (#906).
 *
 * Two rules, both about what `detail::load_le` would otherwise do silently:
 *
 * - **Non-empty.** An empty payload loads as `0`, which for `type` is `ALLOW` and for
 *   `expires_ns` is "never expires" — an absent value must not read as a permissive one.
 * - **Never WIDER than the field.** `load_le` reads only the low `min(size, sizeof(T))`
 *   bytes, so an over-wide payload is truncated: a `type` sent big-endian as u16
 *   `0x0001` (DENY) would read as `0x00` (ALLOW), and an over-wide `access_mask` would
 *   drop its high bytes while still counting as present.
 *
 * A payload NARROWER than the field is accepted: little-endian zero-extension is exact,
 * so it names the same integer — the canonical `access_mask` width is u32 (RFC-0026,
 * matching what `encode_acl` and the `acl/acl-aces` vector spell), but a narrower
 * pre-RFC-0026 spelling names the same rights and stays readable.
 */
[[nodiscard]] inline bool ace_field_ok(std::span<const std::byte> payload,
                                       std::size_t width) noexcept {
    return !payload.empty() && payload.size() <= width;
}

/**
 * @brief One ACE key the parser accepts: its wire spelling and its payload width.
 *
 * The table row's INDEX is the key's slot: its bit in the seen-mask and its index in
 * the parsed-value array (#1799).
 */
struct ace_key_t {
    std::string_view key; /**< @brief The `NAME` key spelling. */
    std::size_t width;    /**< @brief Max payload bytes; @ref kAceOpaque for `subject`. */
};

/**
 * @brief Width marker for the opaque `subject` token (ADR-0018): any TLV type is
 *        accepted and only non-emptiness is checked.
 */
inline constexpr std::size_t kAceOpaque = std::numeric_limits<std::size_t>::max();

/** @brief Every ACE key, in slot order; anything else is rejected (#906). */
inline constexpr std::array<ace_key_t, 5> kAceKeys{{
    {"type", sizeof(std::uint8_t)},
    {"flags", sizeof(std::uint8_t)},
    {"subject", kAceOpaque},
    {"access_mask", sizeof(std::uint32_t)},
    {"expires_ns", sizeof(std::uint64_t)},
}};

inline constexpr std::size_t kAceType = 0;    /**< @brief `type` slot in @ref kAceKeys. */
inline constexpr std::size_t kAceFlags = 1;   /**< @brief `flags` slot in @ref kAceKeys. */
inline constexpr std::size_t kAceSubject = 2; /**< @brief `subject` slot in @ref kAceKeys. */
inline constexpr std::size_t kAceMask = 3;    /**< @brief `access_mask` slot in @ref kAceKeys. */
inline constexpr std::size_t kAceExpires = 4; /**< @brief `expires_ns` slot in @ref kAceKeys. */

/** @brief Seen-mask bits of the required keys: `type`, `subject`, `access_mask`. */
inline constexpr unsigned kAceRequired = (1U << kAceType) | (1U << kAceSubject) | (1U << kAceMask);

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
        // The wildcard spelling is reserved (#908): a subject that spells it is not a
        // principal, so it matches nothing here. Hoisted out of the loop — one compare per
        // evaluation, not per ACE.
        if (is_reserved_subject(subject)) return acl_verdict_t::NO_MATCH;
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
        if (is_reserved_subject(subject)) return acl_verdict_t::NO_MATCH;  // #908, as above
        for (const ace_t& ace : aces) {
            if (!detail_acl::ace_applies(ace, subject, bit, now, required_flags)) continue;
            return ace.type == ace_type_t::DENY ? acl_verdict_t::DENY : acl_verdict_t::ALLOW;
        }
        return acl_verdict_t::NO_MATCH;
    }
};

// The `acl_policy_t` binding lives in libtracer/config.hpp (ADR-0068): an override fragment
// rebinds the alias there as plain C++ — no `#if` in this header. Both
// policy structs above are always compiled and unit-tested regardless of the binding.

/**
 * @brief One vertex's EFFECTIVE ACL (ADR-0020/0050): its own ACEs plus the
 *        INHERIT-flagged ancestor ACEs, pre-merged in evaluation order.
 *
 * The pure owner of the effective-ACL semantics that previously lived inline in
 * `graph_t::acl_allows`: build the merged list with @ref append_own (the target's
 * ACEs, stored order) followed by one @ref append_ancestor per ancestor,
 * NEAREST-FIRST (which filters to `kAceInherit` — a non-INHERIT ancestor ACE
 * applies to that vertex only), then evaluate with @ref allows. Pure like the
 * policies it drives: no graph access, no locks, no clock reads of its own —
 * unit-testable with synthetic ACE lists.
 *
 * The merged list is what the graph caches per vertex (the ADR-0050 cached
 * effective-ACE merge): only the MERGE is cached, never a verdict — `expires_ns`
 * is evaluated against the caller's `now` at check time, so expiry needs no
 * invalidation.
 */
class effective_acl_t {
   public:
    /** @brief Append the target vertex's own ACEs (all of them, stored order).
     *  @note Call BEFORE any @ref append_ancestor — own ACEs evaluate first
     *        (the effective-ACL ordering of ADR-0020). */
    void append_own(std::span<const ace_t> aces) {
        merged_.insert(merged_.end(), aces.begin(), aces.end());
    }

    /**
     * @brief Append one ancestor's ACEs, keeping only the `kAceInherit`-flagged
     *        ones (a non-INHERIT ACE applies to that vertex only, ADR-0020).
     *
     * Call once per strict ancestor, NEAREST-FIRST, so the full policy's
     * first-match-per-bit ordering follows the effective-ACL definition.
     */
    void append_ancestor(std::span<const ace_t> aces) {
        for (const ace_t& ace : aces)
            if ((ace.flags & kAceInherit) != 0) merged_.push_back(ace);
    }

    /** @brief The merged effective-ACE list, in evaluation order. */
    [[nodiscard]] const std::vector<ace_t>& merged() const noexcept { return merged_; }

    /** @brief Move the merged list out (what the graph stores in its per-vertex cache). */
    [[nodiscard]] std::vector<ace_t> release() noexcept { return std::move(merged_); }

    /**
     * @brief The final ACL verdict over a pre-merged effective-ACE list.
     *
     * Hands @p merged to the pure @p Policy ONCE (the merge already applied the
     * per-list `required_flags` filtering and the own-before-ancestors order, so
     * one pass is verdict-identical to the per-list walk) and applies the
     * open-by-default rule: no effective ACE at all ⇒ allowed (enforcement is
     * opt-in via ACL presence); ANY present ACE — even an expired one — closes
     * the vertex, so `NO_MATCH` over a non-empty list denies.
     *
     * @param merged  A list built by @ref append_own / @ref append_ancestor
     *                (or this instance's @ref merged, via the member overload).
     * @param subject The resolved subject token bytes (ADR-0018).
     * @param bit     The requested right (one `acl_right_t` bit).
     * @param now     Check-time wall clock, ns since the UNIX epoch.
     * @param required_flags ACEs lacking these bits are skipped, exactly as
     *                `Policy::allows` skips them: `0` evaluates the whole list (the
     *                bearer's own check), `kAceInherit` evaluates the inheritable
     *                **subsequence** — what a BARE descendant sees. Filtering in place
     *                rather than against a pre-projected copy is what lets the merge be
     *                stored once; it is order-identical by construction, since skipping
     *                elements cannot reorder the ones that remain, which matters because
     *                the full policy is first-match-per-bit in stored order.
     * @return true iff @p subject may exercise @p bit.
     */
    template <class Policy = acl_policy_t>
    [[nodiscard]] static bool allows(std::span<const ace_t> merged,
                                     std::span<const std::byte> subject, std::uint32_t bit,
                                     std::uint64_t now, std::uint8_t required_flags = 0) noexcept {
        const acl_verdict_t verdict = Policy::allows(subject, bit, merged, now, required_flags);
        if (verdict == acl_verdict_t::ALLOW) return true;
        if (verdict == acl_verdict_t::DENY) return false;
        // Open by default; any ACE that PASSES the flag filter closes. The filter must
        // apply here too: a bearer holding only NON-inheritable ACEs presents an empty
        // inheritable subsequence, so a bare descendant stays open — which is exactly what
        // testing a pre-projected (and therefore empty) list used to yield.
        if (required_flags == 0) return merged.empty();
        return std::none_of(merged.begin(), merged.end(), [required_flags](const ace_t& a) {
            return (a.flags & required_flags) == required_flags;
        });
    }

    /** @brief The final ACL verdict over THIS instance's merged list (the static
     *         @ref allows over @ref merged). */
    template <class Policy = acl_policy_t>
    [[nodiscard]] bool allows(std::span<const std::byte> subject, std::uint32_t bit,
                              std::uint64_t now) const noexcept {
        return allows<Policy>(merged_, subject, bit, now);
    }

   private:
    std::vector<ace_t> merged_; /**< @brief Effective ACEs, evaluation order. */
};

namespace detail_acl {

/** @brief Append @p ace to a `std::vector` table. */
inline void push_ace(std::vector<ace_t>& out, ace_t&& ace) { out.push_back(std::move(ace)); }
/** @brief Append @p ace to a core table the caller reserved, so the append cannot be refused. */
inline void push_ace(mem::block_array_t<ace_t>& out, ace_t&& ace) noexcept {
    (void)out.push_back(std::move(ace));
}

/** @brief The one body of both @ref parse_acl spellings: @p out is cleared, then filled. */
template <class Policy, class Out>
[[nodiscard]] result_t<void> parse_acl_into(const wire::tlv_node_t& acl, Out& out) {
    using wire::tlv_node_t;
    using wire::type_t;
    out.clear();
    for (const tlv_node_t entry : acl.children()) {
        if (entry.type() != type_t::ACL || !entry.opt().pl)
            return std::unexpected(status_t::TYPE_MISMATCH);
        unsigned seen = 0;
        std::array<std::span<const std::byte>, detail_acl::kAceKeys.size()> raw{};
        const wire::tlv_children_t ch = entry.children();
        for (auto it = ch.begin(); it != ch.end();) {
            // Key slot. Pair-consuming (#927): the walk steps PAST the value below, so a
            // NAME-typed value is never resynchronized onto as the next key.
            const tlv_node_t key = *it;
            // Positional (NAME key, value) pairs: a key with no value is an odd count — a
            // trailing key whose value the sender believes it wrote.
            if (++it == ch.end() || key.type() != type_t::NAME)
                return std::unexpected(status_t::TYPE_MISMATCH);
            const tlv_node_t val = *it;
            ++it;
            const auto row =
                std::ranges::find(detail_acl::kAceKeys, tr::detail::as_string_view(key.payload()),
                                  &detail_acl::ace_key_t::key);
            const auto k = static_cast<std::size_t>(row - detail_acl::kAceKeys.begin());
            // REJECT, never skip: an unknown key (ignoring it would drop a restrictive
            // attribute a newer writer meant to apply), a repeated key, a numeric key
            // whose value is not a VALUE (a dropped `expires_ns` turns a time-limited
            // grant permanent), and an empty or over-wide payload. The `subject` token
            // is opaque bytes (ADR-0018): any TLV type — VALUE recommended, NAME for the
            // "EVERYONE@" spelling — and a structured value reads as an empty payload.
            if (k == detail_acl::kAceKeys.size() || (seen & (1U << k)) != 0 ||
                (val.type() != type_t::VALUE && row->width != detail_acl::kAceOpaque) ||
                !detail_acl::ace_field_ok(val.payload(), row->width))
                return std::unexpected(status_t::TYPE_MISMATCH);
            seen |= 1U << k;
            raw[k] = val.payload();
        }
        // `load_le` zero-extends a narrower payload exactly, and an absent optional
        // field loads as its 0 default.
        const std::uint64_t type = tr::detail::load_le<std::uint64_t>(raw[detail_acl::kAceType]);
        const std::uint64_t flags = tr::detail::load_le<std::uint64_t>(raw[detail_acl::kAceFlags]);
        // ALLOW=0 / DENY=1, DENY only where the policy evaluates it (the highest type
        // accepted is `kAcceptsDeny`); and the single INHERIT flag bit only
        // (INHERIT_ONLY/NO_PROPAGATE/GROUP would be silently mis-evaluated by the
        // merge) — never store semantics the evaluator would silently weaken.
        if ((seen & detail_acl::kAceRequired) != detail_acl::kAceRequired ||
            type > std::uint64_t{Policy::kAcceptsDeny} ||
            (flags & ~std::uint64_t{kAceInherit}) != 0)
            return std::unexpected(status_t::TYPE_MISMATCH);
        ace_t ace;
        ace.type = static_cast<ace_type_t>(type);
        ace.flags = static_cast<std::uint8_t>(flags);
        ace.subject.assign(raw[detail_acl::kAceSubject].begin(),
                           raw[detail_acl::kAceSubject].end());
        ace.access_mask = tr::detail::load_le<std::uint32_t>(raw[detail_acl::kAceMask]);
        ace.expires_ns = tr::detail::load_le<std::uint64_t>(raw[detail_acl::kAceExpires]);
        push_ace(out, std::move(ace));  // the core-array caller reserved one slot per entry
    }
    return {};
}

}  // namespace detail_acl

/**
 * @brief Parse a decoded `:acl` ACL TLV into typed ACEs (docs/reference/05 §0x0A).
 *
 * STRICT by construction, because an ACL is a security document: a shape the builder
 * never emits is rejected with `TYPE_MISMATCH` at write time rather than read
 * leniently, since leniency here does not lose a field — it INVERTS or WIDENS a grant
 * (#906). Rejected, per ACE:
 *
 * - a DENY ACE under a policy that cannot evaluate one (`Policy::kAcceptsDeny`), and
 *   any flag bit beyond `kAceInherit` — the inheritance-only subset both adapters honor
 *   today; richer NFSv4 flags gate on the graph's merge honoring them first;
 * - a missing `type` / `subject` / `access_mask`, or an empty `subject` token;
 * - a numeric field whose payload is empty or wider than the field
 *   (`%detail_acl::ace_field_ok` — `type`/`flags` u8, `access_mask` u32,
 *   `expires_ns` u64), which is where a big-endian u16 `type` of `0x0001` used to
 *   truncate from DENY to ALLOW;
 * - a KNOWN key carrying the wrong value TLV type — rejected, never skipped: a
 *   dropped `expires_ns` turns a time-limited grant permanent;
 * - an UNKNOWN key, a repeated key, a non-`NAME` child in a key slot, and an odd child
 *   count (a key with no value, or a value with no key).
 *
 * The walk is **pair-consuming**, the mechanics of `wire::config_reader_t` (#927): it
 * steps one whole `(NAME key, value)` pair at a time, so a value can never be re-read
 * as the next key — which a `subject` sent as a `NAME` (a spelling this function
 * accepts, for `EVERYONE@`) previously could be. The unknown-key ruling is the
 * OPPOSITE of that reader's, deliberately: config is where a newer peer legitimately
 * sends more than the receiver understands, so it skips the pair; an ACL is not, so a
 * silently dropped attribute would widen access.
 *
 * Every key runs through the same checks, driven by one `{key, width}` table
 * (`%detail_acl::kAceKeys`) and a seen-bitmask, so the required-field rule is a single
 * compare against `%detail_acl::kAceRequired` (#1799).
 *
 * @tparam Policy The accepting policy (defaults to the target's selection).
 * @param acl A validated ACL node (`ACL{ ACL{NAME/VALUE…}* }`), walked in place (#1829).
 * @return The typed ACE list, in wire order, or `TYPE_MISMATCH`.
 */
template <class Policy = acl_policy_t>
[[nodiscard]] result_t<std::vector<ace_t>> parse_acl(const wire::tlv_node_t& acl) {
    std::vector<ace_t> out;
    if (const result_t<void> r = detail_acl::parse_acl_into<Policy>(acl, out); !r)
        return std::unexpected(r.error());
    return out;
}

/**
 * @brief @ref parse_acl into a core array (#1781): @p out is replaced by the typed ACEs in
 *        wire order. The same strict shape rules; the table is drawn from @p out's source.
 * @retval status_t::TYPE_MISMATCH The ACL is not a shape @ref parse_acl accepts.
 * @retval status_t::BACKPRESSURE @p out's source refused the table.
 * On any refusal @p out holds no ACE of this ACL.
 */
template <class Policy = acl_policy_t>
[[nodiscard]] result_t<void> parse_acl(const wire::tlv_node_t& acl,
                                       mem::block_array_t<ace_t>& out) {
    // One slot per entry, reserved up front: the parse below then cannot be refused midway.
    std::size_t entries = 0;
    for ([[maybe_unused]] const wire::tlv_node_t entry : acl.children()) ++entries;
    out.clear();
    if (!out.reserve(entries)) return std::unexpected(status_t::BACKPRESSURE);
    const result_t<void> r = detail_acl::parse_acl_into<Policy>(acl, out);
    if (!r) out.clear();
    return r;
}

namespace detail_acl {

/** @brief Wire bytes of a `(NAME key, VALUE)` pair whose value body is @p value_len bytes. */
[[nodiscard]] constexpr std::size_t pair_bytes(std::string_view key,
                                               std::size_t value_len) noexcept {
    return wire::header_bytes(wire::opt_t{}) + key.size() +
           wire::header_bytes(wire::opt_t{.ll = value_len > 0xFFFFu}) + value_len;
}

/** @brief Body bytes of one encoded ACE entry — what @ref encode_acl writes inside its `ACL`. */
[[nodiscard]] constexpr std::size_t ace_entry_bytes(const ace_t& ace) noexcept {
    return pair_bytes("type", 1) + pair_bytes("flags", 1) +
           pair_bytes("subject", ace.subject.size()) + pair_bytes("access_mask", 4) +
           (ace.expires_ns != 0 ? pair_bytes("expires_ns", 8) : 0);
}

/** @brief Append one `(NAME key, VALUE u<width> LE)` pair.  @retval false The source refused. */
[[nodiscard]] inline bool put_u(mem::bytes_t& out, std::string_view key, std::uint64_t v,
                                std::size_t width) noexcept {
    std::array<std::byte, 8> le{};
    tr::detail::store_le(std::span<std::byte>(le), v, width);
    return wire::emit_name(out, key) && wire::emit_tlv(out, wire::type_t::VALUE, wire::opt_t{},
                                                       std::span<const std::byte>(le).first(width));
}

}  // namespace detail_acl

/**
 * @brief Encode typed ACEs as the wire `ACL{ ACL{…}* }` TLV bytes, appended to a core byte
 *        array (#1781) — the typed builder, the inverse of @ref parse_acl.
 *
 * Emits NAME-tagged `type`(u8) / `flags`(u8) / `subject`(opaque VALUE) /
 * `access_mask`(u32) children, plus `expires_ns`(u64) when non-zero, per
 * docs/reference/05 §0x0A. Encoding is unvalidated by design (tests build
 * deliberately-rejectable ACLs with it); @ref parse_acl is the gate. The bytes are sized
 * first and written in one reservation, so a refusal appends nothing.
 * @retval false The source refused the bytes; @p out is unchanged.
 */
[[nodiscard]] inline bool encode_acl(std::span<const ace_t> aces, mem::bytes_t& out) noexcept {
    using wire::opt_t;
    using wire::type_t;
    std::size_t body = 0;
    for (const ace_t& ace : aces) {
        const std::size_t entry = detail_acl::ace_entry_bytes(ace);
        body += wire::header_bytes(opt_t{.ll = entry > 0xFFFFu}) + entry;
    }
    if (!out.reserve(out.size() + wire::header_bytes(opt_t{.ll = body > 0xFFFFu}) + body))
        return false;
    // Reserved above, so no append below can be refused: the `&&` chain only sequences them.
    bool ok = wire::emit_header(out, type_t::ACL, opt_t{.pl = true, .ll = body > 0xFFFFu}, body);
    for (const ace_t& ace : aces) {
        const std::size_t entry = detail_acl::ace_entry_bytes(ace);
        ok = ok &&
             wire::emit_header(out, type_t::ACL, opt_t{.pl = true, .ll = entry > 0xFFFFu}, entry) &&
             detail_acl::put_u(out, "type", static_cast<std::uint8_t>(ace.type), 1) &&
             detail_acl::put_u(out, "flags", ace.flags, 1) && wire::emit_name(out, "subject") &&
             wire::emit_tlv(out, type_t::VALUE, opt_t{}, ace.subject) &&
             detail_acl::put_u(out, "access_mask", ace.access_mask, 4) &&
             (ace.expires_ns == 0 || detail_acl::put_u(out, "expires_ns", ace.expires_ns, 8));
    }
    return ok;
}

/**
 * @brief The vector spelling of @ref encode_acl, kept until the #1781 contract step: the
 *        core-array form plus one copy.
 * @return The ACL bytes; EMPTY when the heap refused them (an encoded ACL is never empty).
 */
[[nodiscard]] inline std::vector<std::byte> encode_acl(std::span<const ace_t> aces) {
    mem::bytes_t out(mem::heap_source());
    if (!encode_acl(aces, out)) return {};
    return std::vector<std::byte>(out.begin(), out.end());
}

}  // namespace tr::graph
