/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * config_reader — typed accessors over a positional NAME-key / typed-value PAIR container.
 * The pair walk was copied verbatim by all SIX transport-side consumers of a connection
 * config — transport_vertex's universal keys, the tcp / ws / can factories, and the quic
 * and webtransport factories' cert/key — and this is their one home. Each factory still
 * reads ONLY its own keys from the raw config TLV it receives (ADR-0043 §5 leanness:
 * kind-private keys never land in the shared conn_settings_t) — what is shared is the
 * walk, not the vocabulary.
 *
 * The type lives in `tr::wire` (#985): it decodes a `wire::tlv_t`'s children, and the L4
 * readers of the same grammar — `graph_t::create_child` (the creation SPEC) and
 * `parse_subscriber_tlv` (the SUBSCRIBER QoS SETTINGS) — may not depend on `tr::net`
 * (dependencies point up the layers only). Before the hoist those two carried the
 * pair-consuming RULE (#927) as hand-written copies with a comment pointing here, which is
 * exactly the duplication that let #927 survive in six places and then in a seventh. The
 * `tr::net::config_reader_t` spelling remains as an alias so the transport call sites and
 * the public API did not move. `graph::parse_acl` still carries its own walk on purpose,
 * as of #906, under the OPPOSITE unknown-key ruling (reject, not ignore): same mechanics,
 * different disposition, because an ACL is a security document and a connection config is
 * not (#995: security readers reject duplicates; plain NAME-field readers are
 * pair-consuming + last-wins).
 */
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"

/**
 * @file
 * @brief The pair-consuming NAME-key / typed-value walk: `wire::config_reader_t`
 *        (aliased as `tr::net::config_reader_t` for the transport plane).
 */

namespace tr::wire {

/**
 * @brief Typed accessors over a positional-pair TLV's children — a SPEC `config`
 *        SETTINGS, a SUBSCRIBER QoS SETTINGS, or the creation-SPEC envelope itself.
 *
 * The layout is positional NAME-key / value pairs: a `NAME` child carrying the key
 * string, immediately followed by the value child — a `NAME` for string values, a
 * `VALUE` for integers/flags, or a nested `SETTINGS` for a module namespace.
 * Unknown keys are ignored (forward-compat), a key whose value child has the wrong
 * type (or an empty `VALUE` payload) is ignored too, and when a key appears more
 * than once the LAST well-formed occurrence wins.
 *
 * The walk is **pair-consuming**: it advances a whole pair at a time, so an
 * unknown key is skipped together with its value and no value child can ever be
 * re-read as a key (#927). Forward-compat tolerance is deliberate here, and is
 * the OPPOSITE ruling from the `graph::parse_acl` walk (#906), which is to
 * REJECT an unknown key: config is where a newer peer legitimately sends more
 * than a receiver understands, whereas an ACL is a security document in which a
 * silently dropped attribute widens access.
 *
 * @note The returned string_views/spans (and the reader itself) borrow the decoded
 *       TLV's storage — use them while the `tlv_t` is alive.
 */
class config_reader_t {
   public:
    /**
     * @brief Construct over @p config's children.
     *
     * @param config The decoded pair-container TLV; nullptr = no config (every
     *               accessor returns nullopt / nullptr).
     */
    explicit config_reader_t(const tlv_t* config) noexcept : config_(config) {}

    /** @brief The string value of @p key (a `NAME` value child), if present. */
    [[nodiscard]] std::optional<std::string_view> name(std::string_view key) const noexcept {
        const tlv_t* val = find(key, type_t::NAME);
        if (val == nullptr) return std::nullopt;
        return detail::as_string_view(val->payload);
    }

    /**
     * @brief The raw payload bytes of @p key (a `NAME` value child), if present.
     *
     * The byte-span twin of name() for a value that is a wire segment rather than
     * text — `graph_t::create_child` reads the creation SPEC's "name" this way,
     * because the child name is appended to the parent's key verbatim.
     */
    [[nodiscard]] std::optional<std::span<const std::byte>> name_bytes(
        std::string_view key) const noexcept {
        const tlv_t* val = find(key, type_t::NAME);
        if (val == nullptr) return std::nullopt;
        return val->payload;
    }

    /**
     * @brief The nested `SETTINGS` value child of @p key, or nullptr.
     *
     * A module namespace ("config" in the creation SPEC, a per-transport block in a
     * connection config). Borrowed from the decoded TLV, same as every accessor.
     */
    [[nodiscard]] const tlv_t* settings(std::string_view key) const noexcept {
        return find(key, type_t::SETTINGS);
    }

    /** @brief The u8 value of @p key (a non-empty `VALUE` child), if present. */
    [[nodiscard]] std::optional<std::uint8_t> u8(std::string_view key) const noexcept {
        return value_as<std::uint8_t>(key);
    }

    /** @brief The u16 value of @p key (a non-empty `VALUE` child), if present. */
    [[nodiscard]] std::optional<std::uint16_t> u16(std::string_view key) const noexcept {
        return value_as<std::uint16_t>(key);
    }

    /** @brief The u32 value of @p key (a non-empty `VALUE` child), if present. */
    [[nodiscard]] std::optional<std::uint32_t> u32(std::string_view key) const noexcept {
        return value_as<std::uint32_t>(key);
    }

    /** @brief The boolean value of @p key: a non-empty `VALUE` child read as
     *         u8, nonzero = true. */
    [[nodiscard]] std::optional<bool> flag(std::string_view key) const noexcept {
        const std::optional<std::uint8_t> v = u8(key);
        if (!v) return std::nullopt;
        return *v != 0;
    }

   private:
    /** @brief Decode @p key's `VALUE` payload little-endian as @p T. */
    template <class T>
    [[nodiscard]] std::optional<T> value_as(std::string_view key) const noexcept {
        const tlv_t* val = find(key, type_t::VALUE);
        if (val == nullptr) return std::nullopt;
        return detail::load_le<T>(val->payload);
    }

    /**
     * @brief The last well-formed value child for @p key, or nullptr.
     *
     * **Pair-consuming** (#927): the walk steps two children at a time over
     * `(NAME key, value)` pairs and advances past the value it consumed, so a
     * value child is never re-read as the next position's key. An unknown key
     * is skipped as a WHOLE pair — which is exactly what lets forward-compat
     * tolerance coexist with positional pairing. The every-offset scan this
     * replaces made the grammar ambiguous: a pair whose string value textually
     * equalled a known key (`link_hint = "addr"`) silently bound the FOLLOWING
     * child as that key's value, and last-match-wins then overrode a legitimate
     * earlier occurrence.
     *
     * Within the matching pairs the pre-existing semantics are unchanged: a
     * value child of the wrong type (or an empty `VALUE` payload) is ignored
     * and does not clobber an earlier well-formed occurrence, and the last
     * well-formed occurrence wins.
     *
     * A child that is not a `NAME` where a key belongs desynchronizes the pair
     * stream, and the walk stops there rather than guessing a resync —
     * resynchronizing on every offset is the defect above. A trailing unpaired
     * key is ignored.
     */
    [[nodiscard]] const tlv_t* find(std::string_view key, type_t value_type) const noexcept {
        if (config_ == nullptr) return nullptr;
        const std::vector<tlv_t>& ch = config_->children;
        const tlv_t* found = nullptr;
        for (std::size_t i = 0; i + 1 < ch.size(); i += 2) {
            // Key slot. Anything but a NAME means the pairing is lost from here on.
            if (ch[i].type != type_t::NAME) break;
            // Not our key: skip the whole pair, value slot included (forward-compat).
            if (detail::as_string_view(ch[i].payload) != key) continue;
            const tlv_t& val = ch[i + 1];
            if (val.type != value_type) continue;
            if (value_type == type_t::VALUE && val.payload.empty()) continue;
            found = &val;
        }
        return found;
    }

    const tlv_t* config_; /**< @brief The pair-container TLV (nullable, borrowed). */
};

}  // namespace tr::wire

namespace tr::net {

/**
 * @brief The transport plane's historical spelling of @ref tr::wire::config_reader_t.
 *
 * The type moved down to `tr::wire` (#985) so the L4 readers of the same grammar could
 * share it without depending on the transport plane; the six transport-side call sites
 * and the public API keep this name.
 */
using config_reader_t = wire::config_reader_t;

}  // namespace tr::net
