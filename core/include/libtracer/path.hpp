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

/** @brief Max bytes in one NAME segment (docs/reference/03 §limits). */
inline constexpr std::size_t kMaxSegmentBytes = 64;
/** @brief Max bytes in a whole canonical PATH payload (docs/reference/03 §limits). */
inline constexpr std::size_t kMaxPathBytes = 1024;
/** @brief Max NAME segments in a path (docs/reference/03 §limits). */
inline constexpr std::size_t kMaxSegments = 32;
/** @brief Max steps in a `:field` tail (docs/reference/03 §limits). */
inline constexpr std::size_t kMaxFieldDepth = 8;

/**
 * @brief One step of a field path: a NAME and an optional `[index]` / `[]` append /
 *        `[*]` wildcard selector.
 *
 * The parsed form of one `.`-separated component of a `:field.sub[N]` tail
 * (docs/reference/03 §addressing). Exactly one of @ref indexed (with @ref index),
 * @ref append, or @ref wildcard is meaningful when a `[...]` selector is present.
 */
struct field_step_t {
    std::string name;         /**< @brief The step's NAME (the text before any `[...]`). */
    bool indexed = false;     /**< @brief True if a `[...]` selector was present. */
    bool append = false;      /**< @brief True for `[]` — append to a sequence. */
    bool wildcard = false;    /**< @brief True for `[*]` — FIELD index_mode=WILDCARD (RFC-0004 §C). */
    std::uint16_t index = 0;  /**< @brief The `[N]` index; valid when `indexed && !append && !wildcard`. */
    /** @brief Value equality over every field. */
    bool operator==(const field_step_t&) const = default;
};

/**
 * @brief The parsed `:field.sub[N]` tail of a path — a sequence of @ref field_step_t.
 *
 * Empty when the path addresses the vertex value itself (no `:` tail). Drives the
 * field-write / field-read control surface (docs/reference/04).
 */
struct field_path_t {
    std::vector<field_step_t> steps;  /**< @brief The `.`-separated steps; empty ⇒ the vertex value itself. */
    /** @brief True when there is no field tail (addresses the vertex value). */
    [[nodiscard]] bool empty() const noexcept { return steps.empty(); }
    /** @brief Value equality over the step sequence. */
    bool operator==(const field_path_t&) const = default;
};

/**
 * @brief A parsed, canonical path: the PATH-TLV payload bytes (NAME children) plus the
 *        optional @ref field_path_t tail. The payload bytes are the vertex-map key.
 *
 * Dispatch keys on the parsed bytes (@ref key), never the string form — parse once,
 * hold the value, and every read/write compares bytes (docs/reference/02 §dispatch).
 */
class path_t {
   public:
    /** @brief An empty path (no segments, no field tail). */
    path_t() = default;

    /**
     * @brief Construct from a compile-site / known-good path LITERAL, parsing ONCE.
     *
     * `path_t p("/sensor/temp"); write(p, a); write(p, b);` — parse the string a single
     * time, then hold the value and reuse the handle; the graph API takes `const path_t&`
     * so a held path never re-parses on the hot path (docs/reference/02 §dispatch keys on
     * the parsed PATH-TLV bytes, never the string). A malformed literal is a source bug,
     * so this **hard-aborts** rather than yielding a fallible `result_t` the caller
     * would only `*`-deref unchecked. For a RUNTIME string whose validity is a genuine
     * runtime condition, use @ref parse (fallible). `explicit` — construction is always
     * a visible, deliberate parse, never an implicit per-call one. No exceptions (usable
     * under `-fno-exceptions`).
     */
    explicit path_t(std::string_view text);

    /**
     * @brief Parse and canonicalize a path string (fallible — for a RUNTIME string).
     *
     * Accepts `"/sensor/temp"` or `"/sensor/temp:settings.deadline_ns"`. Canonicalizes:
     * strip a trailing `/`, reject empty segments (`//`) and unrooted paths, enforce the
     * `kMaxSegmentBytes` / `kMaxPathBytes` / `kMaxSegments` / `kMaxFieldDepth`
     * limits. A known-good literal uses the parse-once @ref path_t(std::string_view)
     * constructor instead.
     *
     * @param text The path string to parse.
     * @return The parsed @ref path_t, or a `status_t` error (e.g. `INVALID_PATH`).
     */
    [[nodiscard]] static result_t<path_t> parse(std::string_view text);

    /** @brief The vertex-map key: the canonical PATH-TLV payload bytes (NAME children). */
    [[nodiscard]] std::span<const std::byte> key() const noexcept {
        return {payload_.data(), payload_.size()};
    }
    /** @brief The parsed `:field` tail (empty when the path addresses the vertex value). */
    [[nodiscard]] const field_path_t& field() const noexcept { return field_; }
    /** @brief The number of NAME segments in the path. */
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

/** @brief Owned byte key for the vertex map (a copy of a path's canonical payload). */
struct path_key_t {
    std::vector<std::byte> bytes;  /**< @brief The owned canonical PATH payload bytes. */
    /** @brief Value equality over the key bytes. */
    bool operator==(const path_key_t&) const noexcept = default;
};

/** @brief Hash functor for @ref path_key_t (FNV-1a over the key bytes) — the map hasher. */
struct path_key_hash_t {
    /** @brief Hash the key's canonical PATH bytes. */
    [[nodiscard]] std::size_t operator()(const path_key_t& k) const noexcept;
};

}  // namespace tr::graph
