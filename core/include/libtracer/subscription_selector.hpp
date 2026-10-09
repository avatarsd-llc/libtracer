/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "libtracer/app_fields.hpp"
#include "libtracer/config.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/guard.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/path.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/value.hpp"

/**
 * @file
 * @brief The subscription selector (#2024): a vertex that switches its owner's existing
 *        subscriptions among named options by suspending and resuming them in place.
 *
 * An adapter beside the core, not a core module: it drives only the public
 * `%graph_t::set_suspended` (#1533) and the RFC-0010 app-field seams, so a build that never
 * includes this header carries none of it. One instance is one vertex with two app fields:
 *
 * | field | access | value |
 * | --- | --- | --- |
 * | `:settings.app.active` | `rw` | `NAME <option>` selects it; an empty `STATUS` selects none |
 * | `:settings.app.options` | `ro` | the options and their refs, each ref with its state |
 *
 * Selecting is an ordinary field write. The value formats are RFC-0034's.
 *
 * **What it never does.** It never creates, unsubscribes or replaces a subscription: the
 * owner creates each one (suspended, for an option that is not active) and hands its handle
 * over with @ref tr::graph::subscription_selector_t::add. It keeps nothing past the process:
 * the owner stores the configuration and the active option and replays both at boot. It holds
 * no buffer of the library's and copies no name: its tables are fixed arrays inside the
 * object, sized at compile time by the embedder that declares it, the option names are the
 * owner's, borrowed, and a read of `options` stages on the graph's own sources. It reads no
 * clock and arms no timer.
 */

