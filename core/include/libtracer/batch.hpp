/**
 * @file
 * @brief The BATCH record (`0x80`) — folding N sample frames into ONE written value, with
 *        per-sample time DERIVED rather than transmitted (RFC-0025 §4.2.1 / §4.1.2).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A batch is an ordinary structured (`opt.PL=1`) TLV — **zero new grammar** (RFC-0025 §3).
 * What this header adds is the one canonical spelling of the convention, so the reference
 * helpers, the descriptor and the conformance vectors name the same bytes:
 *
 * ```
 * BATCH  (opt.PL=1, type 0x80)
 *   |- TIME  <u64 LE ns>        ; the batch BASE — sample time of frame 0. ONE per batch.
 *   |- [ VALUE <i32[] LE ns> ]  ; NON-UNIFORM streams only: packed per-sample offsets
 *   |                           ;   from the base, one i32 per frame, in frame order.
 *   \- <sample frames...>       ; homogeneous children (the ADR-0008 array shape)
 * ```
 *
 * **Time is DERIVED, not carried, on a uniform stream.** `t(i) = base + i * dt_ns`, where
 * `dt_ns` is the nominal sample period the stream's DESCRIPTOR declares (RFC-0025 §4.3 — a
 * `SETTINGS` LKV beside the data vertex, negotiated once, never repeated per batch). A
 * uniform stream is exactly one whose descriptor declares `dt_ns != 0`, and it spends
 * **0 bytes per sample** on time. A non-uniform stream (`dt_ns == 0`) carries one packed
 * `i32` run — 4 bytes per sample, contiguous, decodable in one span, with no per-child TLV
 * header and no anchor walk.
 *
 * That is also why @ref read_batch takes `dt_ns` as a parameter: whether the child after the
 * `TIME` is an offset array or the first sample frame is a fact about the **descriptor**, not
 * about these bytes. The graph never interprets a BATCH (claim 5); the descriptor tells the
 * consumer how to read one.
 *
 * **The trailer is not a sample clock.** A batch carries no trailer of its own: wire/TX time
 * is `opt.TS` on the **outermost** frame only and is always `TF=0` (RFC-0025 §4.2.1
 * consequence 1). Sample time is the payload `TIME` child above; playout time is never
 * transmitted. The retired spelling — a `TF=0` parent plus a `TF=1` trailer on every child —
 * cost 4 trailer bytes per sample where this costs 0, and would have made a folded stream
 * unencodable as a branch write (RFC-0025 §4.1.2 clause 5).
 *
 * Lives in `tr::wire` (L2/L3): it produces and reads wire bytes from wire types.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::wire {

/** @brief Wire bytes of a batch's `TIME` base child: a 4-byte header plus the u64 LE ns. */
inline constexpr std::size_t kBatchTimeChildBytes = 12;

/** @brief Wire bytes of one packed per-sample offset: a signed i32 LE nanosecond delta. */
inline constexpr std::size_t kBatchOffsetBytes = 4;

/**
 * @brief Append the batch's base-time child: `TIME{ u64 LE ns }`.
 *
 * The SAMPLE clock of frame 0, in the payload where RFC-0025 §4.2.1 puts it — never the
 * trailer, which answers "when did this frame leave an interface" instead.
 *
 * @param base_ns Nanoseconds since the Unix epoch. Negative values are not representable
 *                (docs/reference/05-protocol-tlvs.md §`0x0C`) and are emitted as their
 *                two's-complement u64, which @ref read_batch then declines to read.
 */
inline void emit_batch_time(std::vector<std::byte>& out, std::int64_t base_ns) {
    emit_header(out, type_t::TIME, opt_t{}, 8u);
    detail::append_le(out, static_cast<std::uint64_t>(base_ns), 8u);
}

/**
 * @brief Append the NON-UNIFORM stream's packed offset array as ONE `VALUE` child.
 *
 * One signed i32 LE nanosecond offset from the base per sample frame, in frame order, in a
 * single contiguous run. **One child, not one trailer per sample**: the per-child spelling
 * this replaces spent a 4-byte trailer and an anchor walk per sample to say the same thing.
 *
 * Emitted only when the descriptor declares `dt_ns == 0`; a uniform stream derives every
 * sample time and emits nothing here.
 */
