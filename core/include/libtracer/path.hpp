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
 * @brief A path's learned chain (RFC-0029 §6.2): the `PATH` body an origin adopted from a
 *        reply's `src` — the opaque slot a @ref path_t carries beside its canonical bytes.
 *
 * Two rules shape it: the chain is held *with* the stamp that invalidates it (each PAIR's
 * generation), and "no chain yet" is expressed OUTSIDE the chain (@ref bound) rather than as
 * an invalid one.
 *
 * The body is a run of path elements, origin-first: element 0 is the origin's own PAIR for its
 * first-hop connection vertex, and every later element — a PAIR, or a hop's NAME run — means
 * something only on the host that issued it. This type carries them, compares them and hands
 * them to an encoder, and never interprets one. Nothing about a chain is
 * authorization-shaped: a revoked right takes effect on the very next operation (§6.4).
 *
 * The canonical bytes stay in the @ref path_t alongside it and are never discarded: they are
 * the fallback a refused chain drops back to, which is what makes "drop and re-learn" a
 * complete recovery (§6.3). A VIEW of the path's own storage, valid while that path is
 * unchanged.
 */
struct path_binding_t {
    /** @brief True once a chain has been adopted; false is "never bound", not "invalid". */
    bool bound = false;
    /** @brief The chain's `PATH` body, origin-first. Empty exactly when @ref bound is false. */
    std::span<const std::byte> chain;
    /** @brief Value equality: both flags, and the chains by content. */
    bool operator==(const path_binding_t& o) const noexcept {
        return bound == o.bound && std::ranges::equal(chain, o.chain);
    }
};

/**
 * @brief A parsed, canonical path: the PATH-TLV payload bytes (packed records) plus the
 *        optional @ref field_path_t tail. The payload bytes are the vertex-map key.
 *
 * Dispatch keys on the parsed bytes (@ref key), never the string form — parse once,
 * hold the value, and every read/write compares bytes (docs/reference/02 §dispatch).
 *
 * A path also carries the optional learned chain (@ref binding, RFC-0029 §6.2). It stays a
 * **value type**: the binding is an opaque slot the graph and transport tiers fill and
 * validate, never a handle into either of them, so copying a path copies its binding and
 * neither copy can outlive anything.
 *
 * @section storage Storage (#2286)
 *
 * No standard container: the canonical bytes, the field steps and their names live in ONE
 * block sized exactly at parse, and a learned chain in a second, both drawn from a
 * `tr::mem::block_source_t` — the one given to @ref parse, else the process table source (the
 * same default `path_key_t` spills to, ADR-0083). The root path with no field tail owns no
 * block at all. A copy draws its blocks from the source of the path it copies and, like
 * `path_key_t`'s, stops with a sizing message if that source cannot serve the path block; a
 * chain block it cannot serve leaves the copy unbound instead (the canonical bytes are a
 * complete answer, RFC-0029 §6.3). A move takes the blocks. Nothing here grows after the parse
 * except the chain slot, and that is one exact block per adopted chain.
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

    /** @brief Copy @p o into blocks from @p o's source (see @ref storage). */
    path_t(const path_t& o);
    /** @brief Copy-assign (see the copy constructor). */
    path_t& operator=(const path_t& o);
    /** @brief Take @p o's blocks; @p o is left the empty path. */
    path_t(path_t&& o) noexcept { take(o); }
    /** @brief Move-assign (see the move constructor). */
    path_t& operator=(path_t&& o) noexcept {
        if (this != &o) {
            release();
            take(o);
        }
        return *this;
    }
    /** @brief Return both blocks to their source. */
    ~path_t() { release(); }

    /** @brief The vertex-map key: the canonical PATH-TLV payload bytes (packed records). */
    [[nodiscard]] std::span<const std::byte> key() const noexcept {
        return {block_ + payload_off(), key_len_};
    }
    /** @brief The parsed `:field` tail (empty when the path addresses the vertex value). */
    [[nodiscard]] const field_path_t& field() const noexcept;
    /** @brief The number of segments in the path. */
    [[nodiscard]] std::size_t segment_count() const noexcept { return segments_; }

    /** @brief This path's learned chain, if one has been adopted (RFC-0029 §6.2). */
    [[nodiscard]] path_binding_t binding() const noexcept {
        return {.bound = chain_len_ != 0, .chain = {chain_, chain_len_}};
    }

    /**
     * @brief Record a learned chain — @p chain is a `PATH` body, origin-first.
     *
     * Refuses (leaving the path unbound) an empty chain and one whose body a 16-bit length
     * cannot spell: the canonical path this object still holds is the answer, and refusing
     * beats truncating, which would produce a *valid-looking* chain for a different route.
     * Also refuses, leaving any earlier chain in place, when the source has no block for it.
     *
     * @return true iff the chain was recorded.
     */
    bool bind(std::span<const std::byte> chain) noexcept;

    /**
     * @brief Drop the learned chain and fall back to the canonical path (RFC-0029 §6.3).
     *
     * What an origin does on a refusal. There is nothing to tear down anywhere else — no hop
     * holds state for a chain — so forgetting it IS the teardown, and the next operation goes
     * out canonically and re-learns.
     */
    void clear_binding() noexcept;

   private:
    [[nodiscard]] std::size_t payload_off() const noexcept {
        return has_field_ ? sizeof(field_path_t) : 0;
    }
    [[nodiscard]] tr::mem::block_source_t& source() const noexcept;
    void copy_from(const path_t& o);
    void release() noexcept;
    void take(path_t& o) noexcept;

    tr::mem::block_source_t* src_ = nullptr;  // who serves both blocks; null ⇒ table source
    std::byte* block_ = nullptr;  // [field_path_t if has_field_][key bytes][field names]
    std::byte* chain_ = nullptr;  // the learned chain's PATH body (RFC-0029 §6.2)
    std::uint32_t block_len_ = 0;
    std::uint16_t chain_len_ = 0;  // a chain body is at most 0xFFFF bytes (`bind`)
    std::uint16_t key_len_ = 0;    // at most kMaxPathBytes
    std::uint8_t segments_ = 0;    // at most kMaxSegments
    bool has_field_ = false;
};

