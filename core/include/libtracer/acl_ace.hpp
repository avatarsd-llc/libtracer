/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The ACE RECORDS of a vertex's `:acl` (ADR-0020 / #81): the right bits, the ACE type,
 * the one honored flag, the parsed entry itself, and the owning table a vertex stores them
 * in. Data only — no evaluation, no wire codec, nothing above the core containers.
 *
 * Why this is its own header and not part of `security_acl.hpp`, which is where #868 first
 * put it. The records are UPSTREAM of the graph vertex: `acl_state_t` stores an
 * `ace_table_t`, so whatever declares them is compiled by every net-plane TU. The
 * evaluation and the wire codec are DOWNSTREAM — `graph_t::acl_allows` and the `:acl`
 * field-write door call them, nothing in the vertex core does. Folding both halves into one
 * header forces the downstream half upstream: measured on this tree, it took
 * `security_acl.hpp` from 15 dependent TUs to 100, so editing an ACL evaluation rule would
 * have rebuilt the whole tree — the opposite of what #868 exists to achieve.
 *
 * So there are two ACL headers, and the seam between them is the real shape of the
 * dependency, not a compromise. What #868 actually asked for is satisfied: the ACE data has
 * ONE home and no longer straddles `vertex.hpp` and `security_acl.hpp`.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "libtracer/mem_source.hpp"

/**
 * @file
 * @brief `tr::graph` ACE records — the ACL data model (ADR-0020, docs/reference/05 §0x0A).
 */