namespace tr::graph {

/**
 * @brief What one ref of a `subscription_selector_t` is doing now, as `options` lists it.
 */
enum class selector_ref_state_t : std::uint8_t {
    SUSPENDED = 0, /**< @brief The subscription exists and delivers nothing. */
    LIVE = 1,      /**< @brief The subscription exists and delivers. */
    /** @brief The subscription is gone (unsubscribed, evicted, its producer retired). The ref
     *         stays listed and a switch skips it, until the owner removes it or lists the same
     *         address again. */
    INERT = 2,
};

/** @brief The `options` spelling of a @ref selector_ref_state_t (RFC-0034). */
[[nodiscard]] constexpr std::string_view to_string(selector_ref_state_t s) noexcept {
    switch (s) {
        case selector_ref_state_t::LIVE:
            return "live";
        case selector_ref_state_t::INERT:
            return "inert";
        case selector_ref_state_t::SUSPENDED:
            break;
    }
    return "suspended";
}

/**
 * @brief A vertex that switches among named options, each a set of subscription refs, by
 *        suspending and resuming the subscriptions in place (#2024).
 *
 * @tparam kRefs    How many DISTINCT refs one instance holds, across all its options, at most
 *                  64. A ref listed in several options counts once.
 * @tparam kOptions How many options one instance holds, at most 254.
 *
 * **What an instance costs** (RAM, inside the object; nothing else is allocated):
 * - per ref: a producer handle (one pointer) and a 16-bit slot, packed with no padding, so
 *   10 B on LP64 and 6 B on ILP32, plus two bits of state;
 * - per option: the borrowed name (one `std::string_view`, 16 B / 8 B) and one membership bit
 *   per ref, in the smallest unsigned word that holds @p kRefs bits (1, 2, 4 or 8 B);
 * - fixed: the graph pointer, the vertex handle and 6 bytes of counters and flags.
 *
 * The default `<8, 4>` is 176 B on LP64 and 100 B on ILP32 (rv32); `<64, 16>` is 1,064 B and
 * 680 B. The static_asserts after the class pin all four. A ref is not a @ref subscription_t, which
 * is a pointer and a full-width slot (16 B on LP64, padded): the selector keeps the producer's
 * handle and the slot as 16 bits and rebuilds the handle with
 * `%graph_t::subscription_at` for each toggle, which costs nothing. 16 bits are exact on
 * ILP32, where a vertex holds at most 65,536 slots; on LP64 @ref add refuses a slot past
 * 65,535.
 *
 * **A switch from A to B** suspends every ref that is live and not in B, then resumes every
 * ref of B that is suspended. A ref in both is not touched. Suspending first is what rules out
 * a double delivery when A and B reach one target through two different subscriptions; the
 * price is a gap, from the first suspend to the last resume, in which that target receives
 * nothing. Each toggle is `%graph_t::set_suspended`: no unsubscribe, no re-subscribe, no frame,
 * and nothing drawn from any source, so a switch also reaches a subscriber behind a link at once.
 *
 * **The selector owns the suspend bit of every ref it lists.** It acts on what it last set and
 * does not watch the graph: it re-reads a ref's state only for `options`, for @ref state, and
 * when the owner lists the ref again (@ref add). A listed subscription that something else
 * resumes (the owner's own `%graph_t::set_suspended`, or a subscribe that reuses a removed
 * edge's slot) delivers until one of those re-reads it or a switch's suspend reaches it. A
 * peer's (re)subscribe is admitted delivering, like every subscribe, and delivers until the
 * owner lists it and switches: admitting it suspended needs a remote spelling (#2019). "No
 * double delivery" is a promise over the refs listed here, while the selector owns their bit.
 *
 * **A refused switch.** A resume can answer `BACKPRESSURE` (a resume after an edge republish on
 * its producer was refused for want of memory). The switch then reports `BACKPRESSURE` and is
 * partly applied (RFC-0034 §2.1): B stays the active option and @ref settled turns false; every
 * ref outside B is suspended, the refs of B that resumed deliver, and the refused ones stay
 * suspended. Selecting B again (or writing `active` again) retries only the refs still
 * suspended; a retry succeeds once the producer's source has room. It is the one refused
 * `active` write that leaves state behind, which the admit seam's doc
 * (`handlers_t::on_app_field_admit`) allows.
 *
 * **A dangling ref** (its subscription is gone) answers `NOT_FOUND` to a toggle. The selector
 * then marks it INERT, skips it on every later switch, and keeps listing it, until the owner
 * removes it (@ref remove) or lists the same address again (@ref add), which re-reads it.
 *
 * **One operation at a time, and nobody waits.** The `active` write seam runs on the writer's
 * thread with no graph lock held, so the owner's calls and a peer's switch can meet. Each
 * operation takes a one-byte busy flag for its length and RELEASES it at its end; one that
 * finds the flag taken answers `BACKPRESSURE` (a read of `options`: an empty value) and
 * changes nothing, and a retry completes it. Nothing ever waits on the flag, so it needs no
 * OS lock, and it cannot invert priorities. Taking it is one atomic exchange where the core
 * has one, else a load and a store inside the build's guard (`tr::graph::guard_t`), as
 * `tr::rmw_counter_t` does. A switch holds it across `%graph_t::set_suspended`, which takes the
 * graph's own locks; a guard section could not be held there. @ref active and @ref settled read
 * atomics and never refuse.
 *
 * **What an `options` read can cost.** A ref can be listed in every option, so a read renders
 * at most `kRefs × kOptions` ref records, each with a producer key of up to 1 KiB
 * (`kMaxPathBytes`): 32 records at the default, and 1,024 records, about 1.1 MiB, at
 * `<64, 16>`. The levels are staged and then wrapped, and the result copied into the value
 * source, so the read holds about twice its encoded size at its peak, on the graph's table
 * source (RFC-0034 §2.2). A refused draw answers the read with an empty value.
 *
 * @warning A @ref subscription_t carries no generation yet (#1932). If a ref's subscription is
 *          removed and a later subscribe on the same producer reuses its slot BEFORE the selector
 *          has seen the slot empty, the ref names the new edge. The selector never toggles a ref
 *          again once it is INERT, which narrows that window to the first toggle after removal.
 * @note @ref attach, @ref vertex and the destructor are the owner's, from one thread. Destroy
 *       the instance only when no field operation on its vertex can be in flight: it is the
 *       context of its vertex's field seams. The destructor retires the vertex and leaves every
 *       subscription in the state it holds.
 */
template <std::size_t kRefs = 8, std::size_t kOptions = 4>
class subscription_selector_t {
    static_assert(kRefs > 0 && kRefs <= 64, "a selector holds 1 to 64 refs: one bit each");
    static_assert(kOptions > 0 && kOptions < 255, "a selector holds 1 to 254 options");

