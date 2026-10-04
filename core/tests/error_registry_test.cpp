/**
 * @file
 * @brief Unit tests for the RFC-0002 protocol error registry (error.hpp).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Pins every registered code's path, severity and disposition against the registry table in
 * docs/reference/05-protocol-tlvs.md §Error registry, one row per code. Before this suite
 * `err_severity` had no test at all, and `err_path` / `err_disposition` were checked only for
 * the one or two codes their callers happened to touch.
 */

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

#include "libtracer/error.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;
using tr::wire::err_disposition_t;
using tr::wire::err_severity_t;
using tr::wire::err_t;

/** @brief One row of the registry table: the code and the three facts registered for it. */
struct row_t {
    err_t code;                    /**< The registered u16 wire code. */
    std::string_view path;         /**< The frozen `tr::<concept>::<error>` path. */
    err_severity_t severity;       /**< The registry severity. */
    err_disposition_t disposition; /**< The registry disposition. */
};

/** @brief The registry table, transcribed row for row from the reference. */
constexpr std::array<row_t, 15> kRegistry{{
    {err_t::FRAME_TRUNCATED, "tr::frame::truncated", err_severity_t::ERROR,
     err_disposition_t::TRANSIENT},
    {err_t::FRAME_INVALID, "tr::frame::invalid", err_severity_t::ERROR,
     err_disposition_t::PERMANENT},
    {err_t::FRAME_CRC_FAIL, "tr::frame::crc_fail", err_severity_t::ERROR,
     err_disposition_t::TRANSIENT},
    {err_t::TLV_NESTING_TOO_DEEP, "tr::tlv::nesting_too_deep", err_severity_t::ERROR,
     err_disposition_t::PERMANENT},
    {err_t::PATH_NOT_FOUND, "tr::path::not_found", err_severity_t::WARN,
     err_disposition_t::PERMANENT},
    {err_t::PATH_INVALID, "tr::path::invalid", err_severity_t::WARN, err_disposition_t::PERMANENT},
    {err_t::PATH_IN_USE, "tr::path::in_use", err_severity_t::WARN, err_disposition_t::PERMANENT},
    {err_t::SCHEMA_TYPE_MISMATCH, "tr::schema::type_mismatch", err_severity_t::ERROR,
     err_disposition_t::PERMANENT},
    {err_t::SCHEMA_NOT_FOUND, "tr::schema::not_found", err_severity_t::WARN,
     err_disposition_t::PERMANENT},
    {err_t::FLOW_BACKPRESSURE, "tr::flow::backpressure", err_severity_t::WARN,
     err_disposition_t::TRANSIENT},
    {err_t::FLOW_TIMEOUT, "tr::flow::timeout", err_severity_t::WARN, err_disposition_t::TRANSIENT},
    {err_t::FLOW_ADDRESS_SHIFT_GAP, "tr::flow::address_shift_gap", err_severity_t::ERROR,
     err_disposition_t::PERMANENT},
    {err_t::ACCESS_DENIED, "tr::access::denied", err_severity_t::ERROR,
     err_disposition_t::PERMANENT},
    {err_t::TRANSPORT_DOWN, "tr::transport::down", err_severity_t::ERROR,
     err_disposition_t::TRANSIENT},
    {err_t::VERSION_MISMATCH, "tr::version::mismatch", err_severity_t::CRITICAL,
     err_disposition_t::FATAL},
}};

}  // namespace

int main() {
    std::printf("RFC-0002 §D — the registry, row by row:\n");
    for (const row_t& r : kRegistry) {
        const std::string name{r.path};
        check(tr::wire::err_path(r.code) == r.path, name + ": path");
        check(tr::wire::err_severity(r.code) == r.severity, name + ": severity");
        check(tr::wire::err_disposition(r.code) == r.disposition, name + ": disposition");
    }

    // The three severities are each reached, so a function that answered one constant for
    // every code could not pass the table above.
    static_assert(tr::wire::err_severity(err_t::PATH_NOT_FOUND) == err_severity_t::WARN);
    static_assert(tr::wire::err_severity(err_t::FRAME_INVALID) == err_severity_t::ERROR);
    static_assert(tr::wire::err_severity(err_t::VERSION_MISMATCH) == err_severity_t::CRITICAL);

    // An unregistered value has no path, and severity falls back to ERROR.
    const auto unregistered = static_cast<err_t>(0x7FFF);
    check(tr::wire::err_path(unregistered).empty(), "an unregistered code has no path");
    check(tr::wire::err_severity(unregistered) == err_severity_t::ERROR,
          "an unregistered code reports ERROR severity");

    return tr::testing::summary("error_registry");
}
