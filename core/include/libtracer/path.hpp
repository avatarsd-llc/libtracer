/**
 * @file
 * @brief L4 addressing: `path_t`, the canonical PATH-payload bytes that key a vertex, and the
 *        field-path tail.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * L4 addressing. A path_t is the canonical PATH-TLV payload bytes (a sequence of
 * packed `[u8 len][bytes]` segment records, RFC-0018) that key a vertex in the graph map —
 * docs/reference/02 §dispatch keys on the PATH payload bytes, never on the string form.
 * path_t::parse builds and validates those bytes once (at registration); the hot path compares
 * bytes. A field tail after ':' (e.g. ":settings.deadline_ns", ":subscribers[]") parses into a
 * field_path_t for field-write/read. See docs/reference/03-addressing.md.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/status.hpp"

namespace tr::graph {

class graph_t;

/** @brief Max bytes in one path segment (docs/reference/03 §limits; the packed record's
 *         `u8` length field caps it at 255 forever, RFC-0018 §5). */
inline constexpr std::size_t kMaxSegmentBytes = 64;
/** @brief Max bytes in a whole canonical PATH payload (docs/reference/03 §limits). */
inline constexpr std::size_t kMaxPathBytes = 1024;
/** @brief Max segments in a path (reference/03 §limits; RFC-0023: min(255, byte cap)). */
inline constexpr std::size_t kMaxSegments = 255;
/** @brief Max steps in a `:field` tail (docs/reference/03 §limits). */
inline constexpr std::size_t kMaxFieldDepth = 8;

/**
 * @brief True iff @p seg is valid as ONE segment of the addressing grammar.
 *
 * THE segment predicate (ADR-0073 §1): every boundary where a name enters the graph —
 * the local string parser, a wire `SPEC` creation carrying a child name, a module
 * registration — calls this ONE function, so the tiers cannot drift (#688; the drift,
 * not any single missing check, is the recurring defect — cf. #681). A name that fails
 * here answers `INVALID_PATH` wherever it is rejected.
 *
 * Checks: non-empty, at most `kMaxSegmentBytes`, and none of the SEVEN reserved
 * characters of reference/03 §Reserved characters — `/` and `:` are separators, `.`
 * separates field levels, `[` / `]` delimit the grammar's index suffix (which sits
 * OUTSIDE `name`: `segment = name [ index ]`), `*` is the wildcard selector, `?` is
 * reserved for the future. Rejecting the brackets is the normative MUST (reference/03
 * §Reserved characters, incorporated by spec v1 §3); the pre-#996 five-character
 * subset admitted them on the theory that `frame[7]` travels inside the NAME bytes —
 * ruled the other way: address-index addressing, if it lands, lives outside the NAME
 * bytes (#996, cf. `.out-of-scope/range-slice-addressing.md`). The character set is
 * pinned cross-tier by the `path/path-reserved-brackets` conformance vector.
 */
[[nodiscard]] inline bool valid_segment(std::string_view seg) noexcept {
    return !seg.empty() && seg.size() <= kMaxSegmentBytes &&
           seg.find_first_of("/:.[]*?") == std::string_view::npos;
}

/**
 * @brief One step of a field path: a NAME and an optional `[index]` / `[]` append /
 *        `[*]` wildcard selector.
 *
 * The parsed form of one `.`-separated component of a `:field.sub[N]` tail
 * (docs/reference/03 §addressing). Exactly one of @ref indexed (with @ref index),
 * @ref append, or @ref wildcard is meaningful when a `[...]` selector is present.
 *
 * A step owns nothing (ADR-0083 Q15: no owning standard type crosses core's API): @ref name
 * views bytes that whoever built the step keeps alive — a @ref path_t's own block, the
 * request frame a resolver is reading, or a string literal.
 */
