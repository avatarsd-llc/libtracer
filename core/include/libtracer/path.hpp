/*
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
 * @brief A path's PATH-LABEL spelling: the packed `PATH` body a fully-minted reply came back
 *        with (RFC-0027 §6.1) — the second opaque slot a @ref path_t carries beside its
 *        canonical bytes.
 *
 * Always the qualified **path label** (§11.1 collision 1, ruled *(a) qualify* at acceptance):
 * an unqualified "label" is RFC-0004 §E.1's per-link `u16` and nothing else.
 *
 * RFC-0027's distribution is passive: no host asks for a label and no frame carries a request.
 * Each forwarding hop rewrites its own local part of `src`/`dst` from string to the label it
 * minted, on a reply it was relaying anyway, so the FIRST reply returns to the original sender
 * already minted — and this is where that sender keeps it.
 *
 * **Opaque to the application, and that is normative (§9).** These bytes are filled and
 * validated by the net tier and by nobody else: a label never appears in a host-API call, never
 * in a path the application constructs, and never in a local resolution. `path_t::key()` keeps
 * answering the canonical bytes whatever is cached here, because a labelled `PATH` is not a
 * `path_lookup_key` (§5.3 amendment 5) and the vertex map must stay pure-string for
 * `key_view_t`'s byte-prefix-implies-ancestor invariant to hold.
 *
 * The canonical bytes are never discarded: they are what every label was minted FROM and what a
 * refused label falls back to, which is why "drop and re-mint" is a complete recovery and no
 * address is ever reachable in label form alone (§7.2).
 */
struct path_label_cache_t {
    /** @brief True once a labelled spelling has been cached; false is "never minted". */
    bool cached = false;
    /** @brief The labelled packed `PATH` body — carried, compared, handed to an encoder, and
     *         NEVER interpreted here. Empty exactly when @ref cached is false. */
    std::vector<std::byte> body;
    /** @brief Value equality over both fields. */
    bool operator==(const path_label_cache_t&) const = default;
};

/**
 * @brief A parsed, canonical path: the PATH-TLV payload bytes (packed records) plus the
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

    /** @brief The vertex-map key: the canonical PATH-TLV payload bytes (packed records). */
    [[nodiscard]] std::span<const std::byte> key() const noexcept {
        return {payload_.data(), payload_.size()};
    }
    /** @brief The parsed `:field` tail (empty when the path addresses the vertex value). */
    [[nodiscard]] const field_path_t& field() const noexcept { return field_; }
    /** @brief The number of segments in the path. */
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

    /**
     * @brief This path's PATH-LABEL spelling, if a reply came back minted (RFC-0027 §6.1).
     *
     * Named for the qualified term throughout, never bare "label": §11.1 collision 1 was ruled
     * **(a) qualify** at acceptance, so an unqualified "label" still means RFC-0004 §E.1's
     * per-link `u16` and nothing else (`route_handle.hpp`'s is one), and this RFC's concept is
     * always the **path label**.
     */
    [[nodiscard]] const path_label_cache_t& path_label() const noexcept { return labels_; }

    /**
     * @brief Cache the path-label spelling @p body a minted reply came back with (§6.1).
     *
     * **The NET TIER's call, never the application's** (§9). The bytes are a packed `PATH`
     * body: a mixture of literal segments and label elements, in any order, exactly as the
     * reply carried them — mixed paths are legal and expected, because a hop that does not mint
     * simply leaves its own part a string (§5.2).
     *
     * Refused (leaving the path unlabelled, and nothing else touched) when the spelling has no
     * reading or no point:
     *
     * - a body that does not walk cleanly, or that refuses the address on any record — a
     *   malformed element is `tr::path::invalid`, and caching one would hand the next
     *   operation an address the far hop must refuse;
     * - a body carrying **no** path-label element — a pure-string spelling is what @ref key
     *   already holds, and caching a second copy of it would buy a second thing to invalidate;
     * - a body past `%kMaxPathBytes`, the same bound the canonical form is parsed under;
     * - a path that is already BOUND (`PATH_REF`). §11.2 recommends against carrying two
     *   compressions of one address — they save the bytes one of them already saved and double
     *   the staleness surface for a single route — and this is the one arm where the refusal
     *   costs nothing to state, because the slot is new. It is deliberately NOT symmetric:
     *   `bind` is RFC-0024's shipped surface and keeps its behaviour exactly, since §11.2 is a
     *   SHOULD pending §12.4's measurement and the per-route choice is the mint call site's
     *   (car 4). @ref clear_path_label frees this arm whenever a caller wants the other form.
     *
     * @return true iff the path-label spelling was cached.
     */
    bool cache_path_label(std::span<const std::byte> body);

    /**
     * @brief Drop the path-label spelling and fall back to the canonical one (RFC-0027 §7.2).
     *
     * What an origin does on a `NOT_FOUND`-class refusal. There is nothing to withdraw
     * anywhere — no unbind frame, no lease, no TTL, no aging (§7.3) — so forgetting the bytes
     * IS the recovery: the next operation goes out in strings, which always work, and the
     * reply after it re-mints.
     */
    void clear_path_label() noexcept {
        labels_.body.clear();
        labels_.cached = false;
    }

   private:
    std::vector<std::byte> payload_;  // canonical PATH-TLV payload (packed segment records)
    field_path_t field_;
    std::size_t segments_ = 0;
    // The RFC-0024 §7.4 opaque slot. Empty for every path that has never been bound, which is
    // every path until an origin asks for a mint — so an unbound path costs one empty vector
    // and no allocation, and nothing on the canonical hot path reads it.
    path_binding_t binding_;
    // The RFC-0027 §6.1 opaque slot. Empty for every path until a minted reply comes back, so a
    // never-labelled path costs one empty vector and no allocation, and nothing on the canonical
    // hot path reads it — §9's local-IO exclusion is structural, not a convention.
    path_label_cache_t labels_;
};

/**
 * @brief A `path_t` is FIFTEEN pointer-widths: four owning containers plus a count.
 *
 * Pinned, not observed. A path is a parse-once VALUE the API takes by `const&`, and each of the
 * two opaque slots (RFC-0024's binding, RFC-0027's path label) added a container to it — the
 * exact growth an unwatched type absorbs one RFC at a time until somebody embeds it in a hot
 * struct and pays for it per vertex. Expressed in `sizeof(void*)` so it holds identically on
 * rv32 and on a 64-bit host, which is the whole point of pinning a shape rather than a number:
 * a change here is a deliberate edit, and it is the moment to ask whether the new member wants
 * to be a slot at all. Same instrument, same reason, as `sizeof(path_label_t) == 4`.
 */
static_assert(sizeof(path_t) == 15 * sizeof(void*),
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
 * name fits and costs NO heap block (a `std::vector` here allocated ~32 B per named
 * vertex). Longer records spill to one owned heap allocation. Immutable after
 * construction/assignment (matches its use: a vertex's name never changes,
 * ADR-0057). Move leaves the source empty.
 */
class path_key_t {
   public:
    /** @brief Records at or under this many bytes are stored inline (no heap): a packed
     *         record is one length byte + the segment text, so names up to 15
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

    /** @brief Store @p b (callers guarantee the key currently owns nothing). */
    void assign(std::span<const std::byte> b) {
        owner_slot_ = kNoOwnerSlot;  // a fresh set of bytes has no owner (#1486)
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
        owner_slot_ = kNoOwnerSlot;  // the memo names an OWNER, and bytes moving have none
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
     * stays 0 (the #1285 straddle gate), and both `config_t` ratchets still pass pinned to
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
