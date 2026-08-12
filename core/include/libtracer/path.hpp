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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/path_ref.hpp"
#include "libtracer/status.hpp"

namespace tr::graph {

/** @brief Max bytes in one NAME segment (docs/reference/03 §limits). */
inline constexpr std::size_t kMaxSegmentBytes = 64;
/** @brief Max bytes in a whole canonical PATH payload (docs/reference/03 §limits). */
inline constexpr std::size_t kMaxPathBytes = 1024;
/** @brief Max NAME segments in a path (reference/03 §limits; RFC-0023: min(255, byte cap)). */
inline constexpr std::size_t kMaxSegments = 255;
/** @brief Max steps in a `:field` tail (docs/reference/03 §limits). */
inline constexpr std::size_t kMaxFieldDepth = 8;

/**
 * @brief True iff @p seg is valid as ONE NAME segment of the addressing grammar.
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
 */
struct field_step_t {
    std::string name;     /**< @brief The step's NAME (the text before any `[...]`). */
    bool indexed = false; /**< @brief True if a `[...]` selector was present. */
    bool append = false;  /**< @brief True for `[]` — append to a sequence. */
    /** @brief True for `[*]` — FIELD index_mode=WILDCARD (RFC-0004 §C). */
    bool wildcard = false;
    /** @brief The `[N]` index; valid when `indexed && !append && !wildcard`. */
    std::uint16_t index = 0;
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
    /** @brief The `.`-separated steps; empty ⇒ the vertex value itself. */
    std::vector<field_step_t> steps;
    /** @brief True when there is no field tail (addresses the vertex value). */
    [[nodiscard]] bool empty() const noexcept { return steps.empty(); }
    /** @brief Value equality over the step sequence. */
    bool operator==(const field_path_t&) const = default;
};

/**
 * @brief A path's BOUND form: the stack of node-scoped vertex refs a mint answered with
 *        (RFC-0024 §7.4) — the opaque slot a @ref path_t carries beside its canonical bytes.
 *
 * Shaped on `tr::net::resolved_binding_t`, which has been the "a resolution plus its own
 * staleness signal" record since ADR-0062, and for the same two reasons: the answer is held
 * *with* the stamp that invalidates it, and "no resolution yet" is expressed OUTSIDE the
 * resolution (@ref bound) rather than as an invalid one. It is a separate type only because
 * that one lives in the transport plane, and an address must not depend downward on it.
 *
 * The elements are **opaque here and everywhere but on the host that minted each one**: this
 * type carries them, compares them and hands them to an encoder, and never interprets one.
 * Nothing about a binding is authorization-shaped — a bound path holds no ACL state at all,
 * which is why a revoked right takes effect on the very next operation over an already-minted
 * binding (RFC-0024 §6.2).
 *
 * The canonical bytes stay in the @ref path_t alongside it and are never discarded: they are
 * the key the binding was minted FROM and the fallback a failed binding drops back to, which
 * is what makes "drop and re-mint" a complete recovery (RFC-0024 §1, §5.3).
 */
struct path_binding_t {
    /** @brief True once a mint has completed; false is "never bound", not "invalid". */
    bool bound = false;
    /** @brief The route's vertex refs, origin-first — element *i* means something only on
     *         host *i*. Empty exactly when @ref bound is false. */
    std::vector<wire::path_ref_element_t> elements;
    /** @brief Value equality over both fields. */
    bool operator==(const path_binding_t&) const = default;
};

/**
 * @brief A parsed, canonical path: the PATH-TLV payload bytes (NAME children) plus the
 *        optional @ref field_path_t tail. The payload bytes are the vertex-map key.
 *
 * Dispatch keys on the parsed bytes (@ref key), never the string form — parse once,
 * hold the value, and every read/write compares bytes (docs/reference/02 §dispatch).
 *
 * A path also carries the optional bound form (@ref binding, RFC-0024 §7.4). It stays a
 * **value type**: the binding is an opaque slot the graph and transport tiers fill and
 * validate, never a handle into either of them, so copying a path copies its binding and
 * neither copy can outlive anything.
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

    /** @brief This path's bound form, if a mint has completed (RFC-0024 §7.4). */
    [[nodiscard]] const path_binding_t& binding() const noexcept { return binding_; }

    /**
     * @brief Record the bound form a mint answered with — @p elements in ROUTE order.
     *
     * Element 0 is the origin's own reference to its first-hop connection vertex; the last is
     * the terminus host's reference to the target vertex. A mint answers only for the hosts
     * that saw the operation, so the origin stacks its own element under what came back.
     *
     * Refuses (leaving the path unbound) past the normative element bound: a route with more
     * hosts than a `PATH_REF` can spell has no bound spelling, and the canonical path this
     * object still holds is the answer — refusing beats truncating, which would produce a
     * *valid-looking* binding for a different route.
     *
     * @return true iff the binding was recorded.
     */
    bool bind(std::span<const wire::path_ref_element_t> elements) {
        if (elements.empty() || elements.size() > wire::kMaxPathRefElements) return false;
        binding_.elements.assign(elements.begin(), elements.end());
        binding_.bound = true;
        return true;
    }

