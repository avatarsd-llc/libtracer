/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The RFC-0002 protocol error registry — the frozen `tr::<concept>::<error>`
 * namespace keyed by stable protocol concept (ADR-0009). Each built-in error
 * carries a registered u16 wire code (the ERROR TLV's first-child VALUE payload,
 * LE), plus registry-side severity and disposition that never travel on the
 * wire. Protocol-only, closed set — applications never emit protocol errors
 * (ADR-0010). Header-only.
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace tr::wire {

/**
 * @brief Registered protocol error codes (RFC-0002 §D) — the u16 wire identity
 *        of each built-in `tr::…` error path, carried LE in the ERROR TLV's
 *        first-child VALUE.
 */
enum class err_t : std::uint16_t {
    FRAME_TRUNCATED = 0x0001, /**< `tr::frame::truncated` */
    FRAME_INVALID = 0x0002,   /**< `tr::frame::invalid` */
    FRAME_CRC_FAIL = 0x0003,  /**< `tr::frame::crc_fail` */
    /** @brief `tr::tlv::nesting_too_deep` — exceeds this RECEIVER's decode
     *  resources (RFC-0006; depth is resource-bounded, never a constant). */
    TLV_NESTING_TOO_DEEP = 0x0010,
    PATH_NOT_FOUND = 0x0020,         /**< `tr::path::not_found` */
    PATH_INVALID = 0x0021,           /**< `tr::path::invalid` */
    PATH_IN_USE = 0x0022,            /**< `tr::path::in_use` */
    SCHEMA_TYPE_MISMATCH = 0x0030,   /**< `tr::schema::type_mismatch` */
    SCHEMA_NOT_FOUND = 0x0031,       /**< `tr::schema::not_found` */
    FLOW_BACKPRESSURE = 0x0040,      /**< `tr::flow::backpressure` */
    FLOW_TIMEOUT = 0x0041,           /**< `tr::flow::timeout` */
    FLOW_ADDRESS_SHIFT_GAP = 0x0042, /**< `tr::flow::address_shift_gap` */
    ACCESS_DENIED = 0x0050,          /**< `tr::access::denied` */
    TRANSPORT_DOWN = 0x0060,         /**< `tr::transport::down` */
    VERSION_MISMATCH = 0x0070,       /**< `tr::version::mismatch` */
};

/**
 * @brief Registry-side severity of a protocol error (RFC-0002 §D) — advisory
 *        metadata that never travels on the wire.
 */
enum class err_severity_t : std::uint8_t {
    WARN,     /**< Expected-in-operation outcome (e.g. a missing path). */
    ERROR,    /**< A genuine protocol-level failure. */
    CRITICAL, /**< The peer relationship itself is unsound. */
};

/**
 * @brief Registry-side disposition of a protocol error (RFC-0002 §D) — what a
 *        caller should do next; never travels on the wire.
 */
enum class err_disposition_t : std::uint8_t {
    TRANSIENT, /**< Retry may succeed (congestion, timing, lossy link). */
    PERMANENT, /**< Don't retry this request as-is. */
    FATAL,     /**< Tear down the peer relationship. */
};

/**
 * @brief The canonical `tr::…` namespace path of a registered error code
 *        (RFC-0002 §A/§D) — the string identity a NAME-form ERROR would carry.
 *
 * @param e the registered error code
 * @return the frozen `tr::<concept>::<error>` path (empty for an unregistered
 *         value)
 */
[[nodiscard]] constexpr std::string_view err_path(err_t e) noexcept {
    switch (e) {
        case err_t::FRAME_TRUNCATED:
            return "tr::frame::truncated";
        case err_t::FRAME_INVALID:
            return "tr::frame::invalid";
        case err_t::FRAME_CRC_FAIL:
            return "tr::frame::crc_fail";
        case err_t::TLV_NESTING_TOO_DEEP:
            return "tr::tlv::nesting_too_deep";
        case err_t::PATH_NOT_FOUND:
            return "tr::path::not_found";
        case err_t::PATH_INVALID:
            return "tr::path::invalid";
        case err_t::PATH_IN_USE:
            return "tr::path::in_use";
        case err_t::SCHEMA_TYPE_MISMATCH:
            return "tr::schema::type_mismatch";
        case err_t::SCHEMA_NOT_FOUND:
            return "tr::schema::not_found";
        case err_t::FLOW_BACKPRESSURE:
            return "tr::flow::backpressure";
        case err_t::FLOW_TIMEOUT:
            return "tr::flow::timeout";
        case err_t::FLOW_ADDRESS_SHIFT_GAP:
            return "tr::flow::address_shift_gap";
        case err_t::ACCESS_DENIED:
            return "tr::access::denied";
        case err_t::TRANSPORT_DOWN:
            return "tr::transport::down";
        case err_t::VERSION_MISMATCH:
            return "tr::version::mismatch";
    }
    return {};
}

/**
 * @brief The registry severity of a registered error code (RFC-0002 §D).
 *
 * @param e the registered error code
 * @return `WARN` / `ERROR` / `CRITICAL` per the frozen registry table
 */
[[nodiscard]] constexpr err_severity_t err_severity(err_t e) noexcept {
    switch (e) {
        case err_t::PATH_NOT_FOUND:
        case err_t::PATH_INVALID:
        case err_t::PATH_IN_USE:
        case err_t::SCHEMA_NOT_FOUND:
        case err_t::FLOW_BACKPRESSURE:
        case err_t::FLOW_TIMEOUT:
            return err_severity_t::WARN;
        case err_t::VERSION_MISMATCH:
            return err_severity_t::CRITICAL;
        case err_t::FRAME_TRUNCATED:
        case err_t::FRAME_INVALID:
        case err_t::FRAME_CRC_FAIL:
        case err_t::TLV_NESTING_TOO_DEEP:
        case err_t::SCHEMA_TYPE_MISMATCH:
        case err_t::FLOW_ADDRESS_SHIFT_GAP:
        case err_t::ACCESS_DENIED:
        case err_t::TRANSPORT_DOWN:
            return err_severity_t::ERROR;
    }
    return err_severity_t::ERROR;
}

/**
 * @brief The registry disposition of a registered error code (RFC-0002 §D).
 *
 * @param e the registered error code
 * @return `TRANSIENT` / `PERMANENT` / `FATAL` per the frozen registry table
 */
[[nodiscard]] constexpr err_disposition_t err_disposition(err_t e) noexcept {
    switch (e) {
        case err_t::FRAME_TRUNCATED:
        case err_t::FRAME_CRC_FAIL:
        case err_t::FLOW_BACKPRESSURE:
        case err_t::FLOW_TIMEOUT:
        case err_t::TRANSPORT_DOWN:
            return err_disposition_t::TRANSIENT;
        case err_t::VERSION_MISMATCH:
            return err_disposition_t::FATAL;
        case err_t::FRAME_INVALID:
        case err_t::TLV_NESTING_TOO_DEEP:
        case err_t::PATH_NOT_FOUND:
        case err_t::PATH_INVALID:
        case err_t::PATH_IN_USE:
        case err_t::SCHEMA_TYPE_MISMATCH:
        case err_t::SCHEMA_NOT_FOUND:
        case err_t::FLOW_ADDRESS_SHIFT_GAP:
        case err_t::ACCESS_DENIED:
            return err_disposition_t::PERMANENT;
    }
    return err_disposition_t::PERMANENT;
}

}  // namespace tr::wire
