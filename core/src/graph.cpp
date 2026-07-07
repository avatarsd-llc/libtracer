/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/graph.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/key_view.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_arena.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/view.hpp"

namespace tr::graph {

using wire::encode;
using wire::key_view_t;
using wire::opt_t;
using wire::tlv_t;
using wire::type_t;
using wire::view_as_tlv;
namespace {

// Emit a VALUE TLV holding a `width`-byte little-endian integer — the one bespoke
// emitter for building a :schema POINT; NAME/SETTINGS/POINT use wire::emit_*.
void emit_value(std::vector<std::byte>& out, std::uint64_t value, int width) {
    std::vector<std::byte> payload(static_cast<std::size_t>(width));
    detail::store_le(payload, value, static_cast<std::size_t>(width));
    wire::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
}

// Canonical-key NAME navigation (last segment, parent, ancestor/child, level
// split) lives in one locus: tr::wire::key_view_t (key_view.hpp).

// Absolute wall-clock ns since the UNIX epoch — the ACE `expires_ns` reference clock.
[[nodiscard]] std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

// True iff `ace` grants `bit` to `subject` right now: not expired, mask carries the
// bit, and the subject matches byte-for-byte (or the ACE names "EVERYONE@").
[[nodiscard]] bool ace_grants(const ace_t& ace, std::span<const std::byte> subject,
                              std::uint32_t bit, std::uint64_t now) {
    if ((ace.access_mask & bit) == 0) return false;
    if (ace.expires_ns != 0 && ace.expires_ns <= now) return false;
    static constexpr std::string_view kEveryone = "EVERYONE@";
    if (detail::as_string_view(ace.subject) == kEveryone) return true;
    return std::ranges::equal(ace.subject, subject);
}

// Parse a decoded :acl ACL TLV into the core-subset ACE list (#81, ADR-0020 /
// docs/reference/05 §0x0A). STRICT: anything beyond the core subset — a DENY ACE,
// flag bits beyond INHERIT, a missing type/subject/access_mask, a non-ACL child —
// is rejected with TYPE_MISMATCH at write time, so the stored ACEs never carry
// semantics the ALLOW-only evaluator would silently weaken (full DENY / ordered
// first-match-per-bit evaluation is the security_acl host module).
[[nodiscard]] result_t<std::vector<ace_t>> parse_aces(const tlv_t& acl) {
    std::vector<ace_t> out;
    out.reserve(acl.children.size());
    for (const tlv_t& entry : acl.children) {
        if (entry.type != type_t::ACL || !entry.opt.pl)
            return std::unexpected(status_t::TYPE_MISMATCH);
        ace_t ace;
        bool has_type = false;
        bool has_subject = false;
        bool has_mask = false;
        const std::vector<tlv_t>& ch = entry.children;
        for (std::size_t i = 0; i + 1 < ch.size(); ++i) {
            if (ch[i].type != type_t::NAME) continue;
            const std::string_view key = detail::as_string_view(ch[i].payload);
            const tlv_t& val = ch[i + 1];
            if (key == "type" && val.type == type_t::VALUE) {
                // ALLOW=0 only; DENY (or any unknown type) is beyond the core subset.
                if (detail::load_le<std::uint8_t>(val.payload) != 0)
                    return std::unexpected(status_t::TYPE_MISMATCH);
                has_type = true;
            } else if (key == "flags" && val.type == type_t::VALUE) {
                ace.flags = detail::load_le<std::uint8_t>(val.payload);
                // Single INHERIT bit only: INHERIT_ONLY/NO_PROPAGATE/GROUP would be
                // silently mis-evaluated by the subset, so reject rather than weaken.
                if ((ace.flags & static_cast<std::uint8_t>(~kAceInherit)) != 0)
                    return std::unexpected(status_t::TYPE_MISMATCH);
            } else if (key == "subject") {
                // The subject token is opaque bytes (ADR-0018) — accept any opaque
                // TLV's payload (VALUE recommended; NAME for "OWNER@"/"EVERYONE@").
                ace.subject.assign(val.payload.begin(), val.payload.end());
                has_subject = ace.subject.size() > 0;
            } else if (key == "access_mask" && val.type == type_t::VALUE) {
                ace.access_mask = static_cast<std::uint32_t>(detail::load_le(val.payload));
                has_mask = true;
            } else if (key == "expires_ns" && val.type == type_t::VALUE) {
                ace.expires_ns = detail::load_le<std::uint64_t>(val.payload);
            }
        }
        if (!has_type || !has_subject || !has_mask) return std::unexpected(status_t::TYPE_MISMATCH);
        out.push_back(std::move(ace));
    }
    return out;
}

// True iff the node's opt byte carries no trailer bits. A branch write (RFC-0005)
// stores refcount subviews of the written frame, so a trailer inside the tree
// cannot be sliced off without a copy — trailer-carrying nodes are rejected
// (TYPE_MISMATCH), keeping stored values trailer-less at rest (ADR-0041 §4).
[[nodiscard]] bool trailer_less(const wire::arena_tlv_t& node) noexcept {
    const opt_t& o = node.opt;
    return !o.ts && !o.cr && !o.cw && !o.tf;
}

// The subview of `frame_view` covering `span` — a refcount bump on the written
// frame's segment, never a byte copy (RFC-0005 §decomposition). Precondition:
// `span` points into `frame_view.bytes()` (it is an arena span over that frame).
[[nodiscard]] view_t slice_of(const view_t& frame_view, std::span<const std::byte> span) {
    const std::size_t off = static_cast<std::size_t>(span.data() - frame_view.bytes().data());
    return frame_view.subview(off, span.size());
}

// One landing site of a branch write (RFC-0005): the vertex key, the VALUE slice
// that lands there (empty when the node carries no value of its own), and the
// slice this vertex's subscribers are notified with (the VALUE for a leaf node,
// the node's whole POINT subtree for an interior node — the smallest subview
// covering every write at-or-below the subscription point).
struct branch_node_t {
    std::vector<std::byte> key;
    view_t store{};
    view_t notify{};
    bool subtree_has_value = false;
};

// Parse one POINT node of a branch write into `out` (post-order; children precede
// their parent). `key` is this node's canonical vertex key — the caller already
// folded the node's leading NAME into it. STRICT (like parse_aces): children are
// the leading NAME, at most one VALUE (the node's own value), and POINT
// sub-branches; anything else — or any trailer-carrying node — is TYPE_MISMATCH,
// so a stored slice never carries semantics the decomposition would silently
// mangle. Returns whether a VALUE lands at-or-below this node. Recursion depth is
// bounded by the codec's kMaxDepth (32) — the arena would not have decoded deeper.
[[nodiscard]] result_t<bool> parse_branch_node(const wire::tlv_arena_t& a, std::uint32_t node,
                                               const view_t& frame_view, std::vector<std::byte> key,
                                               std::vector<branch_node_t>& out) {
    if (!a[node].opt.pl || !trailer_less(a[node])) return std::unexpected(status_t::TYPE_MISMATCH);
    const std::uint32_t end = a[node].end;
    std::uint32_t i = wire::tlv_arena_t::first_child(node);
    if (i >= end || a[i].type != type_t::NAME) return std::unexpected(status_t::TYPE_MISMATCH);
    view_t store{};
    bool has_value = false;
    bool has_point_child = false;
    bool subtree_value = false;
    for (i = a.next_sibling(i); i < end; i = a.next_sibling(i)) {
        const wire::arena_tlv_t& c = a[i];
        if (c.type == type_t::VALUE) {
            if (has_value || !trailer_less(c)) return std::unexpected(status_t::TYPE_MISMATCH);
            has_value = true;
            store = slice_of(frame_view, c.wire);
        } else if (c.type == type_t::POINT) {
            has_point_child = true;
            const std::uint32_t cn = wire::tlv_arena_t::first_child(i);
            if (cn >= c.end || a[cn].type != type_t::NAME)
                return std::unexpected(status_t::TYPE_MISMATCH);
            std::vector<std::byte> child_key = key;
            wire::emit_name(child_key, a[cn].body);
            const result_t<bool> sub =
                parse_branch_node(a, i, frame_view, std::move(child_key), out);
            if (!sub) return std::unexpected(sub.error());
            subtree_value = subtree_value || *sub;
        } else {
            return std::unexpected(status_t::TYPE_MISMATCH);
        }
    }
    subtree_value = subtree_value || has_value;
    branch_node_t bn;
    bn.notify = has_point_child ? slice_of(frame_view, a[node].wire) : store;
    bn.store = std::move(store);
    bn.subtree_has_value = subtree_value;
    bn.key = std::move(key);
    out.push_back(std::move(bn));
    return subtree_value;
}

}  // namespace

graph_t::graph_t() {
    // The one built-in creation-catalog type (#82, ADR-0017): `stored_value` makes a
    // plain last-writer-wins vertex at the composed child key. Its optional SPEC
    // `config` SETTINGS is ignored for now (a stored-value has no instantiation params
    // beyond the standard `:settings` field, written separately). Devices add richer
    // types (controllers, transport connections — #83) via register_child_type.
    register_child_type(
        "stored_value",
        [](graph_t& g, std::vector<std::byte> child_key, const tlv_t*) -> result_t<vertex_t*> {
            return g.register_vertex_key(std::move(child_key), role_t::STORED_VALUE);
        });
}

void graph_t::register_child_type(std::string type, child_factory_t factory) {
    child_types_.insert_or_assign(std::move(type), std::move(factory));
}

result_t<vertex_t*> graph_t::register_vertex(const path_t& path, role_t role, handlers_t handlers,
                                             settings_t settings) {
    return register_vertex_key(std::vector<std::byte>(path.key().begin(), path.key().end()), role,
                               std::move(handlers), settings);
}

result_t<vertex_t*> graph_t::register_vertex_key(std::vector<std::byte> key, role_t role,
                                                 handlers_t handlers, settings_t settings) {
    path_key_t k{std::move(key)};
    const std::unique_lock lock(map_mutex_);
    if (vertices_.find(k) != vertices_.end()) return std::unexpected(status_t::PATH_IN_USE);
    auto vertex = std::make_unique<vertex_t>(role, k, settings, std::move(handlers));
    // Subtree-subscription init (RFC-0005): a vertex born under a subscribed
    // ancestor starts with the ancestor-listener count already summed — O(depth)
    // lookups here (under the same unique lock note_subscriber_* excludes, so the
    // sum and a concurrent subscribe walk never double-count) keep the write
    // path's is-anyone-listening check a single relaxed load.
    std::uint32_t above = 0;
    key_view_t kk{k.bytes};
    while (!kk.empty()) {
        kk = kk.parent();
        const auto pit = vertices_.find(
            path_key_t{std::vector<std::byte>(kk.bytes().begin(), kk.bytes().end())});
        if (pit != vertices_.end()) above += pit->second->own_subs_.load(std::memory_order_relaxed);
        if (kk.empty()) break;
    }
    vertex->listeners_above_.store(above, std::memory_order_relaxed);
    vertex_t* ptr = vertex.get();
    vertices_.emplace(std::move(k), std::move(vertex));
    return ptr;
}

result_t<vertex_t*> graph_t::ensure_vertex(std::span<const std::byte> key,
                                           std::string_view caller) {
    if (vertex_t* v = find(key)) return v;
    // Write-creates (RFC-0005): gate CREATE on the nearest EXISTING ancestor — its
    // effective ACL is exactly what every vertex of the missing chain would inherit
    // (the core subset's INHERIT walk, ADR-0020). No ancestor at all ⇒ open, the
    // ACL-presence opt-in of docs/reference/05 §0x0A.
    {
        key_view_t k{key};
        vertex_t* ancestor = nullptr;
        while (!k.empty()) {
            k = k.parent();
            ancestor = find(k.bytes());
            if (ancestor != nullptr || k.empty()) break;
        }
        if (ancestor != nullptr && !acl_allows(ancestor, caller, acl_right_t::CREATE))
            return std::unexpected(status_t::PERMISSION_DENIED);
    }
    // Validate the key's NAME-encoding framing and collect the per-level prefixes,
    // then create every missing level shallowest-first (`mkdir -p`).
    std::vector<key_view_t> levels;
    if (!key_view_t{key}.split_levels(levels)) return std::unexpected(status_t::INVALID_PATH);
    vertex_t* leaf = nullptr;
    for (const key_view_t level : levels) {
        const std::span<const std::byte> pk = level.bytes();
        if (vertex_t* existing = find(pk)) {
            leaf = existing;
            continue;
        }
        result_t<vertex_t*> made =
            register_vertex_key(std::vector<std::byte>(pk.begin(), pk.end()), role_t::STORED_VALUE);
        if (made) {
            leaf = *made;
            continue;
        }
        if (made.error() == status_t::PATH_IN_USE) {  // lost a benign creation race
            leaf = find(pk);
            if (leaf != nullptr) continue;
        }
        return std::unexpected(made.error());
    }
    return leaf;  // never null: the deepest level was just found or created
}

std::uint64_t graph_t::ancestor_walks() const noexcept {
    return ancestor_walks_.load(std::memory_order_relaxed);
}

void graph_t::note_subscriber_added(vertex_t* v) {
    // Shared map lock: excludes concurrent vertex creation (unique lock), so a
    // newborn either sees the bumped own_subs_ in its creation-time sum or is
    // already enumerable by this walk — never both. Counters are atomics; a
    // byte-prefix of concatenated NAME encodings can only match on a segment
    // boundary (the length header differs otherwise), so prefix ⇒ descendant.
    const std::shared_lock lock(map_mutex_);
    v->own_subs_.fetch_add(1, std::memory_order_relaxed);
    const key_view_t prefix{v->key().bytes};
    for (const auto& [key, vert] : vertices_) {
        if (prefix.is_ancestor_of(key_view_t{key.bytes}))
            vert->listeners_above_.fetch_add(1, std::memory_order_relaxed);
    }
}

void graph_t::note_subscriber_removed(vertex_t* v) {
    const std::shared_lock lock(map_mutex_);
    v->own_subs_.fetch_sub(1, std::memory_order_relaxed);
    const key_view_t prefix{v->key().bytes};
    for (const auto& [key, vert] : vertices_) {
        if (prefix.is_ancestor_of(key_view_t{key.bytes}))
            vert->listeners_above_.fetch_sub(1, std::memory_order_relaxed);
    }
}

vertex_t* graph_t::find(std::span<const std::byte> key) const {
    path_key_t k{std::vector<std::byte>(key.begin(), key.end())};
    const std::shared_lock lock(map_mutex_);
    const auto it = vertices_.find(k);
    // The returned raw pointer is used by callers OUTSIDE this shared_lock. That is
    // sound only because `vertices_` is insert-only (see its declaration): the
    // heap-owned vertex_t is pointer-stable across rehash and is never destroyed
    // while the graph lives. Do NOT add vertex erasure/retirement without first
    // giving vertices a lifetime scheme — a bare erase would dangle these pointers.
    return it == vertices_.end() ? nullptr : it->second.get();
}

void graph_t::set_subject_resolver(subject_resolver_t resolver) {
    subject_resolver_ = std::move(resolver);
}

bool graph_t::acl_allows(vertex_t* v, std::string_view caller, acl_right_t right) const {
    if (!subject_resolver_) return true;  // enforcement disabled — the one hot-path check
    const std::optional<subject_token_t> subject = subject_resolver_(caller);
    if (!subject) return true;  // trusted caller (no subject) — e.g. a local API call
    const auto bit = static_cast<std::uint32_t>(right);
    const std::uint64_t now = now_ns();
    // Effective ACL = own ACEs + INHERIT-flagged ancestor ACEs (ADR-0020), computed at
    // check time (control-plane frequency, no caching). Locks are taken one vertex at
    // a time — own first, then each ancestor — never nested, so no ordering hazard.
    bool effective_nonempty = false;
    {
        const std::lock_guard lock(v->m_);
        for (const ace_t& ace : v->aces_) {
            effective_nonempty = true;
            if (ace_grants(ace, *subject, bit, now)) return true;
        }
    }
    key_view_t key{v->key_.bytes};
    while (!(key = key.parent()).empty()) {
        vertex_t* ancestor = find(key.bytes());
        if (!ancestor) continue;  // an unregistered intermediate level holds no ACL
        const std::lock_guard lock(ancestor->m_);
        for (const ace_t& ace : ancestor->aces_) {
            if ((ace.flags & kAceInherit) == 0) continue;  // non-INHERIT: that vertex only
            effective_nonempty = true;
            if (ace_grants(ace, *subject, bit, now)) return true;
        }
    }
    // Open by default: no effective ACE at all => allowed (enforcement is opt-in via
    // ACL presence). Any present ACE — even an expired one — closes the vertex.
    return !effective_nonempty;
}

result_t<rope_t> graph_t::read(vertex_t* v, std::string_view caller) const {
    if (!acl_allows(v, caller, acl_right_t::READ))
        return std::unexpected(status_t::PERMISSION_DENIED);
    if (v->role_ == role_t::HANDLER) {
        if (v->handlers_.on_read) return v->handlers_.on_read();
        return std::unexpected(status_t::NOT_FOUND);
    }
    const std::shared_ptr<const rope_t> sp = v->lkv_.load();  // lock-free
    if (!sp) return std::unexpected(status_t::NOT_FOUND);
    return *sp;  // copies the rope => clones each link's segment_ptr_t (refcount bump, no byte
                 // copy)
}

void graph_t::fan_out(vertex_t* v, const rope_t& value, int depth) {
    // Snapshot every active edge UNDER the lock, then dispatch OUTSIDE it (callbacks /
    // re-dispatch may re-enter the graph). Delivery is value-agnostic — no per-subscriber
    // comparison — so every active edge receives `value`; WHICH vertices propagate is the
    // per-vertex delivery_mode decided by the sweep (RFC-0008). The snapshot carries only
    // the dispatch-relevant fields; small fan-out (the common case) uses a stack buffer —
    // no per-publish heap allocation — and large fan-out reserves the heap vector once.
    struct disp_t {
        std::function<void(const rope_t&)> callback;
        std::vector<std::byte> target_key;
        // Remote fan-out (#136): a non-empty `link` ⇒ hand this delivery to remote_sink_.
        // Copied under the lock (like target_key) so the slot may be cleared concurrently
        // once we dispatch outside it; an in-process slot leaves both empty.
        std::string link;
        // A refcount clone of the stored route segment (ADR-0041 §2) — snapshotting
        // under the lock is a refcount bump, not a byte copy, and the clone keeps
        // the route alive across a concurrent unsubscribe while we dispatch.
        view_t return_route;
        bool delivery_compact = false;
        // The edge's stored ACL caller context (#81): the target re-dispatch runs the
        // fan-in WRITE gate under the subscription creator's identity, not the
        // producer-side writer's. Copied under the lock, like target_key.
        std::string caller;
    };
    constexpr std::size_t kInlineFanout = 8;
    std::array<disp_t, kInlineFanout> inline_buf;
    std::vector<disp_t> heap_buf;
    std::size_t inline_n = 0;
    bool use_heap = false;
    {
        const std::lock_guard lock(v->m_);
        if (v->subs_.size() > kInlineFanout) {
            use_heap = true;
            heap_buf.reserve(v->subs_.size());
        }
        for (const subscriber_t& s : v->subs_) {
            if (!s.active) continue;
            disp_t d{s.callback,     s.target_key,       s.link,
                     s.return_route, s.delivery_compact, s.caller};
            if (use_heap)
                heap_buf.push_back(std::move(d));
            else
                inline_buf[inline_n++] = std::move(d);
        }
    }
    const auto dispatch = [&](const disp_t& d) {
        if (d.callback)
            d.callback(value);  // the rope value by const ref (callback may clone links)
        if (!d.target_key.empty() && depth + 1 < kMaxDispatchDepth) {
            if (vertex_t* target = find(d.target_key)) {
                // Fan-in gate (#81, ADR-0026): the re-dispatch is an ordinary write to
                // the target, gated inside write_impl by the TARGET's :acl WRITE right
                // under the edge's stored caller context. Denial drops this delivery.
                (void)write_impl(target, value, depth + 1, d.caller);  // value cloned
            }
        }
        // Remote delivery (#136): a write fans out to a remote subscriber as a
        // FWD{WRITE} (or auto-promoted COMPACT) via the injected sink — outside the
        // vertex lock, like every other dispatch leg, since the sink does transport I/O.
        if (!d.link.empty() && remote_sink_) {
            remote_sink_(remote_delivery_t{.link = d.link,
                                           .return_route = d.return_route,
                                           .delivery_compact = d.delivery_compact},
                         value);
        }
    };
    if (use_heap)
        for (const disp_t& d : heap_buf) dispatch(d);
    else
        for (std::size_t i = 0; i < inline_n; ++i) dispatch(inline_buf[i]);
}

result_t<void> graph_t::store_value(vertex_t* v, rope_t value) {
    if (v->role_ == role_t::HANDLER) {
        if (!v->handlers_.on_write) return std::unexpected(status_t::NOT_FOUND);
        result_t<void> r = v->handlers_.on_write(value);
        if (!r) return r;
        const std::lock_guard lock(v->m_);
        ++v->write_seq_;
        v->cv_.notify_all();
        return {};
    }

    // One allocation (make_shared): the rope's inline small-buffer holds the single-link
    // trivial case, so a scalar write costs exactly what the view_t slot cost (ADR-0053 §6).
    auto sp = std::make_shared<const rope_t>(std::move(value));
    v->lkv_.store(sp);  // lock-free publish of the new last-known-value

    const std::lock_guard lock(v->m_);
    if (v->role_ == role_t::STREAM) {
        v->history_.push_back(std::move(sp));
        const std::size_t keep =
            v->settings_.history_keep_last ? v->settings_.history_keep_last : 1;
        while (v->history_.size() > keep) v->history_.pop_front();
    }
    ++v->write_seq_;
    v->cv_.notify_all();
    return {};
}

void graph_t::bubble_up(vertex_t* v, const rope_t& value, int depth) {
    // Entered only when v->listeners_above_ says an ancestor subscriber exists —
    // the idle write path never walks (RFC-0005 §near-free-when-idle; the counter
    // below is what tests/benches assert on via ancestor_walks()).
    ancestor_walks_.fetch_add(1, std::memory_order_relaxed);
    key_view_t key{v->key_.bytes};
    while (!key.empty()) {
        key = key.parent();
        if (vertex_t* ancestor = find(key.bytes())) fan_out(ancestor, value, depth);
        if (key.empty()) break;  // the root vertex (empty key) was just visited
    }
}

namespace {
// A branch write: a POINT payload (type 0x07, opt.PL=1) written to a value vertex
// decomposes across descendants (RFC-0005 §decomposition); anything else — VALUE,
// user-range records, other structured TLVs — stores as-is. The header sits at the
// start of the first link (a decomposable POINT is contiguous); a device-memory link
// is never dereferenced (and never decomposes).
[[nodiscard]] bool is_branch_point(const rope_t& value, role_t role) {
    if (role == role_t::HANDLER || value.link_count() < 1 || !value.links()[0].is_host())
        return false;
    const std::span<const std::byte> head = value.links()[0].bytes();
    return head.size() >= 4 &&
           std::to_integer<std::uint8_t>(head[0]) == std::to_underlying(type_t::POINT) &&
           (std::to_integer<std::uint8_t>(head[1]) & 0x40) != 0;
}
}  // namespace

result_t<void> graph_t::write_impl(vertex_t* v, rope_t value, int depth, std::string_view caller) {
    if (!acl_allows(v, caller, acl_right_t::WRITE))
        return std::unexpected(status_t::PERMISSION_DENIED);
    // `write` is the RFC-0008 §D composition — assign the vertex, then deliver exactly
    // what it stored (a leaf VALUE, or each landed descendant of a branch POINT). This is
    // the FWD{WRITE}-terminus behavior: a TARGETED delivery of the written vertex(es), not
    // a subtree sweep. propagate(v) is the separate accumulate-then-flush primitive.
    if (is_branch_point(value, v->role_))
        return write_branch(v, value, depth, caller, /*notify=*/true);
    const rope_t notify = value;  // refcount clone — store_value consumes `value`
    result_t<void> stored = store_value(v, std::move(value));
    if (!stored) return stored;
    if (v->role_ == role_t::STREAM) {
        // Deliver the just-appended ring entry and advance the drain cursor, so a later
        // propagate on this stream does not re-deliver it (RFC-0008 §E).
        deliver_current(v, depth);
    } else {
        deliver_vertex(v, notify, depth);
    }
    clear_pending(v);  // eager delivery flushes any pending mark a prior assign left
    return {};
}

result_t<void> graph_t::assign(vertex_t* v, rope_t value, std::string_view caller) {
    if (!acl_allows(v, caller, acl_right_t::WRITE))
        return std::unexpected(status_t::PERMISSION_DENIED);
    // The STATE half only (RFC-0008 §A): swap the last-known-value / append the stream
    // ring / bump the write sequence (waking await), then mark v for the next covering
    // sweep. A branch POINT assigns each descendant the same way. Sends nothing.
    if (is_branch_point(value, v->role_))
        return write_branch(v, value, 0, caller, /*notify=*/false);
    result_t<void> stored = store_value(v, std::move(value));
    if (!stored) return stored;
    mark_pending(v);
    return {};
}

result_t<void> graph_t::write_branch(vertex_t* v, const rope_t& value, int depth,
                                     std::string_view caller, bool notify) {
    // A decomposable POINT is contiguous, so decode reads the materialized head:
    // single-link (the ④a case — ingress values are single-link until ④b), that is
    // the sole link with zero copy; a multi-link POINT pays one flatten here (the
    // interim until the ④b rope-cursor decode). Every node span points into `head`,
    // so each landed slice is head.subview(...) — a refcount bump, never a byte copy.
    const view_t head = value.materialize();
    std::array<std::byte, 4096> stack;
    std::pmr::monotonic_buffer_resource mr(stack.data(), stack.size());
    const std::expected<wire::tlv_arena_t, wire::err_t> arena = wire::decode_into(head.bytes(), mr);
    if (!arena) return std::unexpected(status_t::TYPE_MISMATCH);
    const wire::tlv_arena_t& a = *arena;

    // The root POINT's leading NAME must name this vertex (the written tree is
    // rooted AT `v`); a mismatch is an addressing error, not a shape error.
    const std::uint32_t n0 = wire::tlv_arena_t::first_child(0);
    if (n0 >= a.root().end || a[n0].type != type_t::NAME)
        return std::unexpected(status_t::TYPE_MISMATCH);
    if (!std::ranges::equal(a[n0].body, key_view_t{v->key_.bytes}.last_segment()))
        return std::unexpected(status_t::INVALID_PATH);

    std::vector<branch_node_t> plan;  // post-order; plan.back() is the root
    const result_t<bool> parsed =
        parse_branch_node(a, 0, head, std::vector<std::byte>(v->key_.bytes), plan);
    if (!parsed) return std::unexpected(parsed.error());
    if (!*parsed) return {};  // a value-free branch is a no-op write

    // Admission: resolve-or-create every landing vertex (write-creates, CREATE-
    // gated) and gate WRITE on each BEFORE any store, so a denial rejects the
    // whole branch with nothing landed. (Created-but-empty intermediates may
    // persist past a later denial — the `mkdir -p` analogy; RFC-0005 §ACL.)
    struct site_t {
        vertex_t* vx;
        const branch_node_t* node;
    };
    std::vector<site_t> sites;
    sites.reserve(plan.size());
    for (const branch_node_t& node : plan) {
        if (node.store.empty()) continue;
        vertex_t* vx = nullptr;
        if (std::ranges::equal(node.key, v->key_.bytes)) {
            vx = v;  // the root value — `v` itself, already WRITE-gated by write_impl
        } else {
            const result_t<vertex_t*> ensured = ensure_vertex(node.key, caller);
            if (!ensured) return std::unexpected(ensured.error());
            vx = *ensured;
            if (!acl_allows(vx, caller, acl_right_t::WRITE))
                return std::unexpected(status_t::PERMISSION_DENIED);
        }
        sites.push_back(site_t{vx, &node});
    }

    // Apply: land every slice. Admission was atomic; application is per-vertex and
    // best-effort (a handler-role landing site may refuse its slice without
    // un-landing the others) — the branch is NOT a transaction (RFC-0005
    // §atomicity non-promise; each leaf is its own consistent refcounted snapshot).
    for (const site_t& site : sites) (void)store_value(site.vx, site.node->store);

    if (!notify) {
        // The assign half (RFC-0008 §B branch-assign): mark each landed vertex for the
        // next covering propagate sweep; deliver nothing, bubble nothing.
        for (const site_t& site : sites) mark_pending(site.vx);
        return {};
    }

    // Notify: one delivery per covered subscription point, with its slice — the
    // VALUE for a leaf landing site, the node's POINT subtree for an interior
    // node, and the whole written TLV as-is at the root and (via bubbling) above.
    for (const branch_node_t& node : plan) {
        const bool is_root = &node == &plan.back();
        if (!node.subtree_has_value) continue;
        const view_t& slice = is_root ? head : node.notify;
        if (slice.empty()) continue;
        vertex_t* vx = is_root ? v : find(node.key);
        if (vx != nullptr) fan_out(vx, slice, depth);
    }
    if (v->listeners_above_.load(std::memory_order_relaxed) > 0) bubble_up(v, value, depth);
    // Eager branch delivered these landing sites — clear any pending mark (a prior assign)
    // and advance stream drain cursors so a later sweep does not re-deliver (RFC-0008 §E).
    for (const site_t& site : sites) {
        clear_pending(site.vx);
        if (site.vx->role_ == role_t::STREAM) {
            const std::lock_guard lock(site.vx->m_);
            site.vx->last_flushed_seq_ = site.vx->write_seq_;
        }
    }
    return {};
}

void graph_t::deliver_vertex(vertex_t* v, const rope_t& value, int depth) {
    fan_out(v, value, depth);
    // Vertical bubbling (RFC-0005): every subscription observes its vertex AND all
    // descendants, so a delivery also fans out to each ancestor's subscribers. Gated on
    // one relaxed load when nobody listens above.
    if (v->listeners_above_.load(std::memory_order_relaxed) > 0) bubble_up(v, value, depth);
}

void graph_t::deliver_current(vertex_t* v, int depth) {
    if (v->role_ == role_t::STREAM) {
        // A stream is a queue (RFC-0008 §E): drain the ring entries appended since the
        // last flush, in order — NOT a coalesce. Snapshot under the lock, deliver outside.
        std::vector<std::shared_ptr<const rope_t>> batch;
        {
            const std::lock_guard lock(v->m_);
            const std::uint64_t now = v->write_seq_;
            if (now == v->last_flushed_seq_) return;  // nothing appended since the last flush
            const std::uint64_t n_new = now - v->last_flushed_seq_;
            v->last_flushed_seq_ = now;
            const auto take =
                static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(n_new, v->history_.size()));
            batch.assign(v->history_.end() - take, v->history_.end());
        }
        for (const std::shared_ptr<const rope_t>& sp : batch) deliver_vertex(v, *sp, depth);
        return;
    }
    // STORED_VALUE: the last-known-value, once. HANDLER / never-assigned: null LKV, nothing.
    const std::shared_ptr<const rope_t> sp = v->lkv_.load();
    if (!sp) return;
    deliver_vertex(v, *sp, depth);
}

