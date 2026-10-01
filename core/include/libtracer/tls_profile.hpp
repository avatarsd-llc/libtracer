/**
 * @file
 * @brief tls_profile_t — app-owned TLS trust material for the TLS transport kinds
 *        (`quic`, `webtransport`), selected by name from a creation SPEC.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A creation SPEC can arrive from any writer, a remote one included, so the files a
 * TLS link trusts and serves are never read out of it. The application owns them: it
 * builds a table of named profiles and hands that table to the kind's factory at
 * registration. A SPEC may at most NAME one of those profiles (the kind-private `tls`
 * config key); it never carries a path. A name the table does not hold is refused.
 *
 * The table is a span over app-owned storage: the factory copies nothing and keeps no
 * buffer of its own, and a `constexpr` table costs no RAM at all. The strings are views,
 * so the storage behind them must outlive every transport the factory constructs (the
 * same lifetime rule the factory's `rx_backend` already carries).
 */
#pragma once

#include <span>
#include <string_view>

namespace tr::net {

/**
 * @brief One app-registered TLS profile: the trust anchor a DIAL verifies against and
 *        the credential a LISTEN serves, under the name a SPEC's `tls` key selects.
 *
 * A profile may fill only the half its links use: a dial-only profile leaves the
 * credential empty, a listen-only one leaves the anchor empty.
 */
struct tls_profile_t {
    std::string_view name;      /**< @brief The value a SPEC's `tls` key selects this profile
                                            by. The EMPTY name is the default profile: the
                                            one a SPEC that carries no `tls` key gets. */
    std::string_view ca_file;   /**< @brief DIAL: PEM CA-bundle path the peer's certificate
                                            is verified against; empty = the system trust
                                            store. */
    std::string_view cert_file; /**< @brief LISTEN: PEM server-certificate path. A LISTEN
                                            whose profile leaves it empty is refused. */
    std::string_view key_file;  /**< @brief LISTEN: PEM private-key path matching
                                            @ref cert_file. */
};

/**
 * @brief The profile in @p profiles named @p name, or nullptr when the table holds none.
 *
 * First match wins, so a table that repeats a name keeps its earlier entry. A linear
 * scan, run once per connection creation and never on the frame path.
 *
 * @param profiles The app's table (the factory's view of it).
 * @param name     The SPEC's `tls` value; empty when the key is absent.
 * @return The matching profile, or nullptr.
 */
[[nodiscard]] constexpr const tls_profile_t* find_tls_profile(
    std::span<const tls_profile_t> profiles, std::string_view name) noexcept {
    for (const tls_profile_t& p : profiles)
        if (p.name == name) return &p;
    return nullptr;
}

}  // namespace tr::net
