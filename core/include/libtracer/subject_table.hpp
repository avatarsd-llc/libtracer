/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The interned SUBJECT a remote subscription edge delivers to and acts as (RFC-0028 D8,
 * slice 8; #1622): one 4-byte id where the edge's cold half used to carry two `std::string`s.
 */
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

/**
 * @file
 * @brief `tr::graph` interned subjects: the (delivery link, caller subject) pair a remote
 *        subscription edge names, held as a 4-byte refcounted id (RFC-0028 §6.8).
 */

namespace tr::graph {

/**
 * @brief The interned identity of a remote edge's far end — its delivery LINK and the caller
 *        SUBJECT its deliveries run under — as one 4-byte id (RFC-0028 D8).
 *
 * RFC-0028 D8 defines a subject as "a link plus an optional per-writer suffix", so the pair is
 * interned as ONE value rather than as a link id beside a subject id: every edge a peer
 * admits under the same (link, subject) shares one entry, and the edge's cold half spends 4
 * bytes on both names instead of 64 (`subscriber_remote_t`, 120 B → 56 B on a 64-bit host).
 *
 * The encoding is `index | (has_link << 30)`. Bit 31 is always clear, so a holder may keep one
 * flag of its own beside the id in the same word (`subscriber_remote_t` keeps
 * `delivery_compact` there). @ref has_link is a property of the interned VALUE, set by
 * @ref intern_subject, so the per-edge "does this edge have a remote leg" test on the fan-out
 * loop stays one load and one bit test, without reading the entry.
 *
 * A default-constructed id names no entry: an edge with neither a link nor a caller.
 */
struct subject_id_t {
    std::uint32_t bits = 0; /**< @brief `index | (has_link << 30)`; `0` ⇒ no subject. */

    /** @brief Set iff the interned delivery link is non-empty. */
    static constexpr std::uint32_t kLinkBit = std::uint32_t{1} << 30;
    /** @brief The table-index part of @ref bits. */
    static constexpr std::uint32_t kIndexMask = kLinkBit - 1;

    /** @brief True iff this id names an interned entry (a non-empty link or caller). */
    [[nodiscard]] constexpr bool valid() const noexcept { return bits != 0; }
    /** @brief True iff the interned delivery link is non-empty — the remote-leg test. */
    [[nodiscard]] constexpr bool has_link() const noexcept { return (bits & kLinkBit) != 0; }
    /** @brief The entry's table index (`0` ⇒ none). */
    [[nodiscard]] constexpr std::uint32_t index() const noexcept { return bits & kIndexMask; }

    /** @brief Ids compare by value — the same entry and the same link bit. */
    [[nodiscard]] friend constexpr bool operator==(subject_id_t, subject_id_t) noexcept = default;
};

static_assert(sizeof(subject_id_t) == 4, "the interned subject is a 4-byte id (RFC-0028 D8)");

/** @brief The two names a @ref subject_id_t resolves to, borrowed from the table entry. */
struct subject_names_t {
    std::string_view link;   /**< @brief This node's NAME for the delivery link; may be empty. */
    std::string_view caller; /**< @brief The ACL fan-in subject (#81); may be empty. */
};

}  // namespace tr::graph

namespace tr::detail {

/** @brief The size of one @ref subject_entry_t: one cache line. */
inline constexpr std::size_t kSubjectEntryBytes = 64;
/** @brief The entry fields before @ref subject_entry_t::inline_chars. */
inline constexpr std::size_t kSubjectEntryHeadBytes = 2 * sizeof(const char*) + 5 * 4;

/**
 * @brief One interned (link, caller) pair — ONE cache line, spellings included when they fit.
 *
 * The fields a reader touches (@ref link, @ref caller and their lengths) are written under the
 * table mutex BEFORE the id is handed out, and cleared only when the last holder releases it,
 * so a reader holding a reference reads them without a lock: the id reached the reader through
 * the edge record, whose publication already orders these stores before the read.
 *
 * The characters live IN the entry whenever they fit (@ref inline_chars — 28 bytes on a 64-bit
 * host, 36 on rv32, and the default subject stores its one spelling once), so resolving a
 * subject on the delivery path costs the entry's cache line and nothing behind it. Measured on
 * `bench_libtracer fan-remote` with distinct 19-character `host:port` links: spellings in a
 * separate block cost ~12 % of remote fan-out throughput (a second dependent miss per edge);
 * inline, the entry is the only line the resolution touches. Longer spellings take one heap
 * block, as the `std::string` members they replaced did. Entries never move (chunks are never
 * reallocated), which is what lets @ref link point into the entry itself.
 */
struct alignas(kSubjectEntryBytes) subject_entry_t {
    const char* link = nullptr;   /**< @brief The link's characters: @ref inline_chars or a heap
                                   *          block owned by the entry. */
    const char* caller = nullptr; /**< @brief The caller's characters; aliases @ref link when the
                                   *          two spellings are equal (the default subject). */
    std::uint32_t link_len = 0;   /**< @brief @ref link's length. */
    std::uint32_t caller_len = 0; /**< @brief @ref caller's length. */
    std::uint32_t refs = 0;       /**< @brief Holders; `0` ⇒ free. Guarded by the table mutex. */
    std::uint32_t hash = 0;       /**< @brief The pair's hash (the dedup probe's first compare). */
    std::uint32_t next_free = 0;  /**< @brief The free list's link while @ref refs is `0`. */
    /** @brief Where the spellings live when both fit — the rest of the cache line. */
    char inline_chars[kSubjectEntryBytes - kSubjectEntryHeadBytes] = {};
};

static_assert(sizeof(subject_entry_t) == kSubjectEntryBytes,
              "an interned subject is exactly one cache line, spellings included");

/** @brief Entries in chunk 0; chunk `c` holds `kSubjectChunkBase << c`. */
inline constexpr std::uint32_t kSubjectChunkBase = 4;
/** @brief `log2(kSubjectChunkBase)`. */
inline constexpr int kSubjectChunkBaseBits = 2;
static_assert(std::uint32_t{1} << kSubjectChunkBaseBits == kSubjectChunkBase);
/** @brief Chunks needed to cover every 30-bit index: index `i` lives at position
 *         `i + kSubjectChunkBase`, whose bit width is at most 31. */
inline constexpr std::size_t kSubjectChunks = 31 - kSubjectChunkBaseBits;

/**
 * @brief Chunk 0, STATIC: index 0 is its permanently empty entry, so resolving the default
 *        id needs no branch — it reads two null spellings of length 0 like any other entry.
 *
 * Four entries (256 B) and not more, because this is paid in `.bss` by every node, including
 * the MCU node that never admits a remote subscriber; the directory adds one pointer per chunk.
 */
inline constinit subject_entry_t subject_chunk0[kSubjectChunkBase]{};

/**
 * @brief The entry directory: geometric chunks that are allocated once and never move.
 *
 * Chunked rather than one growing array so that a lock-free reader never races a
 * reallocation: a chunk pointer is published once (release) and stays valid for the life of
 * the process. Chunk 0 is @ref subject_chunk0 from the start. `constinit` and trivially
 * destructible, so it is valid before any static constructor runs and after every static
 * destructor has, which is what lets an edge record owned by a static object release its
 * subject at exit.
 */
inline constinit std::atomic<subject_entry_t*> subject_chunks[kSubjectChunks]{subject_chunk0};

/** @brief Chunk number and offset of table index @p index. */
struct subject_slot_t {
    std::size_t chunk;  /**< @brief Which chunk. */
    std::size_t offset; /**< @brief Where in it. */
};

/** @brief Where index @p index lives — a bit scan, a shift and a subtract, no table read. */
[[nodiscard]] constexpr subject_slot_t subject_slot(std::uint32_t index) noexcept {
    const std::uint32_t pos = index + kSubjectChunkBase;
    const auto chunk = static_cast<std::size_t>(std::bit_width(pos) - 1 - kSubjectChunkBaseBits);
    return {chunk, pos - (kSubjectChunkBase << chunk)};
}

}  // namespace tr::detail

namespace tr::graph {

/**
 * @brief Intern the pair (@p link, @p caller) and take one reference on it.
 *
 * Idempotent by value: the same pair always answers the same id while any holder keeps it.
 * Runs under the table's mutex, once per admission — never on a delivery path.
 *
 * @param link   This node's NAME for the delivery link; empty for an edge with no remote leg.
 * @param caller The ACL fan-in subject; empty for a locally-wired edge.
 * @return The id with one reference taken; a default id (and no reference) when both names
 *         are empty; `std::nullopt` when the table is exhausted — out of memory, or all
 *         2^30 − 1 indices live. Exhaustion is a value: the admitting door refuses the edge.
 */
[[nodiscard]] std::optional<subject_id_t> intern_subject(std::string_view link,
                                                         std::string_view caller) noexcept;

/**
 * @brief Drop one reference taken by @ref intern_subject; the last one frees the entry.
 *
 * Takes the table mutex, which is a LEAF lock (nothing is acquired under it), so it is safe
 * from any context an edge record can die in, including under a vertex stripe lock.
 * A default id is a no-op.
 */
void release_subject(subject_id_t id) noexcept;

/**
 * @brief The names @p id resolves to — lock-free; valid while the caller holds a reference.
 *
 * On the delivery path (one per remote or caller-gated edge per publish): a chunk-directory
 * load and the entry's four fields.
 */
[[nodiscard]] inline subject_names_t subject_names(subject_id_t id) noexcept {
    // Branch-free: the default id is index 0, chunk 0's permanently empty entry.
    const detail::subject_slot_t at = detail::subject_slot(id.index());
    const detail::subject_entry_t& e =
        detail::subject_chunks[at.chunk].load(std::memory_order_acquire)[at.offset];
    return {std::string_view(e.link, e.link_len), std::string_view(e.caller, e.caller_len)};
}

/** @brief How many interned pairs are live, process-wide — diagnostics and the leak test. */
[[nodiscard]] std::size_t live_subject_count() noexcept;

}  // namespace tr::graph