   public:
    /** @brief A selector over @p g, attached to no vertex yet. @p g must outlive it. */
    explicit subscription_selector_t(graph_t& g) noexcept : g_(&g) {}

    /** @brief Retire the selector's vertex, if attached; every subscription keeps its state. */
    ~subscription_selector_t() {
        if (attached_) (void)g_->retire(std::bit_cast<vertex_handle_t>(vertex_));
    }

    subscription_selector_t(const subscription_selector_t&) = delete;
    subscription_selector_t& operator=(const subscription_selector_t&) = delete;

    /**
     * @brief Register the selector's own vertex at the canonical PATH key @p key, with its two
     *        fields and their seams.
     *
     * The spelling a creation hook (`%graph_t::set_creation_hook`, RFC-0030 §7.2) calls with
     * the child key it is handed, which is how an instance is created through the creator door.
     * @retval PATH_IN_USE  @p key is already registered.
     * @retval INVALID_PATH This selector is already attached.
     */
    [[nodiscard]] result_t<vertex_handle_t> attach(std::span<const std::byte> key) {
        if (attached_) return std::unexpected(status_t::INVALID_PATH);
        handlers_t h;
        h.on_app_field_admit = {&admit_field, this};
        h.on_app_field_read = {&read_field, this};
        auto made = g_->register_vertex_key(key, role_t::STORED_VALUE, h,
                                            vertex_policy_t{.app_fields = kFields});
        if (made) vertex_ = std::bit_cast<handle_bytes_t>(*made);
        attached_ = made.has_value();
        return made;
    }

    /** @brief @ref attach at @p path. */
    [[nodiscard]] result_t<vertex_handle_t> attach(const path_t& path) {
        return attach(path.key());
    }

    /**
     * @brief List @p ref in @p option, creating the option on its first ref; for a ref already
     *        listed, re-read its state from the graph.
     *
     * Records only: it toggles nothing. The ref keeps the state it has until the next
     * @ref select, which brings it in line with the active option, so the owner creates a
     * subscription suspended unless its option is the active one. Options list in the order
     * they were created, and refs in the order they were added. Adding a ref that is already
     * listed (in this option or another) is how the owner hands back a ref whose address a
     * new edge took, an INERT one included: the selector then takes the graph's word for it.
     *
     * @warning @p option is BORROWED, not copied: the bytes must outlive the selector (a
     *          string literal, or the owner's own static configuration). A name already
     *          created keeps its first view.
     * @retval INVALID_PATH @p option is not one NAME segment (1 to 64 bytes, no reserved
     *                      character).
     * @retval NOT_FOUND    @p ref names no live subscription.
     * @retval BACKPRESSURE A new ref past @p kRefs, a new option past @p kOptions, a slot past
     *                      65,535, or another operation on this selector is running.
     */
    [[nodiscard]] result_t<void> add(std::string_view option, subscription_t ref) {
        if (!valid_segment(option)) return std::unexpected(status_t::INVALID_PATH);
        const result_t<bool> suspended = g_->is_suspended(ref);
        if (!suspended) return std::unexpected(suspended.error());
        const std::optional<ref_t> key = pack(ref);
        const hold_t hold(*this);
        if (!key || !hold) return std::unexpected(status_t::BACKPRESSURE);
        const std::size_t o = find_option(option);
        const std::size_t r = find_ref(*key);
        if ((o == n_options_ && n_options_ == kOptions) || (r == n_refs_ && n_refs_ == kRefs))
            return std::unexpected(status_t::BACKPRESSURE);
        if (o == n_options_) names_[n_options_++] = option;
        if (r == n_refs_) refs_[n_refs_++] = *key;
        inert_ = with(inert_, r, false);
        live_ = with(live_, r, !*suspended);
        members_[o] = with(members_[o], r, true);
        return {};
    }