namespace tr::graph {

/**
 * @brief One right bit of an ACE `access_mask` (docs/reference/05 §0x0A, RFC-0036 §3.1).
 *
 * Two rights, and nothing else: a `:` field is a path under its vertex and is gated by the
 * vertex's own READ and WRITE (RFC-0036 §3.2). Single-bit values so a gate tests exactly one
 * right. A mask bit outside @ref kAceRights is refused when the ACL is installed. Writing
 * `:acl` is not a right: only the owner, the empty local caller, may do it (RFC-0036 §3.3).
 */
enum class acl_right_t : std::uint32_t {
    READ = 0x01,  /**< @brief Read the vertex value and its `:` fields, and subscribe to it. */
    WRITE = 0x02, /**< @brief Write the vertex value and its `:` fields, and create under it. */
};

/** @brief Every right bit an ACE may carry (RFC-0036 §3.1); any other bit is refused at install. */
inline constexpr std::uint32_t kAceRights = 0x03;

/** @brief The one ACE flag the core subset honors: propagate to the subtree (ADR-0020). */
inline constexpr std::uint8_t kAceInherit = 0x1;

/** @brief An ACE's type (ADR-0020): ALLOW grants; DENY refuses (full policy only). */
enum class ace_type_t : std::uint8_t {
    ALLOW = 0, /**< @brief The ACE grants its mask's rights. */
    DENY = 1,  /**< @brief The ACE refuses them — evaluated only by `full_acl_policy_t`
                    (ADR-0050); the ALLOW-only profile rejects DENY at parse time. */
};

/**
 * @brief An ACE's subject token, VIEWED (#2287): a `std::span<const std::byte>` that refuses
 *        to be built from a temporary container.
 *
 * `std::span<const std::byte>` binds to an rvalue `std::vector<std::byte>` and dangles at the
 * end of the statement, which for an ACE written as `{.subject = bytes_of("alice")}` is before
 * the ACE is ever read. This view deletes that one conversion, so the mistake is a compile
 * error; a span, an lvalue container and a borrowed range all convert as before.
 */
class subject_view_t : public std::span<const std::byte> {
   public:
    /** @brief The empty subject. */
    constexpr subject_view_t() noexcept = default;
    /** @brief View @p s. */
    constexpr subject_view_t(
        std::span<const std::byte> s) noexcept  // NOLINT(google-explicit-constructor)
        : std::span<const std::byte>(s) {}
    /** @brief View a container that outlives the statement — an lvalue or a borrowed range. */
    template <class R>
        requires std::is_convertible_v<const R&, std::span<const std::byte>>
    constexpr subject_view_t(const R& r) noexcept  // NOLINT(google-explicit-constructor)
        : std::span<const std::byte>(r) {}
    /** @brief Refused: a temporary container would be gone before the view is read. */
    template <class R>
        requires(!std::is_lvalue_reference_v<R> && !std::ranges::borrowed_range<R> &&
                 std::ranges::contiguous_range<R>)
    subject_view_t(R&&) = delete;
};

/**
 * @brief View the characters of @p s as a subject token — `{.subject = subject_of("alice")}`.
 *        The view is good for as long as the characters are (a literal's, forever).
 */
[[nodiscard]] inline subject_view_t subject_of(std::string_view s) noexcept {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

/**
 * @brief One ACE of a vertex's `:acl` (ADR-0020 / #81) — a record that VIEWS its subject.
 *
 * Evaluation is the pure per-target policy of ADR-0050 (`%security_acl.hpp`): the
 * default ALLOW-only MCU profile rejects a DENY ACE (or any flag bit beyond
 * `kAceInherit`) at write time with TYPE_MISMATCH, so stored ACEs never carry
 * semantics the selected evaluator would silently weaken; the full `security_acl`
 * host policy (`acl_policy_t = full_acl_policy_t`) stores DENY and evaluates ordered
 * first-match-per-bit.
 *
 * The subject is a view (#2287): an ACE parsed from an `:acl` value points into that value,
 * an ACE an application writes points at its own bytes, and an ACE a vertex stores points into
 * the @ref ace_table_t that holds it. So the record is trivially copyable and owns nothing —
 * whoever holds the bytes decides how long the view stays good.
 */
struct ace_t {
    ace_type_t type = ace_type_t::ALLOW; /**< @brief ALLOW or DENY (policy-gated at parse). */
    std::uint8_t flags = 0;              /**< @brief ACE flags; only `kAceInherit` is accepted. */
    subject_view_t subject;              /**< @brief Opaque subject token (ADR-0018), viewed;
                                              the special subject `"EVERYONE@"` matches any
                                              resolved subject. */
    std::uint32_t access_mask = 0; /**< @brief Granted rights (an OR of `acl_right_t` bits). */
    std::uint64_t expires_ns = 0;  /**< @brief Absolute expiry in ns on the APPLICATION's clock
                                        (the `now` its subject-lookup hook supplies, RFC-0037);
                                        0 = never expires. An expired ACE grants nothing. */
};

/**
 * @brief An OWNING ACE list (#2287): the ACEs and every subject token they view, in ONE block
 *        drawn from one source — what a vertex stores as its `:acl` and its cached merge.
 *
 * The block holds the ACEs first and their subject bytes after them, and each stored ACE's
 * @ref ace_t::subject views its own bytes in that tail, so the table is self-contained: a
 * caller's ACEs, or another table's, can be appended and then dropped. An append builds the
 * whole next block and swaps it in, so a refused one leaves the table as it was; appends happen
 * at `:acl` writes and cache rebuilds, never per operation. Reads are a `std::span<const ace_t>`
 * (the table converts to one), which is what every evaluator takes.
 */
class ace_table_t {
   public:
    /** @brief An empty table that draws its block from @p src. */
    explicit ace_table_t(mem::block_source_t& src) noexcept : slots_(src) {}

    /** @brief Non-copyable — a copy is a failable allocation; use @ref append. */
    ace_table_t(const ace_table_t&) = delete;
    /** @brief Non-assignable by copy. */
    ace_table_t& operator=(const ace_table_t&) = delete;
    /** @brief Move: the block changes hands, and @p o is left empty. */
    ace_table_t(ace_table_t&& o) noexcept
        : slots_(std::move(o.slots_)), n_(std::exchange(o.n_, 0)) {}
    /** @brief Move-assign: this table's block goes back to its source first. */
    ace_table_t& operator=(ace_table_t&& o) noexcept {
        slots_ = std::move(o.slots_);
        n_ = std::exchange(o.n_, 0);
        return *this;
    }
    /** @brief Hand the block back to its source. */
    ~ace_table_t() = default;

    /**
     * @brief Append, in order, every ACE of @p from that carries all of @p required_flags,
     *        copying its subject into this table — `0` appends them all, `kAceInherit` the
     *        inheritable ones (what a descendant's merge takes from an ancestor, ADR-0020).
     * @param from Must not view this table.
     * @retval false The source refused the block; the table is unchanged.
     */
    [[nodiscard]] bool append(std::span<const ace_t> from,
                              std::uint8_t required_flags = 0) noexcept {
        std::size_t n = n_;
        std::size_t text = 0;
        for (const ace_t& a : *this) text += a.subject.size();
        for (const ace_t& a : from) {
            if ((a.flags & required_flags) != required_flags) continue;
            ++n;
            text += a.subject.size();
        }
        if (n == n_) return true;
        mem::block_array_t<ace_t> next(slots_.source());
        if (!next.resize_for_overwrite(n + (text + sizeof(ace_t) - 1) / sizeof(ace_t)))
            return false;
        ace_t* out = next.data();
        auto* tail = reinterpret_cast<std::byte*>(next.data() + n);
        const auto put = [&out, &tail](const ace_t& a) {
            *out = a;
            if (!a.subject.empty()) std::memcpy(tail, a.subject.data(), a.subject.size());
            out->subject = std::span<const std::byte>(tail, a.subject.size());
            tail += a.subject.size();
            ++out;
        };
        for (const ace_t& a : *this) put(a);
        for (const ace_t& a : from)
            if ((a.flags & required_flags) == required_flags) put(a);
        slots_ = std::move(next);
        n_ = n;
        return true;
    }

    /** @brief Drop every ACE; the block is kept. */
    void clear() noexcept {
        slots_.clear();
        n_ = 0;
    }

    /** @brief The first ACE. */
    [[nodiscard]] const ace_t* begin() const noexcept { return slots_.data(); }
    /** @brief One past the last ACE. */
    [[nodiscard]] const ace_t* end() const noexcept { return slots_.data() + n_; }
    /** @brief The ACEs, contiguous. */
    [[nodiscard]] const ace_t* data() const noexcept { return slots_.data(); }
    /** @brief ACE @p i, unchecked. */
    [[nodiscard]] const ace_t& operator[](std::size_t i) const noexcept { return slots_[i]; }
    /** @brief How many ACEs the table holds. */
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    /** @brief True when the table holds no ACE. */
    [[nodiscard]] bool empty() const noexcept { return n_ == 0; }
    /** @brief The source the block is drawn from. */
    [[nodiscard]] mem::block_source_t& source() const noexcept { return slots_.source(); }

   private:
    mem::block_array_t<ace_t> slots_; /**< @brief The ACEs, then their subject bytes. */
    std::size_t n_ = 0;               /**< @brief How many leading slots are ACEs. */
};

/**
 * @brief The `now` an ACL gate evaluates with when the application supplied none (RFC-0037).
 *
 * The subject-lookup hook starts with its `now_ns` out-parameter at this value and the
 * application overwrites it with its own clock. Left here, no lease can be proved either
 * live or lapsed, so a leased ALLOW grants nothing and a leased DENY still refuses: the gate
 * fails closed both ways. A standing ACE (`expires_ns == 0`) never consults `now`.
 */
inline constexpr std::uint64_t kAceNowUnknown = ~std::uint64_t{0};

}  // namespace tr::graph
