// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// L4 addressing. A Path is the canonical PATH-TLV payload bytes (a sequence of
// NAME children) that key a vertex in the graph map — docs/reference/02 §dispatch
// keys on the PATH payload bytes, never on the string form. Path::parse builds
// and validates those bytes once (at registration); the hot path compares bytes.
// A field tail after ':' (e.g. ":settings.deadline_ns", ":subscribers[]") parses
// into a FieldPath for field-write/read. See docs/reference/03-addressing.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/status.hpp"

namespace tr::graph {

// Addressing limits (docs/reference/03 §limits).
inline constexpr std::size_t kMaxSegmentBytes = 64;
inline constexpr std::size_t kMaxPathBytes = 1024;
inline constexpr std::size_t kMaxSegments = 32;
inline constexpr std::size_t kMaxFieldDepth = 8;

// One step of a field path: a NAME and an optional [index] / [] append.
struct FieldStep {
    std::string name;
    bool indexed = false;     // true if "[...]" was present
    bool append = false;      // true for "[]" (append to a sequence)
    std::uint16_t index = 0;  // valid when indexed && !append
    bool operator==(const FieldStep&) const = default;
};

struct FieldPath {
    std::vector<FieldStep> steps;  // empty => addresses the vertex value itself
    [[nodiscard]] bool empty() const noexcept { return steps.empty(); }
    bool operator==(const FieldPath&) const = default;
};

// A parsed, canonical path: the PATH-TLV payload bytes (NAME children) plus the
// optional field tail. The payload bytes are the vertex-map key.
class Path {
   public:
    // Parse "/sensor/temp" or "/sensor/temp:settings.deadline_ns". Validates and
    // canonicalizes: strip trailing '/', reject empty segments ("//") and
    // unrooted paths, enforce the size/depth limits above.
    [[nodiscard]] static Result<Path> parse(std::string_view text);

    // The vertex-map key: the canonical PATH-TLV payload bytes (NAME children).
    [[nodiscard]] std::span<const std::byte> key() const noexcept {
        return {payload_.data(), payload_.size()};
    }
    [[nodiscard]] const FieldPath& field() const noexcept { return field_; }
    [[nodiscard]] std::size_t segment_count() const noexcept { return segments_; }

   private:
    std::vector<std::byte> payload_;  // canonical PATH-TLV payload (NAME children)
    FieldPath field_;
    std::size_t segments_ = 0;
};

// Owned byte key for the vertex map (a copy of a path's canonical payload).
struct PathKey {
    std::vector<std::byte> bytes;
    bool operator==(const PathKey&) const noexcept = default;
};

struct PathKeyHash {
    [[nodiscard]] std::size_t operator()(const PathKey& k) const noexcept;
};

}  // namespace tr::graph
