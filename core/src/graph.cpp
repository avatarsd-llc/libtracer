/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/graph.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
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
#include "libtracer/security_acl.hpp"
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
namespace {

/**
 * @brief Emit a VALUE TLV holding a `width`-byte little-endian integer — the one bespoke emitter
 *        for building a :schema POINT; NAME/SETTINGS/POINT use wire::emit_*.
 */
void emit_value(std::vector<std::byte>& out, std::uint64_t value, int width) {
    std::vector<std::byte> payload(static_cast<std::size_t>(width));
    detail::store_le(payload, value, static_cast<std::size_t>(width));
    wire::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
}

/**
 * @brief The flat descriptor-table key of an app-field path — field steps [2..) dot-joined
 *        (RFC-0010 §A.1: nesting below `settings.app.` is the owner's; the runtime keys the
 *        joined spelling as one flat string). Empty ⇒ a `[...]` selector step was present
 *        (no app field has an indexed surface — the caller maps that to SCHEMA_NOT_FOUND).
 */
[[nodiscard]] std::string app_field_key(const field_path_t& field) {
    std::string key;
    for (std::size_t i = 2; i < field.steps.size(); ++i) {
        const field_step_t& s = field.steps[i];
        if (s.indexed || s.append || s.wildcard) return {};
        if (i > 2) key += '.';
        key += s.name;
    }
    return key;
}

/** @brief True iff @p s is a plain NAME step (no `[N]` / `[]` / `[*]` selector). */
[[nodiscard]] bool plain_step(const field_step_t& s) noexcept {
    return !s.indexed && !s.append && !s.wildcard;
}

/** @brief Emit the RFC-0010 §A.4 app-container members into @p out: each declared,
 *         non-`wo` field HOLDING a value, in table order — `NAME <name>` then the stored
 *         TLV bytes verbatim (`wo` has no read surface; unset fields are omitted). */
void emit_app_container(std::vector<std::byte>& out, const std::vector<app_field_t>& table) {
    for (const app_field_t& f : table) {
        if (f.access == app_access_t::WO || f.value.empty()) continue;
        wire::emit_name(out, f.name);
        out.insert(out.end(), f.value.begin(), f.value.end());
    }
}

// Canonical-key NAME navigation (last segment, parent, ancestor/child, level
// split) lives in one locus: tr::wire::key_view_t (key_view.hpp).

/** @brief Absolute wall-clock ns since the UNIX epoch — the ACE `expires_ns` reference clock. */
[[nodiscard]] std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

// ACE evaluation and the typed :acl parse live in security_acl.hpp (ADR-0050):
// a pure per-target policy (acl_policy_t — ALLOW-only MCU profile by default,
// the full first-match-per-bit host policy under LIBTRACER_ACL_FULL), the
// effective-ACL merge semantics (effective_acl_t), and parse_acl/encode_acl.
// The graph keeps only the ancestor walk (inside the lazy per-vertex cache
// rebuild) and the subtree-precise invalidation below.

/**
 * @brief True iff the node's opt byte carries no trailer bits.
 *
 * A branch write (RFC-0005)
 * stores refcount subviews of the written frame, so a trailer inside the tree
 * cannot be sliced off without a copy — trailer-carrying nodes are rejected
 * (TYPE_MISMATCH), keeping stored values trailer-less at rest (ADR-0041 §4).
 */
[[nodiscard]] bool trailer_less(const wire::arena_tlv_t& node) noexcept {
    const opt_t& o = node.opt;
    return !o.ts && !o.cr && !o.cw && !o.tf;
}

/**
 * @brief The subview of `frame_view` covering `span` — a refcount bump on the written frame's
 *        segment, never a byte copy (RFC-0005 §decomposition).
 *
 * Precondition:
 * `span` points into `frame_view.bytes()` (it is an arena span over that frame).
 */
[[nodiscard]] view_t slice_of(const view_t& frame_view, std::span<const std::byte> span) {
    const std::size_t off = static_cast<std::size_t>(span.data() - frame_view.bytes().data());
    return frame_view.subview(off, span.size());
}

/**
 * @brief One landing site of a branch write (RFC-0005): the vertex key, the VALUE slice that lands
 *        there (empty when the node carries no value of its own), and the slice this vertex's
 *        subscribers are notified with (the VALUE for a leaf node, the node's whole POINT subtree
 *        for an interior node — the smallest subview covering every write at-or-below the
 *        subscription point).
 */
struct branch_node_t {
    std::vector<std::byte> key;
    view_t store{};
    view_t notify{};
    bool subtree_has_value = false;
};

/**
 * @brief Parse the POINT tree of a branch write into @p out (post-order;
 *        children precede their parent).
 *
 * @p key is the root node's canonical vertex key — the caller already folded
 * the root's leading NAME into it. STRICT (like parse_acl): children are the
 * leading NAME, at most one VALUE (the node's own value), and POINT
 * sub-branches; anything else — or any trailer-carrying node — is
 * TYPE_MISMATCH, so a stored slice never carries semantics the decomposition
 * would silently mangle. ITERATIVE (an explicit open-node stack): nesting depth
 * is bounded only by the receiver's decode resources (RFC-0006), so a recursive
 * walk over wire-derived structure could overflow the call stack on a
 * deep-but-admitted frame.
 *
 * @return Whether a VALUE lands anywhere in the tree, or the strict-shape error.
 */
[[nodiscard]] result_t<bool> parse_branch_node(const wire::tlv_arena_t& a, std::uint32_t root,
                                               const view_t& frame_view, std::vector<std::byte> key,
                                               std::vector<branch_node_t>& out) {
    /**
     * @brief One open POINT node: its arena index, the sibling cursor over its
     *        remaining children, its key, and the strict-shape accumulators.
     */
    struct open_t {
        std::uint32_t node = 0;       /**< @brief This node's arena pre-order index. */
        std::uint32_t next = 0;       /**< @brief Next unvisited child (arena pre-order index). */
        std::vector<std::byte> key;   /**< @brief This node's canonical vertex key. */
        view_t store{};               /**< @brief The node's own VALUE slice, if any. */
        bool has_value = false;       /**< @brief A VALUE child was seen. */
        bool has_point_child = false; /**< @brief A POINT sub-branch was seen. */
        bool subtree_value = false;   /**< @brief A VALUE landed below this node. */
    };

    std::vector<open_t> stack;
    // Validate a POINT node's shape (structured, trailer-less, leading NAME) and
    // open it with the sibling cursor past that NAME.
    const auto open = [&a, &stack](std::uint32_t node, std::vector<std::byte> k) -> result_t<void> {
        if (!a[node].opt.pl || !trailer_less(a[node]))
            return std::unexpected(status_t::TYPE_MISMATCH);
        const std::uint32_t cn = wire::tlv_arena_t::first_child(node);
        if (cn >= a[node].end || a[cn].type != type_t::NAME)
            return std::unexpected(status_t::TYPE_MISMATCH);
        stack.push_back(open_t{.node = node, .next = a.next_sibling(cn), .key = std::move(k)});
        return {};
    };
    if (const result_t<void> o = open(root, std::move(key)); !o) return std::unexpected(o.error());

    for (;;) {
        open_t& top = stack.back();
        if (top.next >= a[top.node].end) {
            // Node complete — emit its landing site (post-order) and fold its
            // subtree-has-value into the parent.
            const bool subtree_value = top.subtree_value || top.has_value;
            branch_node_t bn;
            bn.notify = top.has_point_child ? slice_of(frame_view, a[top.node].wire) : top.store;
            bn.store = std::move(top.store);
            bn.subtree_has_value = subtree_value;
            bn.key = std::move(top.key);
            out.push_back(std::move(bn));
            stack.pop_back();
            if (stack.empty()) return subtree_value;
            stack.back().subtree_value = stack.back().subtree_value || subtree_value;
            continue;
        }
        const std::uint32_t ci = top.next;
        const wire::arena_tlv_t& c = a[ci];
        top.next = a.next_sibling(ci);
        if (c.type == type_t::VALUE) {
            if (top.has_value || !trailer_less(c)) return std::unexpected(status_t::TYPE_MISMATCH);
            top.has_value = true;
            top.store = slice_of(frame_view, c.wire);
        } else if (c.type == type_t::POINT) {
            top.has_point_child = true;
            const std::uint32_t cn = wire::tlv_arena_t::first_child(ci);
            if (cn >= c.end || a[cn].type != type_t::NAME)
                return std::unexpected(status_t::TYPE_MISMATCH);
            std::vector<std::byte> child_key = top.key;
            wire::emit_name(child_key, a[cn].body);
            // `top` is invalidated by the push inside open().
            if (const result_t<void> o = open(ci, std::move(child_key)); !o)
                return std::unexpected(o.error());
        } else {
            return std::unexpected(status_t::TYPE_MISMATCH);
        }
    }
}

/**
 * @brief One Composite child record of `key` starting at `i` (ADR-0057 decomposition): the end of
 *        the well-framed NAME record at `i`, EXTENDED to the key's end when the record itself or
 *        the remainder after it is ragged — mirroring key_view_t::parent()'s framing (a ragged tail
 *        glues onto the last well-framed record), so tree decomposition and byte navigation
 *        (ancestor keys, bubbling order) agree even on malformed register_vertex_key blobs.
 */
[[nodiscard]] std::size_t segment_end(std::span<const std::byte> key, std::size_t i) noexcept {
    const auto record_end = [&key](std::size_t p) noexcept -> std::size_t {  // 0 => ragged
        if (p + 4 > key.size()) return 0;
        const std::size_t len = detail::load_le<std::uint16_t>(key.subspan(p + 2, 2));
        return p + 4 + len > key.size() ? 0 : p + 4 + len;
    };
    const std::size_t e = record_end(i);
    if (e == 0 || e == key.size()) return key.size();
    return record_end(e) == 0 ? key.size() : e;  // ragged remainder: glue it onto this record
}

}  // namespace

