// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// L4 graph result + status. The read/write/await control surface returns
// std::expected<T, Status> (the same shape as M1's frame::Error). The Status
// names mirror the documented protocol error codes (docs/reference/05 §error
// codes); they reconcile with the tr:: error namespace when RFC-0002 lands
// (gated). Protocol-only, closed set — not for user error codes (ADR-0010).
#pragma once

#include <expected>

namespace tr::graph {

enum class Status {
    NotFound,          // path doesn't resolve / no last-known-value
    InvalidPath,       // malformed path or non-UTF-8 NAME segment
    TypeMismatch,      // payload type incompatible with the vertex/field
    Backpressure,      // queue full / dispatch-depth cap hit
    Timeout,           // await deadline expired
    SchemaNotFound,    // field read/write on a vertex that doesn't expose it
    PermissionDenied,  // ACL rejected (enforcement deferred; reserved)
    PathInUse,         // registration collided with an existing vertex
};

// Success is the value side of the expected; an empty STATUS=OK on the wire maps
// to a Result with a value (or Result<void> success).
template <class T>
using Result = std::expected<T, Status>;

[[nodiscard]] constexpr const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::NotFound:
            return "not_found";
        case Status::InvalidPath:
            return "invalid_path";
        case Status::TypeMismatch:
            return "type_mismatch";
        case Status::Backpressure:
            return "backpressure";
        case Status::Timeout:
            return "timeout";
        case Status::SchemaNotFound:
            return "schema_not_found";
        case Status::PermissionDenied:
            return "permission_denied";
        case Status::PathInUse:
            return "path_in_use";
    }
    return "unknown";
}

}  // namespace tr::graph
