/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * L4 addressing. A path_t is the canonical PATH-TLV payload bytes (a sequence of
 * NAME children) that key a vertex in the graph map — docs/reference/02 §dispatch
 * keys on the PATH payload bytes, never on the string form. path_t::parse builds
 * and validates those bytes once (at registration); the hot path compares bytes.
 * A field tail after ':' (e.g. ":settings.deadline_ns", ":subscribers[]") parses
 * into a field_path_t for field-write/read. See docs/reference/03-addressing.md.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/status.hpp"

namespace tr::graph {

// Addressing limits (docs/reference/03 §limits).
inline constexpr std::size_t kMaxSegmentBytes = 64;
inline constexpr std::size_t kMaxPathBytes = 1024;
inline constexpr std::size_t kMaxSegments = 32;
inline constexpr std::size_t kMaxFieldDepth = 8;

// One step of a field path: a NAME and an optional [index] / [] append / [*] wildcard.
struct field_step_t {
    std::string name;
    bool indexed = false;     // true if "[...]" was present
    bool append = false;      // true for "[]" (append to a sequence)
    bool wildcard = false;    // true for "[*]" (FIELD index_mode=WILDCARD, RFC-0004 §C)
    std::uint16_t index = 0;  // valid when indexed && !append && !wildcard
    bool operator==(const field_step_t&) const = default;
};

struct field_path_t {
    std::vector<field_step_t> steps;  // empty => addresses the vertex value itself
    [[nodiscard]] bool empty() const noexcept { return steps.empty(); }
    bool operator==(const field_path_t&) const = default;
};

// A parsed, canonical path: the PATH-TLV payload bytes (NAME children) plus the
// optional field tail. The payload bytes are the vertex-map key.
class path_t {
   public:
    path_t() = default;

    /**
     * @brief Construct from a compile-site / known-good path LITERAL, parsing ONCE.
     *
     * `path_t p("/sensor/temp"); write(p, a); write(p, b);` — parse the string a single
     * time, then hold the value and reuse the handle; the graph API takes `const path_t&`
     * so a held path never re-parses on the hot path (docs/reference/02 §dispatch keys on
     * the parsed PATH-TLV bytes, never the string). A malformed literal is a source bug,
     * so this **hard-aborts** rather than yielding a fallible @ref result_t the caller
     * would only `*`-deref unchecked. For a RUNTIME string whose validity is a genuine
     * runtime condition, use @ref parse (fallible). `explicit` — construction is always
     * a visible, deliberate parse, never an implicit per-call one. No exceptions (usable
     * under `-fno-exceptions`).
     */
    explicit path_t(std::string_view text);

    // Parse "/sensor/temp" or "/sensor/temp:settings.deadline_ns". Validates and
    // canonicalizes: strip trailing '/', reject empty segments ("//") and
    // unrooted paths, enforce the size/depth limits above. Fallible — for a RUNTIME
    // string; a known-good literal uses the parse-once constructor above.
    [[nodiscard]] static result_t<path_t> parse(std::string_view text);

    // The vertex-map key: the canonical PATH-TLV payload bytes (NAME children).
    [[nodiscard]] std::span<const std::byte> key() const noexcept {
        return {payload_.data(), payload_.size()};
    }
    [[nodiscard]] const field_path_t& field() const noexcept { return field_; }
    [[nodiscard]] std::size_t segment_count() const noexcept { return segments_; }

   private:
    std::vector<std::byte> payload_;  // canonical PATH-TLV payload (NAME children)
    field_path_t field_;
    std::size_t segments_ = 0;
};

inline path_t::path_t(std::string_view text) {
    result_t<path_t> p = parse(text);
    if (!p) std::abort();  // malformed path LITERAL — a source bug; fail loud, not silent
    *this = std::move(*p);
}

// Owned byte key for the vertex map (a copy of a path's canonical payload).
struct path_key_t {
    std::vector<std::byte> bytes;
    bool operator==(const path_key_t&) const noexcept = default;
};

struct path_key_hash_t {
    [[nodiscard]] std::size_t operator()(const path_key_t& k) const noexcept;
};

}  // namespace tr::graph