void graph_t::propagate(vertex_t* v) { propagate_impl(v, 0); }

void graph_t::propagate_impl(vertex_t* v, int depth) {
    // The argument is always delivered — a direct propagate is never gated by the vertex's
    // own delivery_mode (RFC-0008 §C, the EXPLICIT escape hatch, and "notify its own subs"
    // is policy-independent).
    deliver_current(v, depth);
    // Sweep the strict descendants: DRAIN the IF_NEWER pending set over v's prefix range,
    // and ITERATE the UNCONDITIONAL set over it. A subtree is a contiguous prefix range of
    // the key order (RFC-0008 §B). Snapshot the keys under sweep_mutex_, then deliver
    // outside it — delivery re-enters the graph (fan_out/re-dispatch), like fan_out itself.
    const std::vector<std::byte> lo(v->key_.bytes.begin(), v->key_.bytes.end());
    const auto in_subtree = [&lo](const std::vector<std::byte>& k) {
        return k.size() >= lo.size() && std::equal(lo.begin(), lo.end(), k.begin());
    };
    std::vector<std::vector<std::byte>> to_deliver;
    {
        const std::lock_guard lock(sweep_mutex_);
        for (auto it = pending_.lower_bound(lo); it != pending_.end() && in_subtree(*it);) {
            if (it->size() != lo.size()) to_deliver.push_back(*it);  // strict descendant
            it = pending_.erase(it);  // drain (v itself, if present, was delivered above)
        }
        for (auto it = unconditional_.lower_bound(lo);
             it != unconditional_.end() && in_subtree(*it); ++it) {
            if (it->size() != lo.size()) to_deliver.push_back(*it);  // iterate, do not drain
        }
    }
    for (const std::vector<std::byte>& k : to_deliver) {
        if (vertex_t* u = find(k)) deliver_current(u, depth);
    }
}

