/**
 * @file
 * @brief The frame codec: validate wire bytes and read them in place (tr::wire::tlv_node_t),
 *        and encode a TLV tree back to bytes.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The frame codec: validate one frame and walk it in place (`tlv_node_t::over` +
 * `children()`), and encode a `tlv_t` tree back to bytes. Reading never copies or allocates —
 * nodes are std::span views into the caller's input buffer, which must outlive them. The
 * owning-tree `decode` that returned a `tlv_t` with vector children is gone from the library
 * (#1829); a reader walks nodes, and a caller that needs a bounded materialized tree wants the
 * terminus arena (`wire::decode_into`). See docs/reference/01-data-format.md +
 * 05-protocol-tlvs.md.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

#include "libtracer/error.hpp"
#include "libtracer/grammar.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/view.hpp"

namespace tr::wire {

// Decode failures reuse the RFC-0002 registry codes (error.hpp) directly — the
// grammar returns `err_t` (ADR-0048), so `err_path`/severity/disposition come
// for free and there is no parallel decode-only error vocabulary. Decode only
// ever yields FRAME_TRUNCATED / FRAME_INVALID / FRAME_CRC_FAIL /
// TLV_NESTING_TOO_DEEP ("exceeds this receiver's decode resources", RFC-0006 —
// never reached by this heap-spilled decode, only by a null-spill grammar walk).

/** @brief A decoded trailer CRC: its width and the (zero-extended) checksum value. */
struct crc_t {
    /** @brief The CRC algorithm/width carried in the trailer. */
    enum class width_t {
        CRC32C,      /**< @brief 32-bit CRC-32C (Castagnoli). */
        CRC16_CCITT, /**< @brief 16-bit CRC-16/CCITT. */
    };
    width_t width{};       /**< @brief Which CRC algorithm produced @ref value. */
    std::uint32_t value{}; /**< @brief The checksum; 16-bit values are zero-extended. */
    /** @brief Value equality over width and value. */
    constexpr bool operator==(const crc_t&) const noexcept = default;
};

/** @brief A decoded trailer timestamp: absolute u64 ns or a relative i32 ns delta. */
struct timestamp_t {
    bool relative = false;  /**< @brief false = absolute u64 ns; true = relative i32 ns. */
    std::int64_t value = 0; /**< @brief The timestamp / delta in nanoseconds. */
    /** @brief Value equality over the relative flag and value. */
    constexpr bool operator==(const timestamp_t&) const noexcept = default;
};

/** @brief A decoded TLV trailer: an optional @ref timestamp_t and/or @ref crc_t. */
struct trailer_t {
    std::optional<timestamp_t> ts; /**< @brief The trailer timestamp, if present. */
    std::optional<crc_t> crc;      /**< @brief The trailer CRC, if present. */
    /** @brief Value equality over both optional fields. */
    constexpr bool operator==(const trailer_t&) const noexcept = default;
};

/**
 * @brief A TLV tree a caller BUILDS to encode: the eager, owning model @ref encode serializes.
 *
 * For opaque TLVs (`opt.pl == 0`) @ref payload holds the bytes and @ref children is
 * empty; for structured TLVs (`opt.pl == 1`) @ref children holds the sub-TLVs and
 * @ref payload is empty. @ref payload (and child payloads) BORROW the caller's bytes.
 *
 * The library no longer decodes INTO this type (#1829): reading a frame walks it in place as
 * @ref tlv_node_t, which allocates nothing.
 */
struct tlv_t {
    type_t type{}; /**< @brief The TLV type code. */
    opt_t opt{};   /**< @brief The option bits (pl / cr / ll / …). */
    std::span<const std::byte>
        payload{};                 /**< @brief Opaque bytes (borrowed); empty when structured. */
    std::vector<tlv_t> children{}; /**< @brief Parsed sub-TLVs; empty when opaque. */
    std::optional<trailer_t> trailer{}; /**< @brief The decoded trailer, if present. */
};

/** @brief Structural + byte-content equality (spans compared by content, recursively). */
[[nodiscard]] bool equal(const tlv_t& a, const tlv_t& b) noexcept;

class tlv_children_t;

/**
 * @brief One TLV of a validated frame, read in place: the library's one reader of a frame
 *        (#1648, #1829).
 *
 * A node is two words of borrowed bytes plus the header facts, never a tree. It is minted by
 * @ref over (which validates the WHOLE frame once, CRC trailers included) or by walking a
 * validated node's @ref children, so every node a caller can hold has already passed the
 * grammar, CRC trailers included. That is what lets the walk itself be free: stepping to the
 * next sibling re-reads one header and never fails.
 *
 * A node copies by value and allocates nothing. It borrows the bytes handed to @ref over, which
 * must outlive it and every node walked from it.
 */
