/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/route_handle.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/vertex.hpp"

namespace tr::net {

using wire::opt_t;
using wire::type_t;

std::shared_ptr<route_handle_t::link_tables_t> route_handle_t::tables(std::string_view link) {
    {
        const std::shared_lock lock(links_m_);
        if (const auto it = links_.find(link); it != links_.end()) return it->second;
    }
    const std::unique_lock lock(links_m_);
    // Re-check: another thread may have created it between the two locks.
    if (const auto it = links_.find(link); it != links_.end()) return it->second;
    // The tables node is INDEPENDENTLY owned (allocate_shared draws it from mr_), not
    // stored inline in the map node: the returned shared_ptr copy pins it so clear_link
    // can erase the registry entry while a concurrent writer still holds this table (#488).
    auto sp = std::allocate_shared<link_tables_t>(
        std::pmr::polymorphic_allocator<link_tables_t>(mr_), mr_);
    // Stamp the tables with the clear epoch they were born at (#827), under the same
    // exclusive lock clear_link advances it under — once tables exist, a link's epoch moves
    // only when THAT link is cleared. Before they exist a sample reads the shared counter,
    // so an unrelated clear in the sample→creation window makes the bind refuse spuriously
    // (safe direction: the re-advertise after the stale-label NACK binds normally).
    sp->born_gen = clear_gen_;
    links_.emplace(std::pmr::string(link, mr_), sp);
    return sp;
}

std::uint32_t route_handle_t::link_epoch_locked(std::string_view link) const {
    const auto it = links_.find(link);
    return it == links_.end() ? clear_gen_ : it->second->born_gen;
}

std::uint32_t route_handle_t::link_epoch(std::string_view link) const {
    const std::shared_lock lock(links_m_);
    return link_epoch_locked(link);
}

std::shared_ptr<route_handle_t::link_tables_t> route_handle_t::find_tables(
    std::string_view link) const {
    const std::shared_lock lock(links_m_);
    const auto it = links_.find(link);
    return it == links_.end() ? nullptr : it->second;  // a pinning copy; per-link mutex inside
}

bool route_handle_t::bind_locked(link_tables_t& t, std::uint16_t label,
                                 handle_binding_t&& binding) {
    for (ingress_entry_t& e : t.ingress) {
        if (e.label == label) {
            e.binding = std::move(binding);
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
    if (max_bindings_ != 0 && t.ingress.size() >= max_bindings_) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    t.ingress.push_back(ingress_entry_t{.label = label, .binding = std::move(binding)});
    return true;
}

bool route_handle_t::bind_ingress(std::string_view in_link, std::uint16_t label,
                                  handle_binding_t binding) {
    const std::shared_ptr<link_tables_t> t = tables(in_link);
    const std::lock_guard lock(t->m);
    return bind_locked(*t, label, std::move(binding));
}

bool route_handle_t::bind_ingress_forward(std::string_view in_link, std::uint16_t label,
                                          handle_binding_t binding, std::uint32_t down_epoch) {
    // Create the inbound tables FIRST and outside the registry lock below: creating them
    // needs it exclusively, and a shared holder cannot upgrade. The pinning copy survives a
    // concurrent clear of `in_link` (#488) — a bind into a detached table is discarded, which
    // is the clean slate an inbound reconnect wants anyway.
    const std::shared_ptr<link_tables_t> t = tables(in_link);
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
    return bind_locked(*t, label, std::move(binding));
}

std::optional<handle_binding_t> route_handle_t::lookup_ingress(std::string_view in_link,
                                                               std::uint16_t label) const {
    const std::shared_ptr<link_tables_t> t = find_tables(in_link);
    if (!t) return std::nullopt;
    const std::lock_guard lock(t->m);
    for (const ingress_entry_t& e : t->ingress)
        if (e.label == label) return e.binding;
    return std::nullopt;
}

resolved_binding_t route_handle_t::resolved(std::string_view in_link, std::uint16_t label) const {
    resolved_binding_t out;
    const std::shared_ptr<link_tables_t> t = find_tables(in_link);
    if (!t) return out;
    const std::lock_guard lock(t->m);
    for (const ingress_entry_t& e : t->ingress) {
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

void route_handle_t::cache_resolution(std::string_view in_link, std::uint16_t label,
                                      const resolved_binding_t& r) {
    const std::shared_ptr<link_tables_t> t = find_tables(in_link);
    if (!t) return;
    const std::lock_guard lock(t->m);
    for (ingress_entry_t& e : t->ingress) {
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
                                   std::vector<std::byte> route) {
    const std::shared_ptr<link_tables_t> t = tables(out_link);
    const std::lock_guard lock(t->m);
    for (egress_entry_t& e : t->egress) {
        if (e.label == label) {
            e.route.assign(route.begin(), route.end());
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
    t->egress.push_back(egress_entry_t{
        .label = label, .route = std::pmr::vector<std::byte>(route.begin(), route.end(), mr_)});
    return true;
}

std::optional<std::vector<std::byte>> route_handle_t::egress_route(std::string_view out_link,
                                                                   std::uint16_t label) const {
    const std::shared_ptr<link_tables_t> t = find_tables(out_link);
    if (!t) return std::nullopt;
    const std::lock_guard lock(t->m);
    for (const egress_entry_t& e : t->egress) {
        if (e.label != label) continue;
        // NOTHROW copy-out (#603 defect 1, ADR-0065's direction). This ran on a transport
        // receive thread — `on_nack` reaches it from an inbound HANDLE_NACK — where a
        // throwing allocation is an abort() under the shipping `-fno-exceptions` profile.
        // Exhaustion now takes the SAME `nullopt` the "no route bound for this label" case
        // already takes, so no caller learns a new shape: `on_nack` returns silently either
        // way, and a peer that NACKs simply gets no re-advertise, exactly as when the label
        // was never bound.
        //
        // This is the nothrow half only. The seam half — drawing from an injected
        // `block_source_t` rather than probing the global heap — still owes ADR-0065, and
        // cannot be done here without an API change: `link_tables_t` holds `std::pmr::vector`s
        // and `detail::try_*` is typed to plain `std::vector`, which is the structural reason
        // this file is half-migrated.
        std::vector<std::byte> out;
        if (!detail::try_assign(out, std::span<const std::byte>(e.route))) return std::nullopt;
        return out;
    }
    return std::nullopt;
}

std::pair<std::uint16_t, bool> route_handle_t::ensure_egress(std::string_view out_link,
                                                             std::span<const std::byte> route) {
    const std::shared_ptr<link_tables_t> t = tables(out_link);
    const std::lock_guard lock(t->m);
    // Reuse: the egress table doubles as the route -> label index (a link carries
    // few compact flows; a linear route compare beats a third keyed-by-bytes map).
    for (egress_entry_t& e : t->egress) {
        if (e.route.size() != route.size() ||
            !std::equal(e.route.begin(), e.route.end(), route.begin()))
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
    const std::uint16_t label = t->next_label++;
    t->egress.push_back(egress_entry_t{
        .label = label, .route = std::pmr::vector<std::byte>(route.begin(), route.end(), mr_)});
    // Stamped after the insert, not inside the brace, so the initializer above stays the one
    // four documents cite. A MINT is reclaimable: until some other caller takes this label,
    // the minter may still hand it back (@ref release_egress, #833).
    t->egress.back().sole_take = true;
    return {label, true};
}

void route_handle_t::release_egress(std::string_view out_link, std::uint16_t label,
                                    std::span<const std::byte> route) {
    if (label == 0) return;  // the reserved "none" — never an entry, never an allocation
    // `find_tables`, never `tables()`: a release must not CREATE a link shell. The refusal
    // this pairs with is often a downstream reconnect, whose `clear_link` erased the whole
    // table — re-creating it here would trade a stranded label for a stranded shell, which
    // is the growth #488 bounds.
    const std::shared_ptr<link_tables_t> t = find_tables(out_link);
    if (!t) return;
    const std::lock_guard lock(t->m);
    for (auto it = t->egress.begin(); it != t->egress.end(); ++it) {
        if (it->label != label) continue;
        // Someone else took this label between the mint and this call — since #913 that is
        // the DESIGNED sharing (one label per (link, route), not per advertise), and their
        // binding now depends on the entry. Leave it. An established flow lands here too:
        // its take was a reuse, so its entry never carries the flag at all.
        if (!it->sole_take) return;
        // Identity, not just the label: `record_egress` can replace a route in place under
        // the same label, and an entry that no longer holds the route this caller minted for
        // is not the one it is handing back.
        if (it->route.size() != route.size() ||
            !std::equal(it->route.begin(), it->route.end(), route.begin()))
            return;
        t->egress.erase(it);
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
    const std::shared_ptr<link_tables_t> t = tables(link);
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
    // The registry entry is ERASED, not emptied-in-place. The tables node is owned by a
    // shared_ptr (#488): tables()/find_tables hand out a PINNING copy, so erasing the
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
    if (const auto it = links_.find(link); it != links_.end()) links_.erase(it);
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
    for (const auto& [name, t] : links_) {
        const std::lock_guard tl(t->m);
        std::erase_if(t->ingress, [link](const ingress_entry_t& e) {
            // A terminus binding has no downstream half — its `down_link` is empty and it
            // survives, because nothing about it crossed the cleared link.
            return !e.binding.terminus && e.binding.down_link == link;
        });
    }
}

std::size_t route_handle_t::ingress_count() const {
    const std::shared_lock lock(links_m_);
    std::size_t n = 0;
    for (const auto& [name, t] : links_) {
        const std::lock_guard tl(t->m);
        n += t->ingress.size();
    }
    return n;
}

std::size_t route_handle_t::egress_count() const {
    const std::shared_lock lock(links_m_);
    std::size_t n = 0;
    for (const auto& [name, t] : links_) {
        const std::lock_guard tl(t->m);
        n += t->egress.size();
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