graph_t::graph_t(std::pmr::memory_resource* mr)
    : mr_(mr),
      root_(std::make_unique<vertex_t>(role_t::STORED_VALUE, path_key_t{}, settings_t{},
                                       handlers_t{})) {
    // The one built-in creation-catalog type (#82, ADR-0017): `stored_value` makes a
    // plain last-writer-wins vertex at the composed child key. Its optional SPEC
    // `config` SETTINGS is ignored for now (a stored-value has no instantiation params
    // beyond the standard `:settings` field, written separately). Devices add richer
    // types (controllers, transport connections — #83) via register_child_type.
    register_child_type("stored_value",
                        [](graph_t& g, std::vector<std::byte> child_key,
                           const tlv_t*) -> result_t<vertex_handle_t> {
                            return g.register_vertex_key(std::move(child_key),
                                                         role_t::STORED_VALUE);
                        });
}

void graph_t::register_child_type(std::string type, child_factory_t factory) {
    child_types_.insert_or_assign(std::move(type), std::move(factory));
}

vertex_handle_t graph_t::register_vertex(const path_t& path, role_t role, handlers_t handlers,
                                         settings_t settings) {
    result_t<vertex_handle_t> h = try_register_vertex(path, role, std::move(handlers), settings);
    // PATH_IN_USE on a compile-site literal is a source bug, not a runtime outcome — fail loud
    // (ADR-0056, mirroring path_t(std::string_view)) rather than hand back a result the caller
    // would only `*`-deref unchecked. A genuine runtime path uses try_register_vertex.
    if (!h) std::abort();
    return *h;
}

result_t<vertex_handle_t> graph_t::try_register_vertex(const path_t& path, role_t role,
                                                       handlers_t handlers, settings_t settings) {
    return register_vertex_key(std::vector<std::byte>(path.key().begin(), path.key().end()), role,
                               std::move(handlers), settings);
}

result_t<vertex_handle_t> graph_t::register_vertex_key(std::vector<std::byte> key, role_t role,
                                                       handlers_t handlers, settings_t settings) {
    const std::unique_lock lock(map_mutex_);
    // Descend the Composite tree (ADR-0057), creating unregistered PLACEHOLDER nodes for
    // missing intermediate levels — invisible to find/read_children until a registration
    // fills them in place (matching the flat map, where intermediates did not exist).
    vertex_t* node = root_.get();
    std::size_t i = 0;
    while (i < key.size()) {
        const std::size_t e = segment_end(key, i);
        const std::span<const std::byte> record{key.data() + i, e - i};
        vertex_t* child = node->child_by_record(record);
        if (child == nullptr) {
            auto fresh = std::make_unique<vertex_t>(
                role_t::STORED_VALUE,
                path_key_t{std::vector<std::byte>(record.begin(), record.end())}, settings_t{},
                handlers_t{});
            // Subtree-subscription init (RFC-0005): a vertex born under a subscribed
            // ancestor starts with the ancestor-listener count already summed — O(1) from
            // the parent's maintained counters (under the same unique lock the
            // note_subscriber_* walks exclude, so the sum and a concurrent subscribe walk
            // never double-count); the write path's is-anyone-listening check stays a
            // single relaxed load.
            fresh->init_listeners_above(node->listeners_above() + node->own_subs());
            child = node->add_child(std::move(fresh));
        }
        node = child;
        i = e;
    }
    if (node->registered()) return std::unexpected(status_t::PATH_IN_USE);
    node->fill(role, settings, std::move(handlers));
    return vertex_handle_t{node};
}

result_t<vertex_handle_t> graph_t::ensure_vertex(std::span<const std::byte> key,
                                                 std::string_view caller) {
    result_t<vertex_t*> p = ensure_vertex_ptr(key, caller);
    if (!p) return std::unexpected(p.error());
    return vertex_handle_t{*p};
}