class tlv_node_t {
   public:
    /**
     * @brief Validate @p input as exactly one TLV and return its root node.
     *
     * The acceptance is the grammar's: one @ref grammar::walk over the whole frame with inline
     * walk slots sized for the typical FWD nesting, refusing with `FRAME_TRUNCATED` /
     * `FRAME_INVALID` / `FRAME_CRC_FAIL` / `TLV_NESTING_TOO_DEEP`. Nothing is built, so a frame
     * nested no deeper than the inline slots validates with zero allocations, and a deeper one
     * draws only its walk stack from @p spill — the RFC-0006 decode-resource bound is the
     * caller's to inject (#873, ADR-0079). `over(input, mem::null_source())` refuses the deeper
     * frame instead.
     *
     * @param input The bytes to validate — exactly one TLV; trailing bytes ⇒ `FRAME_INVALID`.
     * @param spill The block source the walk stack spills into past its inline slots.
     * @return The root node (borrowing @p input), or the grammar's `err_t`.
     */
    [[nodiscard]] static std::expected<tlv_node_t, err_t> over(
        std::span<const std::byte> input, mem::block_source_t& spill = mem::heap_source());

    /** @brief The L1↔L2 cast in non-owning form: @ref over on @p v's bytes. */
    [[nodiscard]] static std::expected<tlv_node_t, err_t> over(
        const view::view_t& v, mem::block_source_t& spill = mem::heap_source()) {
        return over(v.bytes(), spill);
    }

    /** @brief The TLV type code. */
    [[nodiscard]] type_t type() const noexcept { return type_; }
    /** @brief The decoded `opt` bits, trailer bits as on the wire. */
    [[nodiscard]] opt_t opt() const noexcept { return opt_; }
    /** @brief The whole TLV: header, body and trailer. */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    /**
     * @brief Header plus body, trailer excluded: the ADR-0041 §4 whole-TLV-copy span, the same
     *        shape as @ref arena_tlv_t::wire (a copier clears the trailer bits of the opt byte).
     */
    [[nodiscard]] std::span<const std::byte> wire() const noexcept {
        return bytes_.first(header_ + length_);
    }
    /** @brief The body: payload bytes when opaque, the packed children region when structured. */
    [[nodiscard]] std::span<const std::byte> body() const noexcept {
        return bytes_.subspan(header_, length_);
    }
    /** @brief The opaque payload, as in @ref tlv_t::payload — the body, or empty when structured.
     */
    [[nodiscard]] std::span<const std::byte> payload() const noexcept {
        return opt_.pl ? std::span<const std::byte>{} : body();
    }
    /** @brief The direct children, in wire order; an empty range for an opaque node. */
    [[nodiscard]] tlv_children_t children() const noexcept;
    /** @brief The trailer's timestamp and CRC values, read from the bytes; `nullopt` if none. */
    [[nodiscard]] std::optional<trailer_t> trailer() const noexcept;

   private:
    friend class tlv_children_t;
    /** @brief The empty node an exhausted walk iterator holds; never handed to a caller. */
    tlv_node_t() = default;
    /** @brief A node over @p bytes, whose header @p h the grammar has already accepted. */
    tlv_node_t(const grammar::header_t& h, std::span<const std::byte> bytes) noexcept
        : bytes_(bytes.first(h.total)),
          type_(h.type),
          opt_(h.opt),
          header_(static_cast<std::uint8_t>(h.header)),
          length_(h.length) {}

    std::span<const std::byte> bytes_; /**< @brief The whole TLV (header + body + trailer). */
    type_t type_{};                    /**< @brief The TLV type code. */
    opt_t opt_{};                      /**< @brief The decoded `opt` bits. */
    std::uint8_t header_ = 0;          /**< @brief Header length: 4, or 6 with `LL`. */
    std::size_t length_ = 0;           /**< @brief Body length. */
};

/**
 * @brief The direct children of a validated @ref tlv_node_t, walked in place (#1648).
 *
 * A forward range over the parent's children region: each step reads ONE header (CRC deferred,
 * since @ref tlv_node_t::over already verified every trailer) and yields that child as a
 * @ref tlv_node_t. No allocation, no error channel — the region was validated before any node
 * over it could exist. Descend by walking a child's own @ref tlv_node_t::children.
 */
class tlv_children_t {
   public:
    /** @brief The forward iterator: the unread rest of the region, its front child cached. */
    class iterator {
       public:
        using value_type = tlv_node_t;          /**< @brief What a step yields. */
        using difference_type = std::ptrdiff_t; /**< @brief Iterator distance type. */
        /** @brief A C++20 forward iterator (multi-pass; it yields its node by value). */
        using iterator_concept = std::forward_iterator_tag;
        /** @brief Legacy category: by-value deref makes it a Cpp17 input iterator only. */
        using iterator_category = std::input_iterator_tag;