inline void emit_batch_offsets(std::vector<std::byte>& out,
                               std::span<const std::int32_t> offsets_ns) {
    emit_header(out, type_t::VALUE, opt_t{}, offsets_ns.size() * kBatchOffsetBytes);
    for (const std::int32_t off : offsets_ns)
        detail::append_le(out, static_cast<std::uint32_t>(off), kBatchOffsetBytes);
}

/**
 * @brief Wire bytes one BATCH record occupies — the whole size function, so a caller can size
 *        a buffer for the shape it is about to fold rather than discovering it afterwards.
 *
 * @param samples_bytes Total bytes of the sample frames' own encodings (each is a complete
 *                      child TLV — the fold CONCATENATES them, it does not re-encode them).
 * @param offsets       How many packed offsets ride along; `0` on a uniform stream.
 */
[[nodiscard]] constexpr std::size_t batch_wire_bytes(std::size_t samples_bytes,
                                                     std::size_t offsets = 0) noexcept {
    const std::size_t body = kBatchTimeChildBytes +
                             (offsets != 0 ? 4u + offsets * kBatchOffsetBytes : 0u) + samples_bytes;
    return (body > 0xFFFFu ? 6u : 4u) + body;
}

/**
 * @brief FOLD @p samples into one BATCH record appended to @p out — N sample frames become
 *        ONE written value.
 *
 * This is the wire encoding of a flush (RFC-0025 §4.1.2): the snapshot when the source vertex
 * is a plain value (one sample frame), the full since-flush list when it is a STREAM (RFC-0008
 * §E). Folding preserves every entry and its order, so batching a STREAM is **not**
 * conflation — it is one frame instead of N.
 *
 * The sample frames are appended as the BYTES the caller already holds: a fold is a
 * concatenation under one header, never a re-encode. Nothing about a sample's own bytes
 * changes by being folded.
 *
 * @param out        Destination, appended to.
 * @param base_ns    The batch base — the sample time of frame 0.
 * @param samples    Each element is one complete, already-encoded child TLV.
 * @param offsets_ns The non-uniform stream's per-sample offsets from @p base_ns, or empty
 *                   (the default) for a uniform stream, whose times are derived at 0 B/sample.
 */
inline void emit_batch(std::vector<std::byte>& out, std::int64_t base_ns,
                       std::span<const std::span<const std::byte>> samples,
                       std::span<const std::int32_t> offsets_ns = {}) {
    std::size_t samples_bytes = 0;
    for (const std::span<const std::byte>& s : samples) samples_bytes += s.size();
    const std::size_t body =
        kBatchTimeChildBytes +
        (offsets_ns.empty() ? 0u : 4u + offsets_ns.size() * kBatchOffsetBytes) + samples_bytes;
    emit_header(out, type_t::BATCH, opt_t{.pl = true, .ll = body > 0xFFFFu}, body);
    emit_batch_time(out, base_ns);
    if (!offsets_ns.empty()) emit_batch_offsets(out, offsets_ns);
    for (const std::span<const std::byte>& s : samples) out.insert(out.end(), s.begin(), s.end());
}

/**
 * @brief A decoded BATCH record: its base, its sample frames, and the arithmetic that turns
 *        an index into a sample time.
 *
 * Borrows @ref read_batch's argument — the `tlv_t` (and the buffer it decoded from) must
 * outlive the view.
 */
struct batch_view_t {
    /** @brief The batch base: the SAMPLE time of frame 0, ns since the Unix epoch. */
    std::int64_t base_ns = 0;
    /** @brief The descriptor's nominal sample period; `0` = non-uniform (RFC-0025 §4.3). */
    std::uint64_t dt_ns = 0;
    /** @brief The packed `i32` LE offset run; EMPTY on a uniform stream. */
    std::span<const std::byte> offsets{};
    /** @brief The sample frames, in order — the children after the base (and the offsets). */
    std::span<const tlv_t> samples{};

    /** @brief How many sample frames this batch folded. */
    [[nodiscard]] constexpr std::size_t size() const noexcept { return samples.size(); }