    /**
     * @brief Drop @p ref from every option, leaving its subscription in the state it holds.
     *
     * The refs after it move up one place; options stay, an emptied one included. This is how
     * the owner frees the place of a ref it will not hand back, an INERT one most of all.
     * @retval NOT_FOUND    @p ref is listed in no option.
     * @retval BACKPRESSURE Another operation on this selector is running.
     */
    [[nodiscard]] result_t<void> remove(subscription_t ref) {
        const std::optional<ref_t> key = pack(ref);
        const hold_t hold(*this);
        if (!hold) return std::unexpected(status_t::BACKPRESSURE);
        const std::size_t r = key ? find_ref(*key) : n_refs_;
        if (r == n_refs_) return std::unexpected(status_t::NOT_FOUND);
        for (std::size_t i = r; i + 1 < n_refs_; ++i) refs_[i] = refs_[i + 1];
        --n_refs_;
        live_ = drop(live_, r);
        inert_ = drop(inert_, r);
        for (std::size_t o = 0; o < n_options_; ++o) members_[o] = drop(members_[o], r);
        return {};
    }

    /**
     * @brief Make @p option the active one and bring every ref in line with it; an empty
     *        @p option selects none and suspends every ref.
     *
     * Selecting the option already active retries a refused switch and changes nothing else.
     * @retval NOT_FOUND    No option is named @p option; nothing changed.
     * @retval BACKPRESSURE A resume was refused: @p option is active, @ref settled is false,
     *                      and selecting it again retries (see the class notes). Or another
     *                      operation on this selector is running, and nothing changed.
     */
    [[nodiscard]] result_t<void> select(std::string_view option) {
        const hold_t hold(*this);
        if (!hold) return std::unexpected(status_t::BACKPRESSURE);
        const std::size_t o = option.empty() ? kNone : find_option(option);
        if (!option.empty() && o == n_options_) return std::unexpected(status_t::NOT_FOUND);
        active_.store(static_cast<std::uint8_t>(o), std::memory_order_release);
        return reconcile();
    }

    /** @brief The active option's name; empty when none is. Never refused. */
    [[nodiscard]] std::string_view active() const noexcept {
        const std::uint8_t a = active_.load(std::memory_order_acquire);
        return a == kNone ? std::string_view{} : names_[a];
    }

    /** @brief False after a refused switch, until a retry resumes every ref it left out. */
    [[nodiscard]] bool settled() const noexcept { return settled_.load(std::memory_order_acquire); }

    /**
     * @brief What @p ref is doing now, re-read from the graph.
     * @retval NOT_FOUND    @p ref is listed in no option.
     * @retval BACKPRESSURE Another operation on this selector is running.
     */
    [[nodiscard]] result_t<selector_ref_state_t> state(subscription_t ref) {
        const std::optional<ref_t> key = pack(ref);
        const hold_t hold(*this);
        if (!hold) return std::unexpected(status_t::BACKPRESSURE);
        const std::size_t r = key ? find_ref(*key) : n_refs_;
        if (r == n_refs_) return std::unexpected(status_t::NOT_FOUND);
        refresh(r);
        return state_at(r);
    }

    /** @brief The selector's vertex; empty until @ref attach succeeds. */
    [[nodiscard]] std::optional<vertex_handle_t> vertex() const noexcept {
        if (!attached_) return std::nullopt;
        return std::bit_cast<vertex_handle_t>(vertex_);
    }

   private:
    /** @brief One membership or state bit per ref, in the smallest word that holds them. */
    using mask_t = std::conditional_t<
        kRefs <= 8, std::uint8_t,
        std::conditional_t<kRefs <= 16, std::uint16_t,
                           std::conditional_t<kRefs <= 32, std::uint32_t, std::uint64_t>>>;
    /** @brief A vertex handle's bytes: a handle has no empty state, so a table of them is kept
     *         as bytes and rebuilt with `std::bit_cast` (it is trivially copyable). */
    using handle_bytes_t = std::array<std::byte, sizeof(vertex_handle_t)>;
    /** @brief One ref: the address `<producer>:subscribers[slot]`, packed with no padding. */
    struct ref_t {
        handle_bytes_t producer; /**< @brief The producer's handle, as bytes. */
        std::uint16_t slot;      /**< @brief The `:subscribers[N]` slot. */
        /** @brief Two refs are equal iff they name the same address. */
        friend bool operator==(const ref_t&, const ref_t&) = default;
    };
    static_assert(std::is_trivially_copyable_v<vertex_handle_t>);
    static_assert(sizeof(ref_t) == sizeof(vertex_handle_t) + 2, "a ref packs with no padding");

