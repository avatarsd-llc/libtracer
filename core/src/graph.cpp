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
#include "libtracer/config_reader.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/key_view.hpp"
#include "libtracer/mem_borrowed.hpp"
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

/**
 * @brief True iff @p field is the "served whole" shape — exactly one plain NAME step.
 *
 * The single shape rule behind every field the protocol serves as ONE record with no member
 * and no slot addressing. Four sites consult it (#869): `field_write`'s `:acl` arm, and
 * `read`'s `:acl`, `:identity` and `:schema` arms. Before it the read door spelled it
 * `plain_step(steps[0]) && steps.size() == 1` and the write door
 * `steps.size() != 1 || !plain_step(step0)` — the same rule written two ways.
 */
[[nodiscard]] bool whole_field(const field_path_t& field) noexcept {
    return field.steps.size() == 1 && plain_step(field.steps[0]);
}

/**
 * @brief The selector shape of an array-addressed field's leading step (`:subscribers`,
 *        `:children`) — the ONE classification both the read and the write door switch on.
 *
 * The shapes are the addressing grammar's, not a per-field policy: WHICH of them a given
 * field accepts, and with what answer, stays at each door (that read/write asymmetry is
 * real — `:children[]` creates on a write and enumerates on a read). What is shared is the
 * decoding of the selector itself, which before #869 existed as sequential `append` /
 * `wildcard` / `indexed` branches on the write side and one `indexed && !append &&
 * !wildcard` conjunction on the read side.
 */
enum class field_sel_t : std::uint8_t {
    TAIL,     /**< @brief More than one step — a tail below an array field names nothing. */
    WILDCARD, /**< @brief `[*]` — index_mode WILDCARD (`indexed` set, no index assigned). */
    APPEND,   /**< @brief `[]` — index_mode ELEMENT with no index. */
    SLOT,     /**< @brief `[N]` — one addressed slot. */
    WHOLE,    /**< @brief bare `:name` — no selector at all. */
};

/**
 * @brief Classify @p field's leading step per @ref field_sel_t.
 *
 * `wildcard` is tested FIRST among the selector bits, so a step carrying the wildcard marker
 * can never be mistaken for an append or a slot. That ordering is #579's rule — a `[*]`
 * marker must never be ignored — applied once here instead of once per door. Neither
 * producer of a `field_step_t` can set `append` and `wildcard` together (`path_t::parse`
 * never sets `wildcard`; the wire decoder's ELEMENT and WILDCARD are exclusive switch arms),
 * so the ordering is observable only to an in-process caller that hand-builds the step.
 */
[[nodiscard]] field_sel_t field_selector(const field_path_t& field) noexcept {
    if (field.steps.size() != 1) return field_sel_t::TAIL;
    const field_step_t& s = field.steps[0];
    if (s.wildcard) return field_sel_t::WILDCARD;
    if (s.append) return field_sel_t::APPEND;
    if (s.indexed) return field_sel_t::SLOT;
    return field_sel_t::WHOLE;
}

/** @brief What a field path names under `:settings` (RFC-0010 §A / RFC-0022 §3.B). */
enum class app_sel_t : std::uint8_t {
    NONE,      /**< @brief Bare `:settings` — the container itself. */
    CORE_KNOB, /**< @brief `:settings.<name>…` with `<name> != "app"` — the EMPTY core
                *          namespace (RFC-0022 §3.B), so an unknown name on both doors. */
    MALFORMED, /**< @brief `app` carries a selector, or a `[...]` step sits below it. */
    CONTAINER, /**< @brief `:settings.app` — the app container. */
    NAMED,     /**< @brief `:settings.app.<name…>` — one declared owner field. */
};

/**
 * @brief Resolve the `settings.…` sub-shape ONCE for both doors (#869).
 *
 * Only steps [1..) are classified. Whether `steps[0]` itself must be plain is deliberately
 * NOT folded in: the two doors disagree about that today (the read door requires
 * `plain_step(steps[0])`, the write door does not) and unifying them would change an answer
 * that leaves the device — see the `:settings[0].app.<name>` note on `graph_t::field_write`.
 *
 * @param key Assigned the flat descriptor-table key on `NAMED`, left untouched otherwise —
 *            an out-param rather than a `{kind, key}` return, which measured 1408 bytes of
 *            graph.cpp `.text` larger at -O3 across the two doors.
 */
[[nodiscard]] app_sel_t app_field_sel(const field_path_t& field, std::string& key) {
    if (field.steps.size() < 2) return app_sel_t::NONE;
    if (field.steps[1].name != "app") return app_sel_t::CORE_KNOB;
    if (!plain_step(field.steps[1])) return app_sel_t::MALFORMED;
    if (field.steps.size() == 2) return app_sel_t::CONTAINER;
    key = app_field_key(field);
    if (key.empty()) return app_sel_t::MALFORMED;
    return app_sel_t::NAMED;
}