    /** @brief True when per-sample time is DERIVED from the descriptor's rate (0 B/sample). */
    [[nodiscard]] constexpr bool uniform() const noexcept { return dt_ns != 0; }

    /** @brief The packed offset of frame @p i — `0` when the stream is uniform or @p i is
     *         out of range. */
    [[nodiscard]] constexpr std::int32_t offset_ns(std::size_t i) const noexcept {
        if ((i + 1) * kBatchOffsetBytes > offsets.size()) return 0;
        return static_cast<std::int32_t>(detail::load_le<std::uint32_t>(
            offsets.subspan(i * kBatchOffsetBytes, kBatchOffsetBytes)));
    }

    /**
     * @brief The SAMPLE time of frame @p i — derived, never read off the wire on a uniform
     *        stream.
     *
     * `t(i) = base + i * dt_ns` when the descriptor declares a rate; `t(i) = base +
     * offsets[i]` when it declares `dt_ns == 0`. Answers nothing for an out-of-range index or
     * for a derivation that would overflow the ns epoch — an unrepresentable time is
     * reported, never wrapped into a plausible-looking one.
     */
    [[nodiscard]] constexpr std::optional<std::int64_t> sample_time_ns(
        std::size_t i) const noexcept {
        if (i >= samples.size()) return std::nullopt;
        if (!uniform()) {
            const std::int64_t off = offset_ns(i);
            if (off > 0 && base_ns > std::numeric_limits<std::int64_t>::max() - off)
                return std::nullopt;
            if (off < 0 && base_ns < std::numeric_limits<std::int64_t>::min() - off)
                return std::nullopt;
            return base_ns + off;
        }
        const std::uint64_t headroom =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - base_ns);
        if (static_cast<std::uint64_t>(i) > headroom / dt_ns) return std::nullopt;
        return base_ns + static_cast<std::int64_t>(static_cast<std::uint64_t>(i) * dt_ns);
    }
};

/** @brief True iff @p tlv is a structured record at the assigned BATCH type code. */
[[nodiscard]] inline bool is_batch(const tlv_t& tlv) noexcept {
    return tlv.type == type_t::BATCH && tlv.opt.pl;
}

/**
 * @brief Read a BATCH record's convention out of a decoded @p tlv, against the descriptor's
 *        @p dt_ns.
 *
 * @param dt_ns The nominal sample period from the stream's §4.3 descriptor. Non-zero ⇒ a
 *              UNIFORM batch: every child after the `TIME` is a sample frame. Zero ⇒ a
 *              NON-UNIFORM batch: the child after the `TIME` is the packed `i32` offset
 *              array, one entry per remaining child.
 *
 * @return Nothing when @p tlv does not spell the convention — a foreign type code, an opaque
 *         body, a missing or mis-sized `TIME` base, a base past the representable epoch, or a
 *         non-uniform offset array whose entry count does not match the sample count. That is
 *         a READER's answer, not the codec's: the frame still decodes, still forwards and
 *         still round-trips byte-for-byte, because the user range is a range the protocol does
 *         not opine on (docs/reference/05-protocol-tlvs.md §User range).
 */
[[nodiscard]] inline std::optional<batch_view_t> read_batch(const tlv_t& tlv,
                                                            std::uint64_t dt_ns) noexcept {
    if (!is_batch(tlv) || tlv.children.empty()) return std::nullopt;
    const tlv_t& base = tlv.children.front();
    if (base.type != type_t::TIME || base.payload.size() != 8u) return std::nullopt;
    const std::uint64_t raw = detail::load_le<std::uint64_t>(base.payload);
    if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;

    batch_view_t v{};
    v.base_ns = static_cast<std::int64_t>(raw);
    v.dt_ns = dt_ns;
    const std::span<const tlv_t> rest{tlv.children.data() + 1, tlv.children.size() - 1};
    if (dt_ns != 0) {
        v.samples = rest;
        return v;
    }
    if (rest.empty()) return std::nullopt;
    if (rest.front().type != type_t::VALUE) return std::nullopt;
    v.offsets = rest.front().payload;
    v.samples = rest.subspan(1);
    if (v.offsets.size() != v.samples.size() * kBatchOffsetBytes) return std::nullopt;
    return v;
}

}  // namespace tr::wire