struct field_step_t {
    std::string_view name; /**< @brief The step's NAME (the text before any `[...]`). */
    bool indexed = false;  /**< @brief True if a `[...]` selector was present. */
    bool append = false;   /**< @brief True for `[]` — append to a sequence. */
    /** @brief True for `[*]` — FIELD index_mode=WILDCARD (RFC-0004 §C). */
    bool wildcard = false;
    /** @brief The `[N]` index; valid when `indexed && !append && !wildcard`. */
    std::uint16_t index = 0;
    /** @brief Value equality over every field (the name by content). */
    bool operator==(const field_step_t&) const = default;
};

/**
 * @brief The steps of a field path, held IN PLACE: at most @ref kMaxFieldDepth of them, the
 *        bound every parser already enforces, so a field path never allocates.
 *
 * The read surface is the one a sequence has (`size`, `[i]`, range-for); the one write is
 * @ref push_back, which refuses a step past the bound instead of growing.
 */
class field_steps_t {
   public:
    /** @brief The number of steps. */
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    /** @brief True when there are no steps. */
    [[nodiscard]] bool empty() const noexcept { return n_ == 0; }
    /** @brief Step @p i; @p i MUST be below @ref size. */
    [[nodiscard]] const field_step_t& operator[](std::size_t i) const noexcept { return s_[i]; }
    /** @brief Step @p i, mutable; @p i MUST be below @ref size. */
    [[nodiscard]] field_step_t& operator[](std::size_t i) noexcept { return s_[i]; }
    /** @brief First step. */
    [[nodiscard]] const field_step_t* begin() const noexcept { return s_.data(); }
    /** @brief One past the last step. */
    [[nodiscard]] const field_step_t* end() const noexcept { return s_.data() + n_; }
    /** @brief First step, mutable. */
    [[nodiscard]] field_step_t* begin() noexcept { return s_.data(); }
    /** @brief One past the last step, mutable. */
    [[nodiscard]] field_step_t* end() noexcept { return s_.data() + n_; }
    /**
     * @brief Append @p step.
     * @return false, appending nothing, when @ref kMaxFieldDepth steps are already held.
     */
    [[nodiscard]] bool push_back(const field_step_t& step) noexcept {
        if (n_ == kMaxFieldDepth) return false;
        s_[n_++] = step;
        return true;
    }
    /** @brief Equality over the held steps (unused slots never compare). */
    bool operator==(const field_steps_t& o) const noexcept { return std::ranges::equal(*this, o); }

   private:
    std::array<field_step_t, kMaxFieldDepth> s_{};
    std::uint8_t n_ = 0;
};

/**
 * @brief The parsed `:field.sub[N]` tail of a path — a sequence of @ref field_step_t.
 *
 * Empty when the path addresses the vertex value itself (no `:` tail). Drives the
 * field-write / field-read control surface (docs/reference/04). A value type with no
 * allocation: its steps are in place and their names are views.
 */
struct field_path_t {
    /** @brief The `.`-separated steps; empty ⇒ the vertex value itself. */
    field_steps_t steps;
    /** @brief True when there is no field tail (addresses the vertex value). */
    [[nodiscard]] bool empty() const noexcept { return steps.empty(); }
    /** @brief Value equality over the step sequence. */
    bool operator==(const field_path_t&) const = default;
};

/**
 * @brief A parsed, canonical path: the PATH-TLV payload bytes (packed records) plus the
 *        optional @ref field_path_t tail. The payload bytes are the vertex-map key.
 *
 * Dispatch keys on the parsed bytes (@ref key), never the string form — parse once,
 * hold the value, and every read/write compares bytes (docs/reference/02 §dispatch).
 *
 * It is a **value type** that carries no route: a learned chain is held by the graph-owned
 * path object (`path_object.hpp`), never by this handle, so copying a path copies only its
 * parse and neither copy can outlive anything.
 *
 * @section storage Storage (#2286)
 *
 * No standard container: the canonical bytes, the field steps and their names live in ONE
 * block sized exactly at parse and drawn from a `tr::mem::block_source_t`: the one given to
 * @ref parse, else the process table source (ADR-0083). The root path with no field tail owns
 * no block at all. A copy draws its block from the source of the path it copies and stops with a
 * sizing message if that source cannot serve it. A move takes the block. Nothing here grows after
 * the parse.
 */