void graph_t::mark_pending(vertex_t* v) {
    // EXPLICIT never rides an ancestor sweep; UNCONDITIONAL is already a permanent sweep
    // member — neither needs a pending mark. IF_NEWER marks only when someone observes at
    // or above v (else a sweep would deliver nowhere — the idle-write fast path keeps the
    // unobserved write off the shared lock, RFC-0005 listeners gate).
    if (v->delivery_mode_ != delivery_mode_t::IF_NEWER) return;
    if (v->own_subs_.load(std::memory_order_relaxed) == 0 &&
        v->listeners_above_.load(std::memory_order_relaxed) == 0)
        return;
    const std::lock_guard lock(sweep_mutex_);
    pending_.emplace(v->key_.bytes.begin(), v->key_.bytes.end());
}

void graph_t::clear_pending(vertex_t* v) {
    // Same idle fast path as mark_pending: an unobserved vertex was never marked.
    if (v->own_subs_.load(std::memory_order_relaxed) == 0 &&
        v->listeners_above_.load(std::memory_order_relaxed) == 0)
        return;
    const std::lock_guard lock(sweep_mutex_);
    pending_.erase(std::vector<std::byte>(v->key_.bytes.begin(), v->key_.bytes.end()));
}

void graph_t::set_delivery_mode(vertex_t* v, delivery_mode_t mode) {
    const std::vector<std::byte> key(v->key_.bytes.begin(), v->key_.bytes.end());
    const std::lock_guard lock(sweep_mutex_);
    v->delivery_mode_ = mode;
    if (mode == delivery_mode_t::UNCONDITIONAL) {
        unconditional_.insert(key);
        pending_.erase(key);  // swept via unconditional_ now — avoid double membership
    } else {
        unconditional_.erase(key);
        if (mode == delivery_mode_t::EXPLICIT) pending_.erase(key);  // never ancestor-swept
    }
}

