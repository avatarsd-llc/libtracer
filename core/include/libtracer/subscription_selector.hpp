/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

#include "libtracer/app_fields.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/graph.hpp"
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
 * no buffer of the library's: its tables are fixed arrays inside the object, sized at compile
 * time by the embedder that declares it, and a read of `options` stages on the graph's own
 * sources. It reads no clock and arms no timer.
 */

namespace tr::graph {

/**
 * @brief What one ref of a `subscription_selector_t` is doing now, as `options` lists it.
 */
enum class selector_ref_state_t : std::uint8_t {
    SUSPENDED = 0, /**< @brief The subscription exists and delivers nothing. */
    LIVE = 1,      /**< @brief The subscription exists and delivers. */
    /** @brief The subscription is gone (unsubscribed, evicted, its producer retired). The ref
     *         stays listed, a switch skips it, and nothing removes it. */
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
 * @tparam kRefs    How many DISTINCT refs one instance holds, across all its options (the
 *                  ticket sizes a node at about 64). A ref listed in several options counts once.
 * @tparam kOptions How many options one instance holds.
 *
 * **A switch from A to B** suspends every ref that is live and not in B, then resumes every
 * ref of B that is suspended. A ref in both is not touched. Suspending first is what rules out
 * a double delivery when A and B reach one target through two different subscriptions; the
 * price is a gap, from the first suspend to the last resume, in which that target receives
 * nothing. Each toggle is `%graph_t::set_suspended`: no unsubscribe, no re-subscribe, no frame,
 * and nothing drawn from any source, so a switch also reaches a subscriber behind a link at once.
 *
 * **A refused switch.** A resume can answer `BACKPRESSURE` (a resume after an edge republish on
 * its producer was refused for want of memory). The switch then reports `BACKPRESSURE`, B stays
 * the active option and @ref settled turns false: every ref outside B is suspended, the refs of
 * B that resumed deliver, and the refused ones stay suspended. Nothing is ever delivered twice.
 * Selecting B again (or writing `active` again) retries only the refs still suspended; a retry
 * succeeds once the producer's source has room.
 *
 * **A dangling ref** (its subscription is gone) answers `NOT_FOUND` to a toggle. The selector
 * then marks it INERT, skips it on every later switch, and keeps listing it.
 *
 * @warning A @ref subscription_t carries no generation yet (#1932). If a ref's subscription is
 *          removed and a later subscribe on the same producer reuses its slot BEFORE the selector
 *          has seen the slot empty, the ref names the new edge. The selector never toggles a ref
 *          again once it is INERT, which narrows that window to the first toggle after removal.
 * @note Thread-safe: every member and both field seams take the instance's mutex. A field seam
 *       runs on the writer's or reader's thread with no graph lock held.
 * @note The instance is the context of its vertex's field seams. Destroy it only when no field
 *       operation on its vertex can be in flight; the destructor retires the vertex and leaves
 *       every subscription in the state it holds.
 */
template <std::size_t kRefs = 64, std::size_t kOptions = 16>
class subscription_selector_t {
    static_assert(kRefs > 0 && kOptions > 0, "a selector holds at least one ref and one option");

   public:
    /** @brief The longest option name: one NAME segment (RFC-0034, 05 §`0x02`). */
    static constexpr std::size_t kNameMax = 64;

    /** @brief A selector over @p g, attached to no vertex yet. @p g must outlive it. */
    explicit subscription_selector_t(graph_t& g) noexcept : g_(&g) {}

    /** @brief Retire the selector's vertex, if attached; every subscription keeps its state. */
    ~subscription_selector_t() {
        if (vertex_) (void)g_->retire(*vertex_);
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
        const std::lock_guard lock(m_);
        if (vertex_) return std::unexpected(status_t::INVALID_PATH);
        handlers_t h;
        h.on_app_field_admit = {&admit_field, this};
        h.on_app_field_read = {&read_field, this};
        auto made = g_->register_vertex_key(key, role_t::STORED_VALUE, h,
                                            vertex_policy_t{.app_fields = kFields});
        if (made) vertex_ = *made;
        return made;
    }

    /** @brief @ref attach at @p path. */
    [[nodiscard]] result_t<vertex_handle_t> attach(const path_t& path) {
        return attach(path.key());
    }

    /**
     * @brief List @p ref in @p option, creating the option on its first ref.
     *
     * Records only: it toggles nothing. The ref keeps the state it has until the next
     * @ref select, which brings it in line with the active option, so the owner creates a
     * subscription suspended unless its option is the active one. Options list in the order
     * they were created, and refs in the order they were added.
     *
     * @retval INVALID_PATH @p option is not one NAME segment (1 to 64 bytes, no reserved
     *                      character).
     * @retval NOT_FOUND    @p ref names no live subscription.
     * @retval PATH_IN_USE  @p ref is already listed in @p option.
     * @retval BACKPRESSURE A new ref past @p kRefs, or a new option past @p kOptions.
     */
    [[nodiscard]] result_t<void> add(std::string_view option, subscription_t ref) {
        if (option.size() > kNameMax || !valid_segment(option))
            return std::unexpected(status_t::INVALID_PATH);
        const result_t<bool> suspended = g_->is_suspended(ref);
        if (!suspended) return std::unexpected(suspended.error());
        const std::lock_guard lock(m_);
        std::size_t o = find_option(option);
        std::size_t r = find_ref(ref);
        if ((o == n_options_ && n_options_ == kOptions) || (r == n_refs_ && n_refs_ == kRefs))
            return std::unexpected(status_t::BACKPRESSURE);
        if (o < n_options_ && r < n_refs_ && options_[o].members[r])
            return std::unexpected(status_t::PATH_IN_USE);
        if (o == n_options_) {
            std::memcpy(options_[o].name.data(), option.data(), option.size());
            options_[o].len = static_cast<std::uint8_t>(option.size());
            ++n_options_;
        }
        if (r == n_refs_) {
            refs_[r] = ref;
            live_[r] = !*suspended;
            ++n_refs_;
        }
        options_[o].members[r] = true;
        return {};
    }

    /**
     * @brief Make @p option the active one and bring every ref in line with it; an empty
     *        @p option selects none and suspends every ref.
     *
     * Selecting the option already active retries a refused switch and changes nothing else.
     * @retval NOT_FOUND    No option is named @p option; nothing changed.
     * @retval BACKPRESSURE A resume was refused: @p option is active, @ref settled is false,
     *                      and selecting it again retries (see the class notes).
     */
    [[nodiscard]] result_t<void> select(std::string_view option) {
        const std::lock_guard lock(m_);
        const std::size_t o = option.empty() ? kNone : find_option(option);
        if (!option.empty() && o == n_options_) return std::unexpected(status_t::NOT_FOUND);
        active_ = o;
        return reconcile();
    }

    /** @brief The active option's name; empty when none is. */
    [[nodiscard]] std::string_view active() const {
        const std::lock_guard lock(m_);
        return active_ == kNone ? std::string_view{} : options_[active_].view();
    }

    /** @brief False after a refused switch, until a retry resumes every ref it left out. */
    [[nodiscard]] bool settled() const {
        const std::lock_guard lock(m_);
        return settled_;
    }

    /**
     * @brief What @p ref is doing now, re-read from the graph.
     * @retval NOT_FOUND @p ref is listed in no option.
     */
    [[nodiscard]] result_t<selector_ref_state_t> state(subscription_t ref) {
        const std::lock_guard lock(m_);
        const std::size_t r = find_ref(ref);
        if (r == n_refs_) return std::unexpected(status_t::NOT_FOUND);
        return refresh(r);
    }

    /** @brief The selector's vertex; empty until @ref attach succeeds. */
    [[nodiscard]] std::optional<vertex_handle_t> vertex() const {
        const std::lock_guard lock(m_);
        return vertex_;
    }

   private:
    /** @brief "No option": the value `active_` holds when none is selected. */
    static constexpr std::size_t kNone = kOptions;
    /** @brief The two declared fields, borrowed from static storage. Both retain nothing:
     *         their reads are answered from the selector's own state. */
    static constexpr app_field_static_t kFields[] = {
        {"options", app_access_t::RO, retention_t::NONE, {}},
        {"active", app_access_t::RW, retention_t::NONE, {}},
    };
    /** @brief One membership bit per ref slot. */
    using mask_t = std::bitset<kRefs>;

    /** @brief One option: its name and which refs it lists. */
    struct option_t {
        std::array<char, kNameMax> name{}; /**< @brief The name bytes, `len` of them. */
        std::uint8_t len = 0;              /**< @brief The name's length. */
        mask_t members{};                  /**< @brief The refs this option lists. */
        /** @brief The name as text. */
        [[nodiscard]] std::string_view view() const noexcept { return {name.data(), len}; }
    };

    /** @brief The option named @p name, or `n_options_`. */
    [[nodiscard]] std::size_t find_option(std::string_view name) const noexcept {
        std::size_t o = 0;
        while (o < n_options_ && options_[o].view() != name) ++o;
        return o;
    }

    /** @brief The ref slot holding @p ref, or `n_refs_`. */
    [[nodiscard]] std::size_t find_ref(const subscription_t& ref) const noexcept {
        std::size_t r = 0;
        while (r < n_refs_ && !(refs_[r] == ref)) ++r;
        return r;
    }

    /**
     * @brief Suspend every live ref the active option leaves out, then resume every ref it
     *        lists that is not live, skipping INERT refs. Caller holds `m_`.
     *
     * One loop, two passes: pass 0 suspends, pass 1 resumes, so no resume runs before the
     * last suspend. A refused resume does not stop the pass; the first refusal is returned.
     */
    [[nodiscard]] result_t<void> reconcile() {
        const mask_t want = active_ == kNone ? mask_t{} : options_[active_].members;
        result_t<void> out{};
        for (const bool resume : {false, true}) {
            for (std::size_t r = 0; r < n_refs_; ++r) {
                if (inert_[r] || live_[r] == want[r] || want[r] != resume) continue;
                const result_t<void> t = g_->set_suspended(refs_[r], !resume);
                inert_[r] = !t && t.error() == status_t::NOT_FOUND;
                live_[r] = t.has_value() && resume;
                if (!t && !inert_[r] && out) out = t;
            }
        }
        settled_ = out.has_value();
        return out;
    }

    /** @brief Re-read ref @p r's state from the graph; a gone subscription turns it INERT for
     *         good. Caller holds `m_`. */
    selector_ref_state_t refresh(std::size_t r) {
        const result_t<bool> s = inert_[r] ? result_t<bool>{std::unexpected(status_t::NOT_FOUND)}
                                           : g_->is_suspended(refs_[r]);
        inert_[r] = !s.has_value();
        live_[r] = s.has_value() && !*s;
        if (inert_[r]) return selector_ref_state_t::INERT;
        return live_[r] ? selector_ref_state_t::LIVE : selector_ref_state_t::SUSPENDED;
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
     *         the selector's own state. An empty reference is a refused draw (BACKPRESSURE). */
    static std::optional<value_ref_t> read_field(void* ctx, std::string_view name) {
        auto& self = *static_cast<subscription_selector_t*>(ctx);
        const std::lock_guard lock(self.m_);
        std::array<std::byte, 512> scratch;
        mem::bump_source_t frame(scratch, self.g_->table_source());
        mem::bytes_t out(frame);
        const bool ok = name == "active" ? self.emit_active(out) : self.emit_options(out);
        return ok ? value_ref_t::copy(mem::as_span(out), self.g_->value_source()) : value_ref_t{};
    }

    /** @brief `NAME <active>`, or an empty `STATUS` when none is. Caller holds `m_`. */
    [[nodiscard]] bool emit_active(mem::bytes_t& out) const {
        if (active_ == kNone) return wire::emit_tlv(out, wire::type_t::STATUS, {}, {});
        return wire::emit_name(out, options_[active_].view());
    }

    /**
     * @brief `SETTINGS{ NAME <option> SETTINGS{ NAME "ref" SETTINGS{ NAME "producer" PATH <key>
     *        NAME "slot" VALUE <u32 LE> NAME "state" NAME <state> } … } … }` (RFC-0034).
     *
     * Every ref's state is re-read first, so one read lists one coherent set of states. Each
     * level is staged and then wrapped, all on @p out's source. Caller holds `m_`.
     * @retval false A draw was refused.
     */
    [[nodiscard]] bool emit_options(mem::bytes_t& out) {
        std::array<selector_ref_state_t, kRefs> states{};
        for (std::size_t r = 0; r < n_refs_; ++r) states[r] = refresh(r);
        mem::block_source_t& src = out.source();
        mem::bytes_t all(src), opt(src), ref(src), key(src);
        constexpr wire::opt_t kPl{.pl = true};
        bool ok = true;
        for (std::size_t o = 0; ok && o < n_options_; ++o) {
            opt.clear();
            for (std::size_t r = 0; ok && r < n_refs_; ++r) {
                if (!options_[o].members[r]) continue;
                const result_t<std::size_t> slot = g_->subscription_address(refs_[r], key);
                ref.clear();
                ok = slot && wire::emit_name(ref, "producer") &&
                     wire::emit_tlv(ref, wire::type_t::PATH, {}, mem::as_span(key)) &&
                     wire::emit_name(ref, "slot") &&
                     wire::emit_value_le(ref, static_cast<std::uint32_t>(*slot), 4) &&
                     wire::emit_name(ref, "state") && wire::emit_name(ref, to_string(states[r])) &&
                     wire::emit_name(opt, "ref") &&
                     wire::emit_tlv(opt, wire::type_t::SETTINGS, kPl, mem::as_span(ref));
            }
            ok = ok && wire::emit_name(all, options_[o].view()) &&
                 wire::emit_tlv(all, wire::type_t::SETTINGS, kPl, mem::as_span(opt));
        }
        return ok && wire::emit_tlv(out, wire::type_t::SETTINGS, kPl, mem::as_span(all));
    }

    graph_t* g_;                               /**< @brief The graph the refs live on. */
    mutable std::mutex m_;                     /**< @brief Guards everything below. */
    std::optional<vertex_handle_t> vertex_;    /**< @brief The selector's vertex, once attached. */
    std::array<subscription_t, kRefs> refs_{}; /**< @brief The distinct refs, in order added. */
    mask_t live_{};                            /**< @brief Refs the selector last saw delivering. */
    mask_t inert_{};                           /**< @brief Refs whose subscription is gone. */
    std::array<option_t, kOptions> options_{}; /**< @brief The options, in order created. */
    std::size_t n_refs_ = 0;                   /**< @brief Refs in use. */
    std::size_t n_options_ = 0;                /**< @brief Options in use. */
    std::size_t active_ = kNone;               /**< @brief The active option, or `kNone`. */
    bool settled_ = true; /**< @brief False after a refused switch, until a retry completes. */
};

}  // namespace tr::graph
