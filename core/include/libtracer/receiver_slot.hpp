/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * receiver_slot — the ONE home of the transport delivery-tier mechanism. Every
 * transport adapter used to re-implement the same three behaviors around its
 * receiver pair: the mutex-guarded storage, a dirty-flag snapshot dance (dodging
 * a per-frame std::function heap copy), and the owning-rope-vs-borrowed-span
 * tier select. The slot owns all three behind four calls; the callbacks are
 * plain {function pointer, context} pairs (ADR-0047 hot-path shape), so the
 * per-frame snapshot is a trivial copy under an uncontended lock and the dirty
 * flag ceases to exist. The `Tag...` pack prepends transport-defined delivery
 * tags (a bus link's sending-peer name) to both sinks.
 */
#pragma once

#include <cstddef>
#include <mutex>
#include <span>
#include <utility>

#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"

namespace tr::net {

/**
 * @brief The delivery-tier receiver slot every transport adapter shares.
 *
 * Holds the two inbound sinks of the ADR-0042/ADR-0053 receiver seam — the
 * borrowed-span sink and the owning-rope sink — as trivially-copyable
 * `{fn, ctx}` pairs, and performs the tier select on delivery: an owning frame
 * prefers the rope sink and falls back to handing the same bytes borrowed; a
 * borrowed frame can only ever go to the span sink (no adapter wraps a span
 * into a rope whose refcounts would lie about lifetime, ADR-0042 §1).
 *
 * Thread contract: setters may race the transport's receive thread; every
 * deliver snapshots the pairs under the lock and dispatches OUTSIDE it (a sink
 * may re-enter the transport). The context pointer's lifetime is the caller's
 * responsibility and must cover every possible delivery.
 *
 * @tparam Tag Extra leading sink parameters a transport tags deliveries with
 *             (e.g. `std::string_view` — a bus link's sending-peer name).
 */
template <typename... Tag>
class receiver_slot_t {
   public:
    /** @brief The borrowed-span sink: the frame is valid only for the call. */
    using span_fn_t = void (*)(void* ctx, Tag..., std::span<const std::byte>);
    /** @brief The owning sink: refcounted rope links the sink may keep or forward. */
    using rope_fn_t = void (*)(void* ctx, Tag..., view::rope_t);

    /**
     * @brief Install (or clear, with nullptr) the borrowed-span sink.
     * @param fn  The sink; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible delivery.
     */
    void set(span_fn_t fn, void* ctx) noexcept {
        const std::lock_guard lock(m_);
        span_fn_ = fn;
        span_ctx_ = ctx;
    }

    /**
     * @brief Install (or clear, with nullptr) the owning-rope sink.
     * @param fn  The sink; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible delivery.
     */
    void set_rope(rope_fn_t fn, void* ctx) noexcept {
        const std::lock_guard lock(m_);
        rope_fn_ = fn;
        rope_ctx_ = ctx;
    }

    /**
     * @brief True iff an owning-rope sink is currently installed.
     *
     * The receive-loop strategy query: a transport that must choose its buffer
     * strategy BEFORE the blocking read (recv into a refcounted segment vs a
     * borrowed scratch) keys it off this, per iteration.
     */
    [[nodiscard]] bool has_rope() const noexcept {
        const std::lock_guard lock(m_);
        return rope_fn_ != nullptr;
    }

    /**
     * @brief True iff ANY sink (span or rope) is currently installed.
     *
     * The precedence query for transports with two slots (a bus link's
     * peer-named slot vs the flat @c transport_t slot): deliver to the
     * higher-precedence slot iff it has a sink, else fall back.
     */
    [[nodiscard]] bool has_any() const noexcept {
        const std::lock_guard lock(m_);
        return rope_fn_ != nullptr || span_fn_ != nullptr;
    }

    /**
     * @brief Deliver one OWNING frame — the tier select.
     *
     * Prefers the rope sink (the frame crosses as a single-link rope the sink
     * may pin, subrope, or forward); with only a span sink installed, hands the
     * same bytes borrowed (the view is released when the call returns). No sink
     * installed drops the frame.
     *
     * @param tag   The transport's delivery tags (the `Tag...` pack).
     * @param frame The frame, narrowed to its exact length, owning its segment.
     */
    void deliver(Tag... tag, view::view_t frame) const {
        const snapshot_t s = snapshot();
        if (s.rope_fn != nullptr) {
            s.rope_fn(s.rope_ctx, tag..., view::rope_t{std::move(frame)});
        } else if (s.span_fn != nullptr) {
            s.span_fn(s.span_ctx, tag..., frame.bytes());
        }
    }

    /**
     * @brief Deliver one OWNING frame that is already a rope (a reassembling
     *        bus's group — chained slice views, never a flatten).
     *
     * The rope sink takes it as-is (zero-copy). A span-only sink needs
     * contiguous bytes: a single-link rope hands its bytes borrowed (zero-copy);
     * a multi-link rope pays ONE materialize into @p backend — the span tier's
     * honesty cost, never the rope tier's.
     *
     * @param tag     The transport's delivery tags (the `Tag...` pack).
     * @param frame   The reassembled frame as the rope it already is.
     * @param backend Where a span-only fallback materializes a multi-link rope.
     */
    void deliver_rope(Tag... tag, view::rope_t frame,
                      mem::mem_backend_t& backend = mem::heap_backend()) const {
        const snapshot_t s = snapshot();
        if (s.rope_fn != nullptr) {
            s.rope_fn(s.rope_ctx, tag..., std::move(frame));
        } else if (s.span_fn != nullptr) {
            const view::view_t flat = frame.materialize(backend);
            s.span_fn(s.span_ctx, tag..., flat.bytes());
        }
    }

    /**
     * @brief Deliver one BORROWED frame — span sink only, by construction.
     *
     * A borrowed span cannot become an owning rope (ADR-0042 §1), so an
     * installed rope sink is honestly ignored here; transports that can hand up
     * owning frames use @ref deliver instead.
     *
     * @param tag   The transport's delivery tags (the `Tag...` pack).
     * @param frame The frame bytes, valid only for the duration of the call.
     */
    void deliver_borrowed(Tag... tag, std::span<const std::byte> frame) const {
        const snapshot_t s = snapshot();
        if (s.span_fn != nullptr) s.span_fn(s.span_ctx, tag..., frame);
    }

   private:
    struct snapshot_t {
        span_fn_t span_fn;
        void* span_ctx;
        rope_fn_t rope_fn;
        void* rope_ctx;
    };

    [[nodiscard]] snapshot_t snapshot() const noexcept {
        const std::lock_guard lock(m_);
        return {span_fn_, span_ctx_, rope_fn_, rope_ctx_};
    }

    mutable std::mutex m_;
    span_fn_t span_fn_ = nullptr;
    void* span_ctx_ = nullptr;
    rope_fn_t rope_fn_ = nullptr;
    void* rope_ctx_ = nullptr;
};

}  // namespace tr::net