result_t<void> graph_t::write(vertex_t* v, rope_t value, std::string_view caller) {
    return write_impl(v, std::move(value), 0, caller);
}

result_t<void> graph_t::write(vertex_t* v, const field_path_t& field, rope_t value,
                              std::string_view caller) {
    if (field.empty()) return write_impl(v, std::move(value), 0, caller);
    // A field write targets a contiguous control TLV (settings / acl / subscribers);
    // materialize it (single-link: zero copy) before the field surface parses it.
    return field_write(v, field, value.materialize(), caller);
}

result_t<rope_t> graph_t::await(vertex_t* v, std::chrono::nanoseconds timeout,
                                std::string_view caller) {
    // await is the readiness form of a data READ — same gate, checked up front so a
    // denied caller cannot camp on the condvar.
    if (!acl_allows(v, caller, acl_right_t::READ))
        return std::unexpected(status_t::PERMISSION_DENIED);
    std::unique_lock lock(v->m_);
    const std::uint64_t seq0 = v->write_seq_;
    if (!v->cv_.wait_for(lock, timeout, [&] { return v->write_seq_ != seq0; })) {
        return std::unexpected(status_t::TIMEOUT);
    }
    lock.unlock();
    const std::shared_ptr<const rope_t> sp = v->lkv_.load();
    if (!sp) return std::unexpected(status_t::NOT_FOUND);  // e.g. a Handler-role write
    return *sp;
}