    /** @brief "No option": the value `active_` holds when none is selected. */
    static constexpr std::size_t kNone = kOptions;
    /** @brief The two declared fields, borrowed from static storage. Both retain nothing:
     *         their reads are answered from the selector's own state. */
    static constexpr app_field_static_t kFields[] = {
        {"options", app_access_t::RO, retention_t::NONE, {}},
        {"active", app_access_t::RW, retention_t::NONE, {}},
    };

    /**
     * @brief The busy flag, held for one operation's length (see the class notes). A holder
     *        that did not get it changes nothing and answers `BACKPRESSURE`.
     */
    class hold_t {
       public:
        /** @brief Try to take @p s's flag; never waits. */
        explicit hold_t(subscription_selector_t& s) noexcept : s_(s), held_(s.try_take()) {}
        /** @brief Give the flag back, if this took it. */
        ~hold_t() {
            if (held_) s_.busy_.store(false, std::memory_order_release);
        }
        hold_t(const hold_t&) = delete;
        hold_t& operator=(const hold_t&) = delete;
        /** @brief Whether this took the flag. */
        explicit operator bool() const noexcept { return held_; }

       private:
        subscription_selector_t& s_; /**< @brief The selector whose flag this holds. */
        bool held_;                  /**< @brief Whether the take succeeded. */
    };

    /** @brief Take the busy flag, or answer false: one exchange on a core with atomic RMW, else
     *         a load and a store inside @p G's section, as `tr::rmw_counter_t` does. */
    template <class G = guard_t>
    [[nodiscard]] bool try_take() noexcept {
        if constexpr (std::atomic<bool>::is_always_lock_free) {
            return !busy_.exchange(true, std::memory_order_acquire);
        } else {
            const guard_scope_t<G> section(&busy_);
            const bool was = busy_.load(std::memory_order_relaxed);
            busy_.store(true, std::memory_order_relaxed);
            return !was;
        }
    }

    /** @brief @p m with bit @p r set to @p on. */
    [[nodiscard]] static mask_t with(mask_t m, std::size_t r, bool on) noexcept {
        const auto b = static_cast<mask_t>(mask_t{1} << r);
        return static_cast<mask_t>(on ? (m | b) : (m & static_cast<mask_t>(~b)));
    }

    /** @brief @p m with bit @p r taken out and the bits above it moved down one. */
    [[nodiscard]] static mask_t drop(mask_t m, std::size_t r) noexcept {
        const auto low = static_cast<mask_t>((mask_t{1} << r) - 1u);
        return static_cast<mask_t>((m & low) | ((m >> 1) & static_cast<mask_t>(~low)));
    }

    /** @brief @p ref's address as a ref; empty for a default handle or a slot past 16 bits. */
    [[nodiscard]] std::optional<ref_t> pack(const subscription_t& ref) const noexcept {
        const result_t<subscription_address_t> at = g_->subscription_address(ref);
        if (!at || at->slot > UINT16_MAX) return std::nullopt;
        return ref_t{std::bit_cast<handle_bytes_t>(at->producer),
                     static_cast<std::uint16_t>(at->slot)};
    }

    /** @brief Ref @p r's producer handle. */
    [[nodiscard]] vertex_handle_t producer(std::size_t r) const noexcept {
        return std::bit_cast<vertex_handle_t>(refs_[r].producer);
    }

    /** @brief The option named @p name, or `n_options_`. */
    [[nodiscard]] std::size_t find_option(std::string_view name) const noexcept {
        std::size_t o = 0;
        while (o < n_options_ && names_[o] != name) ++o;
        return o;
    }

    /** @brief The index holding @p ref, or `n_refs_`. */
    [[nodiscard]] std::size_t find_ref(const ref_t& ref) const noexcept {
        std::size_t r = 0;
        while (r < n_refs_ && !(refs_[r] == ref)) ++r;
        return r;
    }

