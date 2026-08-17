/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/route_handle.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/vertex.hpp"

namespace tr::net {

using wire::opt_t;
using wire::type_t;

// --- the substrate: blocks, blobs, and the refcounted per-link node ------------
//
// #603 defect 1 / #873 family 3. Everything below draws from `src_` (or, once a node exists,
// from the node's own `src`) through `try_alloc`/`release` and NOTHING else — no `std::pmr`,
// no global-heap probe, no throwing container. The rule these functions embody, and the one
// the remaining #873 families copy: **the store owns exact-size blocks; the caller owns the
// bytes it hands in.** Written down in `docs/reference/09-memory-substrate.md`.

namespace {

/**
 * @brief Take the FIRST block of @p a at two entries rather than `block_array_t`'s eight.
 *
 * A NARROW-end RAM decision, and a measured one (`bench_failable_census blocks`).
 * `block_array_t::grow` jumps an empty array straight to 8 elements, which suits the decode
 * arenas it was written for — they fill. A link's label table is the opposite shape: a link
 * carries FEW compact flows, most carry one, and 8 slots of an ~80 B entry is ~500 B of
 * speculative capacity per link on a node whose whole budget is 16 KB. Seeding at 2 costs one
 * extra reallocation on a link that grows past two flows, on the COLD advertise path, and
 * nothing at all after that — `reserve` is a no-op once the capacity is there, so the steady
 * state pays one compare per insert.
 *
 * @retval false The source refused the seed block; the caller refuses the bind.
 */
template <class T>
[[nodiscard]] bool reserve_small(mem::block_array_t<T>& a) noexcept {
    return a.size() != 0 || a.reserve(2);
}

}  // namespace

void route_handle_t::free_blob(link_tables_t& t, blob_t& b) noexcept {
    if (b.p == nullptr) return;
    t.src->release(b.p, b.n, 1);
    b = blob_t{};
}

bool route_handle_t::set_blob(link_tables_t& t, blob_t& out,
                              std::span<const std::byte> src) noexcept {
    if (src.empty()) {
        free_blob(t, out);
        return true;
    }
    // Allocate the REPLACEMENT before freeing the old block, so a refusal leaves the entry
    // exactly as it was. `record_egress` re-records in place on a live label, and a
    // half-applied replacement would strand a flow on a route it never advertised.
    void* const p = t.src->try_alloc(src.size(), 1);
    if (p == nullptr) return false;
    std::memcpy(p, src.data(), src.size());
    free_blob(t, out);
    out.p = static_cast<std::byte*>(p);
    out.n = static_cast<std::uint32_t>(src.size());
    return true;
}

void route_handle_t::free_binding(link_tables_t& t, stored_binding_t& b) noexcept {
    free_blob(t, b.down_link);
    free_blob(t, b.local_route);
}

void route_handle_t::release_tables(link_tables_t* t) noexcept {
    // acq_rel so the last dropper sees every write the other holders made before dropping —
    // the ordinary intrusive-refcount contract, and the one `std::shared_ptr` gave for free.
    if (t->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    mem::block_source_t* const src = t->src;
    const std::size_t bytes = t->block_bytes;
    for (std::size_t i = 0; i < t->ingress.size(); ++i) free_binding(*t, t->ingress[i].binding);
    for (std::size_t i = 0; i < t->egress.size(); ++i) free_blob(*t, t->egress[i].route);
    t->~link_tables_t();  // runs the two block_array_t destructors, returning their blocks
    src->release(t, bytes, alignof(link_tables_t));
}

route_handle_t::~route_handle_t() {
    for (std::size_t i = 0; i < links_.size(); ++i) release_tables(links_[i]);
}

std::size_t route_handle_t::find_slot_locked(std::string_view link) const noexcept {
    for (std::size_t i = 0; i < links_.size(); ++i) {
        if (links_[i]->name() == link) return i;
    }
    return kNoSlot;
}

route_handle_t::tables_ref_t route_handle_t::tables(std::string_view link) {
    {
        const std::shared_lock lock(links_m_);
        if (const std::size_t i = find_slot_locked(link); i != kNoSlot) return pin(links_[i]);
    }
    const std::unique_lock lock(links_m_);
    // Re-check: another thread may have created it between the two locks.
    if (const std::size_t i = find_slot_locked(link); i != kNoSlot) return pin(links_[i]);
    // ONE block for the node AND its name (#603 defect 1). The pair used to be a
    // `std::allocate_shared` control-block-plus-object and a `std::pmr::string`, both throwing
    // and both reachable from an inbound ADVERTISE. There is no nothrow `allocate_shared`, so
    // the pin (#488) is served by the intrusive `refs` counter instead; the name rides inline
    // because a second `try_alloc` would be a second refusal point for no benefit.
    if (link.size() > 0xFFFFu) return tables_ref_t{};
    const std::size_t bytes = link_tables_t::inline_name_offset() + link.size();
    void* const raw = src_->try_alloc(bytes, alignof(link_tables_t));
    // Exhaustion is a REFUSAL, not an abort: every caller of `tables()` already answers
    // "could not record this" by degrading the flow to the full-route FWD form.
    if (raw == nullptr) return tables_ref_t{};
    auto* const t = new (raw) link_tables_t(*src_);
    t->block_bytes = static_cast<std::uint32_t>(bytes);
    t->name_len = static_cast<std::uint32_t>(link.size());
    if (!link.empty())
        std::memcpy(static_cast<std::byte*>(raw) + link_tables_t::inline_name_offset(), link.data(),
                    link.size());
    // Stamp the tables with the clear epoch they were born at (#827), under the same
    // exclusive lock clear_link advances it under — once tables exist, a link's epoch moves
    // only when THAT link is cleared. Before they exist a sample reads the shared counter,
    // so an unrelated clear in the sample→creation window makes the bind refuse spuriously
    // (safe direction: the re-advertise after the stale-label NACK binds normally).
    t->born_gen = clear_gen_;
    if (!links_.push_back(t)) {  // the registry itself can refuse — unwind the node
        release_tables(t);
        return tables_ref_t{};
    }
    return pin(t);
}

std::uint32_t route_handle_t::link_epoch_locked(std::string_view link) const {
    const std::size_t i = find_slot_locked(link);
    return i == kNoSlot ? clear_gen_ : links_[i]->born_gen;
}

std::uint32_t route_handle_t::link_epoch(std::string_view link) const {
    const std::shared_lock lock(links_m_);
    return link_epoch_locked(link);
}

route_handle_t::tables_view_t route_handle_t::view_tables(std::string_view link) const {
    std::shared_lock lock(links_m_);
    const std::size_t i = find_slot_locked(link);
    if (i == kNoSlot) return {};
    // No pin: the shared lock IS the keep-alive, because clear_link erases only under the
    // exclusive one. See `tables_view_t` for why that matters on the per-delivery path.
    return tables_view_t{std::move(lock), links_[i]};
}

route_handle_t::tables_ref_t route_handle_t::find_tables(std::string_view link) const {
    const std::shared_lock lock(links_m_);
    const std::size_t i = find_slot_locked(link);
    // A PINNING reference; the per-link mutex is inside. Taken under the registry lock, which
    // is what makes it safe against a concurrent clear_link: the erase cannot run between the
    // find and the retain.
    return i == kNoSlot ? tables_ref_t{} : pin(links_[i]);
}

bool route_handle_t::bind_locked(link_tables_t& t, std::uint16_t label,
                                 const handle_binding_t& binding) {
    // Copy the caller's borrowed bytes into blocks this table owns. Both copies are attempted
    // into a SCRATCH entry first, so a source refusal on the second leaves nothing half-bound.
    stored_binding_t sb;
    sb.terminus = binding.terminus;
    sb.warm = binding.warm;
    sb.out_label = binding.out_label;
    sb.down_slot = binding.down_slot;
    sb.target = binding.target;
    sb.target_gen = binding.target_gen;
    sb.mount_gen = binding.mount_gen;
    const auto as_bytes = [](std::string_view s) {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
    };
    if (!set_blob(t, sb.down_link, as_bytes(binding.down_link)) ||
        !set_blob(t, sb.local_route, binding.local_route)) {
        free_binding(t, sb);
        refused_.fetch_add(1, std::memory_order_relaxed);
        return false;  // the injected source is exhausted — the SAME refusal a full table gives
    }
    for (std::size_t i = 0; i < t.ingress.size(); ++i) {
        ingress_entry_t& e = t.ingress[i];
        if (e.label == label) {
            free_binding(t, e.binding);  // block_array_t runs no destructors — free by hand
            e.binding = sb;
            return true;  // rebind in place — adds no entry, so the bound cannot refuse it
        }
    }
    // REFUSE rather than evict (#603). Evict-oldest is right for `can_reassembly_t`, whose
    // groups are short-lived so oldest ~ stalest; a label binding is the opposite -- it
    // exists precisely because a flow is long-running, so evicting the oldest preferentially
    // kills the longest-lived stream and makes it re-advertise, forever. True LRU would need
    // a write on `resolved()`, which is the per-delivery hot path, to solve a flow-setup
    // problem. Refusing keeps every established flow untouched and degrades only NEW ones,
    // down the path a full label space already takes.
    if ((max_bindings_ != 0 && t.ingress.size() >= max_bindings_) || !reserve_small(t.ingress) ||
        !t.ingress.push_back(ingress_entry_t{.label = label, .binding = sb})) {
        // Either the injected COUNT bound or the injected SOURCE said no. One answer, because
        // ADR-0079 makes the store's size a bound in its own right (see `refused_bindings`).
        free_binding(t, sb);
        refused_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool route_handle_t::bind_ingress(std::string_view in_link, std::uint16_t label,
                                  handle_binding_t binding) {
    const tables_ref_t t = tables(in_link);
    if (!t) {  // the source could not serve this link's tables — a counted refusal
        refused_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::lock_guard lock(t->m);
    return bind_locked(*t, label, binding);
}

bool route_handle_t::bind_ingress_forward(std::string_view in_link, std::uint16_t label,
                                          handle_binding_t binding, std::uint32_t down_epoch) {
    // Create the inbound tables FIRST and outside the registry lock below: creating them
    // needs it exclusively, and a shared holder cannot upgrade. The pinning copy survives a
    // concurrent clear of `in_link` (#488) — a bind into a detached table is discarded, which
    // is the clean slate an inbound reconnect wants anyway.
    const tables_ref_t t = tables(in_link);
    if (!t) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // The epoch read and the insert are ONE critical section against clear_link's
    // bump-erase-sweep, which holds this lock exclusively (#827). Without that, the sweep can
    // still run between the two and the check answers about a state the bind then leaves.
    // Lock order registry -> table is the one clear_link and ingress_count already establish.
    const std::shared_lock reg(links_m_);
    // `bound_generation_matches` rather than `==`, for the reason RFC-0024 §4.4 gives: at the
    // ceiling the counter STOPS, so a saturated epoch would compare equal forever and this
    // guard would be permanently dead. Refusing at saturation instead costs a node that has
    // seen 2^32 link clears its compacted FORWARDING only — every flow through it degrades to
    // the full-route FWD form, which carries its own route and always works. That is the same
    // degrade an exhausted label space already takes (#603), and it is the safe direction: the
    // alternative is a swap bound against a table that no longer exists, which is #716.
    if (!graph::bound_generation_matches(link_epoch_locked(binding.down_link), down_epoch))
        return false;
    const std::lock_guard lock(t->m);
    return bind_locked(*t, label, binding);
}

binding_copy_t route_handle_t::copy_binding(std::string_view in_link, std::uint16_t label,
                                            std::span<char> link_out,
                                            std::span<std::byte> route_out) const {
    binding_copy_t out;
    const tables_view_t t = view_tables(in_link);
    if (!t) return out;
    const std::lock_guard lock(t->m);
    for (std::size_t i = 0; i < t->ingress.size(); ++i) {
        const ingress_entry_t& e = t->ingress[i];
        if (e.label != label) continue;
        out.found = true;
        out.terminus = e.binding.terminus;
        out.out_label = e.binding.out_label;
        out.mount_gen = e.binding.mount_gen;
        out.down_link_size = e.binding.down_link.n;
        out.local_route_size = e.binding.local_route.n;
        // Copied under the link's own mutex into CALLER storage rather than handed out by
        // reference: the blocks belong to this table, and a concurrent clear_link frees them
        // the moment the last pin drops. The sizes are reported whether or not the copy fit,
        // so truncation stays distinguishable from absence.
        if (out.down_link_size <= link_out.size()) {
            if (out.down_link_size != 0)
                std::memcpy(link_out.data(), e.binding.down_link.p, out.down_link_size);
            out.down_link = std::string_view(link_out.data(), out.down_link_size);
        } else {
            out.truncated = true;
        }
        if (out.local_route_size <= route_out.size()) {
            if (out.local_route_size != 0)
                std::memcpy(route_out.data(), e.binding.local_route.p, out.local_route_size);
            out.local_route = route_out.first(out.local_route_size);
        } else {
            out.truncated = true;
        }
        return out;
    }
    return out;
}

resolved_binding_t route_handle_t::resolved(std::string_view in_link, std::uint16_t label) const {
    resolved_binding_t out;
    const tables_view_t t = view_tables(in_link);
    if (!t) return out;
    const std::lock_guard lock(t->m);
    for (std::size_t i = 0; i < t->ingress.size(); ++i) {
        const ingress_entry_t& e = t->ingress[i];
        if (e.label != label) continue;
        out.found = true;
        out.terminus = e.binding.terminus;
        out.out_label = e.binding.out_label;
        out.warm = e.binding.warm;
        out.down_slot = e.binding.down_slot;
        out.target = e.binding.target;
        out.target_gen = e.binding.target_gen;
        out.mount_gen = e.binding.mount_gen;
        return out;  // ~24 trivially copyable bytes — no string, no vector, no allocation
    }
    return out;
}

std::size_t route_handle_t::copy_local_route(std::string_view in_link, std::uint16_t label,
                                             std::span<std::byte> out) const {
    const tables_view_t t = view_tables(in_link);
    if (!t) return 0;
    const std::lock_guard lock(t->m);
    for (std::size_t i = 0; i < t->ingress.size(); ++i) {
        const ingress_entry_t& e = t->ingress[i];
        if (e.label != label) continue;
        const std::size_t n = e.binding.local_route.n;
        // Report the size even when it does not fit, so the caller can tell "too long for my
        // buffer" (retry against a bigger one) from "no such binding" (nothing to report).
        if (n != 0 && n <= out.size()) std::memcpy(out.data(), e.binding.local_route.p, n);
        return n;
    }
    return 0;
}

void route_handle_t::cache_resolution(std::string_view in_link, std::uint16_t label,
                                      const resolved_binding_t& r) {
    const tables_view_t t = view_tables(in_link);
    if (!t) return;
    const std::lock_guard lock(t->m);
    for (std::size_t i = 0; i < t->ingress.size(); ++i) {
        ingress_entry_t& e = t->ingress[i];
        if (e.label != label) continue;
        e.binding.warm = true;
        e.binding.down_slot = r.down_slot;
        e.binding.target = r.target;
        e.binding.target_gen = r.target_gen;
        return;
    }
    // The binding went away between resolve and cache — nothing to record; the next frame
    // simply resolves again. Never an error: this is best-effort memoization, not state.
}

bool route_handle_t::record_egress(std::string_view out_link, std::uint16_t label,
                                   std::span<const std::byte> route) {
    const tables_ref_t t = tables(out_link);
    if (!t) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::lock_guard lock(t->m);
    for (std::size_t i = 0; i < t->egress.size(); ++i) {
        egress_entry_t& e = t->egress[i];
        if (e.label == label) {
            // `set_blob` allocates the replacement BEFORE freeing the old block, so a refusal
            // here leaves the entry aliasing the route it already advertised rather than a
            // truncated or empty one.
            if (!set_blob(*t, e.route, route)) {
                refused_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            return true;  // re-record in place — adds no entry
        }
    }
    if (max_bindings_ != 0 && t->egress.size() >= max_bindings_) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // `sole_take` keeps its `false` default here: this door records a label the caller already
    // holds by other means, so there is no `ensure_egress` take to hand back and
    // `release_egress` must never erase what it created (#833).
    egress_entry_t fresh{.label = label, .sole_take = false, .route = {}};
    if (!set_blob(*t, fresh.route, route) || !reserve_small(t->egress) ||
        !t->egress.push_back(fresh)) {
        free_blob(*t, fresh.route);
        refused_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

std::size_t route_handle_t::copy_egress_route(std::string_view out_link, std::uint16_t label,
                                              std::span<std::byte> out) const {
    const tables_view_t t = view_tables(out_link);
    if (!t) return 0;
    const std::lock_guard lock(t->m);
    for (std::size_t i = 0; i < t->egress.size(); ++i) {
        const egress_entry_t& e = t->egress[i];
        if (e.label != label) continue;
        // ALLOCATION-FREE copy-out (#603 defect 1, ADR-0065's direction). This runs on a
        // transport receive thread — `on_nack` reaches it from an inbound HANDLE_NACK — where
        // the previous owning form probed the GLOBAL heap through `detail::try_assign`: on
        // `-fno-exceptions` that frees the probe block and then runs a throwing `assign` on
        // the inference the block is still free, and a racer in that window aborts the node
        // inside a `noexcept` (#850, the #981 residual). There is no probe and no window left:
        // the bytes go straight into the caller's buffer under the table's own mutex.
        const std::size_t n = e.route.n;
        if (n != 0 && n <= out.size()) std::memcpy(out.data(), e.route.p, n);
        return n;
    }
    return 0;
}

std::pair<std::uint16_t, bool> route_handle_t::ensure_egress(std::string_view out_link,
                                                             std::span<const std::byte> route) {
    const tables_ref_t t = tables(out_link);
    if (!t) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return {0, false};
    }
    const std::lock_guard lock(t->m);
    // Reuse: the egress table doubles as the route -> label index (a link carries
    // few compact flows; a linear route compare beats a third keyed-by-bytes map).
    for (std::size_t i = 0; i < t->egress.size(); ++i) {
        egress_entry_t& e = t->egress[i];
        if (e.route.n != route.size() ||
            (route.size() != 0 && std::memcmp(e.route.p, route.data(), route.size()) != 0))
            continue;
        // A SECOND take of this label — from here on the mint is no longer alone on it, so
        // the minter may no longer hand it back (#833). Guarded by the load rather than
        // stored unconditionally: this is the per-delivery compact egress leg
        // (`deliver_remote`), and an unconditional store would dirty an entry line every
        // delivery that a shared read leaves alone. The store happens at most once per
        // entry, on the first reuse after its mint.
        if (e.sole_take) e.sole_take = false;
        return {e.label, false};  // already advertised on this link - reuse the label
    }
    // Saturate, never wrap (#603): a wrapped `next_label` handed out the reserved 0 and
    // then re-issued 1, 2, ... while those labels still aliased LIVE routes — a delivery
    // on the reused label resolves the wrong route, which is a misroute, not a drop.
    // Exhaustion returns `{0, false}`, records nothing, and leaves the caller to send the
    // full-route FWD (which carries its own route and needs no label).
    if (t->next_label == 0) return {0, false};
    // Table full (#603): refuse, same answer and same caller degrade as an exhausted label
    // space. Checked AFTER the reuse scan above, so an established flow is never refused —
    // only a new one, and only into the full-route form that always works.
    if (max_bindings_ != 0 && t->egress.size() >= max_bindings_) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return {0, false};
    }
    // The route copy is attempted BEFORE the label is spent, so a source refusal costs
    // nothing out of the saturating 16-bit space — it takes the same `{0, false}` degrade an
    // exhausted space and a full table already take.
    egress_entry_t fresh{.label = 0, .sole_take = false, .route = {}};
    if (!set_blob(*t, fresh.route, route)) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return {0, false};
    }
    const std::uint16_t label = t->next_label++;
    fresh.label = label;
    // A MINT is reclaimable: until some other caller takes this label, the minter may still
    // hand it back (@ref release_egress, #833).
    fresh.sole_take = true;
    if (!reserve_small(t->egress) || !t->egress.push_back(fresh)) {
        free_blob(*t, fresh.route);
        t->next_label = label;  // give the label straight back; nothing ever saw it
        refused_.fetch_add(1, std::memory_order_relaxed);
        return {0, false};
    }
    return {label, true};
}

void route_handle_t::release_egress(std::string_view out_link, std::uint16_t label,
                                    std::span<const std::byte> route) {
    if (label == 0) return;  // the reserved "none" — never an entry, never an allocation
    // `find_tables`, never `tables()`: a release must not CREATE a link shell. The refusal
    // this pairs with is often a downstream reconnect, whose `clear_link` erased the whole
    // table — re-creating it here would trade a stranded label for a stranded shell, which
    // is the growth #488 bounds.
    const tables_ref_t t = find_tables(out_link);
    if (!t) return;
    const std::lock_guard lock(t->m);
    for (std::size_t i = 0; i < t->egress.size(); ++i) {
        egress_entry_t& e = t->egress[i];
        if (e.label != label) continue;
        // Someone else took this label between the mint and this call — since #913 that is
        // the DESIGNED sharing (one label per (link, route), not per advertise), and their
        // binding now depends on the entry. Leave it. An established flow lands here too:
        // its take was a reuse, so its entry never carries the flag at all.
        if (!e.sole_take) return;
        // Identity, not just the label: `record_egress` can replace a route in place under
        // the same label, and an entry that no longer holds the route this caller minted for
        // is not the one it is handing back.
        if (e.route.n != route.size() ||
            (route.size() != 0 && std::memcmp(e.route.p, route.data(), route.size()) != 0))
            return;
        // Swap-erase: `block_array_t` has no `erase`, and none of the three scans over this
        // table is order-sensitive (they key on the label, or on the route bytes). The blob is
        // freed by hand first — the array runs no destructors, which is the price of holding
        // trivially-copyable entries and the reason every drop path goes through `free_blob`.
        free_blob(*t, e.route);
        e = t->egress[t->egress.size() - 1];
        t->egress.pop_back();
        // And the label itself, when it is still the allocator's most recent — the common
        // case, because a refusal follows its own mint. A concurrent mint on the same link
        // moved `next_label` past it, and then the label stays spent: reclaiming a hole
        // would need a free list, and the entry is gone either way. 65535 is deliberately
        // not walked back — `next_label` is 0 there, the saturated state @ref alloc_label
        // documents as permanent, and un-saturating it is the one direction that could
        // re-issue a live label.
        if (t->next_label != 0 &&
            static_cast<std::uint32_t>(t->next_label) == static_cast<std::uint32_t>(label) + 1u)
            t->next_label = label;
        return;
    }
}

std::uint16_t route_handle_t::alloc_label(std::string_view link) {
    const tables_ref_t t = tables(link);
    if (!t) return 0;  // the source could not serve this link's tables — "cannot compact"
    const std::lock_guard lock(t->m);
    // Saturating, monotonic, never 0 (see ensure_egress): 1..65535 then permanently
    // exhausted. Sticky by construction -- handing out 65535 leaves `next_label` at 0, and
    // this returns before incrementing, so the state cannot walk back into live labels.
    if (t->next_label == 0) return 0;
    return t->next_label++;
}

void route_handle_t::clear_link(std::string_view link) {
    // Self-heal: forget the link's ingress/egress bindings and its label allocator so a
    // post-reconnect delivery re-advertises from a clean slate.
    //
    // The registry entry is ERASED, not emptied-in-place. The tables node is REFCOUNTED
    // (#488): tables()/find_tables hand out a PINNING reference, so erasing the
    // entry here cannot free a link_tables_t a concurrent ensure_egress/bind_ingress is
    // mid-write on — the node is destroyed only when the last outstanding reference drops,
    // and its route bytes are freed with it. A writer that raced the erase keeps writing to
    // a now-detached table; those edits are correctly discarded, since the next tables()
    // mints a fresh entry — exactly the clean slate the self-heal wants. Erasing bounds
    // `links_` to LIVE link names instead of retaining an empty shell per departed name.
    const std::unique_lock lock(links_m_);
    // Advance the clear epoch BEFORE the erase, and under the same exclusive lock the erase,
    // the sweep and every table creation take (#827). That makes the whole of this call
    // indivisible against `bind_ingress_forward`: an in-flight advertise either binds entirely
    // before it (and the sweep below erases the binding) or reads an epoch that has already
    // moved (and is refused). There is no third interleaving, which is what the sweep alone
    // could not say — it can only erase bindings that already exist.
    //
    // Saturating, not wrapping (`saturating_next_generation`, RFC-0024 §4.4). A wrapped
    // counter would let a swap minted 2^32 clears ago compare equal to a live link and bind
    // against tables that died long before — the misroute the whole guard exists to refuse.
    clear_gen_ = graph::saturating_next_generation(clear_gen_);
    if (const std::size_t i = find_slot_locked(link); i != kNoSlot) {
        // Swap-erase and drop THIS registry's reference. A concurrent holder's pin keeps the
        // node alive; the last drop frees the node, its inline name and every blob it owns.
        link_tables_t* const dead = links_[i];
        links_[i] = links_[links_.size() - 1];
        links_.pop_back();
        release_tables(dead);
    }
    // The CROSS-LINK half (#716). Erasing `link`'s own tables is not the whole clean slate: a
    // forwarding binding is stored under the INBOUND link while `down_link` names the OUTBOUND
    // one, so a mid-chain node keeps an ingress binding on some OTHER link whose downstream
    // half crossed `link` — pointing at an out-label that died with the table just erased.
    // ADR-0062's erratum named this asymmetry and judged it harmless because the forward step
    // re-resolves by name; it is not, because the label it re-resolves toward is stale. The
    // shipped consequence: the upstream never saw the reconnect, so it never re-advertises and
    // keeps streaming COMPACTs; this node forwards the dead out-label; the downstream NACKs it;
    // `on_nack` looks the label up in the table `clear_link` erased and returns silently. The
    // flow drops every delivery, forever, and the origin is never told.
    //
    // Sweeping those bindings hands the recovery back to machinery that already ships: the
    // upstream's next COMPACT misses, draws the SAME HANDLE_NACK a stale label always drew,
    // and the upstream — whose own egress route is untouched — re-advertises. Every frame the
    // cascade emits is an already-specified frame in an already-specified situation, so no
    // wire surface moves.
    //
    // This is the COLD (re)connect path: O(links x bindings) once per link event, and nothing
    // on the per-delivery path changes. Taking each table's mutex under the registry lock is
    // the order `ingress_count`/`egress_count` already establish (registry then table), so it
    // introduces no new lock ordering; sweeping in place also keeps the teardown allocation-free.
    for (std::size_t li = 0; li < links_.size(); ++li) {
        link_tables_t* const t = links_[li];
        const std::lock_guard tl(t->m);
        // Swap-erase in place, walking backwards so a swapped-in entry is still examined.
        // `std::erase_if` cannot serve `block_array_t` (no iterator erase), and the blobs must
        // be freed by hand anyway — the array runs no destructors.
        std::size_t i = t->ingress.size();
        while (i-- != 0) {
            ingress_entry_t& e = t->ingress[i];
            // A terminus binding has no downstream half — its `down_link` is empty and it
            // survives, because nothing about it crossed the cleared link.
            const bool crosses =
                !e.binding.terminus && e.binding.down_link.n == link.size() &&
                (link.empty() || std::memcmp(e.binding.down_link.p, link.data(), link.size()) == 0);
            if (!crosses) continue;
            free_binding(*t, e.binding);
            e = t->ingress[t->ingress.size() - 1];
            t->ingress.pop_back();
        }
    }
}

std::size_t route_handle_t::ingress_count() const {
    const std::shared_lock lock(links_m_);
    std::size_t n = 0;
    for (std::size_t i = 0; i < links_.size(); ++i) {
        const std::lock_guard tl(links_[i]->m);
        n += links_[i]->ingress.size();
    }
    return n;
}

std::size_t route_handle_t::egress_count() const {
    const std::shared_lock lock(links_m_);
    std::size_t n = 0;
    for (std::size_t i = 0; i < links_.size(); ++i) {
        const std::lock_guard tl(links_[i]->m);
        n += links_[i]->egress.size();
    }
    return n;
}

std::size_t route_handle_t::link_count() const {
    const std::shared_lock lock(links_m_);
    return links_.size();
}

// --- transport-plane frame BUILDERS -------------------------------------------
//
// These three return owning byte vectors and therefore allocate through the throwing global
// heap. Since #885 NO production path calls them: every ADVERTISE / COMPACT / HANDLE_NACK the
// router puts on a link is scatter-gathered off a stack head (`fwd_router.cpp`'s
// `emit_advertise` / `emit_compact` / `emit_handle_nack`), which is what removed the last
// peer-provoked throwing allocation from the label plane. What survives here is the
// bytes-in-hand form: conformance vectors, the test suite's frame injection, and benches that
// need a frame as a value rather than as a send. Do not reach for them from a receive thread.

namespace {

/**
 * @brief A 2-byte little-endian VALUE TLV carrying a u16 label (the FIRST child of every route-
 *        handle frame).
 *
 * Opaque (opt.PL=0), 2-byte length: 6 bytes on the wire.
 */
void emit_label(std::vector<std::byte>& out, std::uint16_t label) {
    const std::array<std::byte, 6> tlv = label_tlv(label);
    out.insert(out.end(), tlv.begin(), tlv.end());
}

}  // namespace

std::array<std::byte, 6> label_tlv(std::uint16_t label) noexcept {
    // The one spelling of the label child's bytes: a 2-byte opaque VALUE. Written field by
    // field rather than as a literal so the length and the payload keep the wire's
    // little-endian order by construction. `emit_label` (and therefore every built encoder
    // below) goes through here, and so does every gather emitter in `fwd_router.cpp`, so a
    // gathered frame head and a built frame cannot disagree; `compact_cache_test` pins these
    // bytes against `wire::emit_tlv` independently, since a shared locus alone would let a
    // wrong layout pass a self-comparison.
    std::array<std::byte, 6> out{};
    out[0] = static_cast<std::byte>(std::to_underlying(type_t::VALUE));
    out[1] = static_cast<std::byte>(opt_t{}.encode());
    detail::store_le<std::uint16_t>(std::span<std::byte>(out).subspan(2, 2), 2);
    detail::store_le<std::uint16_t>(std::span<std::byte>(out).subspan(4, 2), label);
    return out;
}

std::vector<std::byte> encode_advertise(std::uint16_t label,
                                        std::span<const std::byte> route_path) {
    std::vector<std::byte> body;
    emit_label(body, label);
    body.insert(body.end(), route_path.begin(), route_path.end());
    std::vector<std::byte> out;
    wire::emit_tlv(out, type_t::ADVERTISE, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> encode_compact(std::uint16_t label, std::span<const std::byte> payload) {
    std::vector<std::byte> body;
    emit_label(body, label);
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::byte> out;
    wire::emit_tlv(out, type_t::COMPACT, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> encode_handle_nack(std::uint16_t label) {
    std::vector<std::byte> body;
    emit_label(body, label);
    std::vector<std::byte> out;
    wire::emit_tlv(out, type_t::HANDLE_NACK, opt_t{.pl = true}, body);
    return out;
}

}  // namespace tr::net