result_t<std::vector<rope_t>> graph_t::history(vertex_t* v) const {
    if (v->role_ != role_t::STREAM) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    if (!acl_allows(v, {}, acl_right_t::READ))  // local-only helper => local (empty) context
        return std::unexpected(status_t::PERMISSION_DENIED);
    const std::lock_guard lock(v->m_);
    std::vector<rope_t> out;
    out.reserve(v->history_.size());
    for (const auto& sp : v->history_) out.push_back(*sp);  // clone each (refcount bump)
    return out;
}

namespace {

// Parse a SUBSCRIBER TLV into slot fields — the ONE parse every admission door shares
// (ADR-0049; the resolver's parallel subscriber_compact() parse is retired). Extracts
// the first PATH child's target key (may stay empty — the wire door ignores it) and
// the optional qos_settings `delivery_compact` opt-in (NAME "delivery_compact" VALUE
// u8, RFC-0004 §E.1 / docs/reference/05). Back-compat: a SUBSCRIBER without it (or an
// older parser) just keeps the full-route delivery path — existing conformance vectors
// unaffected.
void parse_subscriber_tlv(const tlv_t& sub, subscriber_t& s) {
    for (const tlv_t& child : sub.children) {
        if (child.type == type_t::PATH && s.target_key.empty()) {
            s.target_key = wire::path_key(child);
        } else if (child.type == type_t::SETTINGS) {
            const std::vector<tlv_t>& q = child.children;
            for (std::size_t i = 0; i + 1 < q.size(); ++i) {
                if (q[i].type != type_t::NAME || q[i + 1].type != type_t::VALUE) continue;
                if (detail::as_string_view(q[i].payload) == "delivery_compact")
                    s.delivery_compact = detail::load_le<std::uint8_t>(q[i + 1].payload) != 0;
            }
        }
    }
}

}  // namespace