/**
 * @brief A `path_t` is three pointers and ten bytes of lengths: 40 B on a 64-bit host, 24 B on
 *        rv32.
 *
 * Pinned, not observed. A path is a parse-once VALUE the API takes by `const&`, and an origin
 * holds them per subscription and per vertex, so its size is multiplied RAM. #2286 moved its three
 * remaining owning standard containers into source-drawn blocks: 9 pointer-widths → 3 plus the
 * lengths (72 → 40 B on a 64-bit host, 36 → 24 B on rv32). A change here is a deliberate edit, and
 * it is the moment to ask whether the new member wants to be a slot at all.
 */
static_assert(sizeof(path_t) == (sizeof(void*) == 8 ? 40 : 24),
              "a path_t's shape is a ratchet — grow it deliberately or not at all");

inline path_t::path_t(std::string_view text) {
    result_t<path_t> p = parse(text);
    if (!p) std::abort();  // malformed path LITERAL — a source bug; fail loud, not silent
    *this = std::move(*p);
}

/**
 * @brief Owned byte key (a copy of a path's canonical payload / one segment record).
 *
 * Small-buffer type (#380 §2): records up to @ref kInlineBytes live inline — a packed
 * record is ONE length byte plus the segment text (RFC-0018), so virtually every vertex
 * name fits and costs NO block at all (a `std::vector` here allocated ~32 B per named
 * vertex). A longer record spills to one block drawn from a `tr::mem::block_source_t`
 * (#1991): the graph passes its injected table source through @ref try_make, so a
 * registration takes nothing from the global heap. The spill block records the source that
 * served it in a pointer-sized prefix, so the key frees and copies without any member of
 * its own naming a source, and `sizeof(path_key_t)` stays 24. Immutable after
 * construction/assignment (matches its use: a vertex's name never changes, ADR-0057). Move
 * leaves the source empty.
 */
class path_key_t {
   public:
    /** @brief Records at or under this many bytes are stored inline (no block): a packed
     *         record is one length byte + the segment text, so names up to 15
     *         characters — the overwhelming norm — never allocate. */
    static constexpr std::size_t kInlineBytes = 16;

    path_key_t() noexcept = default;