result_t<vertex_t*> graph_t::ensure_vertex_ptr(std::span<const std::byte> key,
                                               std::string_view caller) {
    if (vertex_t* v = find_ptr(key)) return v;
    // Write-creates (RFC-0005): gate CREATE on the nearest EXISTING ancestor — its
    // effective ACL is exactly what every vertex of the missing chain would inherit
    // (the core subset's INHERIT walk, ADR-0020). No ancestor at all ⇒ open, the
    // ACL-presence opt-in of docs/reference/05 §0x0A.
    {
        key_view_t k{key};
        vertex_t* ancestor = nullptr;
        while (!k.empty()) {
            k = k.parent();
            ancestor = find_ptr(k.bytes());
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
        if (vertex_t* existing = find_ptr(pk)) {
            leaf = existing;
            continue;
        }
        result_t<vertex_handle_t> made =
            register_vertex_key(std::vector<std::byte>(pk.begin(), pk.end()), role_t::STORED_VALUE);
        if (made) {
            leaf = made->get();
            continue;
        }
        if (made.error() == status_t::PATH_IN_USE) {  // lost a benign creation race
            leaf = find_ptr(pk);
            if (leaf != nullptr) continue;
        }
        return std::unexpected(made.error());
    }
    return leaf;  // never null: the deepest level was just found or created
}

std::uint64_t graph_t::ancestor_walks() const noexcept {
    return ancestor_walks_.load(std::memory_order_relaxed);
}

void graph_t::bump_subtree_listeners(vertex_t* v, std::int32_t delta) {
    v->for_each_child([delta](vertex_t& c) {
        c.bump_listeners_above(delta);
        bump_subtree_listeners(&c, delta);
    });
}

void graph_t::note_subscriber_added(vertex_t* v) {
    // Shared map lock: excludes concurrent vertex creation (unique lock), so a
    // newborn either sees the bumped own_subs_ in its creation-time sum or is
    // already linked and walked here — never both. Counters are atomics. The
    // descendants are exactly v's child-link subtree (ADR-0057) — placeholders
    // included, so a later fill inherits a correct count.
    const std::shared_lock lock(map_mutex_);
    v->bump_own_subs(+1);
    bump_subtree_listeners(v, +1);
}

void graph_t::note_subscriber_removed(vertex_t* v) {
    const std::shared_lock lock(map_mutex_);
    v->bump_own_subs(-1);
    bump_subtree_listeners(v, -1);
}

vertex_t* graph_t::find_ptr(std::span<const std::byte> key) const {
    const std::shared_lock lock(map_mutex_);
    // O(segments) Composite child walk from the root (ADR-0057); a placeholder terminus
    // (an unregistered intermediate) is "no such vertex", as under the flat map.
    vertex_t* node = root_.get();
    std::size_t i = 0;
    while (i < key.size()) {
        const std::size_t e = segment_end(key, i);
        node = node->child_by_record(key.subspan(i, e - i));
        if (node == nullptr) return nullptr;
        i = e;
    }
    // The returned raw pointer is used by callers OUTSIDE this shared_lock. That is
    // sound only because the tree is insert-only (see root_'s declaration): each
    // vertex_t is owned by its parent via a non-moving unique_ptr allocation and is
    // never destroyed while the graph lives. Do NOT add vertex erasure/retirement
    // without first giving vertices a lifetime scheme — a bare detach would dangle
    // these pointers.
    return node->registered() ? node : nullptr;
}

std::vector<std::byte> graph_t::build_key(const vertex_t* v) {
    // Render-on-demand full key (ADR-0057): ancestors' NAME records concatenated
    // root-down. Parent links and name bytes are immutable — no lock. Two passes: size,
    // then a single exact allocation filled deepest-record-last.
    std::size_t total = 0;
    for (const vertex_t* n = v; n->parent() != nullptr; n = n->parent())
        total += n->name().bytes.size();
    std::vector<std::byte> key(total);
    std::size_t w = total;
    for (const vertex_t* n = v; n->parent() != nullptr; n = n->parent()) {
        const std::vector<std::byte>& rec = n->name().bytes;
        w -= rec.size();
        std::copy(rec.begin(), rec.end(), key.begin() + static_cast<std::ptrdiff_t>(w));
    }
    return key;
}

std::optional<vertex_handle_t> graph_t::find(std::span<const std::byte> key) const {
    vertex_t* p = find_ptr(key);
    if (p == nullptr) return std::nullopt;
    return vertex_handle_t{p};
}

const settings_t& graph_t::settings(vertex_handle_t v) const noexcept {
    return v.get()->settings();
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
    // #361 §3: ACL state lives only on BEARING vertices (those with own ACEs). A bare
    // vertex walks the immutable parent chain LOCK-FREE (has_own_aces is an atomic;
    // parent links never change) to its nearest bearing ancestor and evaluates that
    // vertex's cached merge through the kAceInherit projection — which IS the bare
    // vertex's effective list (the filter is idempotent and order-preserving over
    // "own + inherited-ancestors"). No cache, no ext block, is ever allocated on the
    // bare descendant; RAM stops scaling as ancestors x descendants.
    vertex_t* bearer = v;
    while (bearer != nullptr && bearer->parent() != nullptr && !bearer->has_own_aces())
        bearer = bearer->parent();
    if (bearer == nullptr || bearer->parent() == nullptr)
        return true;  // no ACL anywhere up the chain (root excluded) — open by default
    const bool self = bearer == v;

    // The ADR-0050 cached effective-ACE merge, now held by the BEARER: the data-plane
    // check evaluates ONE pre-merged list (own ACEs + INHERIT-flagged ancestor ACEs,
    // evaluation order) — no per-operation ancestor rebuild. The walk runs only inside
    // the rebuild lambda, on the first check after a :acl write marked the bearer dirty
    // (subtree-precise via the ADR-0057 child links — see field_write's "acl" branch).
    // The rebuild runs UNLOCKED (#361 §2 striped locks), taking each ancestor's stripe
    // one at a time. Root excluded (the flat-map walk never evaluated the empty key);
    // placeholder intermediates hold empty ACE lists, so merging them is the no-op
    // the old walk's skip was.
    return bearer->with_effective_aces(
        [&](const std::vector<ace_t>& own) {
            effective_acl_t eff;
            eff.append_own(own);
            for (vertex_t* ancestor = bearer->parent();
                 ancestor != nullptr && ancestor->parent() != nullptr;
                 ancestor = ancestor->parent()) {
                ancestor->with_aces(
                    [&](const std::vector<ace_t>& aces) { eff.append_ancestor(aces); });
            }
            return std::move(eff).release();
        },
        [&](const std::vector<ace_t>& merged, const std::vector<ace_t>& inherited) {
            return effective_acl_t::allows(self ? merged : inherited, *subject, bit, now);
        });
}

void graph_t::mark_subtree_acl_dirty(vertex_t* v) {
    v->mark_acl_cache_dirty();
    v->for_each_child([](vertex_t& child) { mark_subtree_acl_dirty(&child); });
}

result_t<rope_t> graph_t::read(vertex_handle_t vh, std::string_view caller) const {
    vertex_t* v = vh.get();
    if (!acl_allows(v, caller, acl_right_t::READ))
        return std::unexpected(status_t::PERMISSION_DENIED);
    if (v->role() == role_t::HANDLER) {
        if (v->handlers().on_read) return v->handlers().on_read();
        return std::unexpected(status_t::NOT_FOUND);
    }
    const std::shared_ptr<const rope_t> sp = v->read_stored();  // lock-free
    if (!sp) return std::unexpected(status_t::NOT_FOUND);
    return *sp;  // copies the rope => clones each link's segment_ptr_t (refcount bump, no byte
                 // copy)
}

void graph_t::dispatch_edge_target(const edge_view_t& e, const rope_t& value, int depth) {
    if (vertex_t* target = find_ptr(e.target_key)) {
        // Fan-in gate (#81, ADR-0026): the re-dispatch is an ordinary write to
        // the target, gated inside write_impl by the TARGET's :acl WRITE right
        // under the edge's stored caller context. Denial drops this delivery.
        (void)write_impl(target, value, depth + 1, e.caller);  // value cloned
    }
}

void graph_t::dispatch_edge_remote(const edge_view_t& e, const rope_t& value) {
    // Remote delivery (#136): a write fans out to a remote subscriber as a
    // FWD{WRITE} (or auto-promoted COMPACT) via the injected sink — outside the
    // vertex lock, like every other dispatch leg, since the sink does transport I/O.
    remote_sink_(
        remote_delivery_t{
            .link = e.link, .return_route = e.return_route, .delivery_compact = e.delivery_compact},
        value);
}

/**
 * @brief `inline` (linkage no-op for a single-TU member; an inliner hint): the wide fan-out loop's
 *        per-edge cost is this function's body, so it must stay inlined in that loop — the
 *        target/remote legs live in the two helpers above precisely to keep this body's inline
 *        estimate small (the callback leg is the in-process hot case).
 */
inline void graph_t::dispatch_edge(const edge_view_t& e, const rope_t& value, int depth) {
    // The ONE dispatch of a subscription edge's three legs — shared by the per-write
    // fan_out and the admission durability latch (ADR-0049), so the legs cannot diverge.
    // Always called OUTSIDE the vertex lock (each leg may re-enter the graph or do I/O).
    if (e.callback != nullptr)
        e.callback(e.callback_ctx, value);  // the rope by const ref (sink may clone links)
    if (!e.target_key.empty() && depth + 1 < kMaxDispatchDepth)
        dispatch_edge_target(e, value, depth);
    if (!e.link.empty() && remote_sink_) dispatch_edge_remote(e, value);
}

void graph_t::fan_out(vertex_t* v, const rope_t& value, int depth) {
    // Snapshot every active edge UNDER the vertex lock (vertex_t::snapshot_edges), then
    // dispatch OUTSIDE it (callbacks / re-dispatch may re-enter the graph). Delivery is
    // value-agnostic — no per-subscriber comparison — so every active edge receives
    // `value`; WHICH vertices propagate is the per-vertex delivery_mode decided by the
    // sweep (RFC-0008). Small fan-out (the common case) placement-constructs into a RAW
    // stack buffer — no per-publish heap allocation and no dead stack zeroing (an
    // edge_view_t array default-construct cost ~18 ns/op of rep-stos zeroing here) —
    // and large fan-out reserves the heap vector once.
    edge_snapshot_t inline_buf;
    std::vector<edge_view_t> heap_buf;
    const std::size_t n = v->snapshot_edges(inline_buf, heap_buf);
    if (heap_buf.empty())
        for (std::size_t i = 0; i < n; ++i) dispatch_edge(inline_buf[i], value, depth);
    else
        for (const edge_view_t& e : heap_buf) dispatch_edge(e, value, depth);
}

result_t<std::shared_ptr<const rope_t>> graph_t::store_value(vertex_t* v, rope_t value) {
    if (v->role() == role_t::HANDLER) {
        if (!v->handlers().on_write) return std::unexpected(status_t::NOT_FOUND);
        result_t<void> r = v->handlers().on_write(value);
        if (!r) return std::unexpected(r.error());
        v->note_write();
        return std::shared_ptr<const rope_t>{};  // handler consumed it — nothing stored
    }
    // The storage verb owns the invariant order: LKV publish (lock-free) BEFORE the
    // lock; ring append + keep-last trim + seq bump + await wake under it.
    return v->store(std::move(value), mr_);
}

void graph_t::bubble_up(vertex_t* v, const rope_t& value, int depth) {
    // Entered only when v->listeners_above() says an ancestor subscriber exists —
    // the idle write path never walks (RFC-0005 §near-free-when-idle; the counter
    // below is what tests/benches assert on via ancestor_walks()).
    ancestor_walks_.fetch_add(1, std::memory_order_relaxed);
    // Parent pointers are immutable once linked (ADR-0057), so the walk takes NO lock —
    // the old per-ancestor find_ptr (a shared-lock + hash lookup per level) is gone. A
    // placeholder ancestor holds no edges, so its fan_out is the no-op the old walk's
    // lookup miss was; the root node is the final (empty-key) stop, as before.
    for (vertex_t* ancestor = v->parent(); ancestor != nullptr; ancestor = ancestor->parent())
        fan_out(ancestor, value, depth);
}

namespace {
/**
 * @brief A branch write: a POINT payload (type 0x07, opt.PL=1) written to a value vertex decomposes
 *        across descendants (RFC-0005 §decomposition); anything else — VALUE, user-range records,
 *        other structured TLVs — stores as-is.
 *
 * The header sits at the
 * start of the first link (a decomposable POINT is contiguous); a device-memory link
 * is never dereferenced (and never decomposes).
 */
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
    if (is_branch_point(value, v->role()))
        return write_branch(v, value, depth, caller, /*notify=*/true);
    if (v->role() == role_t::HANDLER) {
        // A handler stores no LKV (the user handler consumes the value), so the
        // delivery clone survives here — the cold path only. The hot roles below
        // deliver the exact published pointer store_value hands back instead.
        const rope_t notify = value;  // refcount clone — store_value consumes `value`
        const result_t<std::shared_ptr<const rope_t>> stored = store_value(v, std::move(value));
        if (!stored) return std::unexpected(stored.error());
        deliver_vertex(v, notify, depth);
        clear_pending(v);  // eager delivery flushes any pending mark a prior assign left
        return {};
    }
    const result_t<std::shared_ptr<const rope_t>> stored = store_value(v, std::move(value));
    if (!stored) return std::unexpected(stored.error());
    if (v->role() == role_t::STREAM) {
        // Deliver the just-appended ring entry and advance the drain cursor, so a later
        // propagate on this stream does not re-deliver it (RFC-0008 §E).
        deliver_current(v, depth);
    } else {
        // Deliver exactly what was stored (RFC-0008 §D): the published LKV pointer —
        // no notify reclone of the rope on the hot write path.
        deliver_vertex(v, **stored, depth);
    }
    clear_pending(v);  // eager delivery flushes any pending mark a prior assign left
    return {};
}

result_t<void> graph_t::assign(vertex_handle_t vh, rope_t value, std::string_view caller) {
    vertex_t* v = vh.get();
    if (!acl_allows(v, caller, acl_right_t::WRITE))
        return std::unexpected(status_t::PERMISSION_DENIED);
    // The STATE half only (RFC-0008 §A): swap the last-known-value / append the stream
    // ring / bump the write sequence (waking await), then mark v for the next covering
    // sweep. A branch POINT assigns each descendant the same way. Sends nothing.
    if (is_branch_point(value, v->role()))
        return write_branch(v, value, 0, caller, /*notify=*/false);
    const result_t<std::shared_ptr<const rope_t>> stored = store_value(v, std::move(value));
    if (!stored) return std::unexpected(stored.error());
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
    if (!std::ranges::equal(a[n0].body, key_view_t{v->name().bytes}.last_segment()))
        return std::unexpected(status_t::INVALID_PATH);

    // The written tree is rooted AT `v`: render its full key once (ADR-0057
    // render-on-demand) — the node-key prefix of the whole decomposition plan.
    const std::vector<std::byte> root_key = build_key(v);
    std::vector<branch_node_t> plan;  // post-order; plan.back() is the root
    const result_t<bool> parsed =
        parse_branch_node(a, 0, head, std::vector<std::byte>(root_key), plan);
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
        if (std::ranges::equal(node.key, root_key)) {
            vx = v;  // the root value — `v` itself, already WRITE-gated by write_impl
        } else {
            const result_t<vertex_t*> ensured = ensure_vertex_ptr(node.key, caller);
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
        vertex_t* vx = is_root ? v : find_ptr(node.key);
        if (vx != nullptr) fan_out(vx, slice, depth);
    }
    if (v->listeners_above() > 0) bubble_up(v, value, depth);
    // Eager branch delivered these landing sites — clear any pending mark (a prior assign)
    // and advance stream drain cursors so a later sweep does not re-deliver (RFC-0008 §E).
    for (const site_t& site : sites) {
        clear_pending(site.vx);
        if (site.vx->role() == role_t::STREAM) site.vx->mark_flushed();
    }
    return {};
}

void graph_t::deliver_vertex(vertex_t* v, const rope_t& value, int depth) {
    fan_out(v, value, depth);
    // Vertical bubbling (RFC-0005): every subscription observes its vertex AND all
    // descendants, so a delivery also fans out to each ancestor's subscribers. Gated on
    // one relaxed load when nobody listens above.
    if (v->listeners_above() > 0) bubble_up(v, value, depth);
}

void graph_t::deliver_current(vertex_t* v, int depth) {
    if (v->role() == role_t::STREAM) {
        // A stream is a queue (RFC-0008 §E): drain the ring entries appended since the
        // last flush, in order — NOT a coalesce. Snapshot under the lock
        // (vertex_t::drain_unflushed), deliver outside.
        std::vector<std::shared_ptr<const rope_t>> batch;
        if (v->drain_unflushed(batch) == 0) return;  // nothing appended since the last flush
        for (const std::shared_ptr<const rope_t>& sp : batch) deliver_vertex(v, *sp, depth);
        return;
    }
    // STORED_VALUE: the last-known-value, once. HANDLER / never-assigned: null LKV, nothing.
    const std::shared_ptr<const rope_t> sp = v->read_stored();
    if (!sp) return;
    deliver_vertex(v, *sp, depth);
}

void graph_t::propagate(vertex_handle_t v) { propagate_impl(v.get(), 0); }

void graph_t::propagate_impl(vertex_t* v, int depth) {
    // The argument is always delivered — a direct propagate is never gated by the vertex's
    // own delivery_mode (RFC-0008 §C, the EXPLICIT escape hatch, and "notify its own subs"
    // is policy-independent).
    deliver_current(v, depth);
    // Sweep the strict descendants: DRAIN the IF_NEWER pending set over v's prefix range,
    // and ITERATE the UNCONDITIONAL set over it. A subtree is a contiguous prefix range of
    // the key order (RFC-0008 §B). Snapshot the keys under sweep_mutex_, then deliver
    // outside it — delivery re-enters the graph (fan_out/re-dispatch), like fan_out itself.
    const std::vector<std::byte> lo = build_key(v);
    const auto in_subtree = [&lo](const std::vector<std::byte>& k) {
        return k.size() >= lo.size() && std::equal(lo.begin(), lo.end(), k.begin());
    };
    std::vector<std::vector<std::byte>> to_deliver;
    {
        const std::lock_guard lock(sweep_mutex_);
        for (auto it = pending_.lower_bound(lo); it != pending_.end() && in_subtree(*it);) {
            if (it->size() != lo.size()) to_deliver.push_back(*it);  // strict descendant
            it = pending_.erase(it);  // drain (v itself, if present, was delivered above)
            pending_count_.fetch_sub(1, std::memory_order_relaxed);
        }
        for (auto it = unconditional_.lower_bound(lo);
             it != unconditional_.end() && in_subtree(*it); ++it) {
            if (it->size() != lo.size()) to_deliver.push_back(*it);  // iterate, do not drain
        }
    }
    for (const std::vector<std::byte>& k : to_deliver) {
        if (vertex_t* u = find_ptr(k)) deliver_current(u, depth);
    }
}

void graph_t::mark_pending(vertex_t* v) {
    // EXPLICIT never rides an ancestor sweep; UNCONDITIONAL is already a permanent sweep
    // member — neither needs a pending mark. IF_NEWER marks only when someone observes at
    // or above v (else a sweep would deliver nowhere — the idle-write fast path keeps the
    // unobserved write off the shared lock, RFC-0005 listeners gate).
    if (v->delivery_mode() != delivery_mode_t::IF_NEWER) return;
    if (v->own_subs() == 0 && v->listeners_above() == 0) return;
    std::vector<std::byte> key = build_key(v);  // outside the lock (a lock-free parent walk)
    const std::lock_guard lock(sweep_mutex_);
    if (pending_.insert(std::move(key)).second)
        pending_count_.fetch_add(1, std::memory_order_relaxed);
}

void graph_t::clear_pending(vertex_t* v) {
    // Same idle fast path as mark_pending: an unobserved vertex was never marked.
    if (v->own_subs() == 0 && v->listeners_above() == 0) return;
    // Empty-set fast path (the per-eager-write case when nobody uses assign+propagate):
    // no key render, no sweep lock. Racing a concurrent mark_pending here leaves the mark
    // for the next covering sweep — an ordering the locked erase already permitted.
    if (pending_count_.load(std::memory_order_relaxed) == 0) return;
    const std::vector<std::byte> key = build_key(v);  // outside the lock
    const std::lock_guard lock(sweep_mutex_);
    if (pending_.erase(key) != 0) pending_count_.fetch_sub(1, std::memory_order_relaxed);
}

void graph_t::set_delivery_mode(vertex_handle_t vh, delivery_mode_t mode) {
    vertex_t* v = vh.get();
    const std::vector<std::byte> key = build_key(v);
    const std::lock_guard lock(sweep_mutex_);
    v->set_delivery_mode(mode);
    if (mode == delivery_mode_t::UNCONDITIONAL) {
        unconditional_.insert(key);
        // Swept via unconditional_ now — avoid double membership.
        if (pending_.erase(key) != 0) pending_count_.fetch_sub(1, std::memory_order_relaxed);
    } else {
        unconditional_.erase(key);
        if (mode == delivery_mode_t::EXPLICIT &&  // never ancestor-swept
            pending_.erase(key) != 0)
            pending_count_.fetch_sub(1, std::memory_order_relaxed);
    }
}

result_t<void> graph_t::write(vertex_handle_t v, rope_t value, std::string_view caller) {
    return write_impl(v.get(), std::move(value), 0, caller);
}

result_t<void> graph_t::write(vertex_handle_t vh, const field_path_t& field, rope_t value,
                              std::string_view caller) {
    vertex_t* v = vh.get();
    if (field.empty()) return write_impl(v, std::move(value), 0, caller);
    // A field write targets a contiguous control TLV (settings / acl / subscribers);
    // materialize it (single-link: zero copy) before the field surface parses it.
    return field_write(v, field, value.materialize(), caller);
}

result_t<rope_t> graph_t::await(vertex_handle_t vh, std::chrono::nanoseconds timeout,
                                std::string_view caller) {
    vertex_t* v = vh.get();
    // await is the readiness form of a data READ — same gate, checked up front so a
    // denied caller cannot camp on the condvar.
    if (!acl_allows(v, caller, acl_right_t::READ))
        return std::unexpected(status_t::PERMISSION_DENIED);
    const std::uint64_t seq0 = v->current_seq();
    if (!v->wait_for_change(seq0, timeout)) return std::unexpected(status_t::TIMEOUT);
    const std::shared_ptr<const rope_t> sp = v->read_stored();
    if (!sp) return std::unexpected(status_t::NOT_FOUND);  // e.g. a Handler-role write
    return *sp;
}

result_t<std::vector<rope_t>> graph_t::history(vertex_handle_t vh) const {
    vertex_t* v = vh.get();
    if (v->role() != role_t::STREAM) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    if (!acl_allows(v, {}, acl_right_t::READ))  // local-only helper => local (empty) context
        return std::unexpected(status_t::PERMISSION_DENIED);
    return v->history_snapshot();  // clones each entry (refcount bumps)
}

namespace {

/**
 * @brief Parse a SUBSCRIBER TLV into slot fields — the ONE parse every admission door shares
 *        (ADR-0049; the resolver's parallel subscriber_compact() parse is retired).
 *
 * Extracts
 * the first PATH child's target key (may stay empty — the wire door ignores it) and
 * the optional qos_settings `delivery_compact` opt-in (NAME "delivery_compact" VALUE
 * u8, RFC-0004 §E.1 / docs/reference/05). Back-compat: a SUBSCRIBER without it (or an
 * older parser) just keeps the full-route delivery path — existing conformance vectors
 * unaffected.
 */
void parse_subscriber_tlv(const tlv_t& sub, subscriber_t& s) {
    for (const tlv_t& child : sub.children) {
        if (child.type == type_t::PATH && s.target_key.empty()) {
            s.target_key = wire::path_key(child);
        } else if (child.type == type_t::SETTINGS) {
            const std::vector<tlv_t>& q = child.children;
            for (std::size_t i = 0; i + 1 < q.size(); ++i) {
                if (q[i].type != type_t::NAME || q[i + 1].type != type_t::VALUE) continue;
                if (detail::as_string_view(q[i].payload) == "delivery_compact" &&
                    detail::load_le<std::uint8_t>(q[i + 1].payload) != 0)
                    s.ensure_remote().delivery_compact = true;  // cold half only when opted in
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
    // just the wire one (the ADR-0049 behavior alignment). vertex_t::add_edge appends the
    // slot and snapshots the latch's dispatch view + the LKV atomically under the vertex
    // lock; delivery runs OUTSIDE it (the remote sink does transport I/O; a callback /
    // target re-dispatch may re-enter the graph) through the SAME dispatch_edge legs a
    // write fans out with.
    edge_latch_t latch;
    (void)v->add_edge(std::move(s), &latch);
    note_subscriber_added(v);  // RFC-0005: descendants' writes now bubble here

    if (latch.value) dispatch_edge(latch.edge, *latch.value, 0);
    return {};
}

result_t<void> graph_t::subscribe(const path_t& src, const path_t& target) {
    vertex_t* v = find_ptr(src.key());
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

result_t<void> graph_t::subscribe(const path_t& src, subscriber_fn_t fn, void* ctx) {
    vertex_t* v = find_ptr(src.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    // A callback cannot ride a TLV, so this sugar has no parse to share — it still
    // enters the same single admission step as every other door (ADR-0049), under the
    // empty (local) caller context.
    subscriber_t s;
    s.callback = fn;
    s.callback_ctx = ctx;
    return admit_subscriber(v, std::move(s), {});
}

void graph_t::set_app_fields(vertex_handle_t v, std::vector<app_field_t> table) {
    // Owner-facing declaration (RFC-0010 §A.2) — a local host API like register_vertex,
    // so no ACL gate: the owner is updating its own projection. The vertex verb replaces
    // the table atomically with respect to concurrent field operations.
    v.get()->set_app_fields(std::move(table));
}

void graph_t::set_remote_delivery_sink(
    std::function<void(const remote_delivery_t&, const rope_t&)> sink) {
    remote_sink_ = std::move(sink);
}

result_t<void> graph_t::subscribe_wire(vertex_handle_t vh, view_t source_view, view_t return_route,
                                       std::string link) {
    vertex_t* v = vh.get();
    // Parse the owned SUBSCRIBER copy ONCE (ADR-0049) — delivery_compact comes from this
    // parse (the resolver's parallel subscriber_compact() is retired); the tlv_t borrows
    // source_view's bytes, which the slot then retains zero-copy.
    const auto sub = wire::decode(source_view);
    if (!sub || sub->type != type_t::SUBSCRIBER) return std::unexpected(status_t::TYPE_MISMATCH);
    subscriber_t s;
    parse_subscriber_tlv(*sub, s);
    // A PATH child names the consumer at ITS origin — never a local re-dispatch target;
    // remote delivery rides the return route over the link (RFC-0004 §D).
    s.target_key.clear();
    subscriber_remote_t& r = s.ensure_remote();  // a wire subscriber always carries the cold half
    r.caller = link;  // the fan-in gate context this edge's deliveries run under (#81)
    r.return_route = std::move(return_route);
    s.source_view = std::move(source_view);
    r.link = std::move(link);
    const std::string gate_ctx = r.caller;  // survives the move above (the SUBSCRIBE gate
                                            // runs under the inbound link, #81/ADR-0026)
    return admit_subscriber(v, std::move(s), gate_ctx);
}

result_t<void> graph_t::field_write(vertex_t* v, const field_path_t& field, const view_t& value,
                                    std::string_view caller) {
    const field_step_t& step0 = field.steps[0];

    if (step0.name == "subscribers") {
        if (step0.append) {
            const auto sub = wire::decode(value);
            if (!sub || sub->type != type_t::SUBSCRIBER)
                return std::unexpected(status_t::TYPE_MISMATCH);
            subscriber_t s;
            parse_subscriber_tlv(*sub, s);  // the shared door parse (ADR-0049)
            if (s.target_key.empty()) return std::unexpected(status_t::TYPE_MISMATCH);
            s.source_view = value;  // retain the SUBSCRIBER TLV zero-copy (refcount clone) so a
                                    // later :subscribers[] read ropes it into the REPLY (ADR-0035).
            if (!caller.empty())    // the fan-in gate context for this edge's deliveries (#81);
                                    // the empty (local) context needs no cold half
                s.ensure_remote().caller.assign(caller);
            // The single admission step (ADR-0049): SUBSCRIBE gate → append → latch.
            return admit_subscriber(v, std::move(s), caller);
        }
        if (step0.indexed) {  // clear a subscriber slot (unsubscribe) — a control write
            if (!acl_allows(v, caller, acl_right_t::WRITE))
                return std::unexpected(status_t::PERMISSION_DENIED);
            if (v->clear_edge(step0.index))
                note_subscriber_removed(v);  // RFC-0005 counter bookkeeping
            return {};
        }
        return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    }

    if (step0.name == "acl") {
        // Store the :acl (#81, ADR-0018/0020): gate on WRITE_ACL — precisely the `admin`
        // right — then validate + parse the typed ACEs (ADR-0050 parse_acl; strictness
        // follows the selected policy — the default ALLOW-only profile rejects DENY /
        // extra flags with TYPE_MISMATCH) and keep BOTH the raw bytes (served back
        // verbatim by read_acl) and the parsed list (evaluated by acl_allows).
        if (!acl_allows(v, caller, acl_right_t::WRITE_ACL))
            return std::unexpected(status_t::PERMISSION_DENIED);
        const auto acl = wire::decode(value);
        if (!acl || acl->type != type_t::ACL) return std::unexpected(status_t::TYPE_MISMATCH);
        result_t<std::vector<ace_t>> aces = parse_acl(*acl);
        if (!aces) return std::unexpected(aces.error());
        v->set_acl(value.bytes(), std::move(*aces));  // storing replaces; empty => no
                                                      // restrictions
        {
            // Subtree-precise cache invalidation (ADR-0050 via the ADR-0057 child
            // links): every descendant's effective merge embeds this vertex's
            // INHERIT ACEs, so mark the whole subtree dirty (v itself was marked by
            // set_acl; re-marking is idempotent). Wiring-frequency — :acl writes
            // are control-plane-rare. Shared map lock: the walk only excludes
            // concurrent vertex creation; the marks are release stores.
            const std::shared_lock lock(map_mutex_);
            mark_subtree_acl_dirty(v);
        }
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

    if (step0.name == "settings" && field.steps.size() >= 2 && field.steps[1].name == "app") {
        // Owner-declared application fields under the reserved `app` subkey (RFC-0010
        // §A). This branch owns the whole `settings.app.` subtree — the protocol never
        // minted (and per the RFC must never mint) a knob named `app`. A bare
        // `:settings.app` container write and any `[...]`-selector step have no write
        // surface.
        if (field.steps.size() < 3 || !plain_step(field.steps[1]))
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        const std::string key = app_field_key(field);
        if (key.empty()) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        const std::optional<app_access_t> access = v->app_field_access(key);
        if (!access)  // undeclared stays ENOTTY — the table opens only its own names
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (!caller.empty()) {
            // A caller-attributed (remote) write, gated in RFC-0010 §A.3 order. Gate 1:
            // a field not declared remotely writable has NO write surface — the
            // caller-INDEPENDENT identity (the ENOTTY of writing a read-only ioctl),
            // checked before the per-caller ACL right. Gate 2: the ordinary vertex
            // WRITE right, like any control write. The owner (empty caller) skips
            // both — it is updating its own projection, not a caller.
            if (*access == app_access_t::RO) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
            if (!acl_allows(v, caller, acl_right_t::WRITE))
                return std::unexpected(status_t::PERMISSION_DENIED);
        }
        // Store verbatim (§D — bytes in, bytes out; the descriptor is consumer
        // self-description, never a runtime validation schema). A false return means a
        // concurrent table replacement un-declared the name between gate and store.
        if (!v->app_field_store(key, value.bytes()))
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // The owner apply seam (§A.3), OUTSIDE the vertex lock — it may re-enter the
        // graph (apply the config, restructure children, then ANNOUNCE per §C). The
        // field write itself deliberately neither wakes `await` nor propagates:
        // the property plane is silent (ADR-0021 / RFC-0010 §C).
        if (const handlers_t& h = v->handlers(); h.on_app_field_write)
            h.on_app_field_write(key, value);
        return {};
    }

    if (step0.name == "settings" && field.steps.size() >= 2) {
        if (!acl_allows(v, caller, acl_right_t::WRITE))  // QoS knobs are control writes
            return std::unexpected(status_t::PERMISSION_DENIED);
        const auto tlv = wire::decode(value);
        if (!tlv || tlv->type != type_t::VALUE) return std::unexpected(status_t::TYPE_MISMATCH);
        const std::uint64_t n = detail::load_le(tlv->payload);
        const std::string& f = field.steps[1].name;
        return v->update_settings([&](settings_t& st) -> result_t<void> {
            if (f == "reliability") {
                st.reliability = static_cast<std::uint8_t>(n);
            } else if (f == "durability") {
                st.durability = static_cast<std::uint8_t>(n);
            } else if (f == "priority") {
                st.priority = static_cast<std::uint8_t>(n);
            } else if (f == "history_keep_last") {
                st.history_keep_last = static_cast<std::uint32_t>(n);
            } else if (f == "queue_max_bytes") {
                st.queue_max_bytes = static_cast<std::uint32_t>(n);
            } else if (f == "deadline_ns") {
                st.deadline_ns = n;
            } else if (f == "store_ref_min_bytes") {
                st.store_ref_min_bytes = static_cast<std::uint32_t>(n);
            } else {
                return std::unexpected(status_t::SCHEMA_NOT_FOUND);
            }
            return {};
        });
    }

    return std::unexpected(status_t::SCHEMA_NOT_FOUND);
}

result_t<void> graph_t::create_child(vertex_t* parent, const view_t& spec_value) {
    // Parse SPEC{ NAME "type" <sel>, NAME "name" <seg>, SETTINGS "config"? } — the
    // creation spec of docs/reference/05 §0x0E. The two NAMEs are positional pairs
    // (NAME key, NAME/SETTINGS value), same shape as the qos_settings parse above.
    const auto spec = wire::decode(spec_value);
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
    std::vector<std::byte> child_key = build_key(parent);
    wire::emit_name(child_key, child_name);

    result_t<vertex_handle_t> made = it->second(*this, std::move(child_key), config);
    if (!made) return std::unexpected(made.error());  // PATH_IN_USE on a duplicate name
    return {};
}

result_t<view_t> graph_t::read_schema(vertex_t* v) const {
    const settings_t s = v->settings_snapshot();
    // POINT { NAME <vertex name>, SETTINGS { NAME "deadline_ns" VALUE u64,
    //                                        NAME "history_keep_last" VALUE u32 } }
    std::vector<std::byte> settings_children;
    wire::emit_name(settings_children, "deadline_ns");
    emit_value(settings_children, s.deadline_ns, 8);
    wire::emit_name(settings_children, "history_keep_last");
    emit_value(settings_children, s.history_keep_last, 4);

    std::vector<std::byte> point_body;
    wire::emit_name(point_body, key_view_t{v->name().bytes}.last_segment());
    wire::emit_tlv(point_body, type_t::SETTINGS, opt_t{.pl = true},
                   settings_children);  // SETTINGS

    // The owner part (RFC-0010 §B.2), present iff a descriptor table is installed —
    // `NAME "app" SETTINGS{ NAME <field> SETTINGS{…} … }` appended AFTER the synthesized
    // protocol part (precedence by position, zero merge logic; the two parts describe
    // disjoint namespaces by the §A.1 reservation). Each field's record leads with the
    // runtime-projected `access` member — the one §B.1 datum the runtime holds natively,
    // so the schema can never contradict the write gate — then the owner's descriptor
    // bytes verbatim. A vertex without a table keeps today's POINT byte-for-byte.
    const std::vector<app_field_t> table = v->app_fields_snapshot();
    if (!table.empty()) {
        std::vector<std::byte> app_children;
        for (const app_field_t& f : table) {
            std::vector<std::byte> desc;
            wire::emit_name(desc, "access");
            const std::string_view a = to_string(f.access);
            wire::emit_tlv(
                desc, type_t::VALUE, opt_t{},
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(a.data()), a.size()));
            desc.insert(desc.end(), f.descriptor.begin(), f.descriptor.end());
            wire::emit_name(app_children, f.name);
            wire::emit_tlv(app_children, type_t::SETTINGS, opt_t{.pl = true}, desc);
        }
        wire::emit_name(point_body, "app");
        wire::emit_tlv(point_body, type_t::SETTINGS, opt_t{.pl = true}, app_children);
    }

    std::vector<std::byte> point;
    wire::emit_tlv(point, type_t::POINT, opt_t{.pl = true}, point_body);  // POINT

    // `point` is a POINT TLV (never empty); `nullopt` is exactly an alloc failure
    // → BACKPRESSURE. One audited locus for the alloc/copy/over triplet.
    const auto out = view::over_bytes(point);
    if (!out) return std::unexpected(status_t::BACKPRESSURE);
    return *out;
}

result_t<view_t> graph_t::read_settings(vertex_t* v) const {
    // The full settings container (RFC-0010 §A.4): the implemented protocol QoS knobs —
    // in the docs/reference/05 §0x0B payload-layout order, each `NAME <knob> VALUE <int>`
    // — plus the nested `app` record iff a descriptor table is installed. One traversal
    // serves a generic settings renderer protocol knobs and app config in one record,
    // distinguished by the reserved subkey.
    const settings_t s = v->settings_snapshot();
    std::vector<std::byte> children;
    wire::emit_name(children, "reliability");
    emit_value(children, s.reliability, 1);
    wire::emit_name(children, "durability");
    emit_value(children, s.durability, 1);
    wire::emit_name(children, "history_keep_last");
    emit_value(children, s.history_keep_last, 4);
    wire::emit_name(children, "deadline_ns");
    emit_value(children, s.deadline_ns, 8);
    wire::emit_name(children, "priority");
    emit_value(children, s.priority, 1);
    wire::emit_name(children, "queue_max_bytes");
    emit_value(children, s.queue_max_bytes, 4);
    wire::emit_name(children, "store_ref_min_bytes");
    emit_value(children, s.store_ref_min_bytes, 4);
    const std::vector<app_field_t> table = v->app_fields_snapshot();
    if (!table.empty()) {
        std::vector<std::byte> app_children;
        emit_app_container(app_children, table);
        wire::emit_name(children, "app");
        wire::emit_tlv(children, type_t::SETTINGS, opt_t{.pl = true}, app_children);
    }
    std::vector<std::byte> out;
    wire::emit_tlv(out, type_t::SETTINGS, opt_t{.pl = true}, children);
    // `out` is non-empty by construction; `nullopt` is exactly an alloc failure
    // → BACKPRESSURE (the audited alloc/copy/over locus).
    const auto res = view::over_bytes(out);
    if (!res) return std::unexpected(status_t::BACKPRESSURE);
    return *res;
}

result_t<view_t> graph_t::read_settings_app(vertex_t* v) const {
    // The app container alone (RFC-0010 §A.4). No installed table ⇒ the surface stays
    // closed (SCHEMA_NOT_FOUND — byte-for-byte the pre-RFC vertex); an installed table
    // serves the declared, non-`wo`, value-holding fields verbatim (possibly an empty
    // SETTINGS when nothing has been written yet).
    const std::vector<app_field_t> table = v->app_fields_snapshot();
    if (table.empty()) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    std::vector<std::byte> children;
    emit_app_container(children, table);
    std::vector<std::byte> out;
    wire::emit_tlv(out, type_t::SETTINGS, opt_t{.pl = true}, children);
    // `out` is non-empty by construction (the SETTINGS header at minimum); `nullopt` is
    // exactly an alloc failure → BACKPRESSURE (the audited alloc/copy/over locus).
    const auto res = view::over_bytes(out);
    if (!res) return std::unexpected(status_t::BACKPRESSURE);
    return *res;
}

result_t<view_t> graph_t::read_acl(vertex_t* v) const {
    // Serve back the raw :acl TLV bytes stored by field_write (heap-alloc + copy, like
    // read_schema), or NOT_FOUND when none was set. Verbatim — the parsed-ACE evaluation
    // lives in acl_allows; the READ_ACL gate runs in the caller (read(v, field, caller)).
    const std::vector<std::byte> acl = v->acl_bytes();
    if (acl.empty()) return std::unexpected(status_t::NOT_FOUND);
    // `acl` is non-empty (guarded above); `nullopt` is exactly an alloc failure
    // → BACKPRESSURE. One audited locus for the alloc/copy/over triplet.
    const auto out = view::over_bytes(acl);
    if (!out) return std::unexpected(status_t::BACKPRESSURE);
    return *out;
}

result_t<view_t> graph_t::read_children(vertex_t* v) const {
    // The synthesized listing wins (ADR-0044): a transport/connection vertex serves
    // its live bus peers here — a snapshot of traffic, never stored graph structure.
    if (v->handlers().on_children) return v->handlers().on_children();
    // Generic member enumeration (reference 05 §SPEC read-members): the DIRECT
    // children of v in the vertex map — keys of the form <v.key><one NAME record>.
    // Each member is a minimal POINT{NAME} descriptor; order is unspecified.
    std::vector<std::byte> members;
    {
        const std::shared_lock lock(map_mutex_);
        // A direct child's own NAME record IS the POINT body verbatim (ADR-0057 — one
        // child-list walk, no whole-map prefix scan). Placeholders (unregistered
        // intermediate levels) are not members, matching the flat map where they did
        // not exist.
        v->for_each_child([&members](const vertex_t& c) {
            if (c.registered())
                wire::emit_tlv(members, type_t::POINT, opt_t{.pl = true}, c.name().bytes);
        });
    }
    std::vector<std::byte> out;
    wire::emit_tlv(out, type_t::POINT, opt_t{.pl = true}, members);
    // `out` is non-empty by construction; `nullopt` is exactly an alloc failure
    // → BACKPRESSURE (the audited alloc/copy/over locus).
    const auto res = view::over_bytes(out);
    if (!res) return std::unexpected(status_t::BACKPRESSURE);
    return *res;
}

result_t<rope_t> graph_t::read(vertex_handle_t vh, const field_path_t& field,
                               std::string_view caller) const {
    vertex_t* v = vh.get();
    if (field.empty()) return read(vh, caller);  // value read → the stored rope
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
        if (field.steps[0].name == "settings" && plain_step(field.steps[0])) {
            // The RFC-0010 §A.4 read surfaces. Bare ":settings" — the full container
            // (protocol knobs + the nested app record); ":settings.app" — the app
            // container alone; ":settings.app.<name…>" — one declared field's stored
            // TLV verbatim. Per-knob protocol reads (":settings.deadline_ns") remain
            // unimplemented and fall through to SCHEMA_NOT_FOUND, as before.
            if (field.steps.size() == 1) return read_settings(v);
            if (field.steps[1].name == "app" && plain_step(field.steps[1])) {
                if (field.steps.size() == 2) return read_settings_app(v);
                const std::string key = app_field_key(field);
                if (key.empty()) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
                std::vector<std::byte> bytes;
                switch (v->app_field_get(key, bytes)) {
                    case vertex_t::app_read_t::UNDECLARED:  // ENOTTY (undeclared) …
                    case vertex_t::app_read_t::WRITE_ONLY:  // … and `wo` has no read
                        // surface either (the secret never mirrors back) — the same
                        // caller-independent identity, deliberately indistinguishable.
                        return std::unexpected(status_t::SCHEMA_NOT_FOUND);
                    case vertex_t::app_read_t::UNSET:  // declared but empty — distinct
                        return std::unexpected(status_t::NOT_FOUND);
                    case vertex_t::app_read_t::OK:
                        break;
                }
                // `bytes` is a non-empty stored TLV; `nullopt` is exactly an alloc
                // failure → BACKPRESSURE (the audited alloc/copy/over locus).
                const auto out = view::over_bytes(bytes);
                if (!out) return std::unexpected(status_t::BACKPRESSURE);
                return *out;
            }
        }
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
            if (std::optional<view_t> sv = v->edge_source(field.steps[0].index))
                return *sv;  // clone (refcount bump, no byte copy)
            return std::unexpected(status_t::NOT_FOUND);
        }
        return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    }();
    if (!fv) return std::unexpected(fv.error());
    return rope_t{*fv};
}

result_t<std::vector<view_t>> graph_t::read_subscribers(vertex_handle_t vh,
                                                        std::string_view caller) const {
    vertex_t* v = vh.get();
    if (!acl_allows(v, caller, acl_right_t::READ))  // control-surface read, like ":schema"
        return std::unexpected(status_t::PERMISSION_DENIED);
    return v->edge_sources();  // each a clone (refcount bump, no byte copy)
}

result_t<rope_t> graph_t::read(const path_t& path) const {
    vertex_t* v = find_ptr(path.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    // handle-based; one locus for the field surface + ACL gates
    return read(vertex_handle_t{v}, path.field());
}

result_t<void> graph_t::write(const path_t& path, rope_t value) {
    vertex_t* v = find_ptr(path.key());
    if (!v) {
        // Write-creates (RFC-0005): a DATA write to a nonexistent path creates it,
        // mkdir-p style, gated by CREATE on the nearest existing ancestor. The
        // `:field` control surface does not create — a field write to a
        // nonexistent vertex stays NOT_FOUND (there is no vertex to control).
        if (!path.field().empty()) return std::unexpected(status_t::NOT_FOUND);
        const result_t<vertex_t*> made = ensure_vertex_ptr(path.key(), {});
        if (!made) return std::unexpected(made.error());
        v = *made;
    }
    // handle-based; see the vertex_handle_t overload
    return write(vertex_handle_t{v}, path.field(), std::move(value));
}

result_t<rope_t> graph_t::await(const path_t& path, std::chrono::nanoseconds timeout) {
    vertex_t* v = find_ptr(path.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    return await(vertex_handle_t{v}, timeout);
}

}  // namespace tr::graph
