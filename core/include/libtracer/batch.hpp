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
 * That is also why `read_batch` takes `dt_ns` as a parameter: whether the child after the
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
 * **One layout, two carriages.** The table above is the body; the header type byte is the whole
 * of what a carriage changes (@ref tr::wire::batch_carriage_t, RFC-0025 §4.1.2 clause 6 as
 * scoped by the 2026-08-23 erratum). A **standalone** flush is headed `0x80`; a flush **folded**
 * into a `propagate(v, FOLD)` branch write is headed `VALUE`, because RFC-0005 §B's node grammar
 * admits at most one `VALUE` and no user-range child. Both come out of @ref
 * tr::wire::store_batch_head, so the two spellings cannot drift.
 *
 * **Who composes a batch: the application, always** (RFC-0025 §4.1.3, Amendment 4). The graph
 * has no clock, holds no flush counter or window, and never learns that a value is a batch. The
 * app ropes its samples into one value (@ref tr::wire::compose_batch — one owned head segment,
 * the sample bytes REFERENCED), swaps it in through the ordinary atomic LKV publish, and calls
 * `propagate`/push. Whether the vertex is STORED (its LKV is the latest batch) or STREAM (its
 * ring holds a history of batches, one entry per batch) is the app's choice.
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
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/view.hpp"