result_t<void> graph_t::admit_subscriber(vertex_t* v, subscriber_t s, std::string_view caller) {
    // The single admission step (ADR-0049): every door lands here, so the SUBSCRIBE gate
    // and the transient-local durability latch apply UNIFORMLY — which invariants fire no
    // longer depends on which door an edge entered through.
    // Producer fan-out gate (#81, ADR-0026): appending a subscriber edge requires the
    // SUBSCRIBE right on this (the producer's) :acl, under the door's caller context.
    if (!acl_allows(v, caller, acl_right_t::SUBSCRIBE))
        return std::unexpected(status_t::PERMISSION_DENIED);

    // Latch the current value to the new subscriber iff the producer is transient-local
    // (durability == 1) and already holds an LKV (RFC-0004 §D / Q4) — for EVERY door, not
    // just the wire one (the ADR-0049 behavior alignment). Snapshot the slot's delivery
    // fields + the LKV under the lock, then deliver OUTSIDE it (the remote sink does
    // transport I/O; a callback / target re-dispatch may re-enter the graph), mirroring
    // fan_out's lock discipline.
    std::shared_ptr<const rope_t> latch;
    std::function<void(const rope_t&)> latch_callback;
    std::vector<std::byte> latch_target;
    std::string latch_caller;  // owning copies — the snapshot below outlives the lock;
    std::string latch_link;
    view_t latch_route;  // the route snapshot is a refcount clone (no byte copy).
    bool latch_compact = false;
    {
        const std::lock_guard lock(v->m_);
        v->subs_.push_back(std::move(s));
        if (v->settings_.durability == 1) {
            if (std::shared_ptr<const rope_t> lkv = v->lkv_.load()) {
                const subscriber_t& stored = v->subs_.back();
                latch = std::move(lkv);
                latch_callback = stored.callback;
                latch_target = stored.target_key;
                latch_caller = stored.caller;
                latch_link = stored.link;
                latch_route = stored.return_route;
                latch_compact = stored.delivery_compact;
            }
        }
    }
    note_subscriber_added(v);  // RFC-0005: descendants' writes now bubble here

    if (!latch) return {};
    // One immediate delivery, per edge kind — the same three legs fan_out dispatches.
    if (latch_callback) latch_callback(*latch);
    if (!latch_target.empty()) {
        if (vertex_t* target = find(latch_target)) {
            // Fan-in gate (#81): an ordinary write to the target under the edge's stored
            // caller context; denial drops the latch delivery, never the edge.
            (void)write_impl(target, *latch, 1, latch_caller);
        }
    }
    if (!latch_link.empty() && remote_sink_) {
        remote_sink_(
            remote_delivery_t{
                .link = latch_link, .return_route = latch_route, .delivery_compact = latch_compact},
            *latch);
    }
    return {};
}

