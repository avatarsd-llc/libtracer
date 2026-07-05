/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * L4 graph result + status. The read/write/await control surface returns
 * std::expected<T, status_t> (the same shape as the wire codec's err_t). The status_t
 * names mirror the documented protocol error codes (docs/reference/05 §error
 * codes); they reconcile with the tr:: error namespace when RFC-0002 lands
 * (gated). Protocol-only, closed set — not for user error codes (ADR-0010).
 */
#pragma once

#include <expected>

namespace tr::graph {

enum class status_t {
    NOT_FOUND,          // path doesn't resolve / no last-known-value
    INVALID_PATH,       // malformed path or non-UTF-8 NAME segment
    TYPE_MISMATCH,      // payload type incompatible with the vertex/field
    BACKPRESSURE,       // queue full / dispatch-depth cap hit
    TIMEOUT,            // await deadline expired
    SCHEMA_NOT_FOUND,   // field read/write on a vertex that doesn't expose it
    PERMISSION_DENIED,  // ACL rejected (core subset enforced: ALLOW-only ACEs + INHERIT)
    PATH_IN_USE,        // registration collided with an existing vertex
};

// Success is the value side of the expected; an empty STATUS=OK on the wire maps
// to a result_t with a value (or result_t<void> success).
template <class T>
using result_t = std::expected<T, status_t>;

[[nodiscard]] constexpr const char* to_string(status_t s) noexcept {
    switch (s) {
        case status_t::NOT_FOUND:
            return "not_found";
        case status_t::INVALID_PATH:
            return "invalid_path";
        case status_t::TYPE_MISMATCH:
            return "type_mismatch";
        case status_t::BACKPRESSURE:
            return "backpressure";
        case status_t::TIMEOUT:
            return "timeout";
        case status_t::SCHEMA_NOT_FOUND:
            return "schema_not_found";
        case status_t::PERMISSION_DENIED:
            return "permission_denied";
        case status_t::PATH_IN_USE:
            return "path_in_use";
    }
    return "unknown";
}

}  // namespace tr::graph