namespace tr::wire {

/** @brief Wire bytes of a batch's `TIME` base child: a 4-byte header plus the u64 LE ns. */
inline constexpr std::size_t kBatchTimeChildBytes = 12;

/** @brief Wire bytes of one packed per-sample offset: a signed i32 LE nanosecond delta. */
inline constexpr std::size_t kBatchOffsetBytes = 4;

/**
 * @brief Which CARRIAGE a batch is seated in — the whole of what differs between the two
 *        spellings (RFC-0025 §4.1.2 clause 6, as scoped by the 2026-08-23 erratum).
 *
 * A batch has **one layout and two spellings**. The `TIME` base child, the packed `i32` offset
 * run and the sample frames are byte-identical in both; only the header type byte differs,
 * because a folded flush rides a branch-write node whose RFC-0005 §B grammar admits a leading
 * `NAME`, **at most one** `VALUE` and recursive `POINT` children — and no `0x80`.
 */
enum class batch_carriage_t : std::uint8_t {
    /** @brief A standalone flush: its own delivery, headed by the assigned BATCH type `0x80`. */
    STANDALONE,
    /** @brief Folded into a `propagate(v, FOLD)` branch write: the swept node's single
     *         structured `VALUE`, the one value RFC-0005 §B already admits. */
    FOLDED,
};

/** @brief The header type byte @p carriage introduces its batch with — the ONE place the two
 *         spellings diverge, so no layout code is written twice. */
[[nodiscard]] constexpr type_t batch_carriage_type(batch_carriage_t carriage) noexcept {
    return carriage == batch_carriage_t::FOLDED ? type_t::VALUE : type_t::BATCH;
}

/** @brief Wire bytes of a batch's BODY — the `TIME` base, the optional packed offset child, and
 *         the sample frames' own encodings. */
[[nodiscard]] constexpr std::size_t batch_body_bytes(std::size_t samples_bytes,
                                                     std::size_t offsets = 0) noexcept {
    return kBatchTimeChildBytes + (offsets != 0 ? 4u + offsets * kBatchOffsetBytes : 0u) +
           samples_bytes;
}

/**
 * @brief Wire bytes one batch record occupies — the whole size function, so a caller can size
 *        a buffer for the shape it is about to fold rather than discovering it afterwards.
 *
 * Carriage-independent: both spellings spend the same header width on the same body.
 *
 * @param samples_bytes Total bytes of the sample frames' own encodings (each is a complete
 *                      child TLV — the fold CONCATENATES them, it does not re-encode them).
 * @param offsets       How many packed offsets ride along; `0` on a uniform stream.
 */
[[nodiscard]] constexpr std::size_t batch_wire_bytes(std::size_t samples_bytes,
                                                     std::size_t offsets = 0) noexcept {
    const std::size_t body = batch_body_bytes(samples_bytes, offsets);
    return (body > 0xFFFFu ? 6u : 4u) + body;
}

/**
 * @brief Wire bytes of a batch's HEAD — everything up to but excluding the first sample frame.
 *
 * This is exactly what a composer must OWN: the record header, the `TIME` base child and (on a
 * non-uniform stream) the packed offset child. Everything after it is the app's own sample
 * bytes, which a composition references rather than copies (@ref compose_batch).
 */
[[nodiscard]] constexpr std::size_t batch_head_bytes(std::size_t samples_bytes,
                                                     std::size_t offsets = 0) noexcept {
    return batch_wire_bytes(samples_bytes, offsets) - samples_bytes;
}

/**
 * @brief Store the batch's base-time child into an exactly-sized span: `TIME{ u64 LE ns }`.
 *
 * The SAMPLE clock of frame 0, in the payload where RFC-0025 §4.2.1 puts it — never the
 * trailer, which answers "when did this frame leave an interface" instead.
 *
 * @param base_ns Nanoseconds since the Unix epoch. Negative values are not representable
 *                (docs/reference/05-protocol-tlvs.md §`0x0C`) and are emitted as their
 *                two's-complement u64, which @ref read_batch then declines to read.
 * @note Precondition: `out.size() >= kBatchTimeChildBytes`.
 */
inline void store_batch_time(std::span<std::byte> out, std::int64_t base_ns) noexcept {
    store_header(out, type_t::TIME, opt_t{}, 8u);
    detail::store_le(out.subspan(4), static_cast<std::uint64_t>(base_ns), 8u);
}

/**
 * @brief Store the NON-UNIFORM stream's packed offset array into an exactly-sized span, as ONE
 *        `VALUE` child.
 *
 * One signed i32 LE nanosecond offset from the base per sample frame, in frame order, in a
 * single contiguous run. **One child, not one trailer per sample**: the per-child spelling
 * this replaces spent a 4-byte trailer and an anchor walk per sample to say the same thing.
 *
 * Written only when the descriptor declares `dt_ns == 0`; a uniform stream derives every
 * sample time and writes nothing here.
 *
 * @note Precondition: `out.size() >= 4 + offsets_ns.size() * kBatchOffsetBytes`.
 */
inline void store_batch_offsets(std::span<std::byte> out,
                                std::span<const std::int32_t> offsets_ns) noexcept {
    store_header(out, type_t::VALUE, opt_t{}, offsets_ns.size() * kBatchOffsetBytes);
    std::size_t at = 4;
    for (const std::int32_t off : offsets_ns) {
        detail::store_le(out.subspan(at), static_cast<std::uint32_t>(off), kBatchOffsetBytes);
        at += kBatchOffsetBytes;
    }
}

/**
 * @brief Store a batch's whole HEAD — **the one layout implementation**, shared by every
 *        emitter and composer in this header and by both carriages.
 *
 * Record header (typed by @p carriage), then the `TIME{u64}` base, then — on a non-uniform
 * stream — the packed `i32` offset child. Nothing else: the sample frames follow, as the
 * caller's own bytes.
 *
 * @param samples_bytes Total bytes of the sample frames that will follow. It is a *parameter*
 *                      rather than something this function can see, because the header's length
 *                      field (and its u16-vs-u32 width) covers bytes this function never writes.
 * @note Precondition: `out.size() >= batch_head_bytes(samples_bytes, offsets_ns.size())`.
 */
inline void store_batch_head(std::span<std::byte> out, batch_carriage_t carriage,
                             std::int64_t base_ns, std::size_t samples_bytes,
                             std::span<const std::int32_t> offsets_ns = {}) noexcept {
    const std::size_t body = batch_body_bytes(samples_bytes, offsets_ns.size());
    const opt_t opt{.pl = true, .ll = body > 0xFFFFu};
    store_header(out, batch_carriage_type(carriage), opt, body);
    std::size_t at = header_bytes(opt);
    store_batch_time(out.subspan(at), base_ns);
    at += kBatchTimeChildBytes;
    if (!offsets_ns.empty()) store_batch_offsets(out.subspan(at), offsets_ns);
}

/** @brief Append the batch's base-time child — the container form of @ref store_batch_time. */
inline void emit_batch_time(std::vector<std::byte>& out, std::int64_t base_ns) {
    const std::size_t at = out.size();
    out.resize(at + kBatchTimeChildBytes);
    store_batch_time(std::span<std::byte>(out).subspan(at), base_ns);
}

/** @brief Append the packed offset array — the container form of @ref store_batch_offsets. */
inline void emit_batch_offsets(std::vector<std::byte>& out,
                               std::span<const std::int32_t> offsets_ns) {
    const std::size_t at = out.size();
    out.resize(at + 4u + offsets_ns.size() * kBatchOffsetBytes);
    store_batch_offsets(std::span<std::byte>(out).subspan(at), offsets_ns);
}

/**
 * @brief FOLD @p samples into one batch record appended to @p out — N sample frames become
 *        ONE written value, into a contiguous buffer.
 *
 * This is the wire encoding of a flush (RFC-0025 §4.1.2): the snapshot when the source vertex
 * is a plain value (one sample frame), the full since-flush list when it is a STREAM (RFC-0008
 * §E). Folding preserves every entry and its order, so batching a STREAM is **not**
 * conflation — it is one frame instead of N.
 *
 * The sample frames are appended as the BYTES the caller already holds: a fold is a
 * concatenation under one header, never a re-encode. Nothing about a sample's own bytes
 * changes by being folded. It does, however, **copy** them into @p out — a caller whose
 * samples already live in segments it owns wants @ref compose_batch instead, which references
 * them.
 *
 * @param out        Destination, appended to.
 * @param base_ns    The batch base — the sample time of frame 0.
 * @param samples    Each element is one complete, already-encoded child TLV.
 * @param offsets_ns The non-uniform stream's per-sample offsets from @p base_ns, or empty
 *                   (the default) for a uniform stream, whose times are derived at 0 B/sample.
 * @param carriage   Which spelling to head the record with (default: the standalone `0x80`).
 */
inline void emit_batch(std::vector<std::byte>& out, std::int64_t base_ns,
                       std::span<const std::span<const std::byte>> samples,
                       std::span<const std::int32_t> offsets_ns = {},
                       batch_carriage_t carriage = batch_carriage_t::STANDALONE) {
    std::size_t samples_bytes = 0;
    for (const std::span<const std::byte>& s : samples) samples_bytes += s.size();
    const std::size_t head = batch_head_bytes(samples_bytes, offsets_ns.size());
    const std::size_t at = out.size();
    out.resize(at + head);
    store_batch_head(std::span<std::byte>(out).subspan(at, head), carriage, base_ns, samples_bytes,
                     offsets_ns);
    for (const std::span<const std::byte>& s : samples) out.insert(out.end(), s.begin(), s.end());
}

/**
 * @brief COMPOSE a batch as a rope — the app's own sample segments **referenced**, never copied
 *        (RFC-0025 §4.1.3, Amendment 4, clause 4).
 *
 * This is the reference spelling of user-orchestrated batching. The application holds its
 * sample values as views over segments it already owns; this allocates exactly ONE small owned
 * segment for the batch's head (record header + `TIME` base + any offset child) and ropes the
 * samples on behind it as refcounted links. The bytes the samples occupy are never read, moved
 * or duplicated — the composed rope scatter-gathers straight out of the acquisition buffers.
 *
 * The precedent is the composed branch read (RFC-0016) and the reply builder beside it: one
 * small owned header segment per node, the children's existing bytes roped on behind it. It is
 * the same trick the FWD plane's `src` accumulation uses on the way in — existing elements are
 * referenced, never rewritten.
 *
 * The result is a value like any other. The app writes it to its vertex through the ordinary
 * atomic LKV publish and calls `propagate`/push: **compose → swap → push**. There is no
 * batch-specific write path, no new op, no new role, and the graph never learns that the value
 * it moved was a batch (claim 5, trivially).
 *
 * @param backend    Where the ONE head segment comes from. This is the only allocation.
 * @param carriage   `STANDALONE` for the `0x80` record of its own delivery, `FOLDED` for the
 *                   `VALUE` seat of a branch-write node. **Same body bytes either way.**
 * @param base_ns    The batch base — the sample time of frame 0. The APP's number: the graph
 *                   has no clock and never supplies one.
 * @param samples    The sample frames, in order; each a complete, already-encoded child TLV.
 * @param offsets_ns The non-uniform stream's per-sample offsets, or empty for a uniform stream.
 *
 * @return The composed rope: one owned head link followed by one link per sample.
 * @retval {} An EMPTY rope — the backend refused the head segment, or the chain could not
 *            reserve its links. Transient backpressure, distinguishable from a real batch
 *            because a composed batch always has at least the head link.
 */
[[nodiscard]] inline view::rope_t compose_batch(mem::mem_backend_t& backend,
                                                batch_carriage_t carriage, std::int64_t base_ns,
                                                std::span<const view::view_t> samples,
                                                std::span<const std::int32_t> offsets_ns = {}) {
    std::size_t samples_bytes = 0;
    for (const view::view_t& s : samples) samples_bytes += s.bytes().size();
    const std::size_t head = batch_head_bytes(samples_bytes, offsets_ns.size());

    view::segment_ptr_t seg = view::segment_alloc(backend, head);
    if (!seg) return {};
    store_batch_head(seg->bytes.first(head), carriage, base_ns, samples_bytes, offsets_ns);

    view::rope_t out;
    // Reserved up front so no append can spill mid-chain: the chain's growth throws, and under
    // `-fno-exceptions` that is an abort() (rope.hpp @ref try_reserve, #981).
    if (!out.try_reserve(1u + samples.size())) return {};
    out.append(view::view_t::over(std::move(seg)));
    for (const view::view_t& s : samples) out.append(s);  // refcount clone — no byte copy
    return out;
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
        // Two arms, for the same reason the non-uniform arm above is guarded: `read_batch`
        // pins `base_ns >= 0`, but `batch_view_t` is an aggregate a caller may fill from a
        // non-wire source, and `int64max - base_ns` is signed overflow for a negative base
        // (#1600 — the defect #1580 fixed in the playout re-prime).
        const std::uint64_t headroom =
            base_ns >= 0
                ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - base_ns)
                : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
                      (0u - static_cast<std::uint64_t>(base_ns));
        if (static_cast<std::uint64_t>(i) > headroom / dt_ns) return std::nullopt;
        // `base_ns + i * dt_ns` is in range by the guard, so the unsigned sum carries the right
        // two's-complement bits — and, unlike the signed spelling, it stays defined for a step
        // that alone exceeds `int64max` (reachable only when `base_ns < 0`).
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(base_ns) +
                                         static_cast<std::uint64_t>(i) * dt_ns);
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