    /**
     * @brief Copy @p b into a key, spilling to a block from @p src when it exceeds
     *        @ref kInlineBytes — the failable door the graph registers names through.
     *
     * A spilled key, and every copy of it, must not outlive @p src: the copy draws from the
     * same source, and the destructor gives the block back to it.
     *
     * @retval status_t::BACKPRESSURE @p src refused the spill block; nothing is held.
     */
    [[nodiscard]] static result_t<path_key_t> try_make(std::span<const std::byte> b,
                                                       tr::mem::block_source_t& src) noexcept;

    /** @brief Copy @p b into the key (inline when it fits, else one block from the process
     *         table source, `%tr::mem::table_source()`). Any contiguous byte range binds here,
     *         so a `std::vector` or a core array needs no overload. A refused spill stops the
     *         node (`%tr::mem::exhausted_at_init`): a constructor has no other answer, so code
     *         that can meet exhaustion at runtime uses @ref try_make. */
    explicit path_key_t(std::span<const std::byte> b);

    /** @brief Deep-copy @p o's bytes; a spilled key's copy draws from the source that served
     *         @p o. A refused draw stops the node, as for the span constructor. */
    path_key_t(const path_key_t& o) { assign_or_stop(o.bytes(), o.source()); }
    /** @brief Replace this key with a deep copy of @p o's bytes (drawn as for the copy
     *         constructor). */
    path_key_t& operator=(const path_key_t& o) {
        if (this != &o) {
            release();
            assign_or_stop(o.bytes(), o.source());
        }
        return *this;
    }
    /** @brief Take over @p o's bytes (and spill block, if any); @p o reads empty after. */
    path_key_t(path_key_t&& o) noexcept { take(o); }
    /** @brief Release this key's bytes and take over @p o's; @p o reads empty after. */
    path_key_t& operator=(path_key_t&& o) noexcept {
        if (this != &o) {
            release();
            take(o);
        }
        return *this;
    }
    /** @brief Return the spill block to its source, if this key owns one. */
    ~path_key_t() { release(); }