        iterator() = default;
        /** @brief The front child of the unread region. */
        [[nodiscard]] tlv_node_t operator*() const noexcept { return front_; }
        /** @brief Step past the front child. */
        iterator& operator++() noexcept {
            rest_ = rest_.subspan(front_.bytes_.size());
            load();
            return *this;
        }
        /** @brief Post-increment. */
        iterator operator++(int) noexcept {
            iterator was = *this;
            ++*this;
            return was;
        }
        /** @brief True once the region is consumed. */
        [[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept {
            return rest_.empty();
        }
        /** @brief Positional equality: two iterators over the same unread rest. */
        [[nodiscard]] bool operator==(const iterator& o) const noexcept {
            return rest_.data() == o.rest_.data() && rest_.size() == o.rest_.size();
        }

       private:
        friend class tlv_children_t;
        explicit iterator(std::span<const std::byte> region) noexcept : rest_(region) { load(); }
        /**
         * @brief Parse the front header of `rest_` (already validated, so it cannot fail).
         *
         * Defined OUT OF LINE in frame.cpp on purpose (#1859): inline here, every child walk
         * in a translation unit adds a `grammar::parse_header` call site to it, and GCC 13
         * then stops inlining `parse_header` into that unit's hot header readers
         * (`read_fwd_header`, `peek_control` in fwd_router.cpp) — +55% on `compact-forward`.
         * The pin on `read_fwd_header` in bench/symbol_ratchet.json trips if it happens again.
         */
        void load() noexcept;

        std::span<const std::byte> rest_{}; /**< @brief Unread region, front child first. */
        tlv_node_t front_{};                /**< @brief The cached front child. */
    };

    /** @brief An empty range: no children. */
    tlv_children_t() = default;

    /**
     * @brief The children from @p at onward: the unread tail of a range this walk produced,
     *        still validated (#1829). A reader that consumed a fixed prefix (a BATCH base, an
     *        offset run) hands the rest on as a range of its own.
     */
    [[nodiscard]] static tlv_children_t from(const iterator& at) noexcept {
        return tlv_children_t(at.rest_);
    }

    /** @brief The first child. */
    [[nodiscard]] iterator begin() const noexcept { return iterator(region_); }
    /** @brief The end sentinel. */
    [[nodiscard]] static std::default_sentinel_t end() noexcept { return {}; }
    /** @brief True for an opaque parent or an empty children region. */
    [[nodiscard]] bool empty() const noexcept { return region_.empty(); }

   private:
    friend class tlv_node_t;
    explicit tlv_children_t(std::span<const std::byte> region) noexcept : region_(region) {}
    std::span<const std::byte> region_{}; /**< @brief The parent's children region. */
};

inline tlv_children_t tlv_node_t::children() const noexcept {
    return tlv_children_t(opt_.pl ? body() : std::span<const std::byte>{});
}

/**
 * @brief The INJECTED wire-time clock seam (#1109): where a stamping producer's trailer-TS
 *        nanoseconds come from.
 *
 * The library itself never reads an ambient clock — not here, not anywhere on a frame path.
 * A producer that wants wire-time stamps constructs one of these over whatever time source
 * its platform has and passes it to `stamp_ts`; a producer that does not stamp pays
 * nothing, the same per-frame opt-in shape the trailer CRC has always had.
 *
 * The contract is CONTEXT.md's `origin_timestamp`: the value MUST be per-producer MONOTONIC
 * (HLC-style — strictly increasing per origin, wall-clock-seeded where available, bumped
 * logically on coarse clocks or NTP backward jumps). Wall-clock meaning is advisory;
 * cross-producer comparison is undefined by design. On an MCU without SNTP the epoch anchor
 * may be arbitrary — that does not affect the echo/RTT use, where only the origin's own
 * clock is ever read (`RTT = now_ns() - echoed_stamp`).
 */
class wire_clock_t {
   public:
    /** @brief The producer's current wire-time in nanoseconds (TF=0 trailer units). */
    [[nodiscard]] virtual std::int64_t now_ns() noexcept = 0;