class path_t {
   public:
    /** @brief An empty path (no segments, no field tail). */
    path_t() noexcept = default;

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

    /**
     * @brief @ref parse, drawing the path's block from @p src.
     * @return As @ref parse; `BACKPRESSURE` when @p src refuses the block.
     */
    [[nodiscard]] static result_t<path_t> parse(std::string_view text,
                                                tr::mem::block_source_t& src);

    /** @brief Copy @p o into a block from @p o's source (see @ref storage). */
    path_t(const path_t& o);
    /** @brief Copy-assign (see the copy constructor). */
    path_t& operator=(const path_t& o);
    /** @brief Take @p o's block; @p o is left the empty path. */
    path_t(path_t&& o) noexcept { take(o); }
    /** @brief Move-assign (see the move constructor). */
    path_t& operator=(path_t&& o) noexcept {
        if (this != &o) {
            release();
            take(o);
        }
        return *this;
    }
    /** @brief Return the block to its source. */
    ~path_t() { release(); }

    /** @brief The vertex-map key: the canonical PATH-TLV payload bytes (packed records). */
    [[nodiscard]] std::span<const std::byte> key() const noexcept {
        return {block_ + payload_off(), key_len_};
    }
    /**
     * @brief The parsed `:field` tail (empty when the path addresses the vertex value).
     *
     * Its step names view this path's block, and so does a copy of it: hold the path for as
     * long as the field path is used (`auto f = path_t::parse(t)->field();` dangles).
     */
    [[nodiscard]] const field_path_t& field() const noexcept;
    /** @brief The number of segments in the path. */
    [[nodiscard]] std::size_t segment_count() const noexcept { return segments_; }

   private:
    [[nodiscard]] std::size_t payload_off() const noexcept {
        return has_field_ ? sizeof(field_path_t) : 0;
    }
    [[nodiscard]] tr::mem::block_source_t& source() const noexcept;
    void copy_from(const path_t& o);
    void release() noexcept;
    void take(path_t& o) noexcept;

    tr::mem::block_source_t* src_ = nullptr;  // who serves the block; null ⇒ table source
    std::byte* block_ = nullptr;  // [field_path_t if has_field_][key bytes][field names]
    std::uint32_t block_len_ = 0;
    std::uint16_t key_len_ = 0;  // at most kMaxPathBytes
    std::uint8_t segments_ = 0;  // at most kMaxSegments
    bool has_field_ = false;
};

/**
 * @brief A `path_t` is two pointers and eight bytes of lengths: 24 B on a 64-bit host, 16 B on
 *        rv32.
 *
 * Pinned, not observed. A path is a parse-once VALUE the API takes by `const&`, and an origin
 * holds them per subscription and per vertex, so its size is multiplied RAM. #2286 moved its three
 * remaining owning standard containers into source-drawn blocks: 9 pointer-widths → 3 plus the
 * lengths (72 → 40 B on a 64-bit host, 36 → 24 B on rv32), and #2436 dropped the learned-chain
 * slot no caller used (the graph-owned path object holds the chain): 40 → 24 B, 24 → 16 B. A change
 * here is a deliberate edit, and it is the moment to ask whether the new member wants to be a slot
 * at all.
 */
static_assert(sizeof(path_t) == (sizeof(void*) == 8 ? 24 : 16),
              "a path_t's shape is a ratchet — grow it deliberately or not at all");

inline path_t::path_t(std::string_view text) {
    result_t<path_t> p = parse(text);
    if (!p) std::abort();  // malformed path LITERAL — a source bug; fail loud, not silent
    *this = std::move(*p);
}

}  // namespace tr::graph