    /**
     * @brief Drop the bound form and fall back to the canonical one (RFC-0024 §5.3).
     *
     * What an origin does on a failed validation. There is nothing to tear down anywhere else
     * — no hop holds state for a bound path — so forgetting the elements IS the teardown, and
     * the next operation goes out canonically and may re-mint.
     */
    void clear_binding() noexcept {
        binding_.elements.clear();
        binding_.bound = false;
    }

   private:
    std::vector<std::byte> payload_;  // canonical PATH-TLV payload (NAME children)
    field_path_t field_;
    std::size_t segments_ = 0;
    // The RFC-0024 §7.4 opaque slot. Empty for every path that has never been bound, which is
    // every path until an origin asks for a mint — so an unbound path costs one empty vector
    // and no allocation, and nothing on the canonical hot path reads it.
    path_binding_t binding_;
};

inline path_t::path_t(std::string_view text) {
    result_t<path_t> p = parse(text);
    if (!p) std::abort();  // malformed path LITERAL — a source bug; fail loud, not silent
    *this = std::move(*p);
}

/**
 * @brief Owned byte key (a copy of a path's canonical payload / one NAME record).
 *
 * Small-buffer type (#380 §2): records up to @ref kInlineBytes live inline — a NAME
 * record is a 4-byte TLV header plus the segment text, so virtually every vertex name
 * fits and costs NO heap block (a `std::vector` here allocated ~32 B per named
 * vertex). Longer records spill to one owned heap allocation. Immutable after
 * construction/assignment (matches its use: a vertex's name never changes,
 * ADR-0057). Move leaves the source empty.
 */
class path_key_t {
   public:
    /** @brief Records at or under this many bytes are stored inline (no heap): a NAME
     *         record is a 4-byte TLV header + the segment text, so names up to 12
     *         characters — the overwhelming norm — never allocate. */
    static constexpr std::size_t kInlineBytes = 16;

    path_key_t() noexcept = default;
    /** @brief Copy @p b into the key (inline when it fits, else one heap block). */
    explicit path_key_t(std::span<const std::byte> b) { assign(b); }
    /** @brief Copy the vector's bytes (compat shape for `path_key_t{vector}` callers). */
    explicit path_key_t(const std::vector<std::byte>& b) { assign(b); }

    /** @brief Deep-copy @p o's bytes (inline or one spill block, as the length needs). */
    path_key_t(const path_key_t& o) { assign(o.bytes()); }
    /** @brief Replace this key with a deep copy of @p o's bytes. */
    path_key_t& operator=(const path_key_t& o) {
        if (this != &o) {
            release();
            assign(o.bytes());
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
    /** @brief Free the spill block, if this key owns one. */
    ~path_key_t() { release(); }

    /** @brief The key's canonical bytes (inline or heap — one uniform window). */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {len_ > kInlineBytes ? heap_ : inline_, len_};
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
    /** @brief Store @p b (callers guarantee the key currently owns nothing). */
    void assign(std::span<const std::byte> b) {
        len_ = static_cast<std::uint32_t>(b.size());
        std::byte* dst = inline_;
        if (b.size() > kInlineBytes) dst = heap_ = new std::byte[b.size()];
        if (!b.empty()) std::memcpy(dst, b.data(), b.size());
    }
    /** @brief Free the spill block if this key owns one. */
    void release() noexcept {
        if (len_ > kInlineBytes) delete[] heap_;
    }
    /** @brief Move @p o's storage into this key (which must own nothing); member-wise —
     *         a whole-object memcpy trips -Werror=class-memaccess on the ESP-IDF gcc. */
    void take(path_key_t& o) noexcept {
        len_ = o.len_;
        if (len_ > kInlineBytes)
            heap_ = o.heap_;
        else if (len_ != 0)
            std::memcpy(inline_, o.inline_, len_);  // a trivial byte array — memcpy is fine
        o.len_ = 0;                                 // the moved-from key reads empty, owns nothing
    }

    union {
        std::byte inline_[kInlineBytes]; /**< @brief In-place record storage (the norm). */
        std::byte* heap_;                /**< @brief The spill block when `len_ > kInlineBytes`. */
    };
    std::uint32_t len_ = 0; /**< @brief Record length; doubles as the inline/heap tag. */
    static_assert(kInlineBytes >= sizeof(std::byte*), "the union must fit the spill pointer");
};

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