    /** @brief The key's canonical bytes (inline or spilled — one uniform window). */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {len_ > kInlineBytes ? spill_ : inline_, len_};
    }
    /** @brief The key's byte length. */
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    /** @brief True for the empty key (the root vertex's name). */
    [[nodiscard]] bool empty() const noexcept { return len_ == 0; }

    /** @brief Value equality over the key bytes. */
    bool operator==(const path_key_t& o) const noexcept {
        return std::ranges::equal(bytes(), o.bytes());
    }

   private:
    friend class graph_t;  // sole writer/reader of the borrowed slot memo below (#1486).

    /**
     * @brief `%owner_slot_`'s "no memo recorded" sentinel — never a real slot index.
     *
     * `0` cannot serve: slot 0 is the structural root (RFC-0024 §6.4), a real occupant of
     * `graph_t::vertex_slots_`. `0xFFFFFFFF` is unreachable as an index because a slot
     * index is bounded by the number of `vertex_t` allocations a graph made, and the
     * element field is a `u32` whose top value the mint would have to issue.
     */
    static constexpr std::uint32_t kNoOwnerSlot = 0xFFFFFFFFU;

    /** @brief Bytes ahead of a spilled record: the serving source's address (#1991). */
    static constexpr std::size_t kSpillPrefix = sizeof(tr::mem::block_source_t*);

    /** @brief Store @p b, spilling to a block from @p src (callers guarantee the key owns
     *         nothing). False, with the length unchanged, when @p src refuses the block. */
    [[nodiscard]] bool assign(std::span<const std::byte> b, tr::mem::block_source_t& src) noexcept {
        owner_slot_ = kNoOwnerSlot;  // a fresh set of bytes has no owner (#1486)
        std::byte* dst = inline_;
        if (b.size() > kInlineBytes) {
            void* const blk = src.try_alloc(kSpillPrefix + b.size(), alignof(void*));
            if (blk == nullptr) return false;
            tr::mem::block_source_t* const from = &src;
            std::memcpy(blk, &from, kSpillPrefix);  // the prefix names who frees the block
            dst = spill_ = static_cast<std::byte*>(blk) + kSpillPrefix;
        }
        len_ = static_cast<std::uint32_t>(b.size());
        if (!b.empty()) std::memcpy(dst, b.data(), b.size());
        return true;
    }
    /** @brief `%assign` from @p src, or from the process table source when null (an inline
     *         key has no source); a refusal stops the node. Defined in `path.cpp`. */
    void assign_or_stop(std::span<const std::byte> b, tr::mem::block_source_t* src);
    /** @brief The source a spilled key's block came from; null for an inline key. */
    [[nodiscard]] tr::mem::block_source_t* source() const noexcept {
        if (len_ <= kInlineBytes) return nullptr;
        tr::mem::block_source_t* src = nullptr;
        std::memcpy(&src, spill_ - kSpillPrefix, kSpillPrefix);
        return src;
    }
    /** @brief Return the spill block to its source if this key owns one (sized reclaim). */
    void release() noexcept {
        if (tr::mem::block_source_t* const src = source())
            src->release(spill_ - kSpillPrefix, kSpillPrefix + len_, alignof(void*));
    }
    /** @brief Move @p o's storage into this key (which must own nothing); member-wise —
     *         a whole-object memcpy trips -Werror=class-memaccess on the ESP-IDF gcc. */
    void take(path_key_t& o) noexcept {
        owner_slot_ = kNoOwnerSlot;  // the memo names an OWNER, and bytes moving have none
        len_ = o.len_;
        if (len_ > kInlineBytes)
            spill_ = o.spill_;
        else if (len_ != 0)
            std::memcpy(inline_, o.inline_, len_);  // a trivial byte array — memcpy is fine
        o.len_ = 0;                                 // the moved-from key reads empty, owns nothing
    }

    union {
        std::byte inline_[kInlineBytes]; /**< @brief In-place record storage (the norm). */
        std::byte* spill_; /**< @brief The spilled record when `len_ > kInlineBytes`, just past
                            *         its block's source prefix. */
    };
    std::uint32_t len_ = 0; /**< @brief Record length; doubles as the inline/spill tag. */
    /**
     * @brief **These bytes are `vertex_t`'s, not `path_key_t`'s**: the memoized index of the
     *        `graph_t::vertex_slots_` entry that names the vertex this key is the name of
     *        (#1486). `%kNoOwnerSlot` until a graph assigns one.
     *
     * A deliberate, documented layering compromise, and the reason it is HERE rather than a
     * sibling member of `vertex_t` is a C++ layout rule, not a design preference. The #1487
     * footprint census measured 4 B of dead space at bytes 36–39 of `vertex_t` on BOTH
     * ABIs — but only on rv32 is it `vertex_t`'s own alignment hole. On x86-64 it is the
     * TAIL PADDING of the `name_` subobject (this class: a 16-byte union at 0 plus a `u32`
     * `len_`, 8-aligned ⇒ 24 B with 20–23 dead), and C++ does not let a sibling member of
     * `vertex_t` occupy a member subobject's tail padding. The bytes are reachable from
     * inside `path_key_t` and from nowhere else. Measured consequence: this field costs
     * **zero bytes on both targets** — `sizeof(vertex_t)` stays 96 / 72, `offsetof(lkv_)`
     * stays 0, and both `config_t` ratchets still pass pinned to
     * their measurements.
     *
     * What it buys: `graph_t::vertex_slot` was an O(N) reverse scan of a `std::deque` held
     * under the shared `map_mutex_` — measured at 450 ns for 10³ vertices and **410 µs for
     * 10⁶** (#1485/#1496 ladder), paid per binding mint, so route formation over M bindings
     * against N vertices cost O(M×N) (~200 s at M = N = 10⁶) with the lock held throughout.
     * The memo makes it a load.
     *
     * INTERNAL ONLY. No accessor is exposed on `path_key_t` or on `graph_t`: this is not a
     * property of a name, and a public reader would make a `graph_t` implementation detail
     * part of an L4 value type's contract. It is written ONCE, at slot assignment, under a
     * unique `map_mutex_` hold, and never invalidated — the slot index of a `vertex_t` is
     * immortal (ADR-0057: the vertex map is insert-only and pointer-stable, so slots never
     * move and a retire re-virginizes in place rather than renumbering). There is therefore
     * no invalidation logic to get wrong, which is the property that makes a plain
     * non-atomic word correct here.
     *
     * NOT propagated by copy or move: it names the owner, and a copied name has no owner.
     * `graph_t::vertex_slot` re-validates the memo against the deque before trusting it and
     * falls back to the scan when there is none, so a `vertex_t` built outside a graph (the
     * tests do this) still resolves exactly as it did before.
     *
     * @warning DECLARATION ORDER IS LOAD-BEARING: it must stay LAST, after `len_`. Declared
     *          ahead of the union it is not free at all — the 8-aligned union would start at
     *          offset 8 and `sizeof(path_key_t)` would go 24 → 32 on x86-64, spending the
     *          `vertex_t` ratchet this field was chosen to avoid spending.
     */
    std::uint32_t owner_slot_ = kNoOwnerSlot;
    static_assert(kInlineBytes >= sizeof(std::byte*), "the union must fit the spill pointer");
};