    /**
     * @brief Suspend every live ref the active option leaves out, then resume every ref it
     *        lists that is not live, skipping INERT refs. Caller holds the flag.
     *
     * One loop, two passes: pass 0 suspends, pass 1 resumes, so no resume runs before the
     * last suspend. A refused resume does not stop the pass; the first refusal is returned.
     */
    [[nodiscard]] result_t<void> reconcile() {
        const std::size_t a = active_.load(std::memory_order_relaxed);
        const mask_t want = a == kNone ? mask_t{0} : members_[a];
        result_t<void> out{};
        for (const bool resume : {false, true}) {
            for (std::size_t r = 0; r < n_refs_; ++r) {
                const bool w = (want >> r) & 1u;
                if (((inert_ >> r) & 1u) || bool((live_ >> r) & 1u) == w || w != resume) continue;
                const result_t<void> t =
                    g_->set_suspended(g_->subscription_at(producer(r), refs_[r].slot), !resume);
                const bool gone = !t && t.error() == status_t::NOT_FOUND;
                inert_ = with(inert_, r, gone);
                live_ = with(live_, r, t.has_value() && resume);
                if (!t && !gone && out) out = t;
            }
        }
        settled_.store(out.has_value(), std::memory_order_release);
        return out;
    }

    /** @brief Re-read ref @p r's state from the graph; a gone subscription turns it INERT, and
     *         only @ref add brings it back. Caller holds the flag. */
    void refresh(std::size_t r) {
        const result_t<bool> s =
            ((inert_ >> r) & 1u)
                ? result_t<bool>{std::unexpected(status_t::NOT_FOUND)}
                : g_->is_suspended(g_->subscription_at(producer(r), refs_[r].slot));
        inert_ = with(inert_, r, !s.has_value());
        live_ = with(live_, r, s.has_value() && !*s);
    }

    /** @brief Ref @p r's state as last read or set. Caller holds the flag. */
    [[nodiscard]] selector_ref_state_t state_at(std::size_t r) const noexcept {
        if ((inert_ >> r) & 1u) return selector_ref_state_t::INERT;
        return ((live_ >> r) & 1u) ? selector_ref_state_t::LIVE : selector_ref_state_t::SUSPENDED;
    }

    /**
     * @brief The `active` write seam (`handlers_t::on_app_field_admit`): a `NAME` selects that
     *        option, an empty `STATUS` selects none, and the switch's answer is the writer's.
     *
     * The switch runs here, before the store, because this is the one field seam whose answer
     * reaches the writer; the field retains nothing, so there is no store to run after it.
     * `options` has no write surface for anyone, the owner included: refs are listed through
     * @ref add.
     */
    static result_t<view::view_t> admit_field(void* ctx, std::string_view name,
                                              const view::view_t& value,
                                              const write_ctx_t& /*writer*/) {
        auto& self = *static_cast<subscription_selector_t*>(ctx);
        if (name != "active") return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        const auto tlv = wire::tlv_node_t::over(value);
        if (!tlv || tlv->opt().pl) return std::unexpected(status_t::TYPE_MISMATCH);
        const std::span<const std::byte> body = tlv->body();
        const std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());
        const bool none = tlv->type() == wire::type_t::STATUS && body.empty();
        if (!none && (tlv->type() != wire::type_t::NAME || body.empty()))
            return std::unexpected(status_t::TYPE_MISMATCH);
        if (const result_t<void> r = self.select(text); !r) return std::unexpected(r.error());
        return value;
    }

    /** @brief The read seam (`handlers_t::on_app_field_read`): both fields are answered from
     *         the selector's own state. An empty reference is a refused draw or a busy
     *         selector (BACKPRESSURE). */
    static std::optional<value_ref_t> read_field(void* ctx, std::string_view name) {
        auto& self = *static_cast<subscription_selector_t*>(ctx);
        const hold_t hold(self);
        if (!hold) return value_ref_t{};
        std::array<std::byte, 512> scratch;
        mem::bump_source_t frame(scratch, self.g_->table_source());
        mem::bytes_t out(frame);
        const bool ok = name == "active" ? self.emit_active(out) : self.emit_options(out);
        return ok ? value_ref_t::copy(mem::as_span(out), self.g_->value_source()) : value_ref_t{};
    }

