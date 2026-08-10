/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The ENCODER half of the connection-creation grammar, shipped next to the decoder that
 * consumes it (`transport_vertex_t::make_connection` / `config_reader_t`). Before this
 * header the library shipped only the decoder: every consumer that wanted to create a
 * connection — the production first-wiring step — hand-emitted the SPEC from
 * `wire::emit_tlv` / `wire::emit_name` and reached into `tr::detail::store_le` to encode
 * the port. Sixteen private near-copies existed, and they had already drifted: the
 * `tree_of_ropes` example's copy could not spell `kind` or `addr` at all, so the example
 * for "mount a transport" could not express the field that decides which MODULE the
 * connection mounts under.
 *
 * One encoder, one decoder, one grammar. This header adds no wire surface: it emits
 * exactly the bytes docs/modules/connection-config.md documents and `config_reader_t`
 * reads, and it is a builder for that existing grammar, not a change to it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport_vertex.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief `tr::net` connection-creation SPEC builder: @ref tr::net::conn_spec_t and the
 *        @ref tr::net::conn_spec one-liner.
 */

namespace tr::net {

/**
 * @brief Builds the `/net:children[]` connection-creation SPEC — the ONE encoder of the
 *        grammar @ref transport_vertex_t decodes.
 *
 * The payload is
 * `SPEC{ NAME "type" NAME <type>, NAME "name" NAME <name>, NAME "config" SETTINGS{ pairs } }`,
 * written to `/net:children[]` (ADR-0027, RFC-0014, reference/05). `<type>` is the catalog
 * child type — `"client"` (DIAL default) or `"listener"` (LISTEN default) — and `<name>` is
 * the connection's leaf NAME.
 *
 * **The config is built by APPENDING, in call order.** Each setter appends one
 * `(NAME key, value)` pair and returns `*this`, so a chain reads as the SETTINGS it emits
 * and a caller keeps byte-exact control of the order. The reader is order-independent
 * (@ref config_reader_t walks positional pairs and takes the last well-formed occurrence of
 * a key), so order is a byte-level property, not a semantic one. A builder on which no
 * setter ran emits **no `config` at all** — the config-less SPEC, which is the
 * @ref transport_vertex_t::provide_link spelling.
 *
 * **`kind` is how a SPEC names its module.** There is no `module` key and this builder does
 * not invent one: the module segment a connection mounts under is resolved from the
 * *(kind, role)* pair through the application's @ref transport_vertex_t::register_module
 * declaration, and that resolution runs BEFORE the staged-link lookup (#883). So
 * `.kind("ws").role(conn_role_t::DIAL)` selects the module declared for that pair — and a
 * `kind` with no declaration for that role is refused `SCHEMA_NOT_FOUND` rather than mounted
 * under a library-derived name (ADR-0073 §4). Omit `kind` only for the staged-link spelling,
 * where the module comes from the unique matching staging.
 *
 * **Universal vs kind-private keys.** The named setters cover exactly the universal keys
 * `transport_vertex_t` parses into @ref conn_settings_t. A transport kind's PRIVATE keys
 * (quic's `cert`/`key`/`ca`/`insecure`, the tcp/ws servers' `peer_named`/`max_peers`, can's
 * bus identity) never land on that shared record — they are the kind factory's business — so
 * they are spelled through the generic @ref text / @ref u8 / @ref u16 / @ref u32 / @ref flag
 * pairs, whose names mirror @ref config_reader_t's accessors so the encode and decode
 * vocabularies cannot drift.
 *
 * @note Nothing here validates a key against a kind: an unknown key is legal on the wire
 *       (the reader ignores unknown pairs, forward-compat) and a misspelled one is
 *       therefore silent. That is the grammar's property, documented in
 *       docs/modules/connection-config.md; a builder cannot fix it without knowing every
 *       kind's vocabulary, which is exactly the coupling ADR-0043 §5 forbids.
 */
class conn_spec_t {
   public:
    /**
     * @brief Begin a SPEC for catalog child type @p type naming the connection @p name.
     *
     * @param type The registered child type — `"client"` or `"listener"` for the built-in
     *             catalog; the type's default role may be overridden with @ref role.
     * @param name The connection's leaf NAME. It becomes the last segment of the
     *             `/net/<module>/<name>` mount, so it must be a valid path segment —
     *             `graph_t::create_child` gates that before this SPEC ever reaches
     *             `make_connection` (ADR-0073 §1).
     *
     * @note Both are encoded to bytes HERE, not stored: the builder holds two byte buffers
     *       and no `std::string`, so it never borrows a caller's storage (nothing to dangle)
     *       and a call site pays no string machinery for a control-plane payload.
     */
    conn_spec_t(std::string_view type, std::string_view name) {
        wire::emit_name(body_, "type");
        wire::emit_name(body_, type);
        wire::emit_name(body_, "name");
        wire::emit_name(body_, name);
    }

    /** @brief `role` — VALUE u8; overrides the catalog type's default direction. */
    conn_spec_t& role(conn_role_t value) { return u8("role", static_cast<std::uint8_t>(value)); }

    /** @brief `port` — VALUE u16; peer port on a DIAL, bind port on a LISTEN. */
    conn_spec_t& port(std::uint16_t value) { return u16("port", value); }

