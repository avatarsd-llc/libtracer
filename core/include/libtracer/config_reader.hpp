/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * config_reader — typed accessors over a SPEC `config` SETTINGS TLV. The
 * positional NAME-key / typed-value walk was copied verbatim by every consumer
 * of a connection config (transport_vertex's universal keys, the quic factory's
 * cert/key, the can factory's ifname/node/...); this is its one home. Each
 * factory still reads ONLY its own keys from the raw config TLV it receives
 * (ADR-0043 §5 leanness: kind-private keys never land in the shared
 * conn_settings_t) — what is shared is the walk, not the vocabulary.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"

/**
 * @file
 * @brief `tr::net` SPEC-config SETTINGS reader: `config_reader_t`.
 */

namespace tr::net {

/**
 * @brief Typed accessors over a SPEC `config` SETTINGS TLV's children.
 *
 * The SETTINGS layout is positional NAME-key / value pairs: a `NAME` child
 * carrying the key string, immediately followed by the value child — a `NAME`
 * for string values or a `VALUE` for integers/flags. Unknown keys are ignored
 * (forward-compat), a key whose value child has the wrong type (or an empty
 * `VALUE` payload) is ignored too, and when a key appears more than once the
 * LAST well-formed occurrence wins — exactly the semantics of the single-pass
 * walk this class replaces.
 *
 * @note The returned string_views (and the reader itself) borrow the decoded
 *       TLV's storage — use them while the `tlv_t` is alive.
 */
class config_reader_t {
   public:
    /**
     * @brief Construct over @p config's children.
     *
     * @param config The decoded SETTINGS TLV; nullptr = no config (every
     *               accessor returns nullopt).
     */
    explicit config_reader_t(const wire::tlv_t* config) noexcept : config_(config) {}

    /** @brief The string value of @p key (a `NAME` value child), if present. */
    [[nodiscard]] std::optional<std::string_view> name(std::string_view key) const noexcept {
        const wire::tlv_t* val = find(key, wire::type_t::NAME);
        if (val == nullptr) return std::nullopt;
        return detail::as_string_view(val->payload);
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
        const wire::tlv_t* val = find(key, wire::type_t::VALUE);
        if (val == nullptr) return std::nullopt;
        return detail::load_le<T>(val->payload);
    }

    /**
     * @brief The last well-formed value child for @p key, or nullptr.
     *
     * Scans every position (the pairs may overlap — a NAME value doubles as
     * the next position's key candidate, exactly as the original walk saw it):
     * a `NAME` child whose payload equals @p key, followed by a child of
     * @p value_type (non-empty when `VALUE`), yields that value child; the
     * last match wins.
     */
    [[nodiscard]] const wire::tlv_t* find(std::string_view key,
                                          wire::type_t value_type) const noexcept {
        if (config_ == nullptr) return nullptr;
        const std::vector<wire::tlv_t>& ch = config_->children;
        const wire::tlv_t* found = nullptr;
        for (std::size_t i = 0; i + 1 < ch.size(); ++i) {
            if (ch[i].type != wire::type_t::NAME) continue;
            if (detail::as_string_view(ch[i].payload) != key) continue;
            const wire::tlv_t& val = ch[i + 1];
            if (val.type != value_type) continue;
            if (value_type == wire::type_t::VALUE && val.payload.empty()) continue;
            found = &val;
        }
        return found;
    }

    const wire::tlv_t* config_; /**< @brief The SETTINGS TLV (nullable, borrowed). */
};

}  // namespace tr::net