    /** @brief `NAME <active>`, or an empty `STATUS` when none is. Caller holds the flag. */
    [[nodiscard]] bool emit_active(mem::bytes_t& out) const {
        const std::string_view a = active();
        if (a.empty()) return wire::emit_tlv(out, wire::type_t::STATUS, {}, {});
        return wire::emit_name(out, a);
    }

    /**
     * @brief `SETTINGS{ NAME <option> SETTINGS{ NAME "ref" SETTINGS{ NAME "producer" PATH <key>
     *        NAME "slot" VALUE <u32 LE> NAME "state" NAME <state> } … } … }` (RFC-0034).
     *
     * Each ref's state is re-read as it is listed. Each level is staged and then wrapped, all on
     * @p out's source; after a refused draw the rest only runs out, and the read is refused.
     * Caller holds the flag.
     * @retval false A draw was refused.
     */
    [[nodiscard]] bool emit_options(mem::bytes_t& out) {
        mem::block_source_t& src = out.source();
        mem::bytes_t all(src), opt(src), ref(src), key(src);
        constexpr wire::opt_t kPl{.pl = true};
        bool ok = true;
        for (std::size_t o = 0; o < n_options_; ++o) {
            opt.clear();
            for (mask_t m = members_[o]; m != 0; m = static_cast<mask_t>(m & (m - 1u))) {
                const auto r = static_cast<std::size_t>(std::countr_zero(m));
                refresh(r);
                ref.clear();
                key.clear();
                ok = ok && g_->key_of(producer(r), key) && wire::emit_name(ref, "producer") &&
                     wire::emit_tlv(ref, wire::type_t::PATH, {}, mem::as_span(key)) &&
                     wire::emit_name(ref, "slot") &&
                     wire::emit_value_le(ref, std::uint32_t{refs_[r].slot}, 4) &&
                     wire::emit_name(ref, "state") &&
                     wire::emit_name(ref, to_string(state_at(r))) && wire::emit_name(opt, "ref") &&
                     wire::emit_tlv(opt, wire::type_t::SETTINGS, kPl, mem::as_span(ref));
            }
            ok = ok && wire::emit_name(all, names_[o]) &&
                 wire::emit_tlv(all, wire::type_t::SETTINGS, kPl, mem::as_span(opt));
        }
        return ok && wire::emit_tlv(out, wire::type_t::SETTINGS, kPl, mem::as_span(all));
    }

    graph_t* g_;                      /**< @brief The graph the refs live on. */
    std::array<ref_t, kRefs> refs_{}; /**< @brief The distinct refs, in order added. */
    std::array<std::string_view, kOptions> names_{}; /**< @brief Option names, borrowed. */
    std::array<mask_t, kOptions> members_{};         /**< @brief Per option: the refs it lists. */
    handle_bytes_t vertex_{};                        /**< @brief The selector's vertex, as bytes. */
    mask_t live_{0};                                 /**< @brief Refs last seen delivering. */
    mask_t inert_{0};                                /**< @brief Refs whose subscription is gone. */
    std::uint8_t n_refs_ = 0;                        /**< @brief Refs in use. */
    std::uint8_t n_options_ = 0;                     /**< @brief Options in use. */
    std::atomic<std::uint8_t> active_{kNone};        /**< @brief The active option, or `kNone`. */
    std::atomic<bool> settled_{true}; /**< @brief False after a refused switch, until a retry. */
    std::atomic<bool> busy_{false};   /**< @brief The one-operation flag (see `hold_t`). */
    bool attached_ = false;           /**< @brief Whether `vertex_` holds a vertex. */
};

// The sizes the class notes state, pinned on both data models.
static_assert(sizeof(void*) != 8 || sizeof(subscription_selector_t<>) == 176);
static_assert(sizeof(void*) != 8 || sizeof(subscription_selector_t<64, 16>) == 1064);
static_assert(sizeof(void*) != 4 || sizeof(subscription_selector_t<>) == 100);
static_assert(sizeof(void*) != 4 || sizeof(subscription_selector_t<64, 16>) == 680);

}  // namespace tr::graph