   protected:
    /** @brief Seam type: destroyed only as its concrete implementation, never through here. */
    ~wire_clock_t() = default;
};

/**
 * @brief Stamp @p tlv with an ABSOLUTE (TF=0) wire-time trailer of @p now_ns — the writer
 *        half of the trailer timestamp the decoders have always read (#1109).
 *
 * Sets `opt.ts`, clears `opt.tf`, and writes the trailer value, so a stamped TLV can never
 * reach `encode`'s loud opt-without-value refusal. Absolute-only ON PURPOSE: the relative
 * form (TF=1) is anchored to the parent's stamp and the spec's anchorless-reject rule
 * (docs/reference/01-data-format.md §relative) is a conformance gap the reference codec has
 * not closed — writing TF=1 before that check exists mints frames that decode to silent
 * near-epoch garbage on a non-validating receiver. The byte layout for both forms already
 * has one home (`wire::store_trailer_ts`), so the TF=1 writer is a small follow-up gated on
 * that check, not a redesign.
 */
inline void stamp_ts(tlv_t& tlv, std::int64_t now_ns) noexcept {
    tlv.opt.ts = true;
    tlv.opt.tf = false;
    if (!tlv.trailer) tlv.trailer.emplace();
    tlv.trailer->ts = timestamp_t{.relative = false, .value = now_ns};
}

/** @brief `stamp_ts` from the injected @p clock — the one call a stamping hot path makes. */
inline void stamp_ts(tlv_t& tlv, wire_clock_t& clock) noexcept { stamp_ts(tlv, clock.now_ns()); }

/**
 * @brief Encode a TLV to its wire bytes (recomputing the trailer CRC when `opt.cr` is set).
 *
 * The length width is not taken from @p tlv verbatim: a body larger than 0xFFFF widens to the
 * u32 `LL` form regardless of `tlv.opt.ll`, the same rule `wire::emit_tlv` applies (#924), so a
 * programmatically built tree can no longer serialize a length truncated to `size & 0xFFFF`. A
 * body at or under 0xFFFF is emitted unchanged; `tlv.opt.ll` is never cleared. A body over
 * 0xFFFFFFFF still truncates modulo 2^32 — the grammar has no wider length form, so that
 * residual is a wire-format limit rather than something `encode` can express.
 *
 * Encoding is SYMMETRIC with the reader (@ref tlv_node_t::over): the grammar's one per-type
 * structural rule — a
 * `PATH_REF` body is a fixed-stride 8-byte record array, so `opt.PL` and `opt.LL` are both
 * forbidden and the length is bounded (RFC-0024 §4.2/§4.3, `wire::path_ref_body_valid`) — is
 * applied here too, so this codec cannot mint a frame it would itself reject (#886). A refused
 * TLV anywhere in the tree refuses the whole tree rather than silently dropping a component.
 *
 * The trailer timestamp is LOUD, not defaulted (#1109): a TLV whose `opt.ts` is set with no
 * `trailer->ts` value — or whose trailer value's `relative` flag contradicts `opt.tf` — is
 * refused (empty vector), never emitted with a silently-zero stamp. `stamp_ts` sets the bit
 * and the value together, so a stamped TLV cannot reach this refusal.
 *
 * @param tlv The TLV tree to serialize.
 * @return The encoded frame bytes, or an EMPTY vector when @p tlv (or any descendant) is an
 *         ill-formed `PATH_REF` or claims a timestamp it does not carry. Empty is
 *         unambiguous: a serialized TLV always carries at least its 4-byte header, so no
 *         well-formed @p tlv encodes to nothing.
 */
[[nodiscard]] std::vector<std::byte> encode(const tlv_t& tlv);

/**
 * @brief The canonical PATH-payload key of a validated PATH node — the graph vertex-map key.
 *
 * The PATH body's packed `[u8 len][bytes]` segment records, copied (RFC-0018 — `opt.PL = 0`,
 * so the body IS the key). One locus for what `graph_t`, `op_resolver_t`, and `fwd_router_t`
 * each previously rebuilt inline.
 *
 * This is **canonical / key** context, so the framing is checked and the RFC-0018 §5.4
 * `len == 0` escape is REJECTED — a label is not canonical bytes. A ragged or escape-bearing
 * body yields `std::nullopt` rather than a key. Before RFC-0018 the same `nullopt` guarded a
 * non-`NAME` child: this function re-emitted ANY child's payload through `wire::emit_name`, so
 * a peer's `PATH{VALUE "sensor"}` composed byte-identical key bytes to the legal
 * `PATH{NAME "sensor"}` and resolved `/sensor` (#436's shape, one tier up, and #681). A packed
 * body has no children to mistype, so that class of divergence is structurally gone.
 *
 * @note Returning `nullopt` rather than an empty vector is load-bearing. `graph_t::find_ptr`
 *       walks segments from the root, so an EMPTY key exits its loop immediately and resolves
 *       the ROOT vertex — an empty-key rejection would convert this bug into a misroute to `/`,
 *       which is worse than the bug.
 *
 * @param path A validated PATH node, read in place (#1829).
 * @return The canonical key bytes, or `nullopt` if the body is not a run of literal packed
 *         records (ragged framing, an escape record, or a structured `opt.PL = 1` PATH).
 */
[[nodiscard]] std::optional<std::vector<std::byte>> path_key(const tlv_node_t& path);

}  // namespace tr::wire