    /** @brief `kind` — NAME; the transport-factory selector AND the module selector. */
    conn_spec_t& kind(std::string_view value) { return text("kind", value); }

    /** @brief `addr` — NAME; the peer address (IPv4 dotted-quad) a DIAL connects to. */
    conn_spec_t& addr(std::string_view value) { return text("addr", value); }

    /** @brief `keepalive` — VALUE u32, in ms. Parsed into @ref conn_settings_t; no consumer. */
    conn_spec_t& keepalive_ms(std::uint32_t value) { return u32("keepalive", value); }

    /** @brief `max_frame` — VALUE u32; the per-connection inbound frame cap, in bytes. */
    conn_spec_t& max_frame(std::uint32_t value) { return u32("max_frame", value); }

    /** @brief `backoff` — VALUE u32, in ms; the RFC-0014 §4 self-heal retry interval. */
    conn_spec_t& backoff_ms(std::uint32_t value) { return u32("backoff", value); }

    /** @brief `connect_timeout` — VALUE u32, in ms; how long one dial attempt waits for UP. */
    conn_spec_t& connect_timeout_ms(std::uint32_t value) { return u32("connect_timeout", value); }

    /**
     * @brief Append an arbitrary NAME-valued pair — the kind-private escape hatch.
     *
     * The encode counterpart of `config_reader_t::name(key)`. Deliberately not called
     * `name`: this appends a config PAIR, whereas the connection's own NAME is a
     * constructor argument, and the two are one typo apart.
     */
    conn_spec_t& text(std::string_view key, std::string_view value) {
        wire::emit_name(cfg_, key);
        wire::emit_name(cfg_, value);
        return *this;
    }

    /** @brief Append an arbitrary VALUE u8 pair — see @ref text. */
    conn_spec_t& u8(std::string_view key, std::uint8_t value) {
        wire::emit_name(cfg_, key);
        wire::emit_value_le(cfg_, value);
        return *this;
    }

    /** @brief Append an arbitrary VALUE u16 pair — see @ref text. */
    conn_spec_t& u16(std::string_view key, std::uint16_t value) {
        wire::emit_name(cfg_, key);
        wire::emit_value_le(cfg_, value);
        return *this;
    }

    /** @brief Append an arbitrary VALUE u32 pair — see @ref text. */
    conn_spec_t& u32(std::string_view key, std::uint32_t value) {
        wire::emit_name(cfg_, key);
        wire::emit_value_le(cfg_, value);
        return *this;
    }

    /** @brief Append a boolean pair — a VALUE u8 of 0 or 1 (`config_reader_t::flag`). */
    conn_spec_t& flag(std::string_view key, bool value) {
        return u8(key, static_cast<std::uint8_t>(value ? 1 : 0));
    }

    /**
     * @brief The SPEC as raw wire bytes.
     *
     * The `config` SETTINGS is emitted only when at least one setter ran — an untouched
     * builder yields `SPEC{type, name}`, the staged-link spelling.
     */
    [[nodiscard]] std::vector<std::byte> bytes() const {
        std::vector<std::byte> body = body_;
        if (!cfg_.empty()) {
            wire::emit_name(body, "config");
            wire::emit_tlv(body, wire::type_t::SETTINGS, wire::opt_t{.pl = true}, cfg_);
        }
        std::vector<std::byte> out;
        wire::emit_tlv(out, wire::type_t::SPEC, wire::opt_t{.pl = true}, body);
        return out;
    }

    /**
     * @brief The SPEC as an owned @ref tr::view::view_t, ready to `write` to `/net:children[]`.
     *
     * @return An EMPTY view when the copy could not be allocated — the same
     *         degrade-don't-throw contract the rest of the control plane keeps
     *         (`view::over_bytes` is the seam). Writing an empty view fails the create; it
     *         never silently mounts a half-built connection. A caller that wants the
     *         allocation to come from an injected backend uses `view::over_bytes(spec.bytes(),
     *         backend)` instead (#793).
     */
    [[nodiscard]] view::view_t view() const {
        return view::over_bytes(bytes()).value_or(view::view_t{});
    }

   private:
    std::vector<std::byte> body_; /**< @brief The SPEC body's fixed head — the `type` and
                                              `name` pairs, encoded by the constructor. */
    std::vector<std::byte> cfg_;  /**< @brief The `config` SETTINGS body, appended in call
                                              order; empty = emit no `config` at all. */
};

/**
 * @brief The connection SPEC in one call — the shape nearly every call site wants.
 *
 * Equivalent to `conn_spec_t(type, name).role(role).port(port)` plus `.kind(kind)` and
 * `.addr(addr)` when those are non-empty; an empty @p kind or @p addr omits the key. For a
 * kind-private key, a config-less SPEC, or any other field, build a @ref conn_spec_t.
 *
 * @return An owned view of the SPEC bytes (empty on allocation failure — see
 *         @ref conn_spec_t::view).
 */
[[nodiscard]] inline view::view_t conn_spec(std::string_view type, std::string_view name,
                                            conn_role_t role, std::uint16_t port,
                                            std::string_view kind = {},
                                            std::string_view addr = {}) {
    conn_spec_t spec(type, name);
    spec.role(role).port(port);
    if (!kind.empty()) spec.kind(kind);
    if (!addr.empty()) spec.addr(addr);
    return spec.view();
}

}  // namespace tr::net