result_t<void> graph_t::subscribe(const path_t& src, const path_t& target) {
    vertex_t* v = find(src.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    // ADR-0049: the sugar ENCODES the same SUBSCRIBER{PATH} TLV a wire subscribe carries
    // and enters the field-write door — subscribe-time is control-plane-cold, so the
    // encode/parse round-trip is irrelevant, and the edge reads back from :subscribers[]
    // byte-identically to a wire-made one. The target path's key IS the PATH payload
    // (the concatenated NAME children, docs/reference/03), embedded verbatim. Runs under
    // the empty (local) caller context, so a resolver that assigns local callers a
    // subject sees these too (#81, ADR-0026).
    const std::span<const std::byte> key = target.key();
    std::vector<std::byte> sub;
    sub.reserve(8 + key.size());
    wire::emit_header(sub, type_t::SUBSCRIBER, opt_t{.pl = true}, 4 + key.size());
    wire::emit_header(sub, type_t::PATH, opt_t{.pl = true}, key.size());
    sub.insert(sub.end(), key.begin(), key.end());
    const std::optional<view_t> value = view::over_bytes(sub);
    if (!value) return std::unexpected(status_t::BACKPRESSURE);
    field_path_t field;
    field.steps.push_back(field_step_t{.name = "subscribers", .indexed = true, .append = true});
    return field_write(v, field, *value, {});
}

result_t<void> graph_t::subscribe(const path_t& src, std::function<void(const rope_t&)> callback) {
    vertex_t* v = find(src.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    // A std::function cannot ride a TLV, so this sugar has no parse to share — it still
    // enters the same single admission step as every other door (ADR-0049), under the
    // empty (local) caller context.
    subscriber_t s;
    s.callback = std::move(callback);
    return admit_subscriber(v, std::move(s), {});
}

void graph_t::set_remote_delivery_sink(
    std::function<void(const remote_delivery_t&, const rope_t&)> sink) {
    remote_sink_ = std::move(sink);
}

result_t<void> graph_t::subscribe_wire(vertex_t* v, view_t source_view, view_t return_route,
                                       std::string link) {
    // Parse the owned SUBSCRIBER copy ONCE (ADR-0049) — delivery_compact comes from this
    // parse (the resolver's parallel subscriber_compact() is retired); the tlv_t borrows
    // source_view's bytes, which the slot then retains zero-copy.
    const auto sub = view_as_tlv(source_view);
    if (!sub || sub->type != type_t::SUBSCRIBER) return std::unexpected(status_t::TYPE_MISMATCH);
    subscriber_t s;
    parse_subscriber_tlv(*sub, s);
    // A PATH child names the consumer at ITS origin — never a local re-dispatch target;
    // remote delivery rides the return route over the link (RFC-0004 §D).
    s.target_key.clear();
    s.caller = link;  // the fan-in gate context this edge's deliveries run under (#81)
    s.return_route = std::move(return_route);
    s.source_view = std::move(source_view);
    s.link = std::move(link);
    const std::string gate_ctx = s.caller;  // survives the move below (the SUBSCRIBE gate
                                            // runs under the inbound link, #81/ADR-0026)
    return admit_subscriber(v, std::move(s), gate_ctx);
}

result_t<void> graph_t::field_write(vertex_t* v, const field_path_t& field, const view_t& value,
                                    std::string_view caller) {
    const field_step_t& step0 = field.steps[0];

    if (step0.name == "subscribers") {
        if (step0.append) {
            const auto sub = view_as_tlv(value);
            if (!sub || sub->type != type_t::SUBSCRIBER)
                return std::unexpected(status_t::TYPE_MISMATCH);
            subscriber_t s;
            parse_subscriber_tlv(*sub, s);  // the shared door parse (ADR-0049)
            if (s.target_key.empty()) return std::unexpected(status_t::TYPE_MISMATCH);
            s.source_view = value;  // retain the SUBSCRIBER TLV zero-copy (refcount clone) so a
                                    // later :subscribers[] read ropes it into the REPLY (ADR-0035).
            s.caller.assign(caller);  // the fan-in gate context for this edge's deliveries (#81)
            // The single admission step (ADR-0049): SUBSCRIBE gate → append → latch.
            return admit_subscriber(v, std::move(s), caller);
        }
        if (step0.indexed) {  // clear a subscriber slot (unsubscribe) — a control write
            if (!acl_allows(v, caller, acl_right_t::WRITE))
                return std::unexpected(status_t::PERMISSION_DENIED);
            bool removed = false;
            {
                const std::lock_guard lock(v->m_);
                if (step0.index < v->subs_.size() && v->subs_[step0.index].active) {
                    v->subs_[step0.index].active = false;
                    removed = true;
                }
            }
            if (removed) note_subscriber_removed(v);  // RFC-0005 counter bookkeeping
            return {};
        }
        return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    }

    if (step0.name == "acl") {
        // Store the :acl (#81, ADR-0018/0020): gate on WRITE_ACL — precisely the `admin`
        // right — then validate + parse the core-subset ACEs (ALLOW-only, single INHERIT
        // flag; parse_aces rejects anything beyond the subset with TYPE_MISMATCH) and
        // keep BOTH the raw bytes (served back verbatim by read_acl) and the parsed
        // list (evaluated by acl_allows).
        if (!acl_allows(v, caller, acl_right_t::WRITE_ACL))
            return std::unexpected(status_t::PERMISSION_DENIED);
        const auto acl = view_as_tlv(value);
        if (!acl || acl->type != type_t::ACL) return std::unexpected(status_t::TYPE_MISMATCH);
        result_t<std::vector<ace_t>> aces = parse_aces(*acl);
        if (!aces) return std::unexpected(aces.error());
        const std::span<const std::byte> bytes = value.bytes();
        const std::lock_guard lock(v->m_);
        v->acl_.assign(bytes.begin(), bytes.end());  // storing replaces; empty => no restrictions
        v->aces_ = std::move(*aces);
        return {};
    }

    if (step0.name == "children") {
        // In-band vertex creation (#82, ADR-0017): a `:children[]` APPEND of a SPEC
        // instantiates a child of a device-catalog type, gated by the parent's CREATE
        // right (#81, ADR-0020). A `[N]` clear (child removal) is deferred (#66).
        // Read-back (members, not SPECs) is the field-read surface.
        if (step0.append) {
            if (!acl_allows(v, caller, acl_right_t::CREATE))
                return std::unexpected(status_t::PERMISSION_DENIED);
            return create_child(v, value);
        }
        return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    }

    if (step0.name == "settings" && field.steps.size() >= 2) {
        if (!acl_allows(v, caller, acl_right_t::WRITE))  // QoS knobs are control writes
            return std::unexpected(status_t::PERMISSION_DENIED);
        const auto tlv = view_as_tlv(value);
        if (!tlv || tlv->type != type_t::VALUE) return std::unexpected(status_t::TYPE_MISMATCH);
        const std::uint64_t n = detail::load_le(tlv->payload);
        const std::string& f = field.steps[1].name;
        const std::lock_guard lock(v->m_);
        if (f == "reliability") {
            v->settings_.reliability = static_cast<std::uint8_t>(n);
        } else if (f == "durability") {
            v->settings_.durability = static_cast<std::uint8_t>(n);
        } else if (f == "priority") {
            v->settings_.priority = static_cast<std::uint8_t>(n);
        } else if (f == "history_keep_last") {
            v->settings_.history_keep_last = static_cast<std::uint32_t>(n);
        } else if (f == "queue_max_bytes") {
            v->settings_.queue_max_bytes = static_cast<std::uint32_t>(n);
        } else if (f == "deadline_ns") {
            v->settings_.deadline_ns = n;
        } else if (f == "store_ref_min_bytes") {
            v->settings_.store_ref_min_bytes = static_cast<std::uint32_t>(n);
        } else {
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        }
        return {};
    }

    return std::unexpected(status_t::SCHEMA_NOT_FOUND);
}

result_t<void> graph_t::create_child(vertex_t* parent, const view_t& spec_value) {
    // Parse SPEC{ NAME "type" <sel>, NAME "name" <seg>, SETTINGS "config"? } — the
    // creation spec of docs/reference/05 §0x0E. The two NAMEs are positional pairs
    // (NAME key, NAME/SETTINGS value), same shape as the qos_settings parse above.
    const auto spec = view_as_tlv(spec_value);
    if (!spec || spec->type != type_t::SPEC) return std::unexpected(status_t::TYPE_MISMATCH);

    std::string_view type_sel;
    std::span<const std::byte> child_name;
    const tlv_t* config = nullptr;
    const std::vector<tlv_t>& ch = spec->children;
    for (std::size_t i = 0; i + 1 < ch.size(); ++i) {
        if (ch[i].type != type_t::NAME) continue;
        const std::string_view key = detail::as_string_view(ch[i].payload);
        if (key == "type" && ch[i + 1].type == type_t::NAME) {
            type_sel = detail::as_string_view(ch[i + 1].payload);
        } else if (key == "name" && ch[i + 1].type == type_t::NAME) {
            child_name = ch[i + 1].payload;
        } else if (key == "config" && ch[i + 1].type == type_t::SETTINGS) {
            config = &ch[i + 1];
        }
    }
    if (type_sel.empty() || child_name.empty()) return std::unexpected(status_t::INVALID_PATH);

    // Look up the catalog type (ADR-0017): unknown => SCHEMA_NOT_FOUND (ENOTTY). The
    // map is read-only once frames flow (populated at setup), so no lock here.
    const auto it = child_types_.find(type_sel);
    if (it == child_types_.end()) return std::unexpected(status_t::SCHEMA_NOT_FOUND);

    // Compose the child key = parent's canonical PATH-payload + one NAME(child_name).
    // The graph owns this addressing; the factory only sees the finished key.
    std::vector<std::byte> child_key = parent->key_.bytes;
    wire::emit_name(child_key, child_name);

    result_t<vertex_t*> made = it->second(*this, std::move(child_key), config);
    if (!made) return std::unexpected(made.error());  // PATH_IN_USE on a duplicate name
    return {};
}

result_t<view_t> graph_t::read_schema(vertex_t* v) const {
    settings_t s;
    {
        const std::lock_guard lock(v->m_);
        s = v->settings_;
    }
    // POINT { NAME <vertex name>, SETTINGS { NAME "deadline_ns" VALUE u64,
    //                                        NAME "history_keep_last" VALUE u32 } }
    std::vector<std::byte> settings_children;
    wire::emit_name(settings_children, "deadline_ns");
    emit_value(settings_children, s.deadline_ns, 8);
    wire::emit_name(settings_children, "history_keep_last");
    emit_value(settings_children, s.history_keep_last, 4);

    std::vector<std::byte> point_body;
    wire::emit_name(point_body, key_view_t{v->key_.bytes}.last_segment());
    wire::emit_tlv(point_body, type_t::SETTINGS, opt_t{.pl = true},
                   settings_children);  // SETTINGS

    std::vector<std::byte> point;
    wire::emit_tlv(point, type_t::POINT, opt_t{.pl = true}, point_body);  // POINT

    // `point` is a POINT TLV (never empty); `nullopt` is exactly an alloc failure
    // → BACKPRESSURE. One audited locus for the alloc/copy/over triplet.
    const auto out = view::over_bytes(point);
    if (!out) return std::unexpected(status_t::BACKPRESSURE);
    return *out;
}

result_t<view_t> graph_t::read_acl(vertex_t* v) const {
    // Serve back the raw :acl TLV bytes stored by field_write (heap-alloc + copy, like
    // read_schema), or NOT_FOUND when none was set. Verbatim — the parsed-ACE evaluation
    // lives in acl_allows; the READ_ACL gate runs in the caller (read(v, field, caller)).
    std::vector<std::byte> acl;
    {
        const std::lock_guard lock(v->m_);
        if (v->acl_.empty()) return std::unexpected(status_t::NOT_FOUND);
        acl = v->acl_;
    }
    // `acl` is non-empty (guarded above); `nullopt` is exactly an alloc failure
    // → BACKPRESSURE. One audited locus for the alloc/copy/over triplet.
    const auto out = view::over_bytes(acl);
    if (!out) return std::unexpected(status_t::BACKPRESSURE);
    return *out;
}

result_t<view_t> graph_t::read_children(vertex_t* v) const {
    // The synthesized listing wins (ADR-0044): a transport/connection vertex serves
    // its live bus peers here — a snapshot of traffic, never stored graph structure.
    if (v->handlers_.on_children) return v->handlers_.on_children();
    // Generic member enumeration (reference 05 §SPEC read-members): the DIRECT
    // children of v in the vertex map — keys of the form <v.key><one NAME record>.
    // Each member is a minimal POINT{NAME} descriptor; order is unspecified.
    std::vector<std::byte> members;
    {
        const std::shared_lock lock(map_mutex_);
        const key_view_t pk{v->key_.bytes};
        for (const auto& [key, vert] : vertices_) {
            (void)vert;
            // A direct child is `pk` plus exactly one more NAME record; that record
            // IS the child's canonical NAME encoding — the POINT body verbatim.
            if (const auto rec = key_view_t{key.bytes}.child_record_under(pk))
                wire::emit_tlv(members, type_t::POINT, opt_t{.pl = true}, *rec);
        }
    }
    std::vector<std::byte> out;
    wire::emit_tlv(out, type_t::POINT, opt_t{.pl = true}, members);
    // `out` is non-empty by construction; `nullopt` is exactly an alloc failure
    // → BACKPRESSURE (the audited alloc/copy/over locus).
    const auto res = view::over_bytes(out);
    if (!res) return std::unexpected(status_t::BACKPRESSURE);
    return *res;
}

result_t<rope_t> graph_t::read(vertex_t* v, const field_path_t& field,
                               std::string_view caller) const {
    if (field.empty()) return read(v, caller);  // value read → the stored rope
    // A field read serves a contiguous control TLV; it crosses back as a single-link
    // rope (ADR-0053 §6 — the data API returns ropes). Compute the control view, then
    // wrap once. Field reads are gated like data reads (#81): READ for the control
    // surface, READ_ACL — its own right, distinct from acting on the vertex — for ":acl".
    const result_t<view_t> fv = [&]() -> result_t<view_t> {
        if (field.steps.size() == 1 && field.steps[0].name == "acl") {
            if (!acl_allows(v, caller, acl_right_t::READ_ACL))
                return std::unexpected(status_t::PERMISSION_DENIED);
            return read_acl(v);
        }
        if (!acl_allows(v, caller, acl_right_t::READ))
            return std::unexpected(status_t::PERMISSION_DENIED);
        if (field.steps.size() == 1 && field.steps[0].name == "schema") return read_schema(v);
        // ":children[]" (or bare ":children") — member enumeration, the read dual of
        // the SPEC-creating append. A single "[N]" slot has no meaning here (members
        // are named, not indexed) and falls through to SCHEMA_NOT_FOUND.
        if (field.steps.size() == 1 && field.steps[0].name == "children" &&
            !field.steps[0].wildcard && (field.steps[0].append || !field.steps[0].indexed)) {
            return read_children(v);
        }
        // A single slot ":subscribers[N]" — serve the stored SUBSCRIBER view (clone).
        if (field.steps.size() == 1 && field.steps[0].name == "subscribers" &&
            field.steps[0].indexed && !field.steps[0].append && !field.steps[0].wildcard) {
            const std::lock_guard lock(v->m_);
            const std::size_t idx = field.steps[0].index;
            if (idx < v->subs_.size() && v->subs_[idx].active && v->subs_[idx].source_view.owner)
                return v->subs_[idx].source_view;  // clone (refcount bump, no byte copy)
            return std::unexpected(status_t::NOT_FOUND);
        }
        return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    }();
    if (!fv) return std::unexpected(fv.error());
    return rope_t{*fv};
}

result_t<std::vector<view_t>> graph_t::read_subscribers(vertex_t* v,
                                                        std::string_view caller) const {
    if (!acl_allows(v, caller, acl_right_t::READ))  // control-surface read, like ":schema"
        return std::unexpected(status_t::PERMISSION_DENIED);
    const std::lock_guard lock(v->m_);
    std::vector<view_t> out;
    out.reserve(v->subs_.size());
    for (const subscriber_t& s : v->subs_)
        if (s.active && s.source_view.owner) out.push_back(s.source_view);  // clone each (refcount)
    return out;
}

result_t<rope_t> graph_t::read(const path_t& path) const {
    vertex_t* v = find(path.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    return read(v, path.field());  // handle-based; one locus for the field surface + ACL gates
}

result_t<void> graph_t::write(const path_t& path, rope_t value) {
    vertex_t* v = find(path.key());
    if (!v) {
        // Write-creates (RFC-0005): a DATA write to a nonexistent path creates it,
        // mkdir-p style, gated by CREATE on the nearest existing ancestor. The
        // `:field` control surface does not create — a field write to a
        // nonexistent vertex stays NOT_FOUND (there is no vertex to control).
        if (!path.field().empty()) return std::unexpected(status_t::NOT_FOUND);
        const result_t<vertex_t*> made = ensure_vertex(path.key());
        if (!made) return std::unexpected(made.error());
        v = *made;
    }
    return write(v, path.field(), std::move(value));  // handle-based; see the vertex_t* overload
}

result_t<rope_t> graph_t::await(const path_t& path, std::chrono::nanoseconds timeout) {
    vertex_t* v = find(path.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    return await(v, timeout);
}

}  // namespace tr::graph