/**
 * @brief `path_key_t` is 24 bytes on EVERY target, and that is a ratchet (#1486, #1487).
 *
 * The number is identical on x86-64 and on rv32 for different reasons, which is exactly why
 * it is worth pinning: on x86-64 the 16-byte union is 8-aligned so `len_` + `owner_slot_`
 * fill what was already tail padding; on rv32 the union is 4-aligned and the two words pack
 * behind it, consuming what was `vertex_t`'s own alignment hole. Either way this key adds
 * NOTHING to `sizeof(vertex_t)` (96 / 72, both ratchets unmoved) — a property that survives
 * only while the members stay in this order and no third word is added. Grow this and the
 * next reader finds out from a `vertex_t` ratchet failure in an unrelated file.
 */
static_assert(sizeof(path_key_t) == 24,
              "path_key_t's shape is a ratchet — it rides inside vertex_t, where the RAM diet "
              "(#361) has zero headroom by construction on both ABIs");

/**
 * @brief Hash functor for @ref path_key_t (FNV-1a over the key bytes) — the map hasher.
 *
 * Heterogeneous (`is_transparent`): the `std::span<const std::byte>` overload hashes the
 * SAME bytes to the IDENTICAL value, so a by-span lookup keys the vertex map without
 * materializing an owned @ref path_key_t (the hot internal by-key path in
 * `graph_t::find_ptr`, which fans out fan_out / bubble_up / ACL-walk / FWD-resolve).
 */
struct path_key_hash_t {
    using is_transparent = void; /**< @brief Enables heterogeneous (by-span) map lookup. */
    /** @brief Hash the key's canonical PATH bytes. */
    [[nodiscard]] std::size_t operator()(const path_key_t& k) const noexcept;
    /** @brief Hash canonical PATH bytes given as a span — same FNV-1a value as the owned key. */
    [[nodiscard]] std::size_t operator()(std::span<const std::byte> k) const noexcept;
};

/**
 * @brief Heterogeneous equality for the vertex map (`is_transparent`): compares
 *        @ref path_key_t and raw `std::span<const std::byte>` key bytes interchangeably,
 *        so a by-span lookup needs no owned key. Byte-equality, length included.
 */
struct path_key_eq_t {
    using is_transparent = void; /**< @brief Enables heterogeneous (by-span) map lookup. */
    /** @brief True iff the two owned keys hold identical bytes. */
    [[nodiscard]] bool operator()(const path_key_t& a, const path_key_t& b) const noexcept {
        return a == b;
    }
    /** @brief True iff the owned key's bytes equal the span's bytes. */
    [[nodiscard]] bool operator()(const path_key_t& a,
                                  std::span<const std::byte> b) const noexcept {
        return std::ranges::equal(a.bytes(), b);
    }
    /** @brief True iff the span's bytes equal the owned key's bytes. */
    [[nodiscard]] bool operator()(std::span<const std::byte> a,
                                  const path_key_t& b) const noexcept {
        return std::ranges::equal(a, b.bytes());
    }
    /** @brief True iff the two spans hold identical bytes. */
    [[nodiscard]] bool operator()(std::span<const std::byte> a,
                                  std::span<const std::byte> b) const noexcept {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
    }
};

}  // namespace tr::graph