// The flat protocol-knob name table is GONE (RFC-0022 §3.B): `settings_t` is deleted, so
// the vertex `:settings` core namespace has no writable member left. All seven historical
// names — `reliability`, `priority`, `durability`, `deadline_ns`, `queue_max_bytes`,
// `history_keep_last`, `store_ref_min_bytes` — answer `SCHEMA_NOT_FOUND`, which is the
// honest answer an unsupported field already gives. The two survivors did not move to
// another name; they stopped being remotely writable at all and became owner-side
// declarations (`graph_t::set_history_depth`, `graph_t::set_pin_payload_ratio`).

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
    // open it with the sibling cursor past that NAME. The open-node stack grows
    // NOTHROW (#477 — a branch write runs on the writer thread): OOM soft-fails the
    // whole branch write as BACKPRESSURE (the store-leg status), never an abort.
    const auto open = [&a, &stack](std::uint32_t node, std::vector<std::byte> k) -> result_t<void> {
        if (!a[node].opt.pl || !trailer_less(a[node]))
            return std::unexpected(status_t::TYPE_MISMATCH);
        const std::uint32_t cn = wire::tlv_arena_t::first_child(node);
        if (cn >= a[node].end || a[cn].type != type_t::NAME)
            return std::unexpected(status_t::TYPE_MISMATCH);
        if (!detail::try_push_back(
                stack, open_t{.node = node, .next = a.next_sibling(cn), .key = std::move(k)}))
            return std::unexpected(status_t::BACKPRESSURE);
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
            if (!detail::try_push_back(out, std::move(bn)))  // nothrow plan growth (#477)
                return std::unexpected(status_t::BACKPRESSURE);
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
            // The child key = parent key + one NAME record, composed NOTHROW (#477):
            // reserve the exact final size (≤ 6-byte header even if emit_tlv widens),
            // then the copy + emit cannot reallocate.
            std::vector<std::byte> child_key;
            if (!detail::try_reserve(child_key, top.key.size() + 6 + a[cn].body.size()))
                return std::unexpected(status_t::BACKPRESSURE);
            child_key.assign(top.key.begin(), top.key.end());  // within capacity
            wire::emit_name(child_key, a[cn].body);            // within capacity
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
 *
 * The framing is read through key_view_t::record_end, the one locus of it (#888); what stays
 * HERE is only the ragged-tail RULE above, which is this decomposition's own and not shared.
 * `record_end` reports ragged as 0, which is what the local lambda this replaces reported too.
 */
[[nodiscard]] std::size_t segment_end(std::span<const std::byte> key, std::size_t i) noexcept {
    const key_view_t k{key};
    const std::size_t e = k.record_end(i);
    if (e == 0 || e == key.size()) return key.size();
    return k.record_end(e) == 0 ? key.size() : e;  // ragged remainder: glue it onto this record
}

}  // namespace

graph_t::graph_t(std::pmr::memory_resource* mr, mem::mem_backend_t* value_backend,
                 mem::block_source_t* ctl)
    : mr_(mr),
      value_backend_(value_backend),
      root_(std::make_unique<vertex_t>(role_t::STORED_VALUE, path_key_t{}, handlers_t{})),
      // The anchors' private structural root (#1223). It takes NO vertex slot: it is never
      // an anchor itself and no element can name it, and giving it one would put a second
      // unaddressable hole in an index whose only documented hole is slot 0.
      anchor_root_(std::make_unique<vertex_t>(role_t::STORED_VALUE, path_key_t{}, handlers_t{})),
      ctl_(ctl) {
    // Slot 0 is the structural root (RFC-0024 §6.4): the index is seeded here so it stays
    // allocation-ordered from the first vertex_t this graph owns. The root is not a
    // registrable address, so no bound path ever names slot 0 — it is in the vector because
    // leaving a hole there would make "slot i is the i-th vertex_t allocated" false.
    vertex_slots_.push_back(root_.get());
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

vertex_handle_t graph_t::register_vertex(const path_t& path, role_t role, handlers_t handlers) {
    result_t<vertex_handle_t> h = try_register_vertex(path, role, std::move(handlers));
    // PATH_IN_USE on a compile-site literal is a source bug, not a runtime outcome — fail loud
    // (ADR-0056, mirroring path_t(std::string_view)) rather than hand back a result the caller
    // would only `*`-deref unchecked. A genuine runtime path uses try_register_vertex.
    if (!h) std::abort();
    return *h;
}

result_t<vertex_handle_t> graph_t::try_register_vertex(const path_t& path, role_t role,
                                                       handlers_t handlers) {
    return register_vertex_key(std::vector<std::byte>(path.key().begin(), path.key().end()), role,
                               std::move(handlers));
}

result_t<vertex_handle_t> graph_t::register_vertex_key(std::vector<std::byte> key, role_t role,
                                                       handlers_t handlers) {
    const std::unique_lock lock(map_mutex_);
    // Descend the Composite tree (ADR-0057), creating unregistered PLACEHOLDER nodes for
    // missing intermediate levels — invisible to find/read_children until a registration
    // fills them in place (matching the flat map, where intermediates did not exist).
    // NOTHING is inherited (RFC-0022 §3.F). The descent carries no policy down with it:
    // with `settings_t` deleted there is no per-vertex policy left to inherit, so there is
    // no ancestor walk, no cached ancestor reference, and no question about what happens to
    // descendants when a parent's configuration changes after they exist. The two owner-side
    // magnitudes are declared per vertex, by the owner, or they are at their defaults.
    vertex_t* node = root_.get();
    std::size_t i = 0;
    while (i < key.size()) {
        const std::size_t e = segment_end(key, i);
        const std::span<const std::byte> record{key.data() + i, e - i};
        vertex_t* child = node->child_by_record(record);
        if (child == nullptr) {
            // A placeholder is a plain STORED_VALUE with no handlers, so `adopt_identity`'s
            // early return fires and it allocates NO extension block.
            auto fresh =
                std::make_unique<vertex_t>(role_t::STORED_VALUE, path_key_t{record}, handlers_t{});
            // Subtree-subscription init (RFC-0005): a vertex born under a subscribed
            // ancestor starts with the ancestor-listener count already summed — O(1) from
            // the parent's maintained counters (under the same unique lock the
            // note_subscriber_* walks exclude, so the sum and a concurrent subscribe walk
            // never double-count); the write path's is-anyone-listening check stays a
            // single relaxed load.
            fresh->init_listeners_above(node->listeners_above() + node->own_subs());
            child = node->add_child(std::move(fresh));
            // One slot per vertex_t ALLOCATION (RFC-0024 §6.4), appended under the same
            // unique map-lock hold that linked it in, so slot order is allocation order.
            // Placeholders take a slot too: they are ordinary vertex_t objects that a later
            // registration fills IN PLACE, so skipping them here would hand the same object
            // two different slots depending on which side of its fill() the mint happened.
            vertex_slots_.push_back(child);
        }
        node = child;
        i = e;
    }
    if (node->registered()) return std::unexpected(status_t::PATH_IN_USE);
    node->fill(role, std::move(handlers));
    return vertex_handle_t{node};
}

void graph_t::retire_subtree(vertex_t* v, std::vector<std::vector<std::byte>>& keys) {
    // Pre-order, under the UNIQUE map lock. Order within a vertex matters:
    //  (1) read its active-edge count and unwind exactly that contribution from every
    //      descendant's listeners_above_ BEFORE revert zeroes own_subs_ — the mirror of
    //      note_subscriber_removed, done inline because we already hold the unique lock
    //      its shared lock only needed to exclude vertex creation;
    //  (2) record the key for the caller's sweep-set cleanup;
    //  (3) revert the vertex's own state (fail-closed: clears own ACEs first, so the
    //      bearing-ancestor walk stops seeing this vertex before anything else changes);
    //  (4) flip it unregistered (map-lock state — invisible to find from here on);
    //  (5) recurse. Placeholders are walked too (§B.3): reverting one is a harmless no-op,
    //      but a registered descendant may hang below it.
    // Step (5) is now ITERATIVE rather than a recursive call per level (#690): the per-vertex
    // body is hoisted into one lambda applied to `v` and then, in the same pre-order, to every
    // descendant. Steps (1)-(4) keep their order within each vertex, which is what the note
    // above is about; only the descent changed.
    //
    // The lambda mutates vertex STATE (listeners, value seam, registered flag) but never the
    // tree's SHAPE, so it honours for_each_descendant's no-structural-mutation contract -- the
    // walk re-reads the sibling list on each ascent and an insert or erase mid-walk would move
    // the position it resumes from.
    const auto retire_one = [this, &keys](vertex_t& x) {
        const std::uint32_t k = x.own_subs();
        if (k > 0) bump_subtree_listeners(&x, -static_cast<std::int32_t>(k));
        keys.push_back(build_key(&x));
        // Park the detached value seam (if any — the seam exists iff a handler was installed
        // at registration, whatever the role): a lock-free reader may still hold the old
        // pointer, so it is never freed here. The park's other end is the public collect(),
        // which the embedder calls at a moment it knows no reader holds a seam (#576); the
        // graph's own teardown is a growth backstop only — retired_seams_ destructs LAST, so
        // a seam that re-enters the graph must be collected explicitly. Under map_mutex_.
        if (value_handlers_t* seam = x.revert_to_placeholder()) retired_seams_.emplace_back(seam);
        x.mark_unregistered();
    };
    retire_one(*v);
    v->for_each_descendant(retire_one);
}

std::uint32_t graph_t::retire_generation(vertex_handle_t vh) const noexcept {
    const vertex_t* const v = vh.get();
    return v == nullptr ? 0u : v->retire_gen();
}

std::size_t graph_t::vertex_slot_count() const noexcept {
    const std::shared_lock lock(map_mutex_);
    return vertex_slots_.size();
}

/**
 * @brief One anchor's NAME record — the whole of an anchor's key (#1223).
 *
 * `build_key` stops at the node whose parent is null, and an anchor's parent (`anchor_root_`)
 * is that node, so this single record IS the key retirement's sweep cleanup sees. The caller
 * composes @p id to contain characters `path::valid_segment` rejects, which is what makes the
 * rendered bytes unreachable from any address; framing it as an ordinary NAME record is what
 * keeps `key_view_t`'s decomposition well-defined over it.
 */
static std::vector<std::byte> anchor_record(std::string_view id) {
    std::vector<std::byte> rec;
    wire::emit_name(rec, id);
    return rec;
}

result_t<vertex_handle_t> graph_t::register_session_anchor(std::string_view id) {
    const std::vector<std::byte> rec = anchor_record(id);
    const std::unique_lock lock(map_mutex_);
    vertex_t* node = anchor_root_->child_by_record(rec);
    if (node == nullptr) {
        // FIRST sight of this id: one allocation, one slot, forever. Every later arrival on
        // the same id lands on the branch below and re-fills THIS object, which is the whole
        // bounded-across-churn property — a listener with `max_peers` slots can only ever ask
        // for `max_peers` distinct ids, so anchors are bounded by the accept policy and not
        // by how often clients reconnect (ADR-0044 §Amendment's measurement).
        auto fresh =
            std::make_unique<vertex_t>(role_t::STORED_VALUE, path_key_t{rec}, handlers_t{});
        node = anchor_root_->add_child(std::move(fresh));
        vertex_slots_.push_back(node);
    }
    // Live already ⇒ the caller is telling us about a session that never left. Refuse rather
    // than re-fill: a second fill() would NOT bump the generation (only retirement does), so
    // silently succeeding here would hand out a handle whose staleness signal never moved.
    if (node->registered()) return std::unexpected(status_t::PATH_IN_USE);
    // Revive in place — the same call `register_vertex_key` makes on a retired placeholder,
    // against the same object at the same slot. The generation was bumped by the RETIRE that
    // made this reachable, so the revived anchor already reads as a different tenancy to any
    // element minted against its predecessor.
    node->fill(role_t::STORED_VALUE, handlers_t{});
    return vertex_handle_t{node};
}

std::optional<vertex_handle_t> graph_t::find_session_anchor(std::string_view id) const {
    const std::vector<std::byte> rec = anchor_record(id);
    const std::shared_lock lock(map_mutex_);
    vertex_t* const node = anchor_root_->child_by_record(rec);
    if (node == nullptr || !node->registered()) return std::nullopt;
    return vertex_handle_t{node};
}

std::size_t graph_t::session_anchor_slots() const noexcept {
    const std::shared_lock lock(map_mutex_);
    std::size_t n = 0;
    anchor_root_->for_each_child([&n](const vertex_t&) { ++n; });
    return n;
}

std::optional<vertex_slot_t> graph_t::vertex_slot(vertex_handle_t vh) const noexcept {
    const vertex_t* const v = vh.get();
    if (v == nullptr) return std::nullopt;
    // ONE hold covers both fields. Retirement takes this lock uniquely, so the generation
    // read here cannot straddle a retire that the index read did not see; splitting them
    // into two acquisitions is how a mint ends up stamped with the SUCCESSOR tenant's
    // generation — a well-formed element naming a vertex the operation never reached.
    const std::shared_lock lock(map_mutex_);
    // Saturated ⇒ permanently unbindable (RFC-0024 §4.4 rule 3). Refused BEFORE the scan,
    // so the one caller that can be told "no" is told cheaply and falls back to canonical.
    // Read INSIDE the hold for the reason above: at the ceiling the difference between the
    // two orderings is an immortal element, not a stale one.
    const std::uint32_t gen = v->retire_gen();
    if (gen == kGenerationSaturated) return std::nullopt;
    for (std::size_t i = 0; i < vertex_slots_.size(); ++i) {
        if (vertex_slots_[i] == v)
            return vertex_slot_t{.index = static_cast<std::uint32_t>(i), .generation = gen};
    }
    return std::nullopt;  // not this graph's vertex — defensive, unreachable via the API.
}

std::optional<vertex_slot_t> graph_t::vertex_slot_at(std::uint32_t index) const noexcept {
    const std::shared_lock lock(map_mutex_);
    if (index >= vertex_slots_.size()) return std::nullopt;
    // Read under the same hold the index bound was tested under, for the reason
    // `vertex_slot` states: a generation read outside it can straddle a retire and stamp
    // the SUCCESSOR tenant's number onto an element the operation never reached.
    const vertex_t* const v = vertex_slots_[index];
    const std::uint32_t gen = v->retire_gen();
    if (gen == kGenerationSaturated) return std::nullopt;  // permanently unbindable (§4.4 r3)
    // A retired-but-not-yet-revived vertex is a PLACEHOLDER, and minting for one is how an
    // element outlives the tenancy it was issued against: `retire` bumps the generation and
    // clears `registered_`, so an element minted in that window already carries the number
    // the SUCCESSOR tenant will validate under, and `deref_vertex_slot` would honour it once
    // the vertex revives at the same path. The validate-on-use stamp is the whole guard here
    // (#511), so it has to be refused on the side that ISSUES an element as well as on the
    // side that honours one — the same symmetry rule 3 needed. A hop that cannot mint STRIPS
    // the mint answer (§7.1 erratum 1) and the origin stays canonical.
    if (!v->registered()) return std::nullopt;
    return vertex_slot_t{.index = index, .generation = gen};
}

bool graph_t::allows(vertex_handle_t v, std::string_view caller, acl_right_t right) const {
    return acl_allows(v.get(), caller, right);
}

std::optional<vertex_handle_t> graph_t::deref_vertex_slot(std::uint32_t index,
                                                          std::uint32_t generation) const noexcept {
    const std::shared_lock lock(map_mutex_);
    // Bounds check (RFC-0024 §5.1 step 1). Out of range is the only way the deref itself
    // could fault, and it cannot get past here.
    if (index >= vertex_slots_.size()) return std::nullopt;
    vertex_t* const v = vertex_slots_[index];
    // A SATURATED element is refused whatever the slot says (RFC-0024 §4.4 rule 3), and it
    // is refused HERE and not only at the mint. Everywhere below the ceiling "generations
    // only ever move forward" is the whole guard: a stale element compares lower and can
    // never become valid again. At the ceiling the counter stops, so that argument stops
    // too — a saturated element would keep matching its slot through every subsequent
    // retire and revive, delivering tenant A's operations into tenants B, C, D … forever
    // with no drop ever. That is #603's misroute with the guard in place of the address,
    // which is precisely what rule 3 exists to close, so "permanently unbindable" has to be
    // enforced on the side that HONOURS an element, not only on the side that issues one.
    //
    // Both halves live in `bound_generation_matches`, a total function, so the ceiling clause
    // is exercised by static_assert rather than by a test that would need 2^32 retirements.
    //
    // Generation compare (§5.1 step 2). A mismatch means the vertex was retired (and
    // possibly re-created for a DIFFERENT owner) since the mint, so the answer is discarded
    // rather than delivered into whatever now occupies the address.
    if (!bound_generation_matches(v->retire_gen(), generation)) return std::nullopt;
    // A retired-but-not-yet-revived vertex is a PLACEHOLDER: invisible to find/read, so the
    // bound form must not be the one spelling that reaches it. This is the map-lock state
    // the shared hold above is really for — it also covers the never-registered
    // intermediates, which hold slots (they are vertex_t allocations) but are no address.
    if (!v->registered()) return std::nullopt;
    // Authorization is NOT settled here (§6.2): the caller's op re-evaluates acl_allows at
    // this vertex, for its own right, exactly as the canonical spelling does.
    return vertex_handle_t{v};
}

result_t<void> graph_t::retire(vertex_handle_t vh) {
    vertex_t* root = vh.get();
    // The graph root has no parent and is not a retirable vertex.
    if (root == nullptr || root->parent() == nullptr)
        return std::unexpected(status_t::INVALID_PATH);

    std::vector<std::vector<std::byte>> retired_keys;
    {
        const std::unique_lock lock(map_mutex_);
        // Idempotent (§B.4): an already-retired / never-filled placeholder is a no-op.
        if (!root->registered()) return {};
        retire_subtree(root, retired_keys);
    }
    // Drop the retired vertices from the sweep sets — AFTER releasing the map lock, so no
    // map⊃sweep nesting is introduced. A stale entry would otherwise (a) leak, and worse
    // (b) silently re-enroll a revived vertex into UNCONDITIONAL sweeping through the
    // leaked key, overriding the IF_NEWER reset revert_to_placeholder just applied. A
    // concurrent sweep tolerates a not-yet-erased key: find_ptr skips the unregistered
    // vertex, so delivery never lands on a retired one either way.
    if (!retired_keys.empty()) {
        const std::lock_guard slock(sweep_mutex_);
        for (const std::vector<std::byte>& key : retired_keys) {
            unconditional_.erase(key);
            if (pending_.erase(key) != 0) pending_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    return {};
}

/**
 * @brief Free the parked value seams — the embedder-called other end of retirement's park
 *        (#576). The whole point is WHERE the free happens, so read the two scopes below.
 */
void graph_t::collect() {
    std::vector<std::unique_ptr<value_handlers_t>> dead;
    {
        // Under the map lock: nothing but the swap. The lock is what serialises us against
        // retire_subtree's append, and it is all it is here for — a free under it would put
        // arbitrary user-callback destructor code inside the graph's widest lock, which is
        // the mutual-wait every earlier design round died on.
        const std::unique_lock lock(map_mutex_);
        dead.swap(retired_seams_);
    }
    // `dead` destructs HERE — outside every graph lock, on the caller's thread, at a moment
    // the embedder chose. So a seam callback's destructor may re-enter the graph, and a slow
    // one blocks no reader or writer. Do not hoist this into the scope above.
}

std::size_t graph_t::parked_seam_count() const {
    const std::shared_lock lock(map_mutex_);
    return retired_seams_.size();
}

/**
 * @brief Pre-order collect of every vertex holding at least one active subscriber slot —
 *        the snapshot half of @ref graph_t::evict_link_edges. Call with `map_mutex_` held
 *        (shared suffices: the walk only excludes concurrent vertex creation, `own_subs`
 *        is a relaxed atomic, and vertex addresses are pinned for the graph's lifetime —
 *        ADR-0057 — so the collected pointers stay valid past the lock).
 */
static void collect_subscribed(vertex_t* v, std::vector<vertex_t*>& out) {
    // Iterative (#690): the recursive form cost ~80 B a frame at a depth no peer-facing check
    // bounds. `out` still grows on the heap -- that is the caller's snapshot, not the walk.
    const auto take = [&out](vertex_t& c) {
        if (c.own_subs() > 0) out.push_back(&c);
    };
    take(*v);
    v->for_each_descendant(take);
}

std::size_t graph_t::evict_link_edges(std::string_view link_name) {
    // Two-phase, per the graph.hpp lock-order docs: snapshot the candidate vertices under
    // ONE shared map hold (no stripe lock inside), then evict per vertex — each under its
    // own stripe lock inside a FRESH shared map hold. The per-vertex hold makes the
    // {clear edges, unwind counters} pair atomic against a concurrent retire() (unique
    // map lock), which reads own_subs() before zeroing it — interleaving there would
    // double-subtract descendants' listeners_above_. Between vertices everything may
    // interleave: a vertex retired meanwhile has an empty edge list (k == 0, no-op), and
    // a subscribe admitted meanwhile for a DEAD link is the pre-existing races' window,
    // resolved by the transport calling this hook after the link stopped delivering.
    std::vector<vertex_t*> candidates;
    {
        const std::shared_lock lock(map_mutex_);
        collect_subscribed(root_.get(), candidates);
    }
    std::size_t total = 0;
    for (vertex_t* v : candidates) {
        const std::shared_lock lock(map_mutex_);
        const std::size_t k = v->evict_link_edges(link_name);
        if (k == 0) continue;
        // The k-fold mirror of note_subscriber_removed, under the same shared hold as
        // the clear (RFC-0005 bookkeeping: descendants' writes stop bubbling here).
        v->bump_own_subs(-static_cast<std::int32_t>(k));
        bump_subtree_listeners(v, -static_cast<std::int32_t>(k));
        total += k;
    }
    return total;
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

std::uint64_t graph_t::target_canonical_resolves() const noexcept {
    return target_canonical_resolves_.load(std::memory_order_relaxed);
}

std::uint64_t graph_t::ancestor_walks() const noexcept {
    return ancestor_walks_.load(std::memory_order_relaxed);
}

graph_t::delivery_drops_t graph_t::delivery_drops() const noexcept {
    return {.no_target = drops_no_target_.load(std::memory_order_relaxed),
            .denied = drops_denied_.load(std::memory_order_relaxed),
            .out_of_memory = drops_oom_.load(std::memory_order_relaxed),
            .fan_out_truncated = drops_truncated_.load(std::memory_order_relaxed)};
}

void graph_t::count_drop(drop_reason_t why, std::uint64_t n) noexcept {
    // The one counting door (#896). Exhaustive on purpose — no default: a new reason
    // must choose a counter here, it cannot fall through into silence, which is the
    // exact failure this centralization is fixing.
    switch (why) {
        case drop_reason_t::NO_TARGET:
            drops_no_target_.fetch_add(n, std::memory_order_relaxed);
            return;
        case drop_reason_t::DENIED:
            drops_denied_.fetch_add(n, std::memory_order_relaxed);
            return;
        case drop_reason_t::OUT_OF_MEMORY:
            drops_oom_.fetch_add(n, std::memory_order_relaxed);
            return;
        case drop_reason_t::FAN_OUT_TRUNCATED:
            drops_truncated_.fetch_add(n, std::memory_order_relaxed);
            return;
    }
}

void graph_t::count_external_drop(external_drop_t why, std::uint64_t n) noexcept {
    // Translate the narrow public cause into the internal one and go through the SAME door
    // every in-graph site uses (#1068). The mapping is total and the switch exhaustive, so a
    // cause added to the public enum must choose an internal counter here rather than
    // silently counting nothing — the failure mode this whole centralization exists against.
    switch (why) {
        case external_drop_t::NO_TARGET:
            count_drop(drop_reason_t::NO_TARGET, n);
            return;
        case external_drop_t::OUT_OF_MEMORY:
            count_drop(drop_reason_t::OUT_OF_MEMORY, n);
            return;
    }
}

void graph_t::count_snapshot_drops(const vertex_t::snapshot_drops_t& drops) noexcept {
    if (drops.out_of_memory != 0) count_drop(drop_reason_t::OUT_OF_MEMORY, drops.out_of_memory);
    if (drops.truncated != 0) count_drop(drop_reason_t::FAN_OUT_TRUNCATED, drops.truncated);
}

void graph_t::count_store_drops(vertex_t* v, const vertex_t::store_drops_t& drops) noexcept {
    if (!drops.ring_append) return;  // the clean write pays exactly this test
    // A shed STREAM ring append under memory pressure. For a STREAM the drain IS the fan-out,
    // so the entry that never entered the ring is a delivery every subscriber loses — counted
    // at the width it sheds, one per subscriber, never one per event. Same width, same cause
    // and same reasoning as the handler notify-clone leg that sheds a whole fan-out.
    //
    // own-subs-wide by DECISION, not by oversight: the ancestor legs a bubble would also have
    // served stay uncounted because #854's close ruling dropped ancestor-leg drop
    // instrumentation outright. A vertex with no subscribers of its own counts nothing, which
    // is why this is guarded rather than a bare add of zero.
    if (const std::uint64_t n = v->own_subs(); n != 0) count_drop(drop_reason_t::OUT_OF_MEMORY, n);
}

void graph_t::bump_subtree_listeners(vertex_t* v, std::int32_t delta) {
    // Iterative (vertex_t::for_each_descendant): this was self-recursion at ~208 B a frame, and
    // graph depth is a peer-chosen segment count -- see #690 and that function's docs.
    v->for_each_descendant([delta](vertex_t& c) { c.bump_listeners_above(delta); });
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
    // never destroyed while the graph lives. `retire()` (RFC-0009) upholds this — it
    // marks a vertex unregistered and EMPTIES it (re-virginize), but never detaches or
    // frees, so a held handle stays dereferenceable. Any future reclamation still needs
    // a real lifetime scheme (refcount / epoch); a bare detach would dangle these.
    return node->registered() ? node : nullptr;
}

std::vector<std::byte> graph_t::build_key(const vertex_t* v) {
    // Render-on-demand full key (ADR-0057): ancestors' NAME records concatenated
    // root-down. Parent links and name bytes are immutable — no lock. Two passes: size,
    // then a single exact allocation filled deepest-record-last.
    std::size_t total = 0;
    for (const vertex_t* n = v; n->parent() != nullptr; n = n->parent()) total += n->name().size();
    std::vector<std::byte> key(total);
    std::size_t w = total;
    for (const vertex_t* n = v; n->parent() != nullptr; n = n->parent()) {
        const std::span<const std::byte> rec = n->name().bytes();
        w -= rec.size();
        std::copy(rec.begin(), rec.end(), key.begin() + static_cast<std::ptrdiff_t>(w));
    }
    return key;
}

bool graph_t::try_build_key(const vertex_t* v, std::vector<std::byte>& out) noexcept {
    // The NOTHROW twin of build_key for the writer-thread store/delivery legs (#477):
    // the single exact allocation is a throwing vector construction there — an abort()
    // under -fno-exceptions on OOM — so those call sites render through this and drop
    // (or defer) their leg on failure instead. Same two-pass fill, same immutability
    // guarantees; read-plane callers keep build_key.
    std::size_t total = 0;
    for (const vertex_t* n = v; n->parent() != nullptr; n = n->parent()) total += n->name().size();
    out.clear();
    if (!detail::try_reserve(out, total)) return false;
    out.resize(total);  // within capacity — no reallocation, cannot throw
    std::size_t w = total;
    for (const vertex_t* n = v; n->parent() != nullptr; n = n->parent()) {
        const std::span<const std::byte> rec = n->name().bytes();
        w -= rec.size();
        std::copy(rec.begin(), rec.end(), out.begin() + static_cast<std::ptrdiff_t>(w));
    }
    return true;
}

std::optional<vertex_handle_t> graph_t::find(std::span<const std::byte> key) const {
    vertex_t* p = find_ptr(key);
    if (p == nullptr) return std::nullopt;
    return vertex_handle_t{p};
}

bool graph_t::has_first_level_child(std::span<const std::byte> record) const {
    const std::shared_lock lock(map_mutex_);
    // Placeholder-inclusive, unlike find_ptr (which excludes unregistered intermediates):
    // the router shadows a first-level segment whether the top-level vertex is registered
    // or a mere structural parent, so the shadow test must see both.
    return root_->child_by_record(record) != nullptr;
}

std::uint32_t graph_t::pin_payload_ratio(vertex_handle_t v) const noexcept {
    return v.get()->pin_payload_ratio();
}

void graph_t::set_history_depth(vertex_handle_t v, std::uint32_t keep) {
    v.get()->set_history_depth(keep);
}

void graph_t::set_pin_payload_ratio(vertex_handle_t v, std::uint32_t k) {
    v.get()->set_pin_payload_ratio(k);
}

void graph_t::set_subject_resolver(subject_resolver_t resolver) {
    subject_resolver_ = std::move(resolver);
}

bool graph_t::acl_allows(vertex_t* v, std::string_view caller, acl_right_t right) const {
    if (!subject_resolver_) return true;  // enforcement disabled — the one hot-path check
    // The trusted channel is the EMPTY caller context — a local API call — settled HERE,
    // before the resolver runs (#905). It used to be a resolver return value (`nullopt`),
    // whose natural reading ("I cannot name this caller") meant "grant everything", WRITE_ACL
    // and CREATE included. A remote op carries the inbound link's NAME, so it cannot spell
    // this arm: the full-route form through `ensure_remote().caller`, and — since #974 — the
    // COMPACT delivery fast path, whose two terminus write arms in `fwd_router_t::on_compact`
    // pass `inbound_name` too. #974 was that second one missing: unattributed, it landed here
    // and was waved through every ACE the first is checked against. Any further net-plane
    // write path must carry a caller for the same reason — which is why
    // `fwd_router_t::deliver_local` takes its own as a REQUIRED, undefaulted parameter.
    if (caller.empty()) return true;
    const std::expected<subject_token_t, wire::err_t> subject = subject_resolver_(caller);
    if (!subject) return false;  // the resolver DENIED this caller — PERMISSION_DENIED
    // The wildcard spelling is RESERVED in the subject-token space (#908): the wire has one
    // spelling for a subject, so a principal that could BE `EVERYONE@` is indistinguishable
    // from the wildcard ACE. Enforced HERE, which is the only site that invokes
    // `subject_resolver_` at all, rather than left to every integrator to blacklist — and
    // BEFORE the bearing-ancestor walk,
    // so a misconfigured resolver is refused at an unguarded vertex too. Fail closed, exactly
    // like the resolver's own error arm above.
    if (is_reserved_subject(*subject)) return false;
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
        [&](const std::vector<ace_t>& merged) {
            // A bare descendant evaluates the INHERITABLE SUBSEQUENCE of the bearer's merge.
            // Filtered in place (order-identical) rather than against a second, projected
            // vector — see effective_acl_t::allows.
            return effective_acl_t::allows(merged, *subject, bit, now,
                                           self ? std::uint8_t{0} : kAceInherit);
        });
}

void graph_t::mark_subtree_acl_dirty(vertex_t* v) {
    // Iterative (#690). Reached from the `:acl` write, so its depth is peer-chosen.
    v->mark_acl_cache_dirty();
    v->for_each_descendant([](vertex_t& c) { c.mark_acl_cache_dirty(); });
}

result_t<value_ref_t> graph_t::read(vertex_handle_t vh, std::string_view caller) const {
    vertex_t* v = vh.get();
    if (!acl_allows(v, caller, acl_right_t::READ))
        return std::unexpected(status_t::PERMISSION_DENIED);
    if (v->role() == role_t::HANDLER) {
        // Load the seam ONCE: it is an atomic pointer a concurrent retire may swap to
        // null (RFC-0009 §B.6), so a check-then-call across two loads would race — the
        // second load could see the cleared seam and throw bad_function_call. The parked
        // block keeps this reference valid even if the swap fires right after the load.
        const value_handlers_t& h = v->handlers();
        // The handler seam is rope-valued (ADR-0053 section 6), so a handler read COMPOSES:
        // it costs one control block that the published path does not pay. Converting the
        // seam itself is a separate, lateral change.
        if (h.on_read) {
            auto produced = h.on_read();
            if (!produced) return std::unexpected(produced.error());
            return value_ref_t::composed(std::move(*produced));
        }
        return std::unexpected(status_t::NOT_FOUND);
    }
    // The branch/leaf fork (RFC-0005 §C follow-on): a vertex with ≥ 1 registered child
    // serves the composed branch read — the folded POINT tree of its registered
    // descendants' landed LKVs. AFTER the handler seam (a HANDLER target's on_read keeps
    // precedence); a leaf falls through to the LKV path byte-identically to before.
    if (v->has_registered_child()) {
        // The composed branch read BUILDS a value, so it wraps rather than shares. Measured
        // 1.00x against the old copy-out (30 paired samples): the subtree walk dominates the
        // one control block this costs.
        auto folded = read_subtree_folded(vh, caller);
        if (!folded) return std::unexpected(folded.error());
        return value_ref_t::composed(std::move(*folded));
    }
    std::shared_ptr<const rope_t> sp = v->read_stored();  // lock-free
    if (!sp) return std::unexpected(status_t::NOT_FOUND);
    // The published value is handed back BY REFERENCE. This used to be `return *sp`, which
    // copied the rope and so cloned one segment_ptr_t per link — a contended refcount RMW per
    // link, on the line every reader of this vertex shares.
    return value_ref_t{std::move(sp)};
}

namespace {
/**
 * @brief The NOTHROW delivery clone of a stored value (#477), ONE non-inline copy for
 *        both writer-thread clone legs (target-edge dispatch, handler notify): a spilled
 *        (> kInline links) rope's copy grows a heap chain that threw bad_alloc — an
 *        abort() under `-fno-exceptions`. `try_reserve` is a no-op while the chain fits
 *        inline (the hot case), and on OOM the caller drops that delivery leg.
 * @retval false The chain could not be reserved — @p dst is empty, drop the leg.
 */
[[nodiscard]] bool try_clone_rope(rope_t& dst, const rope_t& src) noexcept {
    if (!dst.try_reserve(src.link_count())) return false;
    dst.concat(src);  // reserved (or inline) — the appends cannot reallocate
    return true;
}
}  // namespace

void graph_t::dispatch_edge_target(const edge_view_t& e, const rope_t& value) {
    // The bound spelling first (#830): a slot deref is a bounds check, a slot load and a
    // generation compare — flat at every address depth — where `find_ptr` walks the key
    // segment by segment. `deref_vertex_slot` refuses a stale generation, a saturated one, an
    // out-of-range index and an unregistered (placeholder) vertex, so a refusal here means
    // "this answer is no longer trustworthy", NOT "no target": we fall through to the
    // canonical walk, which is the same resolution the edge did before this cache existed.
    // Drop-never-misroute (RFC-0024 §5.1) is therefore preserved with no drop added at all.
    vertex_t* target = nullptr;
    if (e.binding.bound()) {
        if (const auto bound = deref_vertex_slot(e.binding.index, e.binding.generation))
            target = bound->get();
    }
    if (target == nullptr) {
        target_canonical_resolves_.fetch_add(1, std::memory_order_relaxed);
        target = find_ptr(*e.target_key);
    }
    // Each of the three drops below is counted before returning (delivery_drops()). The
    // drop itself is specified — this leg fails alone and the write still succeeded — but
    // an UNCOUNTED drop is indistinguishable from a delivery that never had to happen, for
    // an operator and for a benchmark alike.
    if (target == nullptr) {
        count_drop(drop_reason_t::NO_TARGET, 1);
        return;
    }
    // Fan-in gate (#81, ADR-0026): the delivery is an ordinary write to the target,
    // gated by the TARGET's :acl WRITE right under the edge's stored caller context.
    // Denial drops this delivery.
    if (!acl_allows(target, e.caller, acl_right_t::WRITE)) {
        count_drop(drop_reason_t::DENIED, 1);
        return;
    }
    // Delivery TERMINATES at the target (ADR-0051 / RFC-0007): apply exactly the
    // target-local effects of a write — store (LKV/history per role), await wake, and the
    // target's own handler reaction (all inside store_value) — and NEVER re-dispatch to the
    // target's own :subscribers[], never bubble. Propagation past a target is exclusively
    // the target's own logic (a controller re-emits on its execution; a handler re-emits
    // when it chooses), so a dispatch-level subscription cycle cannot form — no depth cap,
    // no dedup, no drain queue. An app wanting pure relay subscribes the consumer directly.
    rope_t clone;  // the NOTHROW delivery clone (#477) — on OOM this one leg drops
    if (!try_clone_rope(clone, value)) {
        count_drop(drop_reason_t::OUT_OF_MEMORY, 1);
        return;
    }
    // Delivery TERMINATES here, so the target's own edges are never dispatched by this write —
    // but a STREAM target's ring still feeds the next propagate over it, exactly as an assign's
    // does. A shed append therefore loses that deferred delivery, and is counted at the
    // TARGET's own-subs width (the shed is the target's, not the source's).
    vertex_t::store_drops_t store_drops;
    (void)store_value(target, std::move(clone), store_drops);
    count_store_drops(target, store_drops);
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
inline void graph_t::dispatch_edge(const edge_view_t& e, const rope_t& value) {
    // The ONE dispatch of a subscription edge's three legs — shared by the per-write
    // fan_out and the admission durability latch (ADR-0049), so the legs cannot diverge.
    // Always called OUTSIDE the vertex lock (each leg may re-enter the graph or do I/O).
    if (e.callback != nullptr)
        e.callback(e.callback_ctx, value);  // the rope by const ref (sink may clone links)
    if (e.target_key) dispatch_edge_target(e, value);
    if (!e.link.empty() && remote_sink_) dispatch_edge_remote(e, value);
}

void graph_t::fan_out(vertex_t* v, const rope_t& value) {
    // NOBODY SUBSCRIBED HERE ⇒ do no snapshot work at all (#635). When this gate landed
    // `snapshot_edges` took the vertex STRIPE mutex, shared by kVertexLockStripes-many
    // vertices, so without it two unrelated vertices serialised their writes on nothing but a
    // hash collision — ×8.6 (fan-0) and ×12.3 (fan-1) against the same write on distinct
    // stripes, and NEGATIVELY scaling. The fan-1 half has since deleted that mutex from the
    // read path, so this gate now saves the pin claim and the copy loop, not a lock. RFC-0005's
    // near-free promise is kept this way by mark_pending; fan_out was the verb that did not.
    //
    // This is the delivery-skipping read, so it is the ORDERED one — see
    // vertex_t::own_subs_ordered for the Dekker pairing against ADR-0049's subscribe latch,
    // and graph_t::field_write for the bump that has to precede the slot append to close it.
    // Unsubscribe needs no such care: its decrement lands AFTER clear_edge, so a stale
    // non-zero count only costs a snapshot that finds nothing.
    //
    // It does NOT gate bubbling: an ancestor's subscribers are listeners_above_'s business
    // and both callers check that separately.
    if (v->own_subs_ordered() == 0) return;

    // Snapshot every active edge UNDER AN EDGE PIN (vertex_t::snapshot_edges), released
    // before we dispatch (callbacks / re-dispatch may re-enter the graph). Delivery is
    // value-agnostic — no per-subscriber comparison — so every active edge receives
    // `value`; WHICH vertices propagate is the per-vertex delivery_mode decided by the
    // sweep (RFC-0008). Small fan-out (the common case) placement-constructs into a RAW
    // stack buffer — no per-publish heap allocation and no dead stack zeroing (an
    // edge_view_t array default-construct cost ~18 ns/op of rep-stos zeroing here).
    edge_snapshot_t inline_buf;

    // Wide fan-out (> kInlineFanout) used to malloc a fresh overflow vector EVERY publish
    // — the wide-fan-out alloc cliff (jitter on the very path that is the fan-out moat).
    // Reuse ONE persistent thread-local buffer instead, so a WARM wide publish is zero-alloc
    // (its capacity survives across publishes). Gated on the lock-free own_subs() count so
    // the small-fan-out hot path (incl. the fan-1-vs-Zenoh path) pays NO TLS cost.
    // snapshot_edges re-checks the width under the lock, so a race on the count only costs a
    // rare fallback alloc, never correctness. Re-entrancy: dispatch runs OUTSIDE the lock and
    // a subscriber callback may re-publish (a nested wide fan_out) — the `busy` flag detects
    // that and routes the nested call to a fresh local buffer below, so the outer's buffer is
    // never aliased. The flag resets on scope exit (incl. an exception out of dispatch), so a
    // throwing callback can't wedge the thread onto the slow path.
    if (v->own_subs() > vertex_t::kInlineFanout) {
        static thread_local std::vector<edge_view_t> tls_buf;
        static thread_local bool tls_busy = false;
        if (!tls_busy) {
            tls_busy = true;
            struct reset_t {
                bool& b;
                ~reset_t() noexcept { b = false; }
            } reset{tls_busy};
            tls_buf.clear();  // keeps capacity — the amortised-zero-alloc reuse
            vertex_t::snapshot_drops_t drops;
            const std::size_t n = v->snapshot_edges(inline_buf, tls_buf, drops);
            // Fold BEFORE dispatching: these deliveries were abandoned inside the
            // snapshot, and dispatch re-enters the graph (a callback may publish, and a
            // nested publish must not be able to swallow this tally).
            if (drops.any()) count_snapshot_drops(drops);
            if (tls_buf.empty())
                for (std::size_t i = 0; i < n; ++i) dispatch_edge(inline_buf[i], value);
            else
                for (const edge_view_t& e : tls_buf) dispatch_edge(e, value);
            return;
        }
        // Nested wide fan_out (rare): fall through to a fresh local buffer.
    }

    // Small fan-out (or a nested wide fan-out): fill the stack buffer; the empty local vector
    // never allocates unless the overflow path above genuinely spilled.
    std::vector<edge_view_t> heap_buf;
    vertex_t::snapshot_drops_t drops;
    const std::size_t n = v->snapshot_edges(inline_buf, heap_buf, drops);
    if (drops.any()) count_snapshot_drops(drops);
    if (heap_buf.empty())
        for (std::size_t i = 0; i < n; ++i) dispatch_edge(inline_buf[i], value);
    else  // count race (a subscriber was added between own_subs() and the lock): one alloc
        for (const edge_view_t& e : heap_buf) dispatch_edge(e, value);
}

result_t<std::shared_ptr<const rope_t>> graph_t::store_value(vertex_t* v, rope_t&& value,
                                                             vertex_t::store_drops_t& drops) {
    drops = vertex_t::store_drops_t{};
    if (v->role() == role_t::HANDLER) {
        const value_handlers_t& h = v->handlers();  // load once — a retire may swap it out
        if (!h.on_write) return std::unexpected(status_t::NOT_FOUND);
        result_t<void> r = h.on_write(value);
        if (!r) return std::unexpected(r.error());
        v->note_write();
        return std::shared_ptr<const rope_t>{};  // handler consumed it — nothing stored
    }
    // The storage verb owns the invariant order: LKV publish (lock-free) BEFORE the
    // lock; ring append + keep-last trim + seq bump + await wake under it.
    std::shared_ptr<const rope_t> sp = v->store(std::move(value), mr_, &drops);
    // vertex_t::store soft-fails its LKV allocation nothrow (#477): null here (a
    // non-handler role always publishes a pointer) is exactly OOM — report it as the
    // injected-resource status (BACKPRESSURE, ADR-0060 §3), never abort. Distinct from
    // the handler leg above, whose null shared_ptr is the "consumed, nothing stored"
    // SUCCESS sentinel.
    if (!sp) return std::unexpected(status_t::BACKPRESSURE);
    return sp;
}

void graph_t::bubble_up(vertex_t* v, const rope_t& value) {
    // Entered only when v->listeners_above() says an ancestor subscriber exists —
    // the idle write path never walks (RFC-0005 §near-free-when-idle; the counter
    // below is what tests/benches assert on via ancestor_walks()).
    ancestor_walks_.fetch_add(1, std::memory_order_relaxed);
    // Parent pointers are immutable once linked (ADR-0057), so the walk takes NO lock —
    // the old per-ancestor find_ptr (a shared-lock + hash lookup per level) is gone. A
    // placeholder ancestor holds no edges, so its fan_out is the no-op the old walk's
    // lookup miss was; the root node is the final (empty-key) stop, as before.
    for (vertex_t* ancestor = v->parent(); ancestor != nullptr; ancestor = ancestor->parent())
        fan_out(ancestor, value);
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

result_t<void> graph_t::write_impl(vertex_t* v, rope_t value, std::string_view caller) {
    // The ONE WRITE gate of the value-write path, so counting the refusal here counts it for
    // every plane that enters through it: an API write, a FWD{WRITE} terminus, and both the
    // warm and cold COMPACT terminus arms (#1068). The router discards this status — it has
    // no caller to hand it to — so if the denial were not counted at the gate that produces
    // it, a revoked peer streaming into a protected vertex would look exactly like a quiet
    // link. Counting an API caller's own denial as well is deliberate (see delivery_drops_t
    // ::denied): `denied` means refusals, not refusals-nobody-heard-about, and a counter
    // whose value depended on WHICH door a refusal came through could not be summed.
    if (!acl_allows(v, caller, acl_right_t::WRITE)) {
        count_drop(drop_reason_t::DENIED, 1);
        return std::unexpected(status_t::PERMISSION_DENIED);
    }
    // `write` is the RFC-0008 §D composition — assign the vertex, then deliver exactly
    // what it stored (a leaf VALUE, or each landed descendant of a branch POINT). This is
    // the FWD{WRITE}-terminus behavior: a TARGETED delivery of the written vertex(es), not
    // a subtree sweep. propagate(v) is the separate accumulate-then-flush primitive.
    if (is_branch_point(value, v->role())) return write_branch(v, value, caller, /*notify=*/true);
    if (v->role() == role_t::HANDLER) {
        // A handler stores no LKV (the user handler consumes the value), so the
        // delivery clone survives here — the cold path only. The hot roles below
        // deliver the exact published pointer store_value hands back instead. The
        // clone is NOTHROW (try_clone_rope, #477): on failure the handler still runs
        // (the write succeeds) and only the subscriber delivery drops.
        // Refcount clone taken BEFORE the call: store_value moves from `value` on the
        // storing legs. It does not on the Handler leg — that one only reads `value` and
        // returns — so since #1116 (`rope_t&&`) the caller's rope survives that path.
        rope_t notify;
        const bool can_notify = try_clone_rope(notify, value);
        // A HANDLER stores no LKV and owns no ring, so this tally is structurally clean —
        // required by the signature, and that is the point: the seam cannot be skipped.
        vertex_t::store_drops_t store_drops;
        const result_t<std::shared_ptr<const rope_t>> stored =
            store_value(v, std::move(value), store_drops);
        if (!stored) return std::unexpected(stored.error());
        if (can_notify) {
            deliver_vertex(v, notify);
        } else {
            // A failed clone sheds the ENTIRE fan-out, not one leg: no edge of this
            // vertex is dispatched, and the write still returns success. Keeping the
            // write successful is right (the handler ran; un-running it is impossible),
            // but the drop is the widest one in the graph, so it is counted at the width
            // it sheds — one per subscriber, never one per event. Counted here, at the
            // frame that abandons the delivery, because no inner frame is entered at all.
            // The ancestor legs a bubble would also have served are deliberately not
            // counted, and stay that way: #854's close ruling dropped the ancestor-leg
            // drop instrumentation outright, so this tally is own-subs-wide by decision.
            count_drop(drop_reason_t::OUT_OF_MEMORY, v->own_subs());
        }
        // Eager delivery flushes any pending mark a prior assign left — but only while
        // what this write published is still current (#1185); on the handler leg that is
        // the null "consumed" sentinel, matching the handler's permanently null LKV.
        clear_pending(v, *stored);
        return {};
    }
    vertex_t::store_drops_t store_drops;
    const result_t<std::shared_ptr<const rope_t>> stored =
        store_value(v, std::move(value), store_drops);
    if (!stored) return std::unexpected(stored.error());
    if (v->role() == role_t::STREAM) {
        // Deliver the just-appended ring entry and advance the drain cursor, so a later
        // propagate on this stream does not re-deliver it (RFC-0008 §E).
        //
        // If the append was SHED, this drain finds nothing and returns before a single edge
        // is snapshotted — the whole fan-out abandoned without ever reaching the dispatch
        // plane's counting sites (#1003). The write still succeeds; the tally is what makes
        // the loss something an operator can see.
        count_store_drops(v, store_drops);
        deliver_current(v);
    } else {
        // Deliver exactly what was stored (RFC-0008 §D): the published LKV pointer —
        // no notify reclone of the rope on the hot write path.
        deliver_vertex(v, **stored);
    }
    // Eager delivery flushes any pending mark a prior assign left — but only while what
    // this write published is still v's current LKV (#1185).
    clear_pending(v, *stored);
    return {};
}

result_t<void> graph_t::assign(vertex_handle_t vh, rope_t value, std::string_view caller) {
    vertex_t* v = vh.get();
    if (!acl_allows(v, caller, acl_right_t::WRITE))
        return std::unexpected(status_t::PERMISSION_DENIED);
    // The STATE half only (RFC-0008 §A): swap the last-known-value / append the stream
    // ring / bump the write sequence (waking await), then mark v for the next covering
    // sweep. A branch POINT assigns each descendant the same way. Sends nothing.
    if (is_branch_point(value, v->role())) return write_branch(v, value, caller, /*notify=*/false);
    vertex_t::store_drops_t store_drops;
    const result_t<std::shared_ptr<const rope_t>> stored =
        store_value(v, std::move(value), store_drops);
    if (!stored) return std::unexpected(stored.error());
    // A shed ring append here loses the delivery the NEXT covering sweep would have drained
    // — deferred, not eager, but lost all the same, and the sweep has no way to know an
    // entry was ever meant to be there.
    count_store_drops(v, store_drops);
    mark_pending(v);
    return {};
}

result_t<void> graph_t::write_branch(vertex_t* v, const rope_t& value, std::string_view caller,
                                     bool notify) {
    // A decomposable POINT is contiguous, so decode reads the materialized head:
    // single-link (the ④a case — ingress values are single-link until ④b), that is
    // the sole link with zero copy; a multi-link POINT pays one flatten here (the
    // interim until the ④b rope-cursor decode). Every node span points into `head`,
    // so each landed slice is head.subview(...) — a refcount bump, never a byte copy.
    // The flatten draws from the ADR-0060 value_backend_ — the whole decomposition's
    // durable bytes then live in one pooled segment. The refusal keeps its cause (#917):
    // an exhausted pool surfaces as BACKPRESSURE (§3 — transient, a retry may succeed)
    // rather than letting decode_into read an empty head back as a malformed value, while
    // a DEVICE-link value, which no retry makes CPU-decodable, is TYPE_MISMATCH.
    const std::expected<view_t, tr::view::flatten_err_t> head =
        value.try_materialize(*value_backend_);
    if (!head) {
        return std::unexpected(head.error() == tr::view::flatten_err_t::NO_MEMORY
                                   ? status_t::BACKPRESSURE
                                   : status_t::TYPE_MISMATCH);
    }
    // The #477 residual is CLOSED (#588). This used to be a
    // `std::pmr::monotonic_buffer_resource` over the same stack buffer, whose overflow
    // leg drew from the THROWING default upstream — so a branch tree bigger than the
    // slab reached `__cxa_throw`'s abort() stub on a -fno-exceptions node. A
    // `bump_source_t` carves from the same buffer and, past it, falls back to the
    // NOTHROW heap source: capability is unchanged (a big tree still decodes), but
    // exhaustion is now a value. No node-counting pre-pass was needed after all — the
    // seam alone was the missing piece.
    // The overflow leg draws from the graph's injected control seam, not the global heap:
    // a bounded node that injected `ctl` gets its own store here too, and the default
    // (heap_source) reproduces today's behaviour exactly.
    std::array<std::byte, 4096> stack;
    mem::bump_source_t src(stack, *ctl_);
    const std::expected<wire::tlv_arena_t, wire::err_t> arena =
        wire::decode_into(head->bytes(), src);
    if (!arena) return std::unexpected(status_t::TYPE_MISMATCH);
    const wire::tlv_arena_t& a = *arena;

    // The root POINT's leading NAME must name this vertex (the written tree is
    // rooted AT `v`); a mismatch is an addressing error, not a shape error.
    const std::uint32_t n0 = wire::tlv_arena_t::first_child(0);
    if (n0 >= a.root().end || a[n0].type != type_t::NAME)
        return std::unexpected(status_t::TYPE_MISMATCH);
    if (!std::ranges::equal(a[n0].body, key_view_t{v->name().bytes()}.last_segment()))
        return std::unexpected(status_t::INVALID_PATH);

    // The written tree is rooted AT `v`: render its full key once (ADR-0057
    // render-on-demand) — the node-key prefix of the whole decomposition plan. The key
    // render and its parse copy are NOTHROW (#477): OOM soft-fails the branch write as
    // BACKPRESSURE, the injected-resource status, never an abort on the writer thread.
    std::vector<std::byte> root_key;
    if (!try_build_key(v, root_key)) return std::unexpected(status_t::BACKPRESSURE);
    std::vector<std::byte> parse_key;
    if (!detail::try_assign(parse_key, root_key)) return std::unexpected(status_t::BACKPRESSURE);
    std::vector<branch_node_t> plan;  // post-order; plan.back() is the root
    const result_t<bool> parsed = parse_branch_node(a, 0, *head, std::move(parse_key), plan);
    if (!parsed) return std::unexpected(parsed.error());
    if (!*parsed) return {};  // a value-free branch is a no-op write

    // Admission: resolve-or-create every landing vertex (write-creates, CREATE-
    // gated) and gate WRITE on each BEFORE any store, so a denial rejects the
    // whole branch with nothing landed. (Created-but-empty intermediates may
    // persist past a later denial — the `mkdir -p` analogy; RFC-0005 §ACL.)
    struct site_t {
        vertex_t* vx;
        const branch_node_t* node;
        // What this branch's own store published here — null until the apply loop below,
        // and on a site whose store soft-failed. clear_pending compares it against the
        // vertex's current LKV so a racing assign's mark keeps its delivery (#1185); a
        // null on a vertex that holds an LKV simply fails that compare, which is the safe
        // direction (a duplicate delivery, never a lost one).
        std::shared_ptr<const rope_t> stored;
    };
    std::vector<site_t> sites;
    if (!detail::try_reserve(sites, plan.size()))  // nothrow (#477): OOM => BACKPRESSURE
        return std::unexpected(status_t::BACKPRESSURE);
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
        sites.push_back(site_t{vx, &node, nullptr});
    }

    // Apply: land every slice. Admission was atomic; application is per-vertex and
    // best-effort (a handler-role landing site may refuse its slice without
    // un-landing the others) — the branch is NOT a transaction (RFC-0005
    // §atomicity non-promise; each leaf is its own consistent refcounted snapshot).
    for (site_t& site : sites) {
        vertex_t::store_drops_t store_drops;
        if (result_t<std::shared_ptr<const rope_t>> r =
                store_value(site.vx, site.node->store, store_drops))
            site.stored = std::move(*r);
        // Counted ONLY on the assign half. The notify half below delivers each covered site's
        // slice through fan_out and then mark_flushed()es the cursor, so on that path the ring
        // was never the delivery vehicle: a shed append costs a HISTORY entry, not a delivery,
        // and counting it would be the overcount that makes delivery_drops() lie the other way.
        if (!notify) count_store_drops(site.vx, store_drops);
    }

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
        const view_t& slice = is_root ? *head : node.notify;
        if (slice.empty()) continue;
        vertex_t* vx = is_root ? v : find_ptr(node.key);
        if (vx != nullptr) fan_out(vx, slice);
    }
    if (v->listeners_above() > 0) bubble_up(v, value);
    // Eager branch delivered these landing sites — clear any pending mark (a prior assign)
    // and advance stream drain cursors so a later sweep does not re-deliver (RFC-0008 §E).
    for (const site_t& site : sites) {
        clear_pending(site.vx, site.stored);
        if (site.vx->role() == role_t::STREAM) site.vx->mark_flushed();
    }
    return {};
}

void graph_t::deliver_vertex(vertex_t* v, const rope_t& value) {
    fan_out(v, value);
    // Vertical bubbling (RFC-0005): every subscription observes its vertex AND all
    // descendants, so a delivery also fans out to each ancestor's subscribers. Gated on
    // one relaxed load when nobody listens above.
    if (v->listeners_above() > 0) bubble_up(v, value);
}

void graph_t::deliver_current(vertex_t* v) {
    if (v->role() == role_t::STREAM) {
        // A stream is a queue (RFC-0008 §E): drain the ring entries appended since the
        // last flush, in order — NOT a coalesce. Snapshot under the lock
        // (vertex_t::drain_unflushed), deliver outside.
        std::vector<std::shared_ptr<const rope_t>> batch;
        if (v->drain_unflushed(batch) == 0) return;  // nothing appended since the last flush
        for (const std::shared_ptr<const rope_t>& sp : batch) deliver_vertex(v, *sp);
        return;
    }
    // STORED_VALUE: the last-known-value, once. HANDLER / never-assigned: null LKV, nothing.
    const std::shared_ptr<const rope_t> sp = v->read_stored();
    if (!sp) return;
    deliver_vertex(v, *sp);
}

void graph_t::propagate(vertex_handle_t v) { propagate_impl(v.get()); }

void graph_t::propagate_impl(vertex_t* v) {
    // The argument is always delivered — a direct propagate is never gated by the vertex's
    // own delivery_mode (RFC-0008 §C, the EXPLICIT escape hatch, and "notify its own subs"
    // is policy-independent).
    deliver_current(v);
    // Sweep the strict descendants: DRAIN the IF_NEWER pending set over v's prefix range,
    // and ITERATE the UNCONDITIONAL set over it. A subtree is a contiguous prefix range of
    // the key order (RFC-0008 §B). Snapshot the keys under sweep_mutex_, then deliver
    // outside it — delivery re-enters the graph (fan_out/re-dispatch), like fan_out itself.
    // Every allocation in the snapshot is NOTHROW (#477): an OOM key render skips the
    // sweep, and an OOM mid-collection stops it BEFORE draining the affected mark — the
    // undelivered entries stay in their sets, so the sweep defers instead of aborting.
    std::vector<std::byte> lo;
    if (!try_build_key(v, lo)) return;  // OOM: marks retained — the next sweep retries
    const auto in_subtree = [&lo](const std::vector<std::byte>& k) {
        return k.size() >= lo.size() && std::equal(lo.begin(), lo.end(), k.begin());
    };
    std::vector<std::vector<std::byte>> to_deliver;
    // Nothrow copy of one sweep key into the delivery snapshot; false stops the sweep.
    const auto collect = [&to_deliver](const std::vector<std::byte>& k) noexcept {
        std::vector<std::byte> copy;
        if (!detail::try_assign(copy, k)) return false;
        return detail::try_push_back(to_deliver, std::move(copy));
    };
    {
        const std::lock_guard lock(sweep_mutex_);
        for (auto it = pending_.lower_bound(lo); it != pending_.end() && in_subtree(*it);) {
            // Collect BEFORE the drain: an OOM leaves this and later marks for the next
            // covering sweep instead of silently losing them.
            if (it->size() != lo.size() && !collect(*it)) break;  // strict descendant
            it = pending_.erase(it);  // drain (v itself, if present, was delivered above)
            pending_count_.fetch_sub(1, std::memory_order_relaxed);
        }
        for (auto it = unconditional_.lower_bound(lo);
             it != unconditional_.end() && in_subtree(*it); ++it) {
            // Iterate, do not drain; an OOM defers the rest to the next sweep.
            if (it->size() != lo.size() && !collect(*it)) break;
        }
    }
    for (const std::vector<std::byte>& k : to_deliver) {
        if (vertex_t* u = find_ptr(k)) deliver_current(u);
    }
}

void graph_t::mark_pending(vertex_t* v) {
    // EXPLICIT never rides an ancestor sweep; UNCONDITIONAL is already a permanent sweep
    // member — neither needs a pending mark. IF_NEWER marks only when someone observes at
    // or above v (else a sweep would deliver nowhere — the idle-write fast path keeps the
    // unobserved write off the shared lock, RFC-0005 listeners gate).
    if (v->delivery_mode() != delivery_mode_t::IF_NEWER) return;
    // The OWN half is the SEQ_CST read (#1140), for the same reason fan_out's is (#635) and
    // to the #555 standard — this is a SKIP gate, and the work it skips is never re-offered:
    // a vertex that misses its mark enters no sweep set, so an ancestor propagate delivers it
    // nowhere and only a LATER write can re-mark it. Omitted, not deferred.
    //
    // The pairing, and the outcome it excludes. PUBLISHER (assign): store the LKV, seq_cst
    // (`vertex_t::store` -> `lkv_slot_t::store`), THEN load this count, seq_cst. SUBSCRIBER
    // (`admit_subscriber`): bump the count, seq_cst (`bump_own_subs`, and #635 moved it AHEAD
    // of the slot verb), THEN load the LKV into ADR-0049's durability latch, seq_cst
    // (`vertex_t::add_edge` -> `lkv_.load()`). Both sides seq_cst puts all four accesses in
    // one total order S: a publisher that reads zero here precedes the subscriber's bump in
    // S, hence precedes the subscriber's latch load, so the latch carries the value this
    // skipped mark would have swept. The forbidden observation — the skip says
    // write-before-subscribe while the latch hands the subscriber the PRE-write value, saying
    // subscribe-before-write, and the value reaches nobody — is not in S. A RELAXED load does
    // not join S and excludes nothing: architecturally reachable on aarch64/rv32 (a shipped
    // target), latent on x86-64 only because the seq_cst LKV store lowers to a locked `xchg`.
    // The other interleaving (count already bumped, slot not yet appended) costs one pending
    // mark and one sweep that finds a value the latch also delivered — a duplicate, never a
    // loss. The ANCESTOR half stays relaxed BY RULING (#854, refuted): the latch snapshots
    // the subscribed ANCESTOR's own LKV, never a descendant's, so a stale zero there has no
    // forbidden observation to exclude. See `vertex_t::own_subs_ordered`.
    if (v->own_subs_ordered() == 0 && v->listeners_above() == 0) return;
    // The key render and the set-node insert both allocate on the writer thread —
    // NOTHROW them (#477): on OOM the pending mark is dropped (that deferred delivery
    // is shed, exactly like an eager delivery leg under the same pressure), never an
    // abort. The node probe bounds both mainstream ABIs' RB-tree node + key header.
    //
    // "Exactly like an eager delivery leg" is now true of the COUNTING too (#1003). It was
    // not: the eager legs have counted since the counting door landed while these two shed in
    // silence, and a comment asserting a symmetry the code did not have is what hid this. An
    // unmarked vertex is delivered by no sweep at all, so with no later write to re-mark it
    // the assigned value is never delivered — a lost delivery, not a deferred one. Counted at
    // the same one-per-subscriber width; the rare overcount when a later write DOES re-mark is
    // accepted, because undercounting a real loss is the worse failure.
    static constexpr std::size_t kSetNodeProbe = 8 * sizeof(void*) + sizeof(std::vector<std::byte>);
    std::vector<std::byte> key;  // outside the lock (a lock-free parent walk)
    if (!try_build_key(v, key)) {
        count_drop(drop_reason_t::OUT_OF_MEMORY, v->own_subs());
        return;
    }
    const std::lock_guard lock(sweep_mutex_);
    // Re-read the mode UNDER this lock (#895) — the check at the top is only a fast path.
    // set_delivery_mode holds the SAME lock across the mode store and both set edits, so a
    // flip landing between that unlocked read and this insert would otherwise leave the key
    // in BOTH sets (UNCONDITIONAL ⇒ the next covering propagate collects it from each and
    // delivers the vertex twice in one sweep), or in pending_ for a vertex now EXPLICIT,
    // which an ancestor sweep must never include. Re-reading here is what makes the two
    // sets mutually exclusive by construction. It joins the probe's condition rather than
    // taking a `return` of its own so `key`'s cleanup stays single-exit — worth 4 of the 18
    // instructions per assign the two-exit spelling cost (`perf stat -e instructions:u`).
    // Split into two named conditions so the shed can be ATTRIBUTED without a second probe:
    // only a declined probe is a dropped delivery. A mode that flipped to EXPLICIT /
    // UNCONDITIONAL under the lock sheds nothing (neither wants a mark), and an insert that
    // finds the key already present sheds nothing either — the mark is there and the next
    // covering sweep will deliver. Still single-exit, so `key`'s cleanup keeps the shape the
    // paragraph above paid for; the two locals are registers on the marking path.
    const bool if_newer = v->delivery_mode() == delivery_mode_t::IF_NEWER;
    const bool room = if_newer && detail::probe_bytes(kSetNodeProbe);
    if (room && pending_.insert(std::move(key)).second)
        pending_count_.fetch_add(1, std::memory_order_relaxed);
    else if (if_newer && !room)
        count_drop(drop_reason_t::OUT_OF_MEMORY, v->own_subs());
}

void graph_t::clear_pending(vertex_t* v, const std::shared_ptr<const rope_t>& delivered) {
    // Same idle fast path as mark_pending: an unobserved vertex was never marked.
    if (v->own_subs() == 0 && v->listeners_above() == 0) return;
    // Empty-set fast path (the per-eager-write case when nobody uses assign+propagate):
    // no key render, no sweep lock. Racing a concurrent mark_pending here leaves the mark
    // for the next covering sweep — the always-safe direction (one duplicate delivery of
    // the current LKV at worst, never a lost one).
    if (pending_count_.load(std::memory_order_relaxed) == 0) return;
    // Nothrow key render (#477): on OOM keep the stale mark — the same safe direction.
    // Never an abort on the writer thread.
    std::vector<std::byte> key;  // outside the lock
    if (!try_build_key(v, key)) return;
    const std::lock_guard lock(sweep_mutex_);
    // Erase only while the value this call's own store published is still v's CURRENT LKV
    // (#1185, the #854-survivor locked-erase drop). A concurrent assign publishes a NEW
    // LKV and only then inserts the mark, both after its store — so a differing pointer
    // here says a value this writer never delivered is live, and its mark is the only
    // thing that will ever deliver it. Erasing it would lose that delivery outright;
    // keeping it costs one duplicate delivery of the current LKV at the next covering
    // sweep, exactly what the two fast paths above already permit. The compare is a
    // POINTER identity, and it is sound because `delivered` holds a strong reference for
    // the whole call: the published control block cannot be freed and its address reused
    // underneath the comparison. Both the racing insert and this erase take sweep_mutex_,
    // so a mark that survives is precisely one whose value a sweep still owes. (The
    // handler leg's null "consumed" sentinel matches a handler's permanently null LKV and
    // erases as before — a handler sweep delivers nothing anyway, deliver_current on a
    // null LKV.)
    if (v->read_stored() != delivered) return;
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
    return write_impl(v.get(), std::move(value), caller);
}

result_t<void> graph_t::write(vertex_handle_t vh, const field_path_t& field, rope_t value,
                              std::string_view caller) {
    vertex_t* v = vh.get();
    if (field.empty()) return write_impl(v, std::move(value), caller);
    // A field write targets a contiguous control TLV (settings / acl / subscribers);
    // materialize it (single-link: zero copy) before the field surface parses it. A
    // multi-link value's flatten draws from the ADR-0060 value_backend_. The refusal keeps
    // its cause (#917): an exhausted pool surfaces the injected-resource BACKPRESSURE
    // (§3 — transient) rather than letting field_write read an empty head back as a
    // malformed value, while a DEVICE-link value — permanently un-parsable on the CPU —
    // is TYPE_MISMATCH.
    const std::expected<view_t, tr::view::flatten_err_t> head =
        value.try_materialize(*value_backend_);
    if (!head) {
        return std::unexpected(head.error() == tr::view::flatten_err_t::NO_MEMORY
                                   ? status_t::BACKPRESSURE
                                   : status_t::TYPE_MISMATCH);
    }
    return field_write(v, field, *head, caller);
}

result_t<value_ref_t> graph_t::await(vertex_handle_t vh, std::chrono::nanoseconds timeout,
                                     std::string_view caller) {
    vertex_t* v = vh.get();
    // await is the readiness form of a data READ — same gate, checked up front so a
    // denied caller cannot camp on the condvar.
    if (!acl_allows(v, caller, acl_right_t::READ))
        return std::unexpected(status_t::PERMISSION_DENIED);
    const std::uint64_t seq0 = v->current_seq();
    if (!v->wait_for_change(seq0, timeout)) return std::unexpected(status_t::TIMEOUT);
    std::shared_ptr<const rope_t> sp = v->read_stored();
    if (!sp) return std::unexpected(status_t::NOT_FOUND);  // e.g. a Handler-role write
    return value_ref_t{std::move(sp)};
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
 * Extracts the first PATH child's target key (may stay empty — the wire door ignores it) and,
 * from the SETTINGS child, the `delivery_compact` opt-in (NAME "delivery_compact" VALUE u8,
 * RFC-0004 §E.1 / docs/reference/05) and the packed `delivery_policy` (NAME "delivery_policy"
 * VALUE u16, RFC-0022 §3.A) — the SAME child, so the per-subscription policy introduced no new
 * wire structure. Back-compat: a SUBSCRIBER carrying neither (or an older parser) keeps the
 * full-route delivery path and the all-zero default policy — conformance vectors unaffected.
 * The SETTINGS walk IS `wire::config_reader_t` (#927, hoisted to L2/L3 by #985 so this file no
 * longer carries a hand-written copy of the rule): pair-consuming — a forward-compat pair whose
 * value reads `"delivery_policy"` must not bind the FOLLOWING child as the policy — and
 * last-well-formed-occurrence-wins, the plain NAME-field family semantics (#995).
 *
 * The policy's reserved bits (6–15) are stored VERBATIM and never interpreted: §3.A says a
 * sender MUST write 0 and a receiver MUST ignore them — an ignore, not a reject — so a future
 * sender's bits round-trip through `:subscribers[]` rather than being refused by an older node.
 */
void parse_subscriber_tlv(const tlv_t& sub, subscriber_t& s) {
    for (const tlv_t& child : sub.children) {
        if (child.type == type_t::PATH && !s.target_key) {
            // An illegally-spelled target leaves target_key unset, which falls back to the
            // full-route delivery path exactly as an older parser would (#681).
            if (auto k = wire::path_key(child)) s.target_key = try_make_target_key(*std::move(k));
        } else if (child.type == type_t::SETTINGS) {
            const wire::config_reader_t qos(&child);
            if (qos.flag("delivery_compact").value_or(false))
                s.ensure_remote().delivery_compact = true;  // cold half only when opted in
            if (const std::optional<std::uint16_t> word = qos.u16("delivery_policy"))
                s.policy.bits = *word;
        }
    }
}

/**
 * @brief The wire→`subscriber_t` admission parse, hand-rolled at three doors before #869:
 *        type-check the decoded record, then parse it ONCE (ADR-0049).
 *
 * The three doors are `graph_t::subscribe_wire` and `graph_t::field_write`'s `:subscribers[]`
 * append and `:subscribers[N]` replace arms. What is deliberately NOT in here is everything
 * the doors disagree about: the `[N]` arm's `acl_allows(WRITE)` gate and its empty-STATUS
 * eviction sentinel (both of which must run before this), the two field-write arms' `require
 * a PATH child` rule, and `subscribe_wire`'s inverse — it CLEARS `target_key`, because a
 * PATH child there names the consumer at ITS origin and delivery rides the return route.
 *
 * The zero-copy `source_view` retain stays at each door on purpose. Taking the record by
 * value here so the retain could be shared too measured **+1980 bytes** of graph.cpp `.text`
 * at -O3 — a `view_t` move plus its destructor and landing pad, duplicated at each of the
 * two `field_write` inline sites — for one assignment saved.
 *
 * @param tlv The decoded record. Not re-decoded here: the `[N]` arm must inspect the decode
 *            before this, to discriminate the eviction sentinel.
 * @param s   Filled on success; untouched on the type refusal.
 * @return False iff @p tlv is not a SUBSCRIBER — the doors' one shared TYPE_MISMATCH. A
 *         `bool` rather than a `result_t<void>` because there is exactly one failure.
 */
[[nodiscard]] bool parse_wire_subscriber(const tlv_t& tlv, subscriber_t& s) {
    if (tlv.type != type_t::SUBSCRIBER) return false;
    parse_subscriber_tlv(tlv, s);
    return true;
}

}  // namespace

result_t<subscription_t> graph_t::admit_subscriber(vertex_t* v, subscriber_t s,
                                                   std::string_view caller,
                                                   std::optional<std::size_t> slot) {
    // The single admission step (ADR-0049): every door lands here, so the SUBSCRIBE gate
    // and the transient-local durability latch apply UNIFORMLY — which invariants fire no
    // longer depends on which door an edge entered through.
    // Producer fan-out gate (#81, ADR-0026): appending a subscriber edge requires the
    // SUBSCRIBE right on this (the producer's) :acl, under the door's caller context.
    if (!acl_allows(v, caller, acl_right_t::SUBSCRIBE))
        return std::unexpected(status_t::PERMISSION_DENIED);

    // Mint the local target's binding ONCE, here (#830) — after every door has finished
    // deciding what `s.target_key` is (`subscribe_wire` CLEARS it for a remote binding, so a
    // mint at any earlier door would bind an edge that has no local target). The canonical
    // walk this replaces is `find_ptr` per delivery, linear in the target's address depth;
    // the deref that replaces it is flat. Failure here is not an error: an unbound edge
    // simply keeps the canonical spelling, which is what a target that does not exist yet,
    // one that is a placeholder, and one whose generation has saturated all get.
    if (s.target_key) {
        if (vertex_t* const target = find_ptr(*s.target_key); target != nullptr) {
            if (const auto slot = vertex_slot(vertex_handle_t{target}))
                s.binding = target_binding_t{.index = slot->index, .generation = slot->generation};
        }
    }

    // Latch the current value to the new subscriber iff THIS SUBSCRIBER asked for it
    // (`policy.durability_request()`, RFC-0022 §3.A) and the producer already holds an LKV
    // (RFC-0004 §D / Q4) — for EVERY door, not just the wire one (the ADR-0049 behavior
    // alignment). Before RFC-0022 the predicate was the producer's single
    // `settings.durability` flag, which latched for every subscriber of a transient-local
    // vertex whether or not it wanted the replay. vertex_t::add_edge appends the
    // slot and snapshots the latch's dispatch view + the LKV atomically under the vertex
    // lock; delivery runs OUTSIDE it (the remote sink does transport I/O; a callback /
    // target re-dispatch may re-enter the graph) through the SAME dispatch_edge legs a
    // write fans out with.
    // The observer's inputs, captured BEFORE `s` is moved into the slot verb below and before
    // a replace overwrites the slot it displaces. Both are refcount clones of already-owned
    // segments, so this costs no byte copy — and it is skipped outright unless an observer is
    // installed AND the door is external, which is what keeps the local doors free.
    const bool observe = observing_subscriptions(caller);
    const view_t admitted_tlv = observe ? s.source_view : view_t{};
    const view_t displaced_tlv =
        (observe && slot) ? v->edge_source(*slot).value_or(view_t{}) : view_t{};

    edge_latch_t latch;
    std::size_t idx = 0;
    // The listener count goes up BEFORE the slot exists, not after (#635). fan_out now SKIPS
    // the entire snapshot when this count reads zero, so a bump that TRAILED the append would
    // leave a window where the edge is live and invisible: a publish landing in it delivers to
    // nobody, while the latch taken inside the edge verb below already holds the PREVIOUS
    // value — the new subscriber would miss that write outright. Bumping first inverts the
    // window into the harmless direction (a count with no slot yet ⇒ one snapshot that finds
    // nothing). vertex_t::own_subs_ordered carries the ordering argument.
    note_subscriber_added(v);  // RFC-0005: descendants' writes now bubble here
    if (slot) {
        // RFC-0009 §D.1 replace: the SAME door, so the SUBSCRIBE gate above and the latch
        // below apply identically to a replace and to an append (ADR-0049). An index no
        // slot answers to is a malformed address, not a silent no-op — and refusing it is
        // what stops a wire-supplied `:subscribers[65535]` from growing the slot vector.
        const vertex_t::edge_replace_t r = v->replace_edge(*slot, std::move(s), &latch);
        // Only filling a CLEARED slot is genuinely a new listener; swapping a live one leaves
        // the count be, and a refused index adds nothing — both give the speculative bump
        // back. The unwind costs a second subtree walk on a control-plane-COLD path, which is
        // the right side to pay on: over-counting only ever buys a snapshot that finds
        // nothing, while under-counting drops a delivery.
        if (r != vertex_t::edge_replace_t::FILLED_EMPTY) note_subscriber_removed(v);
        if (r == vertex_t::edge_replace_t::OUT_OF_RANGE)
            return std::unexpected(status_t::INVALID_PATH);
        // A replace that displaced a LIVE edge is two events, in causal order: the old
        // subscription ended and a new one began. Reporting only the ADDED would leave an
        // observer's inventory holding an edge that no longer exists.
        if (r == vertex_t::edge_replace_t::REPLACED_ACTIVE)
            notify_subscription(sub_event_t::kind_t::REMOVED, v, caller, displaced_tlv, *slot);
        idx = *slot;
    } else {
        idx = v->add_edge(std::move(s), &latch);
        // The injected resource could not carry the edge (#477 / #635: publishing the new edge
        // array is the one allocation an append now makes). Nothing was admitted, so give the
        // speculative listener bump back and report it — an admitted-but-unpublished edge
        // would be a subscription that never receives. BACKPRESSURE is the injected-resource
        // status (ADR-0060 §3), the same one the store leg answers on exhaustion.
        if (idx == vertex_t::kNoSlot) {
            note_subscriber_removed(v);
            return std::unexpected(status_t::BACKPRESSURE);
        }
    }

    if (latch.value) dispatch_edge(latch.edge, *latch.value);
    // Last, and only on the success path: an edge the caller was told about is an edge that
    // landed. No graph lock is held here — the slot verb released the vertex stripe lock and
    // note_subscriber_added released the map lock — and the durability latch has already been
    // dispatched, so the observer never runs interleaved with this subscription's own replay.
    notify_subscription(sub_event_t::kind_t::ADDED, v, caller, admitted_tlv, idx);
    return subscription_t{v, idx};
}

void graph_t::set_subscription_observer(sub_observer_t observer) {
    subscription_observer_ = std::move(observer);
}

void graph_t::notify_subscription(sub_event_t::kind_t kind, const vertex_t* v,
                                  std::string_view caller, const view_t& sub_tlv,
                                  std::size_t slot) const {
    // The ONE external/local discrimination in the feature: a non-empty caller context is by
    // construction the inbound link NAME the FWD resolver drives the op under, and the local
    // doors (both subscribe() sugars, a field-write from host code) pass the empty context.
    if (!observing_subscriptions(caller)) return;
    // Decode the target from the record ITSELF, not from the parsed slot: subscribe_wire
    // deliberately drops target_key for a remote binding, but the PATH the peer sent is still
    // what an observer wants to see (sub_event_t::target carries the caveat). A slot with no
    // stored TLV, or one whose PATH is malformed, reports an EMPTY target rather than
    // suppressing the event — the mutation happened either way.
    std::vector<std::byte> target;
    if (!sub_tlv.empty()) {
        if (const auto tlv = wire::decode(sub_tlv); tlv && tlv->type == type_t::SUBSCRIBER) {
            for (const tlv_t& child : tlv->children) {
                if (child.type != type_t::PATH) continue;
                if (auto k = wire::path_key(child)) target = *std::move(k);
                break;
            }
        }
    }
    const std::vector<std::byte> producer = build_key(v);
    subscription_observer_(sub_event_t{.kind = kind,
                                       .producer = wire::key_view_t{producer},
                                       .target = wire::key_view_t{target},
                                       .link = caller,
                                       .slot = slot});
}

result_t<void> graph_t::subscribe(const path_t& src, const path_t& target,
                                  delivery_policy_t policy) {
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
    // The optional SETTINGS child, built first so its size is known when the SUBSCRIBER
    // header is emitted. An all-zero policy emits NOTHING — the absent case of RFC-0022
    // §3.A — so a caller that states no policy produces the exact bytes it did before, and
    // the existing `subscriber-path` conformance vector still describes this encoder.
    std::vector<std::byte> qos;
    if (policy.bits != 0) {
        std::vector<std::byte> members;
        wire::emit_name(members, "delivery_policy");
        emit_value(members, policy.bits, 2);
        wire::emit_tlv(qos, type_t::SETTINGS, opt_t{.pl = true}, members);
    }
    std::vector<std::byte> sub;
    sub.reserve(8 + key.size() + qos.size());
    wire::emit_header(sub, type_t::SUBSCRIBER, opt_t{.pl = true}, 4 + key.size() + qos.size());
    wire::emit_header(sub, type_t::PATH, opt_t{.pl = true}, key.size());
    sub.insert(sub.end(), key.begin(), key.end());
    sub.insert(sub.end(), qos.begin(), qos.end());
    const std::optional<view_t> value = view::over_bytes(sub);
    if (!value) return std::unexpected(status_t::BACKPRESSURE);
    field_path_t field;
    field.steps.push_back(field_step_t{.name = "subscribers", .indexed = true, .append = true});
    return field_write(v, field, *value, {});
}

result_t<subscription_t> graph_t::subscribe(const path_t& src, subscriber_fn_t fn, void* ctx,
                                            delivery_policy_t policy) {
    vertex_t* v = find_ptr(src.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    // A callback cannot ride a TLV, so this sugar has no parse to share — it still
    // enters the same single admission step as every other door (ADR-0049), under the
    // empty (local) caller context. The policy lands on the slot directly, which is where
    // the wire door's parse would have put it.
    subscriber_t s;
    s.callback = fn;
    s.callback_ctx = ctx;
    s.policy = policy;
    return admit_subscriber(v, std::move(s), {});
}

result_t<void> graph_t::unsubscribe(const subscription_t& sub) {
    if (sub.vertex_ == nullptr) return std::unexpected(status_t::NOT_FOUND);
    // The in-process counterpart of the wire ":subscribers[N] clear" (field_write below):
    // deactivate the slot, then unwind the RFC-0005 listener bookkeeping — the SAME order and
    // the SAME helper the wire path uses, so both doors leave identical counters. clear_edge
    // RECLAIMS the slot's retained state (target key, segment pin, cold remote half) and
    // leaves an inert, index-stable shell that add_edge reuses; an in-flight delivery already
    // snapshotted the edge (ADR-0041 §2) and completes untouched.
    if (!sub.vertex_->clear_edge(sub.slot_)) return std::unexpected(status_t::NOT_FOUND);
    note_subscriber_removed(sub.vertex_);
    return {};
}

void graph_t::set_app_fields(vertex_handle_t v, std::vector<app_field_t> table) {
    // Owner-facing declaration (RFC-0010 §A.2) — a local host API like register_vertex,
    // so no ACL gate: the owner is updating its own projection. The vertex verb replaces
    // the table atomically with respect to concurrent field operations.
    v.get()->set_app_fields(std::move(table));
}

void graph_t::set_app_fields_static(vertex_handle_t v, borrowed_fields_t table) {
    // Borrowed-declaration install (ADR-0058): same owner-facing, no-ACL-gate semantics as
    // set_app_fields; the vertex verb stores views into the caller's static storage.
    v.get()->set_app_fields_static(table);
}

void graph_t::set_remote_delivery_sink(
    std::function<void(const remote_delivery_t&, const rope_t&)> sink) {
    remote_sink_ = std::move(sink);
}

result_t<void> graph_t::subscribe_wire(vertex_handle_t vh, view_t source_view, view_t return_route,
                                       std::string link) {
    vertex_t* v = vh.get();
    // The route is this door's precondition, not an optional extra (#1055). Every edge this
    // door admits carries a link, and `dispatch_edge` gates its remote leg on that link while
    // handing the sink the RETURN ROUTE — so admitting a routeless edge here bought one
    // `FWD{WRITE}` per publish whose `dst` was a zero-byte PATH. Refusing it at the door is
    // what lets the fan-out body keep testing the link alone: that test is the wide-fan-out
    // loop's per-edge cost and is kept inlinable on purpose, so the invariant is established
    // once, here, rather than re-checked on every delivery. INVALID_PATH because that is what
    // an empty PATH TLV is, and what `fwd_router_t::subscribe_toward` already answers for the
    // empty residual it refuses to build a route from.
    if (return_route.empty()) return std::unexpected(status_t::INVALID_PATH);
    // Parse the owned SUBSCRIBER copy ONCE (ADR-0049) — delivery_compact comes from this
    // parse (the resolver's parallel subscriber_compact() is retired); the tlv_t borrows
    // source_view's bytes, which the slot then retains zero-copy.
    const auto sub = wire::decode(source_view);
    if (!sub) return std::unexpected(status_t::TYPE_MISMATCH);
    subscriber_t s;
    // The shared door parse (ADR-0049, #869) — type check + parse. The retain stays here;
    // `sub`'s spans survive the move (a `view_t` move transfers the segment, not the bytes)
    // and are not read again after it.
    if (!parse_wire_subscriber(*sub, s)) return std::unexpected(status_t::TYPE_MISMATCH);
    s.source_view = std::move(source_view);
    // A PATH child names the consumer at ITS origin — never a local re-dispatch target;
    // remote delivery rides the return route over the link (RFC-0004 §D). This door is the
    // one that CLEARS the key the two field-write arms REQUIRE, so it stays out of the
    // shared helper.
    s.target_key.reset();
    subscriber_remote_t& r = s.ensure_remote();  // a wire subscriber always carries the cold half
    r.caller = link;  // the fan-in gate context this edge's deliveries run under (#81)
    r.return_route = std::move(return_route);
    r.link = std::move(link);
    const std::string gate_ctx = r.caller;  // survives the move above (the SUBSCRIBE gate
                                            // runs under the inbound link, #81/ADR-0026)
    // A wire subscribe carries no host handle back — discard the slot (unsubscribe is the
    // wire :subscribers[N] clear, not this door's return).
    if (const auto r2 = admit_subscriber(v, std::move(s), gate_ctx); !r2)
        return std::unexpected(r2.error());
    return {};
}

result_t<void> graph_t::field_write(vertex_t* v, const field_path_t& field, const view_t& value,
                                    std::string_view caller) {
    const field_step_t& step0 = field.steps[0];

    if (step0.name == "subscribers") {
        // `:subscribers` is addressed WHOLE — `[]` appends an edge, `[N]` clears one, and
        // there is nothing INSIDE a slot to address: a SUBSCRIBER record is stored and
        // served as one TLV, never member-wise. So any further step names nothing.
        //
        // This gate is not cosmetic (#580). The `[N]` arm below is an unconditional
        // `clear_edge`, so before it, `:subscribers[0].liveness.last_seen_ns` — or any
        // typo'd tail at all — DESTROYED the slot and answered `kind=RESULT`, byte-identical
        // to a legitimate `[0]` clear. A caller aiming at a member wrote nothing, unbound a
        // live subscriber, and was told it succeeded. The read half already required
        // `steps.size() == 1` (see read_field's `:subscribers[N]` arm), so this makes the
        // two halves agree rather than inventing a rule.
        //
        // Resolved BEFORE the ACL gate, exactly as `:acl` below: a shape that names nothing
        // is not an access question. Note that leaves the `[]` / `[N]` shapes untouched, so
        // `plain_step` — the `:acl` guard's second half — must NOT be used here: an append
        // is `append == true` and a clear is `indexed == true`, and both are legal.
        //
        // The selector decode itself is `field_selector` (#869) — the SAME classification the
        // read door switches on, so the two halves can no longer drift on WHICH shape they
        // are looking at. They still disagree on the ANSWER per shape, deliberately: that
        // asymmetry (a `[]` write subscribes, a `[]` read enumerates; a `[*]` write is
        // INVALID_PATH, a `[*]` read is SCHEMA_NOT_FOUND) is what each arm below states.
        const field_sel_t sel = field_selector(field);
        if (sel == field_sel_t::TAIL) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (sel == field_sel_t::APPEND) {
            const auto sub = wire::decode(value);
            if (!sub) return std::unexpected(status_t::TYPE_MISMATCH);
            subscriber_t s;
            // The shared door parse (ADR-0049, #869): type check + parse. Then retain the
            // SUBSCRIBER TLV zero-copy (a refcount clone of `value`) so a later
            // `:subscribers[]` read ropes it into the REPLY (ADR-0035).
            if (!parse_wire_subscriber(*sub, s)) return std::unexpected(status_t::TYPE_MISMATCH);
            s.source_view = value;
            if (!s.target_key) return std::unexpected(status_t::TYPE_MISMATCH);
            // The fan-in gate context for this edge's deliveries (#81); the empty (local)
            // context needs no cold half. It is ALSO what makes the edge reclaimable: this
            // door leaves `subscriber_remote_t::link` empty (there is no return route to
            // deliver over — the edge fans out to a LOCAL target), so
            // `vertex_t::evict_link_edges` falls back to this context to find the link the
            // edge was admitted over (#943). Do NOT "fix" that by assigning `link` here:
            // `graph_t::dispatch_edge` gates its remote leg on `!e.link.empty()`, so a
            // non-empty link would add a phantom `FWD{WRITE}` per publish carrying an EMPTY
            // return route. That is the invariant `subscribe_wire` now enforces at its own
            // door (#1055, INVALID_PATH on an empty route) — this arm holds up the other half
            // of it, by binding no link when it binds no route.
            //
            // Reachability, as of this commit: the ONE in-tree wire door
            // (`op_resolve_walk.hpp`'s WRITE case) routes a remote `:subscribers[]` append
            // bearing a SUBSCRIBER to `subscribe_wire` instead (its `remote_sub` test), and
            // any other payload fails the TYPE_MISMATCH above — so the non-empty branch is
            // reached only through the public `graph_t::write(v, field, value, caller)`,
            // which an embedder may drive with an inbound link name. The `[N]` arm below has
            // no such diversion and IS reached from the wire.
            if (!caller.empty()) s.ensure_remote().caller.assign(caller);
            // The single admission step (ADR-0049): SUBSCRIBE gate → append → latch.
            // A field-write subscribe returns no host handle — discard the slot.
            if (const auto r = admit_subscriber(v, std::move(s), caller); !r)
                return std::unexpected(r.error());
            return {};
        }
        // `[*]` sets indexed=true AND wildcard=true and never assigns `index` — so it
        // arrives here with `index == 0`, OUTSIDE the validity window path.hpp declares
        // ("valid when indexed && !append && !wildcard"). Testing `indexed` alone therefore
        // clears SLOT 0 and answers RESULT: silent data loss reported as success (#579).
        // `field_selector` is what keeps that from ever being retried: WILDCARD is decided
        // before APPEND and SLOT, so no arm below can see a wildcard step at all.
        // The WRITE grammar has no wildcard axis, so a write bearing one is a malformed
        // address, not a missing schema entry — `:subscribers` plainly exists.
        //
        // DIVERGENCE (#869), pinned NOT fixed: the READ door answers `:subscribers[*]`
        // SCHEMA_NOT_FOUND, and it answers it BELOW the READ gate, so a denied caller is
        // told PERMISSION_DENIED. Which of the two codes is right is a wire question —
        // docs/reference/03-addressing.md §`[*]` treated as `[0]` calls `[*]` "legal only
        // where the field chain's first step is subscribers" and reads it as "every slot",
        // which points at an enumerating READ rather than at either error — so unifying it
        // needs an RFC, not this refactor. `field_wildcard_divergence` pins both answers.
        if (sel == field_sel_t::WILDCARD) return std::unexpected(status_t::INVALID_PATH);
        if (sel == field_sel_t::SLOT) {  // `[N]` — clear or replace, per RFC-0009 §D.1
            if (!acl_allows(v, caller, acl_right_t::WRITE))
                return std::unexpected(status_t::PERMISSION_DENIED);
            // §D.1 is payload-DISCRIMINATING. Before this, every indexed write cleared the
            // slot payload-blind, so a peer writing a SUBSCRIBER to slot N — plainly meaning
            // to replace that edge — silently destroyed it and was told RESULT.
            const auto tlv = wire::decode(value);
            if (!tlv) return std::unexpected(status_t::TYPE_MISMATCH);
            // The eviction sentinel: an empty STATUS (`09 00 00 00`, the smallest valid TLV).
            if (tlv->type == type_t::STATUS && tlv->payload.empty() && tlv->children.empty()) {
                // The observer's view of the departing edge must be taken BEFORE the clear —
                // clear_edge RECLAIMS the slot's stored SUBSCRIBER, so afterwards there is
                // nothing left to name the target with. Skipped entirely on a local clear or
                // with no observer installed (observing_subscriptions).
                const view_t cleared_tlv = observing_subscriptions(caller)
                                               ? v->edge_source(step0.index).value_or(view_t{})
                                               : view_t{};
                if (v->clear_edge(step0.index)) {
                    note_subscriber_removed(v);  // RFC-0005 counter bookkeeping
                    // Only a slot that WAS active is an unsubscribe; clearing an already-empty
                    // one changed nothing and must not be reported as a removal.
                    notify_subscription(sub_event_t::kind_t::REMOVED, v, caller, cleared_tlv,
                                        step0.index);
                }
                return {};
            }
            subscriber_t s;
            // The shared door parse (ADR-0049, #869) — the same two steps the append arm and
            // `subscribe_wire` run. It sits AFTER the WRITE gate and AFTER the sentinel
            // discrimination above, which are this arm's alone.
            if (!parse_wire_subscriber(*tlv, s)) return std::unexpected(status_t::TYPE_MISMATCH);
            s.source_view = value;  // retain the SUBSCRIBER TLV zero-copy, as the append arm does
            if (!s.target_key) return std::unexpected(status_t::TYPE_MISMATCH);
            // As in the append arm: the stored context is also the link this edge was
            // admitted over, which is what `vertex_t::evict_link_edges` falls back to when
            // the cold half carries no delivery link (#943).
            if (!caller.empty()) s.ensure_remote().caller.assign(caller);
            // Through the SAME admission door as an append, so a replace passes the
            // SUBSCRIBE gate — §D.1's "admitted through the same admission door".
            if (const auto r = admit_subscriber(v, std::move(s), caller, step0.index); !r)
                return std::unexpected(r.error());
            return {};
        }
        return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    }

    if (step0.name == "acl") {
        // Store the :acl (#81, ADR-0018/0020): gate on WRITE_ACL — the `admin` right — then
        // validate + parse the typed ACEs (ADR-0050 parse_acl) and store THAT LIST ALONE,
        // no verbatim byte copy beside it (#907): read_acl re-encodes, so read-back cannot
        // describe a policy other than the one acl_allows walks. The outer SHAPE is checked
        // too, not just the type code — only `opt.pl` populates children, so a PRIMITIVE
        // ACL parses as ZERO ACEs and CLEARS enforcement on a write that looks like it
        // installs one; an EMPTY CONTAINER is the sanctioned clear. `set_acl` REPLACES, so
        // an unresolved shape is no harmless no-op: before this bound `:acl.bogus` /
        // `:acl[0]` / `:acl[]` all reached it and silently replaced the whole list. There is
        // no member or slot addressing (an ACE is not separately writable), so any other
        // shape names nothing: SCHEMA_NOT_FOUND, resolved BEFORE the gate like any field.
        // `whole_field` is that rule, shared with the read door (#869).
        //
        // DIVERGENCE (#869), pinned NOT fixed: the READ door resolves an unaccepted `:acl`
        // shape BELOW its READ gate, so `:acl[0]` is SCHEMA_NOT_FOUND here (caller-
        // independent) but PERMISSION_DENIED there for a denied caller. Moving the read's
        // resolution above its gate changes a code that leaves the device; `field_shape_matrix`
        // pins both answers.
        if (!whole_field(field)) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (!acl_allows(v, caller, acl_right_t::WRITE_ACL))
            return std::unexpected(status_t::PERMISSION_DENIED);
        const auto acl = wire::decode(value);
        if (!acl || acl->type != type_t::ACL || !acl->opt.pl)
            return std::unexpected(status_t::TYPE_MISMATCH);
        result_t<std::vector<ace_t>> aces = parse_acl(*acl);
        if (!aces) return std::unexpected(aces.error());
        v->set_acl(std::move(*aces));  // storing replaces; empty => no restrictions
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
        //
        // `:children` is addressed WHOLE, like `:subscribers` above (#581). `append` was
        // the sole predicate here, so `:children[].bogus` — or `[].a.b.c` — created the
        // child exactly as the sanctioned `:children[]` does and answered `kind=RESULT`,
        // byte-identical, with the tail provably inert (two different tails produced the
        // same reply and the same graph). On `/net` that spelling built a LIVE connection
        // vertex and wired it into the router. The READ of the byte-identical selector
        // already answered SCHEMA_NOT_FOUND, so the two halves disagreed; this makes them
        // agree. Before the CREATE gate, like `:acl`.
        //
        // Same `field_selector` classification the read door uses for `:children` (#869) —
        // the shapes are one rule, the answers are per side: `[]` CREATES here and
        // ENUMERATES there, and `:children` bare has no write surface at all.
        if (field_selector(field) != field_sel_t::APPEND)
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (!acl_allows(v, caller, acl_right_t::CREATE))
            return std::unexpected(status_t::PERMISSION_DENIED);
        return create_child(v, value);
    }

    if (step0.name != "settings") return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    // The `settings.…` sub-shape, resolved by the SAME `app_field_sel` the read door uses
    // (#869) — one place that knows what `app`, `app.<name…>` and a core knob name look like.
    std::string app_key;
    if (app_field_sel(field, app_key) == app_sel_t::NAMED) {
        // Owner-declared application fields under the reserved `app` subkey (RFC-0010
        // §A). This branch owns the whole `settings.app.` subtree — the protocol never
        // minted (and per the RFC must never mint) a knob named `app`. A bare
        // `:settings.app` container write and any `[...]`-selector step have no write
        // surface — CONTAINER and MALFORMED fall through to the terminal SCHEMA_NOT_FOUND
        // below, the same answer the pre-#869 in-arm guard gave.
        //
        // DIVERGENCE (#869), pinned NOT fixed: `step0`'s OWN shape is not tested here, so
        // `:settings[0].app.<name>` WRITES the field, while the read door — which does test
        // `plain_step(steps[0])` — answers it SCHEMA_NOT_FOUND. Tightening the write is a
        // change to what leaves the device for that spelling; `field_shape_matrix` pins
        // both answers.
        const std::string& key = app_key;
        // GATE-BEFORE-RESOLVE (#435, RFC-0010 §A erratum 2026-08-12). Owner-defined names
        // are a per-node secret — unlike the protocol's published constants, whose
        // pre-gate resolution #430 justified — so a caller-attributed write evaluates
        // the vertex WRITE right BEFORE any name under `settings.app.` is resolved: a
        // denied caller is told PERMISSION_DENIED whether the name is declared,
        // undeclared, `ro` or `wo`, and the error channel discloses neither the owner's
        // name set nor which spellings exist. The read door has the same order (its READ
        // gate sits above `settings.app.` resolution); #430's write-side hoist left this
        // branch answering SCHEMA_NOT_FOUND pre-gate, which leaked field existence.
        if (!caller.empty() && !acl_allows(v, caller, acl_right_t::WRITE))
            return std::unexpected(status_t::PERMISSION_DENIED);
        const std::optional<app_access_t> access = v->app_field_access(key);
        if (!access)  // undeclared stays ENOTTY — the table opens only its own names
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // A field not declared remotely writable has NO write surface (RFC-0010 §A.3
        // gate 1, the ENOTTY of writing a read-only ioctl) — the answer every ADMITTED
        // caller gets, identically; per the erratum it sits BELOW the ACL gate so it is
        // never an existence oracle for a denied one. The owner (empty caller) skips
        // both checks — it is updating its own projection, not a caller.
        if (!caller.empty() && *access == app_access_t::RO)
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // Store verbatim (§D — bytes in, bytes out; the descriptor is consumer
        // self-description, never a runtime validation schema). A false return means a
        // concurrent table replacement un-declared the name between gate and store.
        if (!v->app_field_store(key, value.bytes()))
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // The owner apply seam (§A.3), OUTSIDE the vertex lock — it may re-enter the
        // graph (apply the config, restructure children, then ANNOUNCE per §C). The
        // field write itself deliberately neither wakes `await` nor propagates:
        // the property plane is silent (ADR-0021 / RFC-0010 §C).
        // Snapshot the seam under the vertex lock (ADR-0058 Step 2 moved it to the lazy
        // app-field group), then fire the copy here, unlocked.
        if (auto seam = v->on_app_field_write(); seam) seam(key, value);
        return {};
    }

    // Everything else under `settings` — every `:settings.<knob>` name the protocol ever
    // minted, and every shape that resolves to none — falls through to the terminal
    // SCHEMA_NOT_FOUND below. RFC-0022 §3.B withdrew the flat core-namespace write surface
    // whole: there is no `settings_t` to write into, so the answer is caller-INDEPENDENT
    // (never PERMISSION_DENIED) and there is no gate here to get the order wrong.

    return std::unexpected(status_t::SCHEMA_NOT_FOUND);
}

result_t<void> graph_t::create_child(vertex_t* parent, const view_t& spec_value) {
    // Parse SPEC{ NAME "type" <sel>, NAME "name" <seg>, SETTINGS "config"? } — the
    // creation spec of docs/reference/05 §0x0E. The two NAMEs are positional pairs
    // (NAME key, NAME/SETTINGS value), read through the ONE pair-consuming walk,
    // wire::config_reader_t (#927 — hoisted to L2/L3 by #985 so this file no longer
    // carries a hand-written copy of the rule).
    const auto spec = wire::decode(spec_value);
    if (!spec || spec->type != type_t::SPEC) return std::unexpected(status_t::TYPE_MISMATCH);

    const wire::config_reader_t spec_pairs(&*spec);
    const std::string_view type_sel = spec_pairs.name("type").value_or(std::string_view{});
    const std::span<const std::byte> child_name =
        spec_pairs.name_bytes("name").value_or(std::span<const std::byte>{});
    const tlv_t* config = spec_pairs.settings("config");
    // The wire boundary runs THE segment predicate (ADR-0073 §1, #688): a peer-supplied
    // name must be expressible in the addressing grammar, or the vertex it creates is
    // enumerable but unaddressable — and a `/` inside one NAME breaks the injectivity of
    // the address→vertex map (reference/02). Same predicate, same INVALID_PATH answer as
    // the local parser, so the tiers cannot drift.
    if (type_sel.empty() || !valid_segment(detail::as_string_view(child_name)))
        return std::unexpected(status_t::INVALID_PATH);

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
    // POINT { NAME <vertex name>, SETTINGS { } }
    //
    // The synthesized protocol part enumerates the implemented `settings.*` knobs, and after
    // RFC-0022 §3.B there are NONE: `settings_t` is deleted, so the vertex's `:settings` core
    // namespace is empty — and therefore, for the first time, COMPLETE. That is the condition
    // #706 was filed about (the schema advertised `deadline_ns`, which nothing consumed, and
    // omitted the one live threshold), dissolved by removing the inputs rather than by
    // extending the view. The empty SETTINGS is emitted rather than omitted so the record
    // keeps its shape: a renderer walks `POINT{ NAME, SETTINGS, [NAME "app" SETTINGS] }`
    // whatever the vertex declares.
    const std::vector<std::byte> settings_children;

    std::vector<std::byte> point_body;
    wire::emit_name(point_body, key_view_t{v->name().bytes()}.last_segment());
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

result_t<void> graph_t::set_identity(std::uint8_t kind, std::span<const std::byte> key) {
    // The RFC-0011 §B identity-kind registry. `0x00` is reserved-invalid; every other
    // kind fixes its key length, so a length that contradicts the kind is a malformed
    // record and never reaches the wire (§B: TYPE_MISMATCH). Additions here are
    // RFC-gated, like the error registry.
    constexpr std::uint8_t kKindEd25519 = 0x01;
    constexpr std::size_t kEd25519KeyBytes = 32;
    if (kind != kKindEd25519 || key.size() != kEd25519KeyBytes)
        return std::unexpected(status_t::TYPE_MISMATCH);

    // SETTINGS(PL=1){ NAME "kind" VALUE u8, NAME "key" VALUE <key> } — the two required
    // members, in the fixed order §B pins. 60 bytes for ed25519.
    std::vector<std::byte> members;
    wire::emit_name(members, "kind");
    emit_value(members, kind, 1);
    wire::emit_name(members, "key");
    wire::emit_tlv(members, type_t::VALUE, opt_t{}, key);

    std::vector<std::byte> record;
    wire::emit_tlv(record, type_t::SETTINGS, opt_t{.pl = true}, members);
    identity_record_ = std::move(record);
    return {};
}

void graph_t::clear_identity() { identity_record_.clear(); }

result_t<view_t> graph_t::read_identity() const {
    // No keypair => the facet is ABSENT, not empty (RFC-0011 §C.3): the ENOTTY of an
    // unsupported field, byte-for-byte the pre-RFC behaviour. An empty record was
    // rejected precisely because it would fabricate an "identity exists but is vacant"
    // state no consumer can act on.
    if (identity_record_.empty()) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    // Pre-serialized at install, so every vertex of this node serves BYTE-IDENTICAL
    // bytes (§C.1) — the invariant that makes the record a valid cross-path key.
    const auto out = view::over_bytes(identity_record_);
    if (!out) return std::unexpected(status_t::BACKPRESSURE);
    return *out;
}

result_t<view_t> graph_t::read_settings(vertex_t* v) const {
    // The settings container KEEPS ITS SHAPE and LOSES ITS KNOBS (RFC-0010 §A.4 as amended
    // by RFC-0022 §4): `SETTINGS{ [NAME "app" SETTINGS{…}] }`. The reserved `app` subkey and
    // the single-traversal renderer contract survive; the core knob namespace is empty
    // because `settings_t` is deleted, so the read still enumerates exactly what the WRITE
    // gate accepts — which is now nothing, honestly, rather than seven names of which four
    // were never honoured. A vertex with no declared app fields reads an EMPTY `SETTINGS{}`,
    // which is honest rather than absent.
    std::vector<std::byte> children;
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
    // RE-ENCODE the stored ACEs (#907): read-back is a projection of the list acl_allows
    // walks, never a copy that could disagree with it. An encoded ACL is never empty, so
    // empty ⇒ no :acl was ever written — NOT_FOUND, distinct from an EMPTY container.
    const auto acl = v->with_acl([](bool set, const std::vector<ace_t>& aces) {
        return set ? encode_acl(aces) : std::vector<std::byte>{};
    });
    if (acl.empty()) return std::unexpected(status_t::NOT_FOUND);
    const auto out = view::over_bytes(acl);
    if (!out) return std::unexpected(status_t::BACKPRESSURE);
    return *out;
}

result_t<view_t> graph_t::read_children(vertex_t* v) const {
    // The synthesized listing wins (ADR-0044): a transport/connection vertex serves
    // its live bus peers here — a snapshot of traffic, never stored graph structure.
    // Load once — a concurrent retire may swap the seam out between check and call.
    if (const value_handlers_t& h = v->handlers(); h.on_children) return h.on_children();
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
                wire::emit_tlv(members, type_t::POINT, opt_t{.pl = true}, c.name().bytes());
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

result_t<rope_t> graph_t::read_children_materialized(vertex_handle_t vh) const {
    const result_t<view_t> mv = read_children(vh.get());
    if (!mv) return std::unexpected(mv.error());
    return rope_t{*mv};
}

namespace {

/**
 * @brief The POINT header width `wire::emit_header` produces for a @p body-byte body — TYPE +
 *        OPT + the u16 length, widening to u32 at the same 0xFFFF boundary `emit_tlv`
 *        auto-widens at.
 */
[[nodiscard]] constexpr std::size_t folded_hdr_len(std::size_t body) noexcept {
    return body > 0xFFFFu ? 6u : 4u;
}

/**
 * @brief One structured POINT header as a single exactly-sized OWNED segment drawn from @p
 *        backend and emitted by cursor. Null ⇒ the seam refused (the caller answers
 *        `BACKPRESSURE` by value; nothing here throws).
 *
 * Byte-identical to `wire::emit_header(out, type_t::POINT, {.pl = true, .ll}, body_len)` into a
 * `std::vector<std::byte>` — the ONE home of the folded reads' header framing, so the `ll`
 * auto-widen boundary cannot drift between the composed-root fold and the ":children" fold.
 * No throwing `std::vector` transient sits on the reply path (the op_resolve_walk assemble
 * pattern).
 *
 * @p backend is the graph's ADR-0060 `value_backend_` at both call sites (#831). These are
 * PAYLOAD framing bytes — the length field wraps the stored TLV and the name records below it —
 * so they are that seam's byte class, NOT the ADR-0074 `egress` seam, which is documented and
 * sized against ROUTE bytes. Both counts are PEER-influenced (a peer picks the composed root,
 * and thus how many subtree nodes fold; or whose ":children" to list, and thus how many members
 * frame), so an ADR-0067-class node with every backend at one slab would otherwise still leak
 * this framing to `malloc`. The segments escape inside the returned reply rope and are freed on
 * whichever thread drops the last reference — exactly the cross-thread self-routed reclaim
 * ADR-0060 §2 already requires of this backend. The default is `&mem::heap_backend()`, so a
 * shipped shape allocates byte-identically.
 */
[[nodiscard]] view::segment_ptr_t folded_point_header(mem::mem_backend_t& backend,
                                                      std::size_t body_len) {
    const bool ll = body_len > 0xFFFFu;  // mirror emit_tlv's auto-widen exactly
    view::segment_ptr_t seg = view::segment_alloc(backend, folded_hdr_len(body_len));
    if (!seg) return seg;  // the seam refused — a null segment_ptr_t IS the refusal
    std::byte* p = seg->bytes.data();
    *p++ = static_cast<std::byte>(std::to_underlying(type_t::POINT));
    *p++ = static_cast<std::byte>(opt_t{.pl = true, .ll = ll}.encode());
    detail::store_le(std::span<std::byte>(p, ll ? 4u : 2u), static_cast<std::uint32_t>(body_len),
                     ll ? 4u : 2u);
    return seg;
}

}  // namespace

result_t<rope_t> graph_t::read_children_folded(vertex_handle_t vh) const {
    vertex_t* v = vh.get();
    // Synthesized listing (ADR-0044): a live bus-peer snapshot, already one contiguous
    // view — a fold has nothing to gather, so it crosses as a single-link rope,
    // byte-identical to the read_children path.
    if (const value_handlers_t& h = v->handlers(); h.on_children) {
        const result_t<view_t> sv = h.on_children();
        if (!sv) return std::unexpected(sv.error());
        return rope_t{*sv};
    }
    // The folded projection of read_children: instead of concatenating every member into
    // one buffer and copying the whole listing (twice — into `out`, then into a segment),
    // gather each POINT{NAME} member as TWO scatter-gather links — the emitted POINT
    // header, then the child's own NAME-record bytes borrowed IN PLACE (zero copy). The
    // child vertex is pinned and insert-only and its `name_` is immutable once linked, so
    // the borrowed bytes outlive this rope. flatten() is byte-identical to read_children:
    // same header (opt.ll auto-widened at the same 0xFFFF boundary) followed by the same
    // name bytes, in the same child order.
    //
    // Every header here — one per registered child, plus the outer one — is framed by
    // folded_point_header from the ADR-0060 value_backend_, NOT from view::over_bytes' global
    // heap (#831). The count is PEER-influenced (a peer picks which vertex's ":children" to
    // READ, and thus how many members frame), and this is the site the wire ":children" field
    // READ routes to — see folded_point_header for the full seam argument, which the
    // composed-root fold below shares verbatim.
    mem::mem_backend_t& hdr_backend = *value_backend_;
    rope_t members;
    std::size_t members_len = 0;
    bool oom = false;
    {
        const std::shared_lock lock(map_mutex_);
        v->for_each_child([&members, &members_len, &oom, &hdr_backend](const vertex_t& c) {
            if (oom || !c.registered()) return;
            const std::span<const std::byte> name = c.name().bytes();
            view::segment_ptr_t mseg = folded_point_header(hdr_backend, name.size());
            view::segment_ptr_t nseg = view::borrow_const(name);
            if (!mseg || !nseg) {
                oom = true;
                return;
            }
            members.append(view::view_t::over(std::move(mseg)));  // owned POINT header
            members.append(view::view_t::over(std::move(nseg)));  // borrowed name (zero copy)
            members_len += folded_hdr_len(name.size()) + name.size();
        });
    }
    if (oom) return std::unexpected(status_t::BACKPRESSURE);
    view::segment_ptr_t oseg = folded_point_header(hdr_backend, members_len);
    if (!oseg) return std::unexpected(status_t::BACKPRESSURE);
    rope_t out{view::view_t::over(std::move(oseg))};
    // The member count is already in hand, so take the join as ONE sized growth instead of
    // the geometric push_back ladder (a wide listing is 2 links per child). Best effort:
    // on soft-fail the concat below still produces the right chain, it just pays the
    // ordinary growth path. `concat` no longer reserves for us — that guard belongs to the
    // caller that knows the count, not to every 1-link delivery clone (#1022).
    static_cast<void>(out.try_reserve(members.link_count()));
    out.concat(members);  // empty members (no children) => header-only rope, len 0
    return out;
}

result_t<rope_t> graph_t::read_subtree_folded(vertex_handle_t vh, std::string_view caller) const {
    vertex_t* root = vh.get();
    if (!acl_allows(root, caller, acl_right_t::READ))
        return std::unexpected(status_t::PERMISSION_DENIED);

    /**
     * @brief One included composed-read node, collected in PRE-ORDER (so the array order IS the
     *        wire order: a node's POINT header precedes its NAME/value/children bytes).
     */
    struct snap_node_t {
        const vertex_t* v = nullptr;       /**< @brief The pinned vertex (name bytes immutable). */
        std::shared_ptr<const rope_t> lkv; /**< @brief Its landed LKV — loaded ONCE, atomically. */
        std::size_t parent = 0;            /**< @brief Parent's index in the array (kNoParent at
                                                       the root). */
        std::size_t body_len = 0;          /**< @brief POINT body length, completed bottom-up. */
    };
    constexpr std::size_t kNoParent = static_cast<std::size_t>(-1);
    // The POINT header width and the header framing itself both come from the file-local
    // folded_hdr_len / folded_point_header, shared with read_children_folded — one home, so
    // the emit_tlv auto-widen boundary cannot drift between the two folded reads (#831).
    mem::mem_backend_t& hdr_backend = *value_backend_;

    // Pass 1 — collect, under ONE shared map lock: an ITERATIVE pre-order stack machine
    // (house style, parse_branch_node). The stack is heap-backed, so its bound is the
    // allocator and it needs no synthetic cap. This comment used to attribute that to graph
    // depth being "kMaxSegments-bounded structurally"; it is not — kMaxSegments is enforced
    // only in path_t::parse, and a wire-driven write-create never passes through it. Per node:
    // the ACL gate (a denied
    // vertex PRUNES its whole subtree, siblings unaffected), the placeholder skip
    // (unregistered levels are not members, exactly as read_children), one read_stored()
    // load, and the node's OWN body contribution (its NAME record below the root; its
    // stored TLV's total length verbatim). Descendant HANDLER on_read seams are NOT
    // invoked — the composed read serves landed LKVs only.
    std::vector<snap_node_t> nodes;
    {
        /** @brief One unvisited subtree root: the vertex and its parent's array index. */
        struct work_t {
            vertex_t* v = nullptr;  /**< @brief The subtree root to collect. */
            std::size_t parent = 0; /**< @brief Its parent's index in `nodes`. */
        };
        const std::shared_lock lock(map_mutex_);
        std::vector<work_t> stack;
        // Every stack/nodes growth is a throwing std::vector spill that aborts under
        // -fno-exceptions on OOM; route each through the nothrow try_push_back and drop
        // the reply (BACKPRESSURE) on failure. A lambda cannot return the error, so the
        // child push latches `oom` and the loop propagates it after each visit.
        bool oom = false;
        if (!detail::try_push_back(stack, work_t{.v = root, .parent = kNoParent}))
            return std::unexpected(status_t::BACKPRESSURE);
        while (!stack.empty()) {
            const work_t w = stack.back();
            stack.pop_back();
            const std::size_t idx = nodes.size();
            snap_node_t n;
            n.v = w.v;
            n.lkv = w.v->read_stored();  // ONE atomic load per node
            n.parent = w.parent;
            n.body_len = (w.parent == kNoParent ? 0 : w.v->name().bytes().size()) +
                         (n.lkv ? n.lkv->total_length() : 0);
            if (!detail::try_push_back(nodes, std::move(n)))
                return std::unexpected(status_t::BACKPRESSURE);
            // Push the children, then reverse the just-pushed run: the LIFO pop then
            // visits siblings in for_each_child's sorted order, keeping the array's
            // pre-order equal to the emitted wire order.
            const auto first = static_cast<std::ptrdiff_t>(stack.size());
            w.v->for_each_child([this, caller, idx, &stack, &oom](vertex_t& c) {
                if (oom) return;              // a prior sibling push failed — stop growing
                if (!c.registered()) return;  // placeholders are not members
                if (!acl_allows(&c, caller, acl_right_t::READ)) return;  // PRUNE the subtree
                if (!detail::try_push_back(stack, work_t{.v = &c, .parent = idx})) oom = true;
            });
            if (oom) return std::unexpected(status_t::BACKPRESSURE);
            std::reverse(stack.begin() + first, stack.end());
        }
    }

    // Pass 2 — body lengths bottom-up: reverse pre-order visits every child before its
    // parent, so each node's completed wire size (header + body) folds into the parent.
    for (std::size_t i = nodes.size(); i-- > 1;)
        nodes[nodes[i].parent].body_len += folded_hdr_len(nodes[i].body_len) + nodes[i].body_len;

    // Pass 3 preamble — the exact final link count, so the reply rope reserves its heap
    // chain ONCE (nothrow) and every append/concat below is guaranteed non-reallocating:
    // per node an owned POINT header (1) + the borrowed NAME below the root (0/1) + the
    // stored TLV's links (0..). A composed-root reply is thousands of links on a
    // fragmented heap — the un-reserved spill is exactly what aborted the node.
    std::size_t total_links = 0;
    for (const snap_node_t& n : nodes)
        total_links += 1u + (n.parent != kNoParent ? 1u : 0u) + (n.lkv ? n.lkv->link_count() : 0u);
    rope_t out;
    if (!out.try_reserve(total_links)) return std::unexpected(status_t::BACKPRESSURE);

    // Pass 3 — emit, in array (= pre-order = wire) order. Per node: an OWNED POINT header
    // link, the BORROWED NAME record link (below the root — the root's identity is the
    // addressed vertex; its own stored TLV leads the root body), then the stored TLV's
    // links refcount-CLONED (no byte copy). Zero flatten anywhere; a view allocation
    // failure is the audited BACKPRESSURE pattern.
    for (const snap_node_t& n : nodes) {
        // The POINT header as one exactly-sized OWNED segment, emitted by cursor straight
        // into the segment's bytes (the op_resolve_walk assemble pattern) — no throwing
        // std::vector<std::byte> transient sits on the reply path. Byte-identical to the
        // retired wire::emit_header(type, {.pl, .ll}, body_len). The bytes come from the
        // ADR-0060 value_backend_ rather than view::heap_alloc's global heap (#831) — the
        // seam argument, shared with read_children_folded, is on folded_point_header.
        view::segment_ptr_t hseg = folded_point_header(hdr_backend, n.body_len);
        if (!hseg) return std::unexpected(status_t::BACKPRESSURE);
        out.append(view::view_t::over(std::move(hseg)));  // owned POINT header
        if (n.parent != kNoParent) {
            // The child's own canonical NAME record IS the leading NAME TLV verbatim
            // (ADR-0057), borrowed in place over the pinned, immutable name bytes —
            // the read_children_folded lifetime argument.
            view::segment_ptr_t nseg = view::borrow_const(n.v->name().bytes());
            if (!nseg) return std::unexpected(status_t::BACKPRESSURE);
            out.append(view::view_t::over(std::move(nseg)));  // borrowed name (zero copy)
        }
        if (n.lkv) out.concat(*n.lkv);  // stored TLV verbatim — links cloned, refcount bump
    }
    return out;
}

result_t<rope_t> graph_t::read(vertex_handle_t vh, const field_path_t& field,
                               std::string_view caller) const {
    vertex_t* v = vh.get();
    if (field.empty()) {
        // The value read now returns a REFERENCE; this overload is rope-valued because every
        // other branch below composes, so materialize here rather than widen the surface.
        auto v_ref = read(vh, caller);
        if (!v_ref) return std::unexpected(v_ref.error());
        return **v_ref;
    }
    // ":children[]" (or bare ":children") — member enumeration, the read dual of the
    // SPEC-creating append — is served FOLDED (L4 fold, Slice 0): a scatter-gather rope
    // (outer POINT header + per-child borrowed NAME), byte-identical on flatten() to the
    // materialized listing (`folded_children_test` gates the differential against
    // `read_children_materialized`). It bypasses the single-view wrap below, which would
    // re-flatten the fold back into one buffer. A single "[N]" slot has no meaning here
    // (members are named, not indexed) and falls through to SCHEMA_NOT_FOUND, as does
    // `[*]` — the same `field_selector` classification the write door switches on (#869),
    // so the two halves cannot drift on which shape they are looking at. The ANSWER stays
    // per side: `[]` enumerates here and CREATES there.
    if (field.steps[0].name == "children") {
        const field_sel_t sel = field_selector(field);
        if (sel == field_sel_t::WHOLE || sel == field_sel_t::APPEND) {
            if (!acl_allows(v, caller, acl_right_t::READ))
                return std::unexpected(status_t::PERMISSION_DENIED);
            return read_children_folded(vh);
        }
    }
    // A field read serves a contiguous control TLV; it crosses back as a single-link
    // rope (ADR-0053 §6 — the data API returns ropes). Compute the control view, then
    // wrap once. Field reads are gated like data reads (#81): READ for the control
    // surface, READ_ACL — its own right, distinct from acting on the vertex — for ":acl".
    const result_t<view_t> fv = [&]() -> result_t<view_t> {
        // PROTOCOL-OWNED NAME VALIDITY RESOLVES ABOVE THE READ GATE (#435, RFC-0010 §A
        // erratum 2026-08-12). The recognised field namespace — {subscribers, acl,
        // children, settings, schema, identity} — is published spec text
        // (docs/reference/05 §0x09 STATUS / §Field namespace), identical on every node,
        // so answering an unknown NAME before the gate discloses nothing; answering it
        // BELOW the gate split one spelling's answer by who asked (SCHEMA_NOT_FOUND for
        // an allowed caller, PERMISSION_DENIED for a denied one) — the caller-dependent
        // disclosure reference/05 §Gating-:identity names as the failure mode — and
        // diverged from the write door, whose unknown-name arm sits before any gate.
        // NAME validity ONLY: every recognised name's VALUE keeps its gate below (a
        // denied caller reading an EXISTENT facet stays PERMISSION_DENIED), the pinned
        // selector-shape divergences (#869: `:acl[0]`, `:subscribers[*]`, …) are
        // untouched, and owner-defined `settings.app.*` resolution stays BELOW the gate
        // — the owner's name set is a secret (see the settings arm past the gate).
        const std::string_view head = field.steps[0].name;
        if (head != "subscribers" && head != "acl" && head != "children" && head != "settings" &&
            head != "schema" && head != "identity")
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // Bare `:subscribers` and any `:subscribers.<tail>` spelling name nothing on
        // either door — the record is addressed whole, per slot — and the write door
        // already answers both ungated (its TAIL/WHOLE arms), so the read resolves the
        // same two spellings at the same pre-gate narrowness. Selector shapes (`[N]`,
        // `[]`, `[*]`) keep their per-door arms below.
        if (head == "subscribers") {
            const field_sel_t sub_sel = field_selector(field);
            if (sub_sel == field_sel_t::WHOLE || sub_sel == field_sel_t::TAIL)
                return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        }
        // Served whole — no member or slot addressing, so `:acl[N]` names nothing
        // (an ACE is not separately addressable). Resolving the shape here keeps
        // `:acl[7]` from being served the entire ACE collection under an OK status.
        // `whole_field` is the shared shape rule (#869), the same predicate the write
        // door's `:acl` arm uses — note only the SHAPE is shared: this door gates on
        // READ_ACL and that one on WRITE_ACL, and an UNACCEPTED shape resolves below the
        // gate here and above it there (the divergence pinned by `field_shape_matrix`).
        if (field.steps[0].name == "acl" && whole_field(field)) {
            if (!acl_allows(v, caller, acl_right_t::READ_ACL))
                return std::unexpected(status_t::PERMISSION_DENIED);
            return read_acl(v);
        }
        // `:identity` (#406, RFC-0011 §C.2) is PRE-AUTH BY DESIGN, so it resolves ABOVE
        // the READ gate — a narrow, named exemption that applies to this field alone.
        // The public key is precisely what an unauthenticated peer must obtain in order
        // to TOFU-pin and to verify the ADR-0045 challenge, so gating it behind READ
        // would deadlock first contact (the default ACL ships closed). It discloses
        // nothing the Noise handshake would not present as its static key anyway.
        // Node-scoped: it takes no vertex, and every vertex answers identically (§C.1).
        // The WHOLE `identity` namespace resolves here, not just the bare spelling: the
        // record is served whole and has no member or indexed addressing (§C.4), so any
        // other shape names nothing and is SCHEMA_NOT_FOUND — caller-independent, like
        // any unknown field. Resolving the namespace (rather than falling through) is
        // what makes that answer caller-independent: below this point sits the READ
        // gate, which would answer a denied caller PERMISSION_DENIED and so contradict
        // §C.4. Nothing is disclosed by the narrower answer — the record itself is
        // world-readable by design one line down.
        if (field.steps[0].name == "identity") {
            if (!whole_field(field)) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
            return read_identity();
        }
        // An UNKNOWN core-namespace `:settings` NAME resolves HERE, above the READ gate —
        // the exact mirror of the write door (see `field_write`'s settings arm: only
        // `settings.app.…` reaches a gate; every other spelling under `settings` falls
        // through to a terminal, ungated SCHEMA_NOT_FOUND). RFC-0022 §3.B deleted
        // `settings_t` outright, so the core namespace is EMPTY and every name in it is an
        // unknown name — which docs/reference/05 §`0x0B` answers with
        // `ERROR{tr::schema::not_found}`, a rule stated with NO caller qualifier.
        //
        // Below this line sits the READ gate, and a denied caller reaching it would be told
        // PERMISSION_DENIED on the READ of a name whose WRITE already answers
        // SCHEMA_NOT_FOUND — one name, two answers, split by who is asking. That is exactly
        // the caller-DEPENDENT disclosure §3.B forbids, so the read must resolve the name
        // first, at the same narrowness the write door does.
        //
        // Nothing leaks: "the core knob namespace is empty" is published spec text, so the
        // narrower answer discloses only what docs/reference/05 already states. Bare
        // `:settings` (the container) and the whole `settings.app.` subtree are untouched —
        // both are KNOWN names and keep their gates.
        //
        // The `settings.…` sub-shape is `app_field_sel` (#869) — resolved ONCE here and
        // reused below the gate, and the SAME classification the write door switches on.
        // A CORE_KNOB is the RFC-0022 §3.B empty namespace on both doors; the write door
        // reaches its terminal SCHEMA_NOT_FOUND for exactly this kind.
        std::string app_key;
        const app_sel_t app =
            field.steps[0].name == "settings" ? app_field_sel(field, app_key) : app_sel_t::NONE;
        if (app == app_sel_t::CORE_KNOB) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (!acl_allows(v, caller, acl_right_t::READ))
            return std::unexpected(status_t::PERMISSION_DENIED);
        // One synthesized POINT, served whole — not an array field, so no `[N]` surface.
        // The shared `whole_field` shape rule (#869).
        if (field.steps[0].name == "schema" && whole_field(field)) return read_schema(v);
        if (field.steps[0].name == "settings" && plain_step(field.steps[0])) {
            // The RFC-0010 §A.4 read surfaces. Bare ":settings" — the container
            // (RFC-0022 §4: the nested app record, and nothing else); ":settings.app" —
            // the app container alone; ":settings.app.<name…>" — one declared field's
            // stored TLV verbatim. A per-knob protocol read (":settings.deadline_ns")
            // names nothing and falls through to SCHEMA_NOT_FOUND, exactly as its write
            // now does.
            //
            // DIVERGENCE (#869), pinned NOT fixed: `plain_step(steps[0])` above is tested
            // HERE and not on the write door, so `:settings[0].app.<name>` is
            // SCHEMA_NOT_FOUND on a read and a successful WRITE. See `field_write`'s
            // settings arm; `field_shape_matrix` pins both answers.
            if (app == app_sel_t::NONE) return read_settings(v);
            if (app == app_sel_t::CONTAINER) return read_settings_app(v);
            if (app == app_sel_t::NAMED) {
                const std::string& key = app_key;
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
        // ":children" is handled above the lambda (folded rope — see read_children_folded).
        // A single slot ":subscribers[N]" — serve the stored SUBSCRIBER view (clone). The
        // shape is the shared `field_selector` classification (#869), replacing the
        // `indexed && !append && !wildcard` conjunction that restated path.hpp's `[N]`
        // validity window a second time. Every other `:subscribers` shape reaches the
        // terminal SCHEMA_NOT_FOUND below — including `[*]`, which the WRITE door answers
        // INVALID_PATH (the pinned #869 divergence; see `field_write`'s WILDCARD arm), and
        // including `[]`, whose whole-array read the wire door serves through
        // `read_subscribers` before ever reaching here.
        if (field.steps[0].name == "subscribers" && field_selector(field) == field_sel_t::SLOT) {
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

result_t<value_ref_t> graph_t::read(const path_t& path) const {
    vertex_t* v = find_ptr(path.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    // A plain value read SHARES the published value; a `:field` read composes one, so it goes
    // through the field surface and wraps. Splitting here rather than inside the field overload
    // keeps the cheap path free of the wrap.
    if (path.field().empty()) return read(vertex_handle_t{v});
    auto composed = read(vertex_handle_t{v}, path.field());
    if (!composed) return std::unexpected(composed.error());
    return value_ref_t::composed(std::move(*composed));
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

result_t<value_ref_t> graph_t::await(const path_t& path, std::chrono::nanoseconds timeout) {
    vertex_t* v = find_ptr(path.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    return await(vertex_handle_t{v}, timeout);
}

}  // namespace tr::graph
