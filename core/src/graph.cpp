/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/graph.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "graph_fields.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/init_fault.hpp"
#include "libtracer/key_view.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_slab_pool.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_arena.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/view.hpp"

namespace tr::graph {

using wire::encode;
using wire::key_view_t;
using wire::opt_t;
using wire::tlv_node_t;
using wire::type_t;
namespace {

/** @brief A canonical `NAME` TLV header: type, `opt = 0`, `u16` length. */
inline constexpr std::size_t kNameHeaderBytes = 4;

/**
 * @brief The stack frame a wide fan-out snapshots into before it reaches the table source
 *        (#1885): eight times the inline width, so it scales with the build's stated stack
 *        budget — 64 views (3 KiB) on a 64-bit host, 16 on the ESP-IDF fragment's two.
 */
inline constexpr std::size_t kFanoutFrameBytes = 8 * vertex_t::kInlineFanout * sizeof(edge_view_t);

/**
 * @brief The link an edge's cold half DELIVERS over — the one it holds (#1816); empty for an
 *        edge with no cold half or a `:subscribers[]` field-write edge, which delivers locally.
 */
std::string_view delivery_link(const remote_ptr_t& remote) noexcept {
    return remote ? std::string_view(remote->link) : std::string_view{};
}

// ---------------------------------------------------------------------------------------------
// ADR-0080 — the reclamation seam's machinery, for all three policies.
//
// The whole mechanism is ONE per-thread object plus, under `reclaim_qsbr` alone, an announcement
// into the shared domain in `%qsbr.hpp`. Both grace points are decided by the SAME per-thread
// depth counter, which is why one bracket serves all three policies:
//
//   * `reclaim_local` — the grace point is a property OF THIS THREAD ("my dispatch stack
//     unwound"), so the state that tracks it is thread-local and never shared. No atomic and no
//     cache line is involved on the dispatch path, and a parked pair provably cannot outlive
//     anything: the thread that parks IS the thread dispatching, and it drains before returning
//     to depth 0.
//   * `reclaim_qsbr` — the grace point is a property OF EVERY THREAD. The depth counter is still
//     what decides WHEN (the transition to 0 is this thread's quiescent state, and the
//     transition from 0 is its announcement), but the retired pairs cannot live here: the thread
//     that retires is typically NOT the thread that will complete the grace period. See
//     `%qsbr.hpp` §qsbr_why_global for why a per-thread retired list would never drain in the
//     very case this policy exists for.
//
// `graph_t` gains NO data member under any policy, so no existing field's offset moves.
//
// NOTHING HERE IS A TEMPLATE, and that is a measured decision rather than an oversight. The
// obvious way to keep the QSBR domain out of a build that does not bind it is to parameterise
// this trio on the policy, on the theory that an `if constexpr` discarded statement outside a
// template still odr-uses what it names. GCC does not behave that way: a discarded branch
// naming `detail_qsbr::registry()` emits ZERO bytes of it at -O0, -O1 -g and -O3 alike (the
// `.bss` is confirmed absent from the default build by `nm`). Templatising anyway is not free —
// it changes every mangled name in this block, which is what stopped the default build's
// `graph.cpp.o` from comparing byte-identical to the one before this policy existed. So the
// trio keeps its shipped shape, and the ONE thing that genuinely must vary with the policy —
// how much parking storage the thread-local carries — varies as an array bound.
// ---------------------------------------------------------------------------------------------

/**
 * @brief The count of retired pairs dropped for want of a parking slot, process-wide.
 *
 * Behind `graph_t::deferred_release_drops`, which sums this with the QSBR domain's own tally.
 * Relaxed: it is a diagnostic that orders nothing, exactly like the graph's snapshot-drop
 * counters. Word-wide, as they are (#1697): a 64-bit atomic is a libatomic call on rv32.
 */
std::atomic<std::size_t> g_deferred_release_drops{0};

/**
 * @brief How many pairs one thread parks locally — @ref kDeferredReleaseSlots, or none under a
 *        policy whose retired pairs live in the SHARED table instead.
 *
 * A QSBR build never calls @ref park_release, so the full array would be dead TLS on every
 * dispatching thread (256 B a piece on a 64-bit host); it collapses to one unused entry, a
 * zero-length array being an extension rather than standard C++. Collapsing the BOUND rather
 * than the struct is what lets the default build's thread-local keep byte-for-byte the layout
 * it had — the array is the only thing that may differ, and only where it is never touched.
 */
inline constexpr std::size_t kLocalParkSlots =
    reclaim_policy_t::kGraceSpansThreads ? 1 : kDeferredReleaseSlots;

/**
 * @brief One thread's dispatch-stack depth and the pairs retired from inside it (ADR-0080).
 *
 * Deliberately a plain aggregate of scalars: it is `thread_local`, so a non-trivial destructor
 * would register a per-thread `__cxa_thread_atexit` handler and a non-constant initializer would
 * put a guard variable check on the dispatch path. Neither is acceptable for a counter whose
 * whole budget is an increment. `retired_callback_t` is trivially copyable for the same reason,
 * so `parked` is storage rather than a container.
 */
struct dispatch_state_t {
    /** @brief Nesting depth of `fan_out` / latch dispatch on this thread; 0 ⇒ quiescent. */
    unsigned depth;
    /** @brief How many of @ref parked are occupied. Stays 0 under `reclaim_qsbr`. */
    unsigned parked_n;
    /** @brief Pairs retired at depth > 0, awaiting this thread's return to depth 0. */
    retired_callback_t parked[kLocalParkSlots];
};

/**
 * @brief This thread's @ref dispatch_state_t.
 *
 * `constinit`-shaped (all-zero, trivially destructible), so the access is a bare TLS address
 * computation with no guard variable and no lazy-init branch.
 */
[[nodiscard]] inline dispatch_state_t& dispatch_state() noexcept {
    static thread_local dispatch_state_t state{};
    return state;
}

/**
 * @brief Run every pair this thread parked, then empty the park — the grace point itself.
 *
 * `noinline` and out of line on purpose: its caller is the dispatch path, whose cost must be the
 * depth decrement and one predictable-not-taken branch. Nothing here runs under a graph lock —
 * the caller is a `fan_out` that has already released its edge pin.
 *
 * The park is emptied BEFORE the first hook runs. A hook is arbitrary user code that may publish,
 * and a nested publish re-enters this function; taking the entries first means it finds an empty
 * park rather than re-running a hook that is already executing.
 *
 * `maybe_unused` because a `reclaim_qsbr` build parks nothing locally and never reaches it.
 */
[[maybe_unused]] [[gnu::noinline]] void run_parked_releases(dispatch_state_t& s) {
    retired_callback_t taken[kLocalParkSlots];
    const unsigned n = s.parked_n;
    for (unsigned i = 0; i < n; ++i) taken[i] = s.parked[i];
    s.parked_n = 0;
    for (unsigned i = 0; i < n; ++i) taken[i].release(taken[i].ctx);
}

/**
 * @brief Bracket one dispatch on this thread — ADR-0080's grace-point instrument.
 *
 * Constructed once per `fan_out` (and once per ADR-0049 durability latch), never per edge, so
 * the cost is independent of fan-out width as the ADR requires. Under a non-deferring policy
 * every member below compiles away and the object is empty.
 */
class dispatch_scope_t {
   public:
    dispatch_scope_t() noexcept {
        if constexpr (reclaim_policy_t::kDefersToDispatchExit) {
            dispatch_state_t& s = dispatch_state();
            if constexpr (reclaim_policy_t::kGraceSpansThreads) {
                // Only the OUTERMOST section announces (`enter` keeps the count, shared with
                // the router's frame bracket). An inner one is already covered by the
                // announcement the outer one made, and re-announcing at a NEWER epoch would let
                // a scan conclude past a retirement the outer snapshot still names.
                detail_qsbr::enter();
            }
            ++s.depth;
        }
    }
    dispatch_scope_t(const dispatch_scope_t&) = delete;
    dispatch_scope_t& operator=(const dispatch_scope_t&) = delete;

    /** @brief Leaving the OUTERMOST dispatch is the grace point; anything inner is a decrement. */
    ~dispatch_scope_t() {
        if constexpr (reclaim_policy_t::kDefersToDispatchExit) {
            dispatch_state_t& s = dispatch_state();
            if constexpr (reclaim_policy_t::kGraceSpansThreads) {
                --s.depth;
                // Go quiescent FIRST, so this thread's own announcement is already withdrawn
                // when the drain scans — otherwise every drain would see itself as a reason to
                // defer, and a node with one dispatching thread would reclaim nothing, ever.
                // A dispatch inside a router frame is not the outermost section: the frame's
                // own exit is the quiescent point.
                if (!detail_qsbr::leave()) return;
                pass_quiescent_state<reclaim_policy_t>();
            } else {
                // Both fields live in the same thread-local object, so the common case — unwind
                // to depth 0 with an empty park — is two loads off one base and a branch nobody
                // takes.
                if (--s.depth == 0 && s.parked_n != 0) run_parked_releases(s);
            }
        }
    }
};

/**
 * @brief Whether this thread is currently inside a delivery — the question `unsubscribe` asks.
 *
 * Under a non-deferring policy the depth is never maintained, so this is a compile-time
 * `false` and `unsubscribe`'s release is unconditionally inline.
 */
[[nodiscard]] inline bool inside_dispatch() noexcept {
    if constexpr (reclaim_policy_t::kDefersToDispatchExit) return dispatch_state().depth != 0;
    return false;
}

/**
 * @brief Park @p pair until this thread unwinds to depth 0, or DROP it if there is no room.
 *
 * The OOM rule, stated once: on a full park the entry is discarded and its hook is **never**
 * run. That is a deliberate leak, and it is the only safe answer — running the hook here would
 * release a context the fan-out one frame up is still holding in its snapshot, which is the
 * very use-after-free this whole seam exists to close. The drop is counted so an undersized
 * `kDeferredReleaseSlots` is visible instead of silent.
 */
[[maybe_unused]] void park_release(const retired_callback_t& pair) {
    dispatch_state_t& s = dispatch_state();
    if (s.parked_n >= kLocalParkSlots) {
        g_deferred_release_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s.parked[s.parked_n++] = pair;
}

/**
 * @brief Retire one `{ctx, release}` pair at the bound policy's grace point — the whole of
 *        `unsubscribe`'s policy body, in one place.
 *
 * Every branch releases INLINE in the case where nothing can still be holding the pair, which is
 * why "quiescent on return" is true of an ordinary unsubscribe under all three policies.
 */
inline void retire_pair(const retired_callback_t& pair) {
    if constexpr (reclaim_policy_t::kGraceSpansThreads) {
        // The domain decides inline-vs-defer by SCANNING, not by asking about this thread: a
        // sibling thread's live snapshot is exactly what the depth counter cannot see. It counts
        // its own drops, which `deferred_release_drops` folds back in below.
        detail_qsbr::retire(pair.ctx, pair.release);
    } else if (inside_dispatch()) {
        park_release(pair);
    } else {
        pair.release(pair.ctx);
    }
}

/** @brief The QSBR domain's drop tally, or a compile-time 0 for a policy that has no domain. */
[[nodiscard]] inline std::uint64_t qsbr_drops() noexcept {
    if constexpr (reclaim_policy_t::kGraceSpansThreads) return detail_qsbr::drops();
    return 0;
}

/**
 * @brief One vertex's own path SEGMENT text — its packed key record minus the length byte.
 *
 * The seam RFC-0018 opened, in one place. A vertex-map key record used to BE a `NAME` TLV, so
 * every `:children` / composed-read member borrowed `name().bytes()` verbatim as its POINT
 * body. A packed record is `[u8 len][text]` and those members are POINT composites, not
 * `PATH` bodies — the RFC removes `NAME` from `PATH` bodies only — so the text is what the
 * member carries and the `NAME` framing is emitted around it. Empty for the root, whose
 * record is empty.
 */
[[nodiscard]] std::span<const std::byte> child_segment(const vertex_t& v) noexcept {
    const std::span<const std::byte> rec = v.name().bytes();
    return rec.empty() ? rec : rec.subspan(1);
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
// the full first-match-per-bit host policy when a fragment binds it), the
// effective-ACL merge semantics (effective_acl_t), and parse_acl/encode_acl.
// The graph keeps only the ancestor walk (inside the lazy per-vertex cache
// rebuild) and the subtree-precise invalidation below.

/**
 * @brief True iff the opt byte @p o carries no trailer bits.
 *
 * A branch write (RFC-0005)
 * stores refcount subviews of the written frame, so a trailer inside the tree
 * cannot be sliced off without a copy — trailer-carrying nodes are rejected
 * (TYPE_MISMATCH), keeping stored values trailer-less at rest (ADR-0041 §4). The FOLD
 * emission holds a stored value to the same rule before framing it as a §B node, so the
 * parse and the emit read one predicate.
 */
[[nodiscard]] bool trailer_less(const opt_t& o) noexcept {
    return !o.ts && !o.cr && !o.cw && !o.tf;
}

/**
 * @brief The subview of `frame_view` covering `span` — a refcount bump on the written frame's
 *        segment, never a byte copy (RFC-0005 §decomposition).
 *
 * Precondition:
 * `span` points into `frame_view.bytes()` (it is an arena span over that frame).
 */
[[nodiscard]] view::view_t slice_of(const view::view_t& frame_view,
                                    std::span<const std::byte> span) {
    const std::size_t off = static_cast<std::size_t>(span.data() - frame_view.bytes().data());
    return frame_view.subview(off, span.size());
}

/**
 * @brief One node of a branch write's plan (RFC-0005), and the landing site it becomes: the vertex
 *        key, the VALUE slice that lands there (empty when the node carries no value of its own),
 *        the slice this vertex's subscribers are notified with, and what the apply half did there.
 *
 * `notify` is RFC-0005 §B's leaf/interior choice, made ONCE by the parse: the VALUE for a leaf
 * node, the node's whole POINT subtree for an interior node (the smallest subview covering every
 * write at-or-below the subscription point), and empty when no value lands at-or-below it. The
 * root is tagged once after the parse — `vx` is the written vertex and `notify` the whole written
 * TLV — so no later pass re-detects it by key or address.
 */
struct branch_node_t {
    mem::bytes_t key;       /**< @brief The landing vertex's canonical key (table source, #1778). */
    view::view_t store{};   /**< @brief The node's own VALUE slice; empty on a value-free node. */
    view::view_t notify{};  /**< @brief The §B notify slice; empty when nothing lands below. */
    vertex_t* vx = nullptr; /**< @brief The resolved landing vertex: the root, or a site with a
                                 VALUE once admitted; null on a value-free interior node. */
    /**
     * @brief What this site's own store published here — null until the apply loop, and on a
     *        site whose store soft-failed. clear_pending compares it against the vertex's
     *        current LKV so a racing assign's mark keeps its delivery (#1185); a null on a vertex
     *        that holds an LKV simply fails that compare, which is the safe direction (a
     *        duplicate delivery, never a lost one).
     */
    value_ref_t stored;
    /**
     * @brief Did this site's own admission filter REFUSE its slice? Distinct from a null
     *        `stored`, which a soft-failed store also produces: the notify half must not fan a
     *        refused slice out, and it must keep fanning out a soft-failed one (that value did
     *        reach the seam; only its retention failed).
     */
    bool refused = false;
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
                                               const view::view_t& frame_view,
                                               std::span<const std::byte> key,
                                               mem::block_array_t<branch_node_t>& out,
                                               mem::block_source_t& src) {
    /**
     * @brief One open POINT node: its arena index, the sibling cursor over its
     *        remaining children, its key, and the strict-shape accumulators.
     */
    struct open_t {
        std::uint32_t node = 0;       /**< @brief This node's arena pre-order index. */
        std::uint32_t next = 0;       /**< @brief Next unvisited child (arena pre-order index). */
        mem::bytes_t key;             /**< @brief This node's canonical vertex key. */
        view::view_t store{};         /**< @brief The node's own VALUE slice, if any. */
        bool has_point_child = false; /**< @brief A POINT sub-branch was seen. */
        bool subtree_value = false;   /**< @brief A VALUE landed at or below this node. */
    };

    // Validate a POINT node's shape (structured, trailer-less, leading NAME) and
    // open it with the sibling cursor past that NAME. The open-node stack, the plan and every
    // key are core containers over @p src (#873 phase 1, #1778): a refusal soft-fails the
    // whole branch write as BACKPRESSURE (the store-leg status), never an abort — and with no
    // probe-then-commit window left (#850, #981), on an MCU node too.
    //
    // The node's key starts as a copy of @p base — the whole target key for the root, the
    // parent's key for a sub-branch — with room reserved for one more packed record (RFC-0018:
    // a length byte plus the name), and the NAME's body is answered so a sub-branch can append
    // that record within capacity. @p base may view a key on the stack: the push relocates the
    // `open_t`, never the key's block.
    mem::block_array_t<open_t> stack(src);
    const auto open = [&a, &stack, &src](
                          std::uint32_t node,
                          std::span<const std::byte> base) -> result_t<std::span<const std::byte>> {
        if (!a[node].opt.pl || !trailer_less(a[node].opt))
            return std::unexpected(status_t::TYPE_MISMATCH);
        const std::uint32_t cn = wire::tlv_arena_t::first_child(node);
        if (cn >= a[node].end || a[cn].type != type_t::NAME)
            return std::unexpected(status_t::TYPE_MISMATCH);
        if (!stack.push_back(
                open_t{.node = node, .next = a.next_sibling(cn), .key = mem::bytes_t(src)}) ||
            !stack.back().key.reserve(base.size() + 1 + a[cn].body.size()) ||
            !stack.back().key.append(base.data(), base.size()))
            return std::unexpected(status_t::BACKPRESSURE);
        return a[cn].body;
    };
    if (const auto o = open(root, key); !o) return std::unexpected(o.error());

    for (;;) {
        open_t& top = stack.back();
        if (top.next >= a[top.node].end) {
            // Node complete — emit its landing site (post-order) and fold its
            // subtree-has-value into the parent. The notify slice is the §B leaf/interior
            // choice; a value-free leaf's empty `store` doubles as its empty notify.
            const bool subtree_value = top.subtree_value;
            view::view_t notify = top.has_point_child && subtree_value
                                      ? slice_of(frame_view, a[top.node].wire)
                                      : top.store;
            if (!out.push_back(branch_node_t{.key = std::move(top.key),
                                             .store = std::move(top.store),
                                             .notify = std::move(notify),
                                             .vx = nullptr,
                                             .stored = value_ref_t{},
                                             .refused = false}))
                return std::unexpected(status_t::BACKPRESSURE);
            stack.pop_back();
            if (stack.empty()) return subtree_value;
            stack.back().subtree_value |= subtree_value;
            continue;
        }
        const std::uint32_t ci = top.next;
        const wire::arena_tlv_t& c = a[ci];
        top.next = a.next_sibling(ci);
        if (c.type == type_t::VALUE) {
            if (!top.store.empty() || !trailer_less(c.opt))
                return std::unexpected(status_t::TYPE_MISMATCH);
            top.subtree_value = true;
            top.store = slice_of(frame_view, c.wire);
        } else if (c.type == type_t::POINT) {
            top.has_point_child = true;
            // The child key = parent key + one NAME record, composed failably (#477): open()
            // reserved the exact final size, so the emit's false means only an illegal
            // segment. `top` is invalidated by the push inside open().
            const auto name = open(ci, mem::as_span(top.key));
            if (!name) return std::unexpected(name.error());
            if (!wire::emit_path_segment(stack.back().key, *name))
                return std::unexpected(status_t::INVALID_PATH);
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

/**
 * @brief True when @p src is the build's default root (@ref mem::default_root) — the test the
 *        constructor's "process-default fold" turns on (#873 phase 1, #1777).
 *
 * A graph handed nothing keeps the ADR-0047 §2 devirtualized `HEAP` reclaim arm on its value
 * segments (@ref mem::heap_backend draws from the root's value sub-pool on a `kSlabPool`
 * build), and on such a build derives its value and table sub-pools from the root. The
 * adapters over an INJECTED root are used only where a host actually injected something. One
 * branch, at construction, never again.
 */
[[nodiscard]] bool is_default_source(const mem::block_source_t* src) noexcept {
    return src == nullptr || src == &mem::default_root();
}

/**
 * @brief The sub-pool @p sub when @p src is the build's default root (the graph DERIVES its
 *        sub-pools from the host root, #1777, or the MCU arena, #1783), else @p src itself: an
 *        injected root serves every purpose.
 */
[[nodiscard]] mem::block_source_t* sub_pool(mem::block_source_t& src,
                                            mem::block_source_t& sub) noexcept {
    return is_default_source(&src) ? &sub : &src;
}

/** @brief The retention a role holds when its policy names none (RFC-0028 §5.4). */
[[nodiscard]] constexpr retention_t default_retention(role_t role) noexcept {
    return role == role_t::HANDLER  ? retention_t::NONE
           : role == role_t::STREAM ? retention_t::N
                                    : retention_t::LAST;
}

/** @brief The legal (role, retention) pairings: `STORED_VALUE` `NONE`|`LAST`, `STREAM`
 *         `NONE`|`N`, `HANDLER` `NONE`. */
[[nodiscard]] constexpr bool retention_legal(role_t role, retention_t r) noexcept {
    return r == retention_t::NONE || (r == retention_t::LAST && role == role_t::STORED_VALUE) ||
           (r == retention_t::N && role == role_t::STREAM);
}

/** @brief Whether @p policy's retention is legal for @p role (unset is always legal). */
[[nodiscard]] bool policy_legal(role_t role, const vertex_policy_t& policy) noexcept {
    return retention_legal(role, policy.retention.value_or(default_retention(role)));
}

/** @brief Whether @p k lies in the subtree whose root key is @p lo: a parent's key is a
 *         byte-prefix of every descendant's (RFC-0008 §B). */
[[nodiscard]] bool in_subtree(std::span<const std::byte> lo,
                              std::span<const std::byte> k) noexcept {
    return k.size() >= lo.size() && std::equal(lo.begin(), lo.end(), k.begin());
}

/** @brief The `[first, last)` run of @p set's keys in the subtree whose root key is @p lo —
 *         contiguous in byte order, starting at the lower bound of @p lo. */
template <class Set>
[[nodiscard]] std::pair<typename Set::pos_t, typename Set::pos_t> subtree_run(
    const Set& set, std::span<const std::byte> lo) {
    const typename Set::pos_t first = set.lower_bound(lo);
    typename Set::pos_t last = first;
    while (last != set.end_pos() && in_subtree(lo, mem::as_span(set.at(last).key)))
        last = set.next(last);
    return {first, last};
}

}  // namespace

graph_t::own_pool_t::own_pool_t(mem::block_source_t& src) noexcept {
    // Over the platform heap; none when @p src is injected. A refused pool is a sizing bug, as
    // for every other construction-time draw. On the MCU arena (#1783) the graph draws its
    // tables from the arena's table sub-pool, which it shares and does not own.
    if (!is_default_source(&src)) return;
    if constexpr (!mem::kSlabPool) {
        pool = &mem::table_source();
    } else {
        // Page-sized base slabs, not the shared pools' 64 KiB: every default graph opens a slab
        // per class it touches, so the base slab IS the per-graph floor (64 KiB: 640 KiB for a
        // graph with one subscribed leaf; 4 KiB: 48 KiB). Tables are control-plane state, so
        // the extra carves a small slab costs never reach a write.
        constexpr std::size_t kSlabBytes = 4096;
        pool = mem::make_in<mem::host_pool_t>(
            mem::heap_source(), "tables",
            std::span<const std::size_t, mem::host_pool_t::classes()>(config_t::kSizeClasses),
            mem::heap_source(), kSlabBytes);
        if (pool == nullptr) mem::exhausted_at_init(mem::heap_source(), "graph_t");
    }
}

graph_t::own_pool_t::~own_pool_t() {
    if constexpr (mem::kSlabPool)
        mem::drop_in(mem::heap_source(), static_cast<mem::host_pool_t*>(pool));
}

void graph_t::trim_tables() noexcept {
    if constexpr (mem::kSlabPool) {
        if (own_tables_.pool != nullptr) static_cast<mem::host_pool_t*>(own_tables_.pool)->trim();
    }
}

graph_t::graph_t(mem::block_source_t& src, graph_hooks_t hooks)
    : own_tables_(src),
      retired_seams_(own_tables_.or_root(src)),
      vertex_slots_(own_tables_.or_root(src)),
      src_backend_(src),
      child_types_(own_tables_.or_root(src)),
      identity_record_(own_tables_.or_root(src)),
      pending_(own_tables_.or_root(src)),
      unconditional_(own_tables_.or_root(src)),
      ctl_(&src),
      values_(sub_pool(src, mem::value_source())),
      tables_(&own_tables_.or_root(src)),
      parked_releases_(own_tables_.or_root(src)) {
    // The process-default FOLD, resolved in the BODY: `&src_backend_` is only taken once its
    // lifetime has started. A few stores at construction, never read again.
    // On the host default root (#1777) values and rings draw from the value sub-pool and
    // every table from the table sub-pool (both resolved in the initializer list, so the link
    // index is built on the right one); `value_backend_` stays `heap_backend()`, which draws
    // from the value sub-pool on that build.
    if (!is_default_source(&src)) value_backend_ = &src_backend_;
    payload_rights_.src = tables_;
    admissions_.src = tables_;
    // Both structural roots, in one table-source block. The anchors' private root (#1223)
    // takes NO vertex slot: it is never an anchor itself and no element can name it, and
    // giving it one would put a second unaddressable hole in an index whose only documented
    // hole is slot 0. A source too small for the roots and the first index chunk is a sizing
    // bug (ADR-0056, ADR-0083).
    roots_ = mem::make_block<roots_t>(*tables_, *tables_);
    if (!roots_ || !vertex_slots_.reserve_next()) mem::exhausted_at_init(*tables_, "graph_t");
    set_hooks(hooks);
    // Slot 0 is the structural root (RFC-0024 §6.4): the index is seeded here so it stays
    // allocation-ordered from the first vertex_t this graph owns. The root is not a
    // registrable address, so no bound path ever names slot 0 — it is in the vector because
    // leaving a hole there would make "slot i is the i-th vertex_t allocated" false.
    vertex_slots_.push_back(root());
    note_owner_slot(*root());
    // The one built-in creation-catalog type (#82, ADR-0017): `stored_value` makes a
    // plain last-writer-wins vertex at the composed child key. Its optional SPEC
    // `config` SETTINGS is ignored for now (a stored-value has no instantiation params
    // beyond the standard `:settings` field, written separately). Devices add richer
    // types (controllers, transport connections — #83) via register_child_type.
    register_child_type("stored_value", {[](void*, graph_t& g, std::vector<std::byte> child_key,
                                            const tlv_node_t*) -> result_t<vertex_handle_t> {
                                             return g.register_vertex_key(std::move(child_key),
                                                                          role_t::STORED_VALUE);
                                         },
                                         nullptr});
}

void graph_t::register_child_type(std::string_view type, child_factory_t factory) {
    // Exclusive: an insert moves the entries `create_child` reads (#1049). Setup-only by
    // doctrine; locked so that a caller who ignores that gets a serialized registration
    // rather than a torn read of a table driven by a peer's bytes.
    const std::unique_lock lock(child_types_mutex_);
    mem::string_t key(*tables_);
    child_factory_t* const slot =
        key.assign(type) ? child_types_.try_emplace(std::move(key), factory).value : nullptr;
    if (slot == nullptr) mem::exhausted_at_init(*tables_, "register_child_type");
    *slot = factory;  // a re-registration replaces
}

vertex_handle_t graph_t::register_vertex(const path_t& path, role_t role, handlers_t handlers,
                                         vertex_policy_t policy,
                                         std::span<const payload_right_t> rights) {
    const std::size_t ceiling_refusals = vertex_ceiling_refusals_.load(std::memory_order_relaxed);
    result_t<vertex_handle_t> h =
        try_register_vertex(path, role, handlers, std::move(policy), rights);
    // PATH_IN_USE on a compile-site literal is a source bug, not a runtime outcome — fail loud
    // (ADR-0056, mirroring path_t(std::string_view)) rather than hand back a result the caller
    // would only `*`-deref unchecked. A genuine runtime path uses try_register_vertex.
    // BACKPRESSURE is a sizing bug either way, reported before the abort: the vertex ceiling
    // when this call bumped its refusal count (#1314), else the table source running dry, named
    // with the sub-pool and the bytes it was asked for (ADR-0056 amendment, ADR-0083).
    if (!h && h.error() == status_t::BACKPRESSURE &&
        vertex_ceiling_refusals_.load(std::memory_order_relaxed) != ceiling_refusals) {
        config_t::fault_sink_t::report(
            init_fault_t{.kind = init_fault_kind_t::VERTEX_CEILING,
                         .call = "register_vertex",
                         .ceiling = vertex_ceiling_.load(std::memory_order_relaxed)});
        std::abort();
    }
    if (!h && h.error() == status_t::BACKPRESSURE)
        mem::exhausted_at_init(*tables_, "register_vertex");
    if (!h) std::abort();
    return *h;
}

result_t<vertex_handle_t> graph_t::try_register_vertex(const path_t& path, role_t role,
                                                       handlers_t handlers, vertex_policy_t policy,
                                                       std::span<const payload_right_t> rights) {
    return register_vertex_key_span(path.key(), role, handlers, rights, {}, std::move(policy));
}

result_t<vertex_handle_t> graph_t::register_vertex_key(std::span<const std::byte> key, role_t role,
                                                       handlers_t handlers, vertex_policy_t policy,
                                                       std::span<const payload_right_t> rights,
                                                       std::span<const std::byte> schema_catalog) {
    // The key is BORROWED for the call (#1781): the descent below never retains the argument —
    // every record it keeps is copied into the vertex's own `path_key_t` — so a caller holding
    // a `std::vector`, a core array or a frame's bytes passes them as they are (#1139).
    return register_vertex_key_span(key, role, handlers, rights, schema_catalog, std::move(policy));
}

result_t<vertex_handle_t> graph_t::register_vertex_key_span(
    std::span<const std::byte> key, role_t role, const handlers_t& handlers,
    std::span<const payload_right_t> rights, std::span<const std::byte> schema_catalog,
    vertex_policy_t policy) {
    // Refused BEFORE the descent, so an illegal policy registers nothing — not even the
    // placeholder levels a descent would create.
    if (!policy_legal(role, policy)) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    const std::unique_lock lock(map_mutex_);
    // Descend the Composite tree (ADR-0057), creating unregistered PLACEHOLDER nodes for
    // missing intermediate levels — invisible to find/read_children until a registration
    // fills them in place (matching the flat map, where intermediates did not exist).
    // NOTHING is inherited (RFC-0022 §3.F). The descent carries no policy down with it:
    // with `settings_t` deleted there is no per-vertex policy left to inherit, so there is
    // no ancestor walk, no cached ancestor reference, and no question about what happens to
    // descendants when a parent's configuration changes after they exist. The two owner-side
    // magnitudes are declared per vertex, by the owner, or they are at their defaults.
    vertex_t* node = root();
    std::size_t i = 0;
    while (i < key.size()) {
        const std::size_t e = segment_end(key, i);
        const std::span<const std::byte> record{key.data() + i, e - i};
        vertex_t* child = node->child_by_record(record);
        if (child == nullptr) {
            // Charge the creation against the vertex-slot census BEFORE allocating (#1314).
            // This is the one door every creation goes through — a local registration, and
            // every vertex a creation hook registers for a write that missed (RFC-0030 §7) —
            // so counting here is what makes hook-created vertices COUNTED vertices. Count, then
            // act (#838's shape): past the ceiling the creation answers BACKPRESSURE, the
            // injected-store exhaustion status, and the refusal is tallied so the bound is
            // observable rather than inferred. Default is kNoVertexCeiling, so an un-sized node is
            // byte-for-byte unchanged.
            if (vertex_slots_.size() >= vertex_ceiling_.load(std::memory_order_relaxed)) {
                vertex_ceiling_refusals_.fetch_add(1, std::memory_order_relaxed);
                return std::unexpected(status_t::BACKPRESSURE);
            }
            // A placeholder is a plain STORED_VALUE with no handlers, so `adopt_identity`'s
            // early return fires and it allocates NO extension block. Every failable step —
            // the index slot, the vertex, its parent's child entry — runs before anything is
            // linked in, so a refused creation (#1778) leaves the tree and the index as they
            // were. Placeholders made by EARLIER levels of this descent stay: they are
            // invisible, and the next registration down this path reuses them.
            vertex_t* const fresh = make_placeholder(record);
            if (fresh == nullptr) return std::unexpected(status_t::BACKPRESSURE);
            // Subtree-subscription init (RFC-0005): a vertex born under a subscribed
            // ancestor starts with the ancestor-listener count already summed — O(1) from
            // the parent's maintained counters (under the same unique lock the
            // note_subscriber_* walks exclude, so the sum and a concurrent subscribe walk
            // never double-count); the write path's is-anyone-listening check stays a
            // single relaxed load.
            fresh->init_listeners_above(node->listeners_above() + node->own_subs());
            child = node->add_child(fresh, *tables_);
            if (child == nullptr) {
                mem::drop_in(*tables_, fresh);
                return std::unexpected(status_t::BACKPRESSURE);
            }
            // One slot per vertex_t ALLOCATION (RFC-0024 §6.4), appended under the same
            // unique map-lock hold that linked it in, so slot order is allocation order.
            // Placeholders take a slot too: they are ordinary vertex_t objects that a later
            // registration fills IN PLACE, so skipping them here would hand the same object
            // two different slots depending on which side of its fill() the mint happened.
            vertex_slots_.push_back(child);
            note_owner_slot(*child);
        }
        node = child;
        i = e;
    }
    if (node->registered()) return std::unexpected(status_t::PATH_IN_USE);
    // The RFC-0014 Amendment 2 declaration is copied in here, before `fill` adopts the
    // handlers: the rows are the graph's (one immortal node per declaring registration), the
    // vertex keeps only the flag bit that says they exist. We are under the unique map lock,
    // which is exactly the hold `declare_payload_rights` requires. The RFC-0014 Amendment 3
    // `:schema` catalog rides the same node, for the same reason and under the same hold.
    // Same treatment, same hold, and for the same reason (see `graph_t::admissions_`): the two
    // ADMISSION filters are taken here, before `fill` adopts the rest, so the seam block
    // `adopt_identity` may allocate is byte-for-byte the one it allocated before this feature.
    // Each declaration raises its vertex flag as it lands (#1778). A node published for a
    // refused registration is never walked once the refusal lowers the flags again, and a later
    // declaration at this address is found first anyway.
    // The policy lands on the still-unregistered node first (#1778), the delivery mode with its
    // sweep-set entry included (#1920): a refusal anywhere in this chain leaves a placeholder,
    // which `find` does not answer for and a sweep skips. The refusal clears every declaration
    // back off it, and takes back a mode that landed with its entry, so the next registration
    // here, through a door that brings no policy (a creation hook's), inherits nothing of the
    // refused one. The sweep lock nests inside the map lock here, and only in this direction:
    // nothing takes the map lock under the sweep lock (ADR-0057).
    const bool mode_moves = node->delivery_mode() != policy.delivery_mode;
    if (!apply_policy(node, role, std::move(policy),
                      mode_moves ? key : std::span<const std::byte>{}) ||
        !declare_payload_rights(node, rights, schema_catalog) ||
        !declare_admission(node, handlers) || !node->fill(role, handlers, *tables_)) {
        node->clear_declarations();
        if (mode_moves) {
            const std::lock_guard slock(sweep_mutex_);
            node->set_delivery_mode(delivery_mode_t::IF_NEWER);
            (void)unconditional_.erase(key);
        }
        return std::unexpected(status_t::BACKPRESSURE);
    }
    return vertex_handle_t{node};
}

bool graph_t::declare_payload_rights(vertex_t* v, std::span<const payload_right_t> rows,
                                     std::span<const std::byte> catalog) {
    // The overwhelming majority: no node, no flag, no cost.
    if (rows.empty() && catalog.empty()) return true;
    // PREPEND, so a re-registration at the same address publishes rows the walk finds before
    // any the previous occupant left behind (the list is never unlinked — see the member's
    // doc for why that is what makes the gate's walk lock-free). Filled before it is
    // published, so a refused copy frees an unpublished node.
    payload_right_node_t* const node = mem::make_in<payload_right_node_t>(*tables_, *tables_);
    if (node == nullptr || !node->rows.append(rows.data(), rows.size()) ||
        !mem::assign_bytes(node->catalog, catalog)) {
        mem::drop_in(*tables_, node);
        return false;
    }
    node->v = v;
    payload_rights_.prepend(node);
    v->mark_payload_rights();
    return true;
}

bool graph_t::declare_admission(vertex_t* v, const handlers_t& h) {
    // The overwhelming majority: no node, no flag, no cost.
    if (!h.on_admit && !h.on_app_field_admit && !h.on_app_field_read) return true;
    // PREPEND, so a re-registration at the same address publishes a filter the walk finds
    // before any the previous occupant left behind (the list is never unlinked — see the
    // member's doc for why that is what makes the read lock-free).
    admission_node_t* const node = mem::make_in<admission_node_t>(
        *tables_,
        admission_node_t{v, h.on_admit, h.on_app_field_admit, h.on_app_field_read, {}, nullptr});
    if (node == nullptr) return false;
    admissions_.prepend(node);
    v->mark_admission();
    return true;
}

const graph_t::admission_node_t* graph_t::admission_for(const vertex_t* v) const noexcept {
    // Walks only for a vertex whose flag says it installed one — tested HERE, so no caller
    // repeats it: the list holds one node per declaring registration, and a node is immortal,
    // so no lock is needed to read one. The FIRST match is the vertex's own newest declaration
    // — an older node left by a previous occupant of this address sits behind it and must
    // never answer for it.
    if (!v->has_admission()) return nullptr;
    for (const admission_node_t* n = admissions_.head.load(std::memory_order_acquire); n != nullptr;
         n = n->next)
        if (n->v == v) return n;
    return nullptr;
}

result_t<void> graph_t::set_creation_hook(vertex_handle_t parent, creation_hook_t hook) {
    if constexpr (!config_t::kCreationHooks) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    vertex_t* const v = parent.get();
    // The unique map lock: `prepend` requires it, and it orders the install against a
    // retirement, which clears the flag under the same hold.
    const std::unique_lock lock(map_mutex_);
    if (v == nullptr || !v->registered()) return std::unexpected(status_t::NOT_FOUND);
    // A NEW node, newest first: a copy of the vertex's own admission node (its filters carry
    // over unchanged) with the hook replaced. Nodes are immortal, so the old one stays readable
    // by any walk already on it, and is never found again.
    const admission_node_t* const own = admission_for(v);
    admission_node_t* const node = mem::make_in<admission_node_t>(
        *tables_, own != nullptr ? *own : admission_node_t{v, {}, {}, {}, {}, nullptr});
    if (node == nullptr) return std::unexpected(status_t::BACKPRESSURE);
    node->on_create = hook;
    admissions_.prepend(node);
    v->mark_creation_hook();
    return {};
}

creation_hook_t graph_t::creation_hook_for(const vertex_t* v) const noexcept {
    // The same walk @ref admission_for makes, behind its own flag, so a miss under a parent
    // with no hook costs one relaxed bit test. Closed out, there is no slot to read.
    if constexpr (config_t::kCreationHooks) {
        if (!v->has_creation_hook()) return {};
        for (const admission_node_t* n = admissions_.head.load(std::memory_order_acquire);
             n != nullptr; n = n->next)
            if (n->v == v) return n->on_create;
    }
    return {};
}

acl_right_t graph_t::declared_write_right(const vertex_t* v, wire::type_t type) const {
    // Walks only for a vertex whose flag says it declared: the list holds one node per
    // declaring registration (a creator endpoint per transport module — a handful), and a
    // node is immortal, so no lock is needed to read one.
    for (const payload_right_node_t* n = payload_rights_.head.load(std::memory_order_acquire);
         n != nullptr; n = n->next) {
        if (n->v != v) continue;
        for (const payload_right_t& row : n->rows)
            if (row.type == type) return row.right;
        // The vertex's own (newest) table had no row for this type — an undeclared type
        // takes `WRITE`, and an older table left by a previous occupant of this address must
        // not answer for it.
        return acl_right_t::WRITE;
    }
    return acl_right_t::WRITE;
}

std::span<const std::byte> graph_t::declared_catalog(const vertex_t* v) const noexcept {
    // The same walk `declared_write_right` makes, for the same reasons: only a flagged vertex
    // gets here, nodes are immortal, and the FIRST match is the vertex's own newest
    // declaration — an older node left by a previous occupant of this address never answers.
    for (const payload_right_node_t* n = payload_rights_.head.load(std::memory_order_acquire);
         n != nullptr; n = n->next)
        if (n->v == v) return mem::as_span(n->catalog);
    return {};
}

void graph_t::retire_subtree(vertex_t* v, gone_edges_t& gone) {
    // Pre-order, under the UNIQUE map lock. First the WHOLE subtree is flipped unregistered
    // (map-lock state — invisible to find from here on), before any vertex's state reverts.
    // The access check reads ACEs with no lock and then tests the registration of the vertex
    // it was asked about once more (`acl_allows`). A retire clears this vertex's ACEs, and a
    // descendant with none of its own is judged by them, so every vertex they gate must
    // already read as gone by the time any of them is cleared. Then, order within a vertex
    // matters:
    //  (1) read its active-edge count and unwind exactly that contribution from every
    //      descendant's listeners_above_ BEFORE revert zeroes own_subs_ — the mirror of
    //      note_subscriber_removed, done inline because we already hold the unique lock
    //      its shared lock only needed to exclude vertex creation;
    //  (2) [the caller clears the subtree's sweep-set keys as one prefix range];
    //  (3) revert the vertex's own state (fail-closed: clears own ACEs first, so the
    //      bearing-ancestor walk stops seeing this vertex before anything else changes);
    //  (4) recurse. Placeholders are walked too (§B.3): reverting one is a harmless no-op,
    //      but a registered descendant may hang below it.
    // Step (4) is now ITERATIVE rather than a recursive call per level (#690): the per-vertex
    // body is hoisted into one lambda applied to `v` and then, in the same pre-order, to every
    // descendant. Steps (1)-(3) keep their order within each vertex, which is what the note
    // above is about; only the descent changed.
    //
    // The lambdas mutate vertex STATE (registered flag, listeners, value seam) but never the
    // tree's SHAPE, so they honour for_each_descendant's no-structural-mutation contract -- the
    // walk re-reads the sibling list on each ascent and an insert or erase mid-walk would move
    // the position it resumes from.
    const auto retire_one = [this, &gone](vertex_t& x) {
        const std::uint32_t k = x.own_subs();
        if (k > 0) bump_subtree_listeners(&x, -static_cast<std::int32_t>(k));
        // Park the detached value seam (if any — the seam exists iff a handler was installed
        // at registration, whatever the role): a lock-free reader may still hold the old
        // pointer, so it is never freed here. The park's other end is the public collect(),
        // which the embedder calls at a moment it knows no reader holds a seam (#576); the
        // graph's own teardown is a growth backstop only — retired_seams_ destructs LAST, so
        // a seam that re-enters the graph must be collected explicitly. Under map_mutex_.
        // Parking links the seam into an intrusive chain, and the slot table moves into room
        // `retire` reserved, so neither step can fail mid-walk (#1778).
        mem::block_array_t<subscriber_t> table(*tables_);
        if (value_handlers_t* seam = x.revert_to_placeholder(table))
            (void)retired_seams_.seams.push_back({seam});            // reserved: cannot fail
        if (!table.empty()) (void)gone.push_back(std::move(table));  // reserved: cannot fail
    };
    v->mark_unregistered();
    v->for_each_descendant([](vertex_t& x) { x.mark_unregistered(); });
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

void graph_t::set_vertex_ceiling(std::size_t max_vertices) noexcept {
    vertex_ceiling_.store(max_vertices, std::memory_order_relaxed);
}

std::size_t graph_t::vertex_ceiling() const noexcept {
    return vertex_ceiling_.load(std::memory_order_relaxed);
}

std::uint64_t graph_t::vertex_ceiling_refusals() const noexcept {
    return vertex_ceiling_refusals_.load(std::memory_order_relaxed);
}

/**
 * @brief One anchor's NAME record — the whole of an anchor's key (#1223).
 *
 * `try_build_key` stops at the node whose parent is null, and an anchor's parent (`anchor_root()`)
 * is that node, so this single record IS the key retirement's sweep cleanup sees. The caller
 * composes @p id to contain characters `path::valid_segment` rejects, which is what makes the
 * rendered bytes unreachable from any address; framing it as an ordinary packed segment
 * record is what keeps `key_view_t`'s decomposition well-defined over it.
 *
 * @return false when the table source refused the record's block (#1778).
 */
static bool anchor_record(std::string_view id, mem::bytes_t& rec) noexcept {
    // Reserved first, so the emit's only failure is an id that is no legal record — which,
    // as before, simply renders nothing.
    if (!rec.reserve(id.size() + 1)) return false;
    (void)wire::emit_path_segment(rec, id);
    return true;
}

result_t<vertex_handle_t> graph_t::register_session_anchor(std::string_view id) {
    mem::bytes_t rec(*tables_);
    if (!anchor_record(id, rec)) return std::unexpected(status_t::BACKPRESSURE);
    const std::unique_lock lock(map_mutex_);
    vertex_t* node = anchor_root()->child_by_record(mem::as_span(rec));
    if (node == nullptr) {
        // FIRST sight of this id: one allocation, one slot, forever. Every later arrival on
        // the same id lands on the branch below and re-fills THIS object, which is the whole
        // bounded-across-churn property — a listener with `max_peers` slots can only ever ask
        // for `max_peers` distinct ids, so anchors are bounded by the accept policy and not
        // by how often clients reconnect (ADR-0044 §Amendment's measurement).
        // Failable steps first, as in the registration descent (#1778).
        vertex_t* const fresh = make_placeholder(mem::as_span(rec));
        node = fresh != nullptr ? anchor_root()->add_child(fresh, *tables_) : nullptr;
        if (node == nullptr) {
            mem::drop_in(*tables_, fresh);
            return std::unexpected(status_t::BACKPRESSURE);
        }
        vertex_slots_.push_back(node);
        note_owner_slot(*node);
    }
    // Live already ⇒ the caller is telling us about a session that never left. Refuse rather
    // than re-fill: a second fill() would NOT bump the generation (only retirement does), so
    // silently succeeding here would hand out a handle whose staleness signal never moved.
    if (node->registered()) return std::unexpected(status_t::PATH_IN_USE);
    // Revive in place — the same call `register_vertex_key` makes on a retired placeholder,
    // against the same object at the same slot. The generation was bumped by the RETIRE that
    // made this reachable, so the revived anchor already reads as a different tenancy to any
    // element minted against its predecessor.
    if (!node->fill(role_t::STORED_VALUE, handlers_t{}, *tables_))
        return std::unexpected(status_t::BACKPRESSURE);
    return vertex_handle_t{node};
}

std::optional<vertex_handle_t> graph_t::find_session_anchor(std::string_view id) const {
    mem::bytes_t rec(*tables_);
    if (!anchor_record(id, rec)) return std::nullopt;
    const std::shared_lock lock(map_mutex_);
    vertex_t* const node = anchor_root()->child_by_record(mem::as_span(rec));
    if (node == nullptr || !node->registered()) return std::nullopt;
    return vertex_handle_t{node};
}

std::optional<graph_t::session_anchor_route_t> graph_t::session_anchor_route(
    vertex_handle_t vh) const noexcept {
    const vertex_t* const v = vh.get();
    if (v == nullptr) return std::nullopt;
    // The anchor id is one PACKED segment record whose payload is `:<mount>/<peer>` (see
    // `session_anchor_id`). Both `:` and `/` are characters `path::valid_segment` forbids,
    // so the leading-`:` test alone separates every anchor from every addressable vertex —
    // no parent walk, no lock (the key record is immutable for the vertex's life).
    const std::span<const std::byte> rec = v->name().bytes();
    if (rec.size() <= 1) return std::nullopt;  // length byte + at least ":m/p"
    const std::string_view id{reinterpret_cast<const char*>(rec.data()) + 1, rec.size() - 1};
    if (id.size() < 4 || id.front() != ':') return std::nullopt;
    const std::size_t slash = id.rfind('/');
    // rfind, not find: a mount NAME may itself be qualified (`net/ws`), while a session's
    // routable name is one segment and can never contain `/` (`path::valid_segment` gates
    // every peer name a transport stamps).
    if (slash == std::string_view::npos || slash <= 1 || slash + 1 >= id.size())
        return std::nullopt;
    return session_anchor_route_t{.mount = id.substr(1, slash - 1), .peer = id.substr(slash + 1)};
}

std::size_t graph_t::session_anchor_slots() const noexcept {
    const std::shared_lock lock(map_mutex_);
    std::size_t n = 0;
    anchor_root()->for_each_child([&n](const vertex_t&) { ++n; });
    return n;
}

vertex_t* graph_t::make_placeholder(std::span<const std::byte> record) noexcept {
    // The name first, from the same injected source (#1991): a record past the key's inline
    // bytes was the one global-heap block a registration took. A placeholder has no handlers,
    // so `adopt_identity` allocates no extension block.
    result_t<path_key_t> name = path_key_t::try_make(record, *tables_);
    return name && vertex_slots_.reserve_next()
               ? mem::make_in<vertex_t>(*tables_, role_t::STORED_VALUE, std::move(*name),
                                        handlers_t{}, *tables_)
               : nullptr;
}

void graph_t::note_owner_slot(vertex_t& v) noexcept {
    // The caller has just appended v, under the unique map lock, so the entry is the last
    // one and the index is final. A u32 is the element field's own width (RFC-0024 §6.4);
    // a graph that somehow held more slots than that could not mint for them anyway, and
    // the memo is re-validated on read, so a truncated stamp degrades to the scan.
    const std::size_t index = vertex_slots_.size() - 1;
    if (index >= path_key_t::kNoOwnerSlot) return;
    v.name_.owner_slot_ = static_cast<std::uint32_t>(index);
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
    // The memo (#1486). Written ONCE at slot assignment under a unique hold on this very
    // lock, never invalidated (slots are immortal — the index is insert-only and
    // pointer-stable, ADR-0057, and a retire re-virginizes in place), so a plain read under
    // the shared hold is race-free and needs no ordering of its own.
    //
    // It is re-validated rather than trusted, and the two compares are not defensive
    // decoration: they are what keeps a `vertex_t` that no graph ever slotted — the tests
    // build them directly, and `anchor_root()` is one — resolving exactly as it did before,
    // by falling through to the scan below. Cost of the fast path is a bounds test and one
    // pointer compare against a vertex-index element, against a scan measured at 450 ns per
    // 10^3 resident vertices and 410 us at 10^6 (#1485/#1496) — held, all of it, under the
    // shared map lock that every reader queues behind. Route formation over M bindings was
    // O(M x N) for it; at M = N = 10^6 that is ~200 s of pure scan.
    const std::uint32_t memo = v->name_.owner_slot_;
    if (memo != path_key_t::kNoOwnerSlot && memo < vertex_slots_.size() && vertex_slots_[memo] == v)
        return vertex_slot_t{.index = memo, .generation = gen};
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

    // The subtree's keys are exactly the sweep-set entries that start with its root's key (a
    // parent's key is a byte-prefix of every descendant's), so one rendered key is the whole
    // cleanup list — rendered BEFORE anything changes, so a refusal retires nothing (#1778).
    mem::bytes_t lo(*tables_);
    if (!try_build_key(root, lo)) return std::unexpected(status_t::BACKPRESSURE);
    gone_edges_t gone(*tables_);  // the slot tables the retirement dropped (#1816, #1778)
    {
        const std::unique_lock lock(map_mutex_);
        // Idempotent (§B.4): an already-retired / never-filled placeholder is a no-op.
        if (!root->registered()) return {};
        // One entry per vertex the walk can visit, reserved BEFORE anything changes, so a
        // refusal retires nothing and the walk below cannot fail halfway (#1778).
        std::size_t n = 1;
        root->for_each_descendant([&n](vertex_t&) { ++n; });
        if (!gone.reserve(n) || !retired_seams_.seams.reserve(retired_seams_.seams.size() + n))
            return std::unexpected(status_t::BACKPRESSURE);
        const std::size_t parked_before = retired_seams_.seams.size();
        retire_subtree(root, gone);
        // Close the epoch AFTER every seam above is unpublished: a router frame that loaded
        // one of them is online at this epoch or older, and collect() waits it out.
        const std::uint32_t epoch = detail_qsbr::advance();
        for (std::size_t i = parked_before; i < retired_seams_.seams.size(); ++i)
            retired_seams_.seams[i].epoch = epoch;
    }
    // Each dropped routed edge gives its link hold back, outside every graph lock: an edge
    // is reported exactly twice over its life, and retirement is one of its ends (#1816).
    // The tables themselves are destroyed when `gone` leaves scope — outside the locks too.
    for (const mem::block_array_t<subscriber_t>& table : gone)
        for (const subscriber_t& e : table)
            if (e.active && e.remote != nullptr && !e.remote->link.empty())
                hold_link(delivery_link(e.remote), false);
    // Drop the retired vertices from the sweep sets — AFTER releasing the map lock, so the
    // sweep lock is not held across the retire walk. A stale entry would otherwise (a) leak, and
    // worse (b) silently re-enroll a revived vertex into UNCONDITIONAL sweeping through the leaked
    // key, overriding the IF_NEWER reset revert_to_placeholder just applied. A concurrent sweep
    // tolerates a not-yet-erased key: find_ptr skips the unregistered vertex, so delivery never
    // lands on a retired one either way.
    //
    // Between the unlock and this lock another thread may register under the subtree and
    // enroll or mark the newcomer (#1884), so the run is filtered, not erased whole: each
    // entry names its vertex, and the entry goes only when that vertex no longer belongs in
    // the set. The retire walk above reset every retired vertex to IF_NEWER and dropped its
    // pending-mark hint; only a newcomer's own mode store or mark puts either back, and both
    // happen under the sweep lock held here. So an UNCONDITIONAL entry whose vertex is not
    // UNCONDITIONAL is the retiree's, and a pending entry whose hint is down is too. The
    // window is closed.
    {
        const std::lock_guard slock(sweep_mutex_);
        const auto [ui, uj] = subtree_run(unconditional_, mem::as_span(lo));
        (void)unconditional_.erase_if(ui, uj, [](const key_set_t::entry_t& e) {
            return e.value->delivery_mode() != delivery_mode_t::UNCONDITIONAL;
        });
        const auto [pi, pj] = subtree_run(pending_, mem::as_span(lo));
        pending_count_.fetch_sub(
            pending_.erase_if(
                pi, pj, [](const key_set_t::entry_t& e) { return !e.value->has_pending_mark(); }),
            std::memory_order_relaxed);
    }
    return {};
}

/**
 * @brief Free every parked value seam and run every parked release that no router frame can
 *        still reach — the embedder-called other end of retirement's park (#576).
 *
 * Epochs grow in park order, so the ripe entries are a prefix of each park. They are taken in
 * stack-sized batches under the map lock, which draws nothing, and freed outside it.
 */
void graph_t::collect() {
    constexpr std::size_t kBatch = 16;
    for (;;) {
        std::array<value_handlers_t*, kBatch> seams{};
        std::array<retired_callback_t, kBatch> releases{};
        std::size_t ns = 0;
        std::size_t nr = 0;
        {
            // Under the map lock: nothing but the take. The lock is what serialises us against
            // retire_subtree's append; no user code runs under it.
            const std::unique_lock lock(map_mutex_);
            auto& s = retired_seams_.seams;
            for (; ns < kBatch && ns < s.size() && detail_qsbr::all_quiescent_past(s[ns].epoch);
                 ++ns)
                seams[ns] = s[ns].seam;
            s.erase_front(ns);
            auto& r = parked_releases_.releases;
            for (; nr < kBatch && nr < r.size() && detail_qsbr::all_quiescent_past(r[nr].epoch);
                 ++nr)
                releases[nr] = r[nr].release;
            r.erase_front(nr);
        }
        // Freed HERE — outside every graph lock, on the caller's thread. So a release may
        // re-enter the graph, and a slow one blocks no reader or writer.
        for (std::size_t i = 0; i < ns; ++i) mem::drop_in(retired_seams_.seams.source(), seams[i]);
        for (std::size_t i = 0; i < nr; ++i) releases[i].release(releases[i].ctx);
        if (ns < kBatch && nr < kBatch) return;
    }
}

result_t<void> graph_t::park_release(retired_callback_t release) {
    const std::unique_lock lock(map_mutex_);
    // The caller has unpublished what @p release frees; this closes the epoch after it.
    if (!parked_releases_.releases.push_back({release, detail_qsbr::advance()}))
        return std::unexpected(status_t::BACKPRESSURE);
    return {};
}

std::size_t graph_t::parked_seam_count() const {
    const std::shared_lock lock(map_mutex_);
    return retired_seams_.seams.size();
}

// ---- The per-link departure index's doors (#1071). The index itself — its slots, the
// carry, the name scan — is `link_index_t` (link_index.cpp, #1710); `graph_t` keeps only
// the evictions below, because they walk the vertex tree under the map lock.

std::size_t graph_t::link_index_name_lookups() const { return link_index_.name_lookups(); }

link_id_t graph_t::intern_link(std::string_view link_name) { return link_index_.intern(link_name); }

link_id_t graph_t::intern_link_hinted(std::string_view link_name, std::uint32_t& hint) {
    return link_index_.intern_hinted(link_name, hint);
}

void graph_t::release_link(link_id_t token) { link_index_.release(token); }

std::size_t graph_t::link_edge_candidates(std::string_view link_name) const {
    return link_index_.candidate_count(link_name);
}

std::size_t graph_t::evict_link_edges(std::string_view link_name) {
    // Two-phase, per the graph.hpp lock-order docs — but the first phase is now an INDEX
    // LOOKUP rather than a walk of the whole vertex tree (#1071). What it replaced collected
    // every vertex in the graph holding any subscriber edge, into a global-heap vector sized
    // to that set, on a path whose caller is a peer hanging up: one browser tab's departure
    // was priced by every other peer's subscriptions, on the ESP-IDF link's shared HTTP
    // server task. The candidates here are exactly the vertices THIS link ever subscribed
    // on, and the list is the index's own entry moved out, so the path allocates nothing.
    //
    // The second phase is unchanged and still carries the whole locking argument: evict per
    // vertex, each under its own stripe lock inside a FRESH shared map hold. The per-vertex
    // hold makes the {clear edges, unwind counters} pair atomic against a concurrent
    // retire() (unique map lock), which reads own_subs() before zeroing it — interleaving
    // there would double-subtract descendants' listeners_above_. Between vertices everything
    // may interleave: a vertex retired meanwhile has an empty edge list (k == 0, no-op), and
    // a subscribe admitted meanwhile for a DEAD link is the pre-existing races' window,
    // resolved by the transport calling this hook after the link stopped delivering — the
    // index does not change that window, because it is populated at the same own_subs bump
    // the old walk's predicate read (see link_index_t).
    //
    // The empty key still matches nothing (#1056), one step earlier than before: it is now
    // refused at the index instead of per vertex.
    if (link_name.empty()) return 0;
    const mem::block_array_t<vertex_t*> candidates =
        link_index_.candidates(link_name, /*take=*/true);
    std::size_t total = 0;
    std::size_t routed = 0;  // the edges that held `link_name` (#1816), given back below
    for (vertex_t* v : candidates) {
        const std::shared_lock lock(map_mutex_);
        std::size_t quiet = 0;  // suspended edges were never counted (#1533)
        const std::size_t k = v->evict_link_edges(link_name, routed, quiet);
        if (k == 0) continue;  // a stale index entry: the vertex's edges went individually
        // The k-fold mirror of note_subscriber_removed, under the same shared hold as
        // the clear (RFC-0005 bookkeeping: descendants' writes stop bubbling here).
        v->bump_own_subs(-static_cast<std::int32_t>(k - quiet));
        bump_subtree_listeners(v, -static_cast<std::int32_t>(k - quiet));
        total += k;
    }
    // Outside every graph lock: the receiver takes its own control-plane lock (#1816).
    hold_link(link_name, false, routed);
    return total;
}

std::size_t graph_t::evict_route_edges(std::string_view link_name,
                                       std::span<const std::byte> route_wire) {
    // Byte-for-byte the evict_link_edges discipline (see its comment): snapshot under one
    // shared hold, evict per vertex under a fresh shared hold + the vertex's stripe lock,
    // unwind the RFC-0005 bookkeeping by exactly the count each vertex reports. Only the
    // per-vertex predicate differs — link AND stored-route equality instead of link alone.
    if (link_name.empty() || route_wire.empty()) return 0;
    // Same index, same saving as its whole-link sibling — but the entry is COPIED, not
    // taken: this reclaims only the edges matching one route, so the link keeps whatever
    // else it holds and must stay indexed for its eventual teardown.
    // Classify the echo's form ONCE, here, where wire types are spoken: a `PATH` whose body
    // opens with an escape record is element-spelled — a refused reverse-list delivery
    // (RFC-0024 §7.1 amendment 1, spelled per RFC-0029 §4.2) — and matches the stored
    // reverse chain's emitted suffix; a canonical route opens with a NAME and runs the
    // byte-equal match. The 4-byte header is the grammar's (`PATH` is never `opt.LL`).
    const bool bound_echo =
        route_wire.size() > 4 &&
        static_cast<wire::type_t>(std::to_integer<std::uint8_t>(route_wire[0])) ==
            wire::type_t::PATH &&
        std::to_integer<std::uint8_t>(route_wire[4]) == wire::kPackedEscapeLen;
    const mem::block_array_t<vertex_t*> candidates =
        link_index_.candidates(link_name, /*take=*/false);
    std::size_t total = 0;
    for (vertex_t* v : candidates) {
        const std::shared_lock lock(map_mutex_);
        std::size_t quiet = 0;
        const std::size_t k = v->evict_route_edges(link_name, route_wire, bound_echo, quiet);
        if (k == 0) continue;
        v->bump_own_subs(-static_cast<std::int32_t>(k - quiet));
        bump_subtree_listeners(v, -static_cast<std::int32_t>(k - quiet));
        total += k;
    }
    // Every match was keyed on its delivery link, so every one held `link_name` (#1816).
    hold_link(link_name, false, total);
    return total;
}

result_t<vertex_handle_t> graph_t::find_or_create(std::span<const std::byte> key,
                                                  std::string_view caller,
                                                  function_ref_t<const view::rope_t&()> payload) {
    result_t<vertex_t*> p = find_or_create_ptr(key, caller, payload);
    if (!p) return std::unexpected(p.error());
    return vertex_handle_t{*p};
}

result_t<void> graph_t::hide_from_enumeration(vertex_handle_t vh) {
    vertex_t* v = vh.get();
    // The UNIQUE map lock, the same one `fill` / `mark_unregistered` take: the bit is read by
    // every `:children[]` walk under the SHARED lock, so writing it under the unique lock is
    // what keeps a listing from being gathered half-hidden. It is set once, at mint, on a
    // control-plane path — nothing here is hot.
    const std::unique_lock lock(map_mutex_);
    if (v == nullptr || !v->registered()) return std::unexpected(status_t::NOT_FOUND);
    v->mark_enumeration_hidden();
    return {};
}

result_t<vertex_t*> graph_t::find_or_create_ptr(std::span<const std::byte> key,
                                                std::string_view caller,
                                                function_ref_t<const view::rope_t&()> payload) {
    if (vertex_t* v = find_ptr(key)) return v;
    // RFC-0030 §7.1: a miss creates nothing, whatever the write's origin. The `mkdir -p` walk
    // that stood here (RFC-0005 §D) is gone, and with the hook policy closed (the default)
    // this is the whole of the miss arm.
    if constexpr (!config_t::kCreationHooks) return std::unexpected(status_t::NOT_FOUND);
    // §7.2: walk shallowest-first; a missing level is decided by its parent's hook, and a level
    // the hook created is the parent of the next. Validate the whole key FIRST: raggedness is
    // found at the LAST record, so a walk that created as it went would let a hook materialize
    // the valid prefix of an illegally-spelled key before refusing it (#436). The walk stores
    // nothing between levels (#1139/#873).
    const key_view_t kv{key};
    if (!kv.for_each_level([](key_view_t) noexcept { return true; }))
        return std::unexpected(status_t::INVALID_PATH);
    vertex_t* parent = root();
    std::optional<status_t> failed;
    (void)kv.for_each_level([&](key_view_t level) {
        const std::span<const std::byte> pk = level.bytes();
        if (vertex_t* const child = find_ptr(pk)) {
            parent = child;
            return true;
        }
        // The CREATE gate runs before the payload is asked for, so a writer the parent's ACL
        // denies provokes no draw (a span-delivered remote payload is copied to be shown).
        const creation_hook_t hook = creation_hook_for(parent);
        const view::rope_t* value = nullptr;
        if (!hook)  // no hook here: the miss is the answer
            failed = status_t::NOT_FOUND;
        else if (!acl_allows(parent, caller, acl_right_t::CREATE))
            failed = status_t::PERMISSION_DENIED;
        else if ((value = &payload())->total_length() == 0)  // the payload could not be held
            failed = status_t::BACKPRESSURE;
        else if (const result_t<void> r = hook(vertex_handle_t{parent}, pk, caller, *value); !r)
            failed = r.error();
        else if ((parent = find_ptr(pk)) == nullptr)  // the hook said yes and made nothing
            failed = status_t::NOT_FOUND;
        return !failed;
    });
    if (failed) return std::unexpected(*failed);
    return parent;  // the deepest level: found, or created by its parent's hook
}

std::uint64_t graph_t::target_canonical_resolves() const noexcept {
    return instrument_.resolve_count();
}

std::uint64_t graph_t::ancestor_walks() const noexcept { return instrument_.walk_count(); }

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
            drops_no_target_.fetch_add(static_cast<std::size_t>(n), std::memory_order_relaxed);
            return;
        case drop_reason_t::DENIED:
            drops_denied_.fetch_add(static_cast<std::size_t>(n), std::memory_order_relaxed);
            return;
        case drop_reason_t::OUT_OF_MEMORY:
            drops_oom_.fetch_add(static_cast<std::size_t>(n), std::memory_order_relaxed);
            return;
        case drop_reason_t::FAN_OUT_TRUNCATED:
            drops_truncated_.fetch_add(static_cast<std::size_t>(n), std::memory_order_relaxed);
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
    // ONE cause since #1448: the per-edge copy cannot fail any more (the cold half is a
    // refcount share, not an owning string copy), so the snapshot's only shed is the
    // wide-fan-out capacity degrade. OUT_OF_MEMORY as a DELIVERY cause is unaffected —
    // the target leg's store (a declined slot, ring or clone) still reports it.
    if (drops.truncated != 0) count_drop(drop_reason_t::FAN_OUT_TRUNCATED, drops.truncated);
}

/**
 * @brief Record a store's sheds as delivery drops — out of line ON PURPOSE.
 *
 * `noinline` keeps `dispatch_edge_target` at its pinned shape: RFC-0028 slice 10 moved
 * graph.cpp's inline budget and GCC began inlining this whole body (the own-subs-wide
 * multiply) into the per-target leg, +43 B on a symbol the ratchet holds flat. As a call it
 * is the one-test early return on a clean write, exactly as before.
 */
[[gnu::noinline]] void graph_t::count_store_drops(vertex_t* v,
                                                  const vertex_t::store_drops_t& drops) noexcept {
    if (!drops.any()) return;  // the clean write pays exactly this test
    // Two sheds now, both at the RECEIVER's ring and both under RFC-0025 §4.4's best-effort
    // arm. `ring_shed` is drop-oldest: the admission was funded by evicting queued entries,
    // each one a delivery the consumer will never see and each one a
    // `tr::flow::address_shift_gap` point (surfaced in order by the drain). `ring_append` is
    // the floor case — the source could not fund the entry even with the ring emptied.
    // Counting BOTH is what keeps the §4.4 promise that every loss is accounted; a shed with
    // no accounting is non-conforming, and silence is the one forbidden behaviour.
    //
    // A shed STREAM ring append under memory pressure. For a STREAM the drain IS the fan-out,
    // so the entry that never entered the ring is a delivery every subscriber loses — counted
    // at the width it sheds, one per subscriber, never one per event. Same width, same cause
    // and same reasoning as the handler notify-clone leg that sheds a whole fan-out.
    //
    // own-subs-wide by DECISION, not by oversight: the ancestor legs a bubble would also have
    // served stay uncounted because #854's close ruling dropped ancestor-leg drop
    // instrumentation outright. A vertex with no subscribers of its own counts nothing, which
    // is why this is guarded rather than a bare add of zero.
    const std::uint64_t lost = (drops.ring_append ? 1U : 0U) + drops.ring_shed;
    if (const std::uint64_t n = v->own_subs(); n != 0)
        count_drop(drop_reason_t::OUT_OF_MEMORY, n * lost);
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

bool graph_t::clear_subscriber_slot(vertex_t* v, std::size_t slot, std::string_view caller,
                                    void** retired_ctx) {
    // ONE out-of-line clear for both doors, so `vertex_t::clear_edge` is compiled once rather
    // than inlined here and again out of line in graph_fields.cpp (#1711, rv32 flash).
    //
    // The observer's view of the departing edge must be taken BEFORE the clear — clear_edge
    // RECLAIMS the slot's stored SUBSCRIBER, so afterwards there is nothing left to name the
    // target with. Skipped entirely on a local clear (`unsubscribe` passes the empty caller)
    // or with no observer installed (observing_subscriptions).
    const view::view_t cleared_tlv = observing_subscriptions(caller)
                                         ? v->edge_source(slot).value_or(view::view_t{})
                                         : view::view_t{};
    remote_ptr_t retired_remote;
    bool was_suspended = false;
    if (!v->clear_edge(slot, retired_ctx, &retired_remote, &was_suspended)) return false;
    if (!was_suspended) note_subscriber_removed(v);   // RFC-0005 counts delivering edges only
    hold_link(delivery_link(retired_remote), false);  // the link hold this clear gives back (#1816)
    // Only a slot that WAS active is an unsubscribe; clearing an already-empty one changed
    // nothing and must not be reported as a removal. A no-op for the empty caller.
    notify_subscription(sub_event_t::kind_t::REMOVED, v, caller, cleared_tlv, slot);
    return true;
}

vertex_t* graph_t::find_ptr(std::span<const std::byte> key) const {
    const std::shared_lock lock(map_mutex_);
    // O(segments) Composite child walk from the root (ADR-0057); a placeholder terminus
    // (an unregistered intermediate) is "no such vertex", as under the flat map.
    vertex_t* node = root();
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

bool graph_t::try_build_key(const vertex_t* v, mem::bytes_t& out) noexcept {
    // Render-on-demand full key (ADR-0057): ancestors' packed records concatenated
    // root-down. Parent links and name bytes are immutable — no lock. Two passes: size,
    // then one exact block filled deepest-record-last. A refusal leaves `out` empty, so a
    // writer-thread mark/drain leg drops or defers (#477) and a wiring caller answers
    // BACKPRESSURE (#1778) — never an abort.
    std::size_t total = 0;
    for (const vertex_t* n = v; n->parent() != nullptr; n = n->parent()) total += n->name().size();
    out.clear();
    if (!out.resize_for_overwrite(total)) return false;
    std::size_t w = total;
    for (const vertex_t* n = v; n->parent() != nullptr; n = n->parent()) {
        const std::span<const std::byte> rec = n->name().bytes();
        w -= rec.size();
        std::memcpy(out.data() + w, rec.data(), rec.size());
    }
    return true;
}

std::optional<vertex_handle_t> graph_t::find(std::span<const std::byte> key) const {
    vertex_t* p = find_ptr(key);
    if (p == nullptr) return std::nullopt;
    return vertex_handle_t{p};
}

std::size_t graph_t::share_threshold_bytes(vertex_handle_t v) const noexcept {
    return v.get()->share_threshold_bytes();
}

retention_t graph_t::retention(vertex_handle_t v) const noexcept { return v.get()->retention(); }

result_t<void> graph_t::set_policy(vertex_handle_t v, vertex_policy_t policy) {
    vertex_t* const vx = v.get();
    if (!policy_legal(vx->role(), policy)) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    // The key is rendered only for a mode that moves; it is scratch, so a refusal here leaves
    // nothing either.
    mem::bytes_t key(*tables_);
    const bool mode_moves = vx->delivery_mode() != policy.delivery_mode;
    if ((mode_moves && !try_build_key(vx, key)) ||
        !apply_policy(vx, vx->role(), std::move(policy),
                      mode_moves ? mem::as_span(key) : std::span<const std::byte>{}))
        return std::unexpected(status_t::BACKPRESSURE);
    return {};
}

/**
 * @brief Apply a (legal) policy all-or-nothing (#1883), skipping every member that already
 *        holds — so a default policy on a fresh vertex touches nothing and allocates nothing.
 *
 * Every block a member can be refused is drawn first (`vertex_t::stage_policy`, and the owned
 * field table built aside), then the delivery mode lands with its sweep-set entry when
 * @p mode_key names the vertex (an empty key: the mode does not move). Only then are the
 * members applied, and none of them can be refused any more: a refusal before that point
 * leaves blocks with no observable effect.
 *
 * Order matters in one place: the ring source is bound BEFORE the retention, because binding
 * drains the ring and a depth set first would be applied to a ring about to be emptied anyway;
 * either order is correct, this one does the drain once.
 */
bool graph_t::apply_policy(vertex_t* vx, role_t role, vertex_policy_t&& policy,
                           std::span<const std::byte> mode_key) {
    const bool ring_moves = std::pair(vx->ring_source(), vx->ring_reliable()) !=
                            std::pair(policy.ring_source, policy.ring_reliable);
    // A HANDLER is NONE by role and carries no bit for it. Only `N` touches the extension
    // block, so `N` is what draws a STREAM's (#2001), and a `NONE` vertex keeps neither slot
    // nor ring — nor, with no other member declared, any extension block at all.
    const retention_t retention = policy.retention.value_or(default_retention(role));
    // Compared as stored, so a threshold that saturates the same way moves nothing.
    const bool threshold_moves = saturate_threshold(vx->share_threshold_bytes()) !=
                                 saturate_threshold(policy.share_threshold_bytes);
    // Owner-facing declaration (RFC-0010 §A.2) — a local host API, so no ACL gate. An owning
    // table is always (re)declared, so its values reset; a borrowed one already installed is
    // the same declaration and keeps its stored values; none declared over a table installed
    // uninstalls it. An empty span is the null one on both sides, so two empties compare equal.
    const app_fields_decl_t& fields = policy.app_fields;
    const std::span<const app_field_slot_t> have = vx->app_field_slots();
    const std::span<const app_field_slot_t> want =
        fields.is_borrowed() ? fields.borrowed().slots() : std::span<const app_field_slot_t>{};
    const bool fields_move = !fields.owned().empty() || std::pair(have.data(), have.size()) !=
                                                            std::pair(want.data(), want.size());
    // Stage: a member that moves draws its blocks now. A ring or a field group already there
    // is kept, so staging one for a member that moves back to its default draws nothing.
    vertex_ext_t* e = nullptr;
    if (ring_moves || threshold_moves || fields_move || retention == retention_t::N) {
        e = vx->stage_policy(ring_moves, fields_move, *tables_);
        if (e == nullptr) return false;
    }
    app_field_table_t built(*tables_);
    built.slots = want;  // a borrowed table is viewed in place and allocates nothing
    if (!vertex_t::build_owning_table(fields.owned(), built) ||
        (!mode_key.empty() && !apply_delivery_mode(vx, policy.delivery_mode, mode_key)))
        return false;
    // Nothing below can be refused: every block is drawn and the mode has landed.
    if (ring_moves) (void)vx->set_ring_source(policy.ring_source, policy.ring_reliable, *tables_);
    if (role != role_t::HANDLER) (void)vx->set_retention(retention, policy.depth, *tables_);
    if (threshold_moves)
        (void)vx->set_share_threshold_bytes(policy.share_threshold_bytes, *tables_);
    if (fields_move) vx->install_app_fields(*e, std::move(built));
    return true;
}

/** @brief Bytes the receiver ring currently holds reserved — the byte bound's observable. */
result_t<std::size_t> graph_t::ring_reserved_bytes(vertex_handle_t v) const {
    vertex_t* const vx = v.get();
    if (vx->role() != role_t::STREAM) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    return vx->ring_reserved_bytes();
}

/** @brief The cumulative gap census: how many shed points this receiver's ring has taken. */
result_t<std::uint64_t> graph_t::stream_gaps(vertex_handle_t v) const {
    vertex_t* const vx = v.get();
    if (vx->role() != role_t::STREAM) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    return vx->ring_gap_count();
}

namespace {
/** @brief The ACL gate's stack frame for a resolved subject token (#1781): past this many
 *         bytes the token spills to the graph's table source. */
constexpr std::size_t kSubjectFrameBytes = 64;
/** @brief How many times the ACL gate re-reads a subject hook slot whose publish is in flight
 *         before it refuses the caller (@ref tr::sink_slot_t::get_settled, which yields every
 *         64 reads). A publish is a handful of stores, so this is only ever approached by a
 *         publisher preempted inside one. */
constexpr std::uint32_t kHookSettleReads = 1U << 16;
}  // namespace

bool graph_t::acl_allows(vertex_t* v, std::string_view caller, acl_right_t right) const {
    // ONE coherent read of the {fn, ctx} pair for the whole gate (#1049) — never a
    // re-read at the call below, which is what let a concurrent install free the
    // std::function predecessor's captures under a gate that was already inside it.
    // Unset, this is the same single relaxed load the old `if (!subject_resolver_)` was.
    if (!subject_lookup_.installed()) return true;  // enforcement disabled — the hot-path check
    // Installed, the gate reads a SETTLED slot: a reader that overlaps a `set_hooks` republish
    // waits the publish out rather than reading the slot as unset. A slot that does not settle
    // within the bound refuses the caller.
    const auto settled = subject_lookup_.get_settled(kHookSettleReads);
    if (!settled) return false;
    const auto lookup = *settled;
    if (lookup.fn == nullptr) return true;  // cleared by the republish it waited out
    // The trusted channel is the EMPTY caller context — a local API call — settled HERE,
    // before the resolver runs (#905). It used to be a resolver return value (`nullopt`),
    // whose natural reading ("I cannot name this caller") meant "grant everything", WRITE_ACL
    // and CREATE included. A remote op carries the inbound link's NAME, so it cannot spell
    // this arm: the full-route form through `ensure_remote(src)->caller`, and — since #974 — the
    // COMPACT delivery fast path, whose two terminus write arms in `fwd_router_t::on_compact`
    // pass `inbound_name` too. #974 was that second one missing: unattributed, it landed here
    // and was waved through every ACE the first is checked against. Any further net-plane
    // write path must carry a caller for the same reason — which is why
    // `fwd_router_t::deliver_local` takes its own as a REQUIRED, undefaulted parameter.
    if (caller.empty()) return true;
    // The token lands in a stack frame first (#1781): a subject is a short name or id, so a
    // gated operation allocates nothing for it, and a longer one spills to the table source.
    std::array<std::byte, kSubjectFrameBytes> frame_bytes;
    mem::bump_source_t frame(frame_bytes, *tables_);
    mem::bytes_t token(frame);
    if (!lookup.fn(lookup.ctx, caller, token)) return false;  // DENIED — PERMISSION_DENIED
    const std::span<const std::byte> subject = mem::as_span(token);
    // The wildcard spelling is RESERVED in the subject-token space (#908): the wire has one
    // spelling for a subject, so a principal that could BE `EVERYONE@` is indistinguishable
    // from the wildcard ACE. Enforced HERE, which is the only site that invokes
    // `subject_resolver_` at all, rather than left to every integrator to blacklist — and
    // BEFORE the bearing-ancestor walk,
    // so a misconfigured resolver is refused at an unguarded vertex too. Fail closed, exactly
    // like the resolver's own error arm above.
    if (is_reserved_subject(subject)) return false;
    const auto bit = static_cast<std::uint32_t>(right);
    // #361 §3: ACL state lives only on BEARING vertices (those with own ACEs). A bare
    // vertex walks the immutable parent chain LOCK-FREE (has_own_aces is an atomic;
    // parent links never change) to its nearest bearing ancestor and evaluates that
    // vertex's cached merge through the kAceInherit projection — which IS the bare
    // vertex's effective list (the filter is idempotent and order-preserving over
    // "own + inherited-ancestors"). No cache, no ext block, is ever allocated on the
    // bare descendant; RAM stops scaling as ancestors x descendants.
    // `v` is never null: every caller hands in a vertex it found or holds, and the walk stops
    // at the root, so it never steps past one either.
    vertex_t* bearer = v;
    while (bearer->parent() != nullptr && !bearer->has_own_aces()) bearer = bearer->parent();
    bool allowed = true;  // no ACL anywhere up the chain (root excluded): open by default
    if (bearer->parent() != nullptr) {
        // The ACE-expiry reference clock is read only HERE, once an ACL will actually be
        // evaluated (#1665): an attributed remote op on an unguarded subtree never pays a
        // `system_clock::now()` — that open-by-default arm skips this block.
        const std::uint64_t now = now_ns();
        const bool self = bearer == v;

        // The ADR-0050 cached effective-ACE merge, now held by the BEARER: the data-plane
        // check evaluates ONE pre-merged list (own ACEs + INHERIT-flagged ancestor ACEs,
        // evaluation order) — no per-operation ancestor rebuild. The walk runs only inside
        // the rebuild lambda, on the first check after a :acl write marked the bearer dirty
        // (subtree-precise via the ADR-0057 child links — see the `:acl` write in
        // graph_fields.cpp). The rebuild runs UNLOCKED (#361 §2 striped locks), taking each
        // ancestor's stripe one at a time. Root excluded (the flat-map walk never evaluated
        // the empty key); placeholder intermediates hold empty ACE lists, so merging them is
        // the no-op the old walk's skip was.
        allowed = bearer->with_effective_aces(
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
                // A bare descendant evaluates the INHERITABLE SUBSEQUENCE of the bearer's
                // merge. Filtered in place (order-identical) rather than against a second,
                // projected vector — see effective_acl_t::allows.
                return effective_acl_t::allows(merged, subject, bit, now,
                                               self ? std::uint8_t{0} : kAceInherit);
            });
    }
    // Recheck the vertex's registration after the gate, on the vertex in hand (vertices are
    // insert-only, so the pointer is still the caller's). Every operation found `v` registered
    // before it got here, but holds no lock since, and a retire reverts a vertex's ACEs with
    // the rest of its state, so the gate may have read the state of a vertex that is gone.
    // `retire_subtree` flips the whole subtree unregistered BEFORE it clears any ACEs (a
    // release store, then the release clear of the OWN_ACES bit and the stripe-locked clear of
    // the list), so a gate that read a cleared ACE sees the flag down here, through this fence,
    // and refuses. The root is the one vertex asked unregistered on purpose: it is the parent
    // of a top-level creation, and it is never registered and never retired.
    std::atomic_thread_fence(std::memory_order_acquire);
    return allowed && (v->registered() || v->parent() == nullptr);
}

void graph_t::mark_subtree_acl_dirty(vertex_t* v) {
    // Iterative (#690). Reached from the `:acl` write, so its depth is peer-chosen.
    v->mark_acl_cache_dirty();
    v->for_each_descendant([](vertex_t& c) { c.mark_acl_cache_dirty(); });
}

/**
 * @brief The HANDLER-role arm of the read contract: answer the value the `on_read` seam
 *        hands back, or `NOT_FOUND` when the vertex exposes none.
 *
 * ONE spelling, called from BOTH read doors — @ref graph_t::read and, since RFC-0008
 * Amendment 2, @ref graph_t::await. `await` is the readiness form of a data READ, so the
 * value it hands back after a wake must be the value `read` would have handed back at that
 * instant; it used to call `read_stored()` directly and so answered `NOT_FOUND` at a HANDLER
 * vertex *after the awaited write arrived*. Sharing the arm is what keeps the two doors from
 * drifting again.
 *
 * @warning The READ gate is the CALLER's: both doors check `acl_right_t::READ` up front (await
 *          must, so a denied caller cannot camp on the condvar), and this arm does NOT
 *          re-check. Do not call it from an ungated site.
 *
 * Out of line, and reached only through a taken branch: the retaining arm of either door keeps
 * its `read_stored()` fast path, which pays no handler-dispatch cost at all.
 */
result_t<value_ref_t> graph_t::composed_or_backpressure(view::rope_t&& r) const noexcept {
    value_ref_t out = value_ref_t::composed(std::move(r), *values_);
    if (!out) return std::unexpected(status_t::BACKPRESSURE);
    return out;
}

[[gnu::noinline]] result_t<value_ref_t> graph_t::read_handler_gated(vertex_t* v) const {
    // Load the seam ONCE: it is an atomic pointer a concurrent retire may swap to
    // null (RFC-0009 §B.6), so a check-then-call across two loads would race — the
    // second load could see the cleared seam and throw bad_function_call. The parked
    // block keeps this reference valid even if the swap fires right after the load.
    const value_handlers_t& h = v->handlers();
    // The handler seam answers the one read type itself (RFC-0028 D11), so its reference
    // goes back as it came: a handler that holds a value already costs no allocation here,
    // and one that computes a value minted the block (one, for a scalar) in the hook. An
    // empty reference on success is the hook's refused allocation, answered as such.
    if (h.on_read) {
        result_t<value_ref_t> produced = h.on_read();
        if (produced && !*produced) return std::unexpected(status_t::BACKPRESSURE);
        return produced;
    }
    return std::unexpected(status_t::NOT_FOUND);
}

result_t<value_ref_t> graph_t::read(vertex_handle_t vh, std::string_view caller) const {
    vertex_t* v = vh.get();
    if (!acl_allows(v, caller, acl_right_t::READ))
        return std::unexpected(status_t::PERMISSION_DENIED);
    if (v->role() == role_t::HANDLER) return read_handler_gated(v);
    // The branch/leaf fork (RFC-0005 §C follow-on): a vertex with ≥ 1 registered child
    // serves the composed branch read — the folded POINT tree of its registered
    // descendants' landed LKVs. AFTER the handler seam (a HANDLER target's on_read keeps
    // precedence); a leaf falls through to the LKV path byte-identically to before.
    if (v->has_registered_child()) {
        // The composed branch read BUILDS a value, so it wraps rather than shares. Measured
        // 1.00x against the old copy-out (30 paired samples): the subtree walk dominates the
        // one control block this costs.
        return read_subtree_folded(vh, caller);
    }
    value_ref_t sp = v->read_stored();  // lock-free
    if (!sp) return std::unexpected(status_t::NOT_FOUND);
    // The published value is handed back BY REFERENCE. This used to be `return *sp`, which
    // copied the rope and so cloned one segment_ptr_t per link — a contended refcount RMW per
    // link, on the line every reader of this vertex shares.
    return sp;
}

[[gnu::noinline]] void graph_t::dispatch_edge_target(const edge_view_t& e, const value_t& value) {
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
        instrument_.tick_resolve();  // compiled out unless kInstrumentCounters (#1664)
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
    //
    // Read TWICE, deliberately (#1448). The context is borrowed from the shared cold half
    // now, so each read is a handle load, a null test and a select — and `acl_allows` may
    // write through the graph, so the compiler cannot CSE the two. Hoisting it into a local
    // that stays live across the ACL call and the clone was MEASURED: 148 -> 145 instructions
    // here, at +36 B of spill, which is the wrong side of the trade for a leg whose byte pin
    // is otherwise unmoved by this change.
    if (!acl_allows(target, e.caller(), acl_right_t::WRITE)) {
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
    // Delivery TERMINATES here, so the target's own edges are never dispatched by this write —
    // but a STREAM target's ring still feeds the next propagate over it, exactly as an assign's
    // does. A shed append therefore loses that deferred delivery, and is counted at the
    // TARGET's own-subs width (the shed is the target's, not the source's).
    //
    // This is the RECEIVER seam: `target` is the consumer's own vertex, so if it is a STREAM
    // the ring materializes THERE and is charged against the source THAT vertex injected
    // (RFC-0025 §4.6.1 clause 2). The §4.4 arm is the receiving vertex's own declaration
    // (`graph_t::set_ring_source`) rather than a bit read off this edge — the RULED reading
    // since RFC-0025's 2026-08-24 §4.4 selector erratum (#1204), which demoted the
    // subscription's `reliability` bits to carried-verbatim-read-by-nothing rather than
    // commissioning a per-edge selector. The arm is NOT read here on purpose, and the erratum
    // banks why: `edge_view_t` is the always-inlined per-edge body of the wide fan-out loop,
    // whose size is a cliff rather than a slope — one added field there was enough to flip the
    // inline estimate and cost 12% (#1223 / #1250). Under the reliable arm the admission is refused
    // and this leg counts the declined delivery; a LOCAL producer writing the receiving vertex
    // directly gets the BACKPRESSURE status itself, which is the whole reach v1 has (the wire
    // carrier waits on the credit window §4.6.1 clause 7 parks).
    //
    // The target ADOPTS the delivered value (RFC-0028 D2, slice 4): its slot takes one more
    // reference on the block the source published — one `retain`, one exchange — instead of
    // cloning the links into a rope and minting a block of its own. K targets are K refcount
    // bumps, not K allocations. The admission filter, the ring admission and the handler
    // reaction all still run, inside the adopting `store_value`, on the shared block.
    vertex_t::store_drops_t store_drops;
    if (const auto stored = store_value(target, value, store_drops, e.caller(), nullptr); !stored) {
        // The cause is now read off the status rather than assumed. Every refusal this leg
        // could see used to be a resource one (`BACKPRESSURE` — a declined slot publish, a
        // declined ring admission, or the rope arm's clone or block), so counting it as
        // OUT_OF_MEMORY was exact.
        // The target's admission filter adds a leg that is not: a filter refusing a delivery
        // is a POLICY refusal, of a piece with the fan-in ACL denial counted a few lines above,
        // and folding it into the memory counter would make an operator read a rejected value
        // as an exhausted node. One status test, on a path already off the hot arm.
        count_drop(stored.error() == status_t::BACKPRESSURE ? drop_reason_t::OUT_OF_MEMORY
                                                            : drop_reason_t::DENIED,
                   1);
        return;
    }
    count_store_drops(target, store_drops);
}

[[gnu::noinline]] void graph_t::dispatch_edge_remote(const edge_view_t& e, const value_t& value) {
    // Remote delivery (#136): a write fans out to a remote subscriber as a
    // FWD{WRITE} (or auto-promoted COMPACT) via the injected sink — outside the
    // vertex lock, like every other dispatch leg, since the sink does transport I/O.
    //
    // The coherent slot read lives HERE, in the noinline leg, not in the always_inline
    // dispatch body that calls it (#1049): `dispatch_edge`'s per-edge test stays the one
    // relaxed load it always was, and this leg — already off the wide-fan-out critical
    // path — pays the sequence read. An empty snapshot means the sink was cleared or is
    // mid-publish; that edge is skipped for this write, which is the sink_slot_t contract.
    const auto sink = remote_sink_.get();
    if (sink.fn == nullptr) return;
    // The five wire fields are read straight off the SHARED cold half (#1448) rather than
    // out of five members the snapshot had copied for us. The snapshot holds a reference to
    // this record, so the two `string_view`s below are valid for the whole sink call and the
    // two route clones are the same refcount bumps `remote_delivery_t` always took — what
    // disappeared is the pair of `std::string` copies and the pair of `view_t` clones the
    // SNAPSHOT paid, per remote edge, per delivery. `has_remote_leg()` gated this call, so
    // the handle is non-null here; assert it rather than re-testing on the hot path.
    const subscriber_remote_t* r = e.remote.get();
    assert(r != nullptr && "dispatch_edge gates the remote leg on a populated cold half");
    sink.fn(sink.ctx,
            remote_delivery_t{.link = r->link,
                              .return_route = r->return_route,
                              .reverse_route = r->reverse_route,
                              .caller = r->caller,
                              .delivery_compact = r->delivery_compact},
            value);
}

/**
 * @brief `always_inline` — and the two legs above `noinline` — because the wide fan-out loop's
 *        per-edge cost is this function's body, so it must stay inlined in that loop: the
 *        target/remote legs live in the two helpers above precisely to keep this body's inline
 *        estimate small (the callback leg is the in-process hot case). The split is ENFORCED
 *        by attribute, not left to the inliner's estimate, because one added aggregate field
 *        in the remote leg was enough to flip that estimate — this body fell out of the loop
 *        and the in-process delivery gate paid 12% (#1223 steps 3+4; same hazard as #1250).
 */
[[gnu::always_inline]] inline void graph_t::dispatch_edge(const edge_view_t& e,
                                                          const value_t& value) {
    // The ONE dispatch of a subscription edge's three legs — shared by the per-write
    // fan_out and the admission durability latch (ADR-0049), so the legs cannot diverge.
    // Always called OUTSIDE the vertex lock (each leg may re-enter the graph or do I/O).
    if (e.callback != nullptr)
        e.callback(e.callback_ctx, value);  // the value by const ref (sink may clone links)
    if (e.target_key) dispatch_edge_target(e, value);
    if (e.has_remote_leg() && remote_sink_.installed()) dispatch_edge_remote(e, value);
}

void graph_t::fan_out_slice(vertex_t* v, const view::view_t& slice) {
    const value_storage_t<1> sv{slice};
    fan_out(v, sv.get());
}

void graph_t::fan_out(vertex_t* v, const value_t& value) {
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

    // ADR-0080's grace-point bracket, taken ONCE for this whole fan-out and deliberately
    // BELOW the gate above: a publish nobody subscribed to — the cheapest and commonest write
    // there is — never touches it at all. Everything from here on may invoke user code, so
    // from here on an `unsubscribe()` reaching this thread is a RE-ENTRANT one and must park
    // its `{ctx, release}` pair rather than free it under the snapshot built below.
    const dispatch_scope_t dispatch_scope;

    // Snapshot every active edge UNDER AN EDGE PIN (vertex_t::snapshot_edges), released
    // before we dispatch (callbacks / re-dispatch may re-enter the graph). Delivery is
    // value-agnostic — no per-subscriber comparison — so every active edge receives
    // `value`; WHICH vertices propagate is the per-vertex delivery_mode decided by the
    // sweep (RFC-0008). Small fan-out (the common case) placement-constructs into a RAW
    // stack buffer — no per-publish allocation and no dead stack zeroing (an
    // edge_view_t array default-construct cost ~18 ns/op of rep-stos zeroing here).
    edge_snapshot_t inline_buf;

    // Wide fan-out (> kInlineFanout) draws its snapshot from THIS call's own stack frame
    // first and from the graph's table source past it, and a dry source is a counted
    // truncation, never an abort (#1885) — the `mem::bump_source_t` shape the propagate paths
    // use (#1778). It replaces a `thread_local` vector that grew to the widest fan-out its
    // thread ever saw and never gave it back: a library-internal buffer, which is exactly what
    // this tree does not keep. A frame per call also deletes that buffer's re-entrancy guard,
    // since a callback that re-publishes gets a frame of its own. The frame is built only on
    // the wide arm, so the small fan-out (incl. the fan-1-vs-Zenoh path) pays one compare for
    // it; on the small arm `overflow` is reached only when a subscriber was added between
    // own_subs() and the snapshot, and then it draws from the table source directly.
    std::array<std::byte, kFanoutFrameBytes> scratch;
    std::optional<mem::bump_source_t> frame;
    if (v->own_subs() > vertex_t::kInlineFanout) frame.emplace(scratch, *tables_);
    mem::block_array_t<edge_view_t> overflow(frame ? static_cast<mem::block_source_t&>(*frame)
                                                   : *tables_);
    vertex_t::snapshot_drops_t drops;
    const std::size_t n = v->snapshot_edges(inline_buf, overflow, drops);
    // Fold BEFORE dispatching: these deliveries were abandoned inside the snapshot, and
    // dispatch re-enters the graph (a callback may publish, and a nested publish must not be
    // able to swallow this tally).
    if (drops.any()) count_snapshot_drops(drops);
    if (overflow.empty())
        for (std::size_t i = 0; i < n; ++i) dispatch_edge(inline_buf[i], value);
    else
        for (const edge_view_t& e : overflow) dispatch_edge(e, value);
}

result_t<value_ref_t> graph_t::store_value(vertex_t* v, view::rope_t&& value,
                                           vertex_t::store_drops_t& drops, std::string_view caller,
                                           const net::link_kind_t* link,
                                           vertex_t::ring_take_t* take) {
    drops = vertex_t::store_drops_t{};
    if (v->role() == role_t::HANDLER) return handler_write_rope(v, std::move(value), caller, link);
    // ADMISSION (the retaining roles' pre-store seam). It sits HERE — inside the one function
    // every store goes through, and above the tail every storing role shares — because that is
    // the only placement under which the filter cannot be bypassed: `write`, `assign`, a
    // `FWD{WRITE}` terminus, an inbound edge's fan-in delivery and each landing site of a
    // branch-POINT decomposition all reach storage through this call and no other. Admission is
    // a property of the VERTEX, not of a door; a filter that only saw remote writes would state
    // an invariant the owner could break by accident.
    //
    // BEFORE `store`, therefore before the sequence bump, the `await` wake, the STREAM ring
    // admission and every delivery: the callers deliver what this function RETURNS (the
    // published LKV pointer) or, on the STREAM arm, what the ring drained, so a normalised value
    // is the only value any subscriber can observe and a refused one is observable nowhere.
    //
    // The flag test is the whole cost to a vertex that installed no filter: one relaxed load of
    // the same word the write gate's `has_payload_rights()` reads, and no list walk. The filter
    // itself lives on the GRAPH (`graph_t::admissions_`) rather than on the vertex, so it costs
    // the seam-bearing and app-field-bearing vertices nothing either — the reason that member
    // states, measured. The null re-test is not redundant with the bit: retirement clears the
    // bit and leaves the node parked, so a racing store may see one without the other, and a
    // vertex mid-retire has no invariant left to defend — it admits, rather than turning a
    // retire into spurious write failures.
    //
    // THE one allocation a publish costs (RFC-0028 §5.1): the value's block, refcount and
    // link chain together, drawn from the graph's source and moved — not cloned — out of the
    // caller's rope. Exhaustion is a `nullptr` by value; the rope is then still the caller's.
    // It is minted BEFORE the filter runs, because the filter reads the value as a `value_t`
    // (RFC-0028 D10) and this block is the one an admitted write publishes anyway: an
    // admitted write still costs exactly one block, and only a refusal pays for one it frees.
    value_ref_t block = value_ref_t::adopt(value_t::make(std::move(value), *values_));
    if (block && v->has_admission()) {
        admission_t decided = admit(v, *block, caller, link);
        if (!decided) return std::unexpected(decided.error());
        // Engaged ⇒ store the NORMALISED rope instead. The writer's block dies here, which is
        // the point: nothing downstream can reach the spelling the filter rejected.
        if (*decided) block = value_ref_t::adopt(value_t::make(std::move(**decided), *values_));
    }
    return publish_value(v, std::move(block), drops, take);
}

[[gnu::noinline]] result_t<value_ref_t> graph_t::handler_write(vertex_t* v, const value_t& value,
                                                               std::string_view caller,
                                                               const net::link_kind_t* link) {
    const value_handlers_t& h = v->handlers();  // load once — a retire may swap it out
    if (!h.on_write) return std::unexpected(status_t::NOT_FOUND);
    // The subject is NOT re-derived here (#375): `caller` is the identical value the
    // WRITE gate one stack frame up passed to `acl_allows`, so the handler and the ACL
    // that admitted the write cannot disagree about who wrote. The ctx is a borrowed
    // view built on the stack — no allocation, nothing stored on the vertex.
    const write_ctx_t ctx{.subject = caller, .link = link};
    if (result_t<void> r = h.on_write(value, ctx); !r) return std::unexpected(r.error());
    v->note_write();
    return value_ref_t{};  // handler consumed it — nothing stored
}

[[gnu::noinline]] result_t<void> graph_t::handler_write_deliver(vertex_t* v, view::rope_t&& value,
                                                                std::string_view caller,
                                                                const net::link_kind_t* link) {
    // A handler stores no LKV (the user handler consumes the value), so there is no
    // published pointer to deliver from — the hot roles deliver the exact pointer
    // store_value hands back. There is no CLONE either, and that is #1505: the ONE value the
    // handler reads by reference (RFC-0028 D10) is the value this vertex's own subscribers are
    // delivered, built once on this frame by MOVING the writer's links in (no block, no
    // refcount traffic), or — past `kUnstoredInline` links — as one block from the graph's
    // source.
    //
    // What the clone #1505 removed cost, measured (#1516's bench_source_role): past the
    // rope's inline link capacity (knee between 2 and 3 links) it was one heap block on EVERY
    // handler write, ~40 ns flat in fan-out — a per-write term, paid in full even at fan-out
    // zero — and it made the non-retaining role the more expensive one at multi-link values,
    // inverting the ordering the role system advertises.
    //
    // No OUT_OF_MEMORY tally here, and the leg it counted is not merely narrower — it is
    // IMPOSSIBLE. This frame used to shed the vertex's ENTIRE fan-out when the notify clone
    // could not be allocated, counted one per subscriber (the widest drop in the graph). The
    // one resource this path can still be refused — the block of a chain past the inline
    // bound — is taken BEFORE the handler runs, so a refusal is the writer's BACKPRESSURE and
    // nothing ran. #854's own-subs-wide ruling is ANNOTATED, not overturned: OUT_OF_MEMORY
    // still counts on the assign path's shed pending mark (mark_pending, at own-subs width)
    // and on dispatch_edge_target's declined store (at width 1), so the reason code stays live
    // and `1 never stands in for N` still holds everywhere it can still be raised.
    const auto run = [&](const value_t& val) -> result_t<void> {
        const result_t<value_ref_t> stored = handler_write(v, val, caller, link);
        if (!stored) return std::unexpected(stored.error());
        deliver_vertex(v, val);
        // Eager delivery flushes any pending mark a prior assign left — but only while
        // what this write published is still current (#1185); on the handler leg that is
        // the null "consumed" sentinel, matching the handler's permanently null LKV.
        clear_pending(v, stored->get());
        return {};
    };
    if (value.link_count() <= kUnstoredInline) {
        const value_storage_t<kUnstoredInline> sv{std::move(value)};
        return run(sv.get());
    }
    const value_ref_t block = value_ref_t::adopt(value_t::make(std::move(value), *values_));
    if (!block) return std::unexpected(status_t::BACKPRESSURE);
    return run(*block);
}

[[gnu::noinline]] result_t<value_ref_t> graph_t::handler_write_rope(vertex_t* v,
                                                                    view::rope_t&& value,
                                                                    std::string_view caller,
                                                                    const net::link_kind_t* link) {
    // The handler reads a `value_t` (RFC-0028 D10). A local write owns its rope, so the links
    // MOVE into storage on this frame — no block, no refcount traffic — exactly the relay's
    // shape; a chain past the inline bound takes one block from the graph's source instead.
    // Out of line so the 200-odd bytes of storage stay off `store_value`'s own frame.
    if (value.link_count() <= kUnstoredInline) {
        const value_storage_t<kUnstoredInline> sv{std::move(value)};
        return handler_write(v, sv.get(), caller, link);
    }
    const value_ref_t block = value_ref_t::adopt(value_t::make(std::move(value), *values_));
    if (!block) return std::unexpected(status_t::BACKPRESSURE);
    return handler_write(v, *block, caller, link);
}

result_t<value_ref_t> graph_t::store_value(vertex_t* v, const value_t& value,
                                           vertex_t::store_drops_t& drops, std::string_view caller,
                                           const net::link_kind_t* link) {
    // A HANDLER stores nothing and reads the value by reference (RFC-0028 D10): it is handed
    // the delivered block itself — no clone of its links, no block of its own — exactly as a
    // stored target adopts it below. What it keeps past the call it keeps through
    // `value_ref_t::keep`, which is also what makes caller-owned storage safe to hand it.
    if (v->role() == role_t::HANDLER) {
        drops = vertex_t::store_drops_t{};
        return handler_write(v, value, caller, link);
    }
    // A value with no source is CALLER-OWNED storage (`value_storage_t`, the slice a branch
    // write delivers without storing): a reference kept past the call would outlive the frame
    // it lives in, so it takes the rope arm above — one clone of the links. `try_rope` is
    // nothrow and allocates nothing while the chain fits the rope's inline links; a refused
    // spill is BACKPRESSURE by value, which the delivery leg counts as OUT_OF_MEMORY.
    if (value.source() == nullptr) {
        view::rope_t clone;
        if (!value.try_rope(clone)) {
            drops = vertex_t::store_drops_t{};
            return std::unexpected(status_t::BACKPRESSURE);
        }
        return store_value(v, std::move(clone), drops, caller, link);
    }
    drops = vertex_t::store_drops_t{};
    // The admission filter still runs, on the SHARED block itself — no clone of its links —
    // and only a normalisation mints a block of the vertex's own. A filter that admits
    // unchanged costs the adoption nothing more. Same seam, same `caller`, same placement above
    // the storing tail as the rope arm — admission is a property of the vertex, not of a door.
    if (v->has_admission()) {
        admission_t decided = admit(v, value, caller, link);
        if (!decided) return std::unexpected(decided.error());
        if (*decided)
            return publish_value(
                v, value_ref_t::adopt(value_t::make(std::move(**decided), *values_)), drops);
    }
    // ADOPT (RFC-0028 D2): one more reference on the block that is already the value. Zero
    // allocations, zero copies — the target's slot and the source's hold the same block, which
    // is released through the source it was drawn from when the last of them lets go.
    return publish_value(v, value_ref_t::share(&value), drops);
}

admission_t graph_t::admit(vertex_t* v, const value_t& value, std::string_view caller,
                           const net::link_kind_t* link) const {
    const admission_node_t* a = admission_for(v);
    if (a == nullptr || !a->on_admit) return std::optional<view::rope_t>{};
    // Same `caller` the ACL gate one frame up ran on (#375): the filter and the gate that
    // admitted the write cannot disagree about who wrote. `link` is the arrival link's catalog
    // identity (#1650) — a pointer the router resolved once per link, never looked up here.
    const write_ctx_t ctx{.subject = caller, .link = link};
    return a->on_admit(value, ctx);
}

result_t<value_ref_t> graph_t::publish_value(vertex_t* v, value_ref_t sp,
                                             vertex_t::store_drops_t& drops,
                                             vertex_t::ring_take_t* take) {
    // The storage verb has ONE tail for every role now (RFC-0025 §4.6.1 clause 1): publish the
    // LKV lock-free, bump the sequence, wake awaiters. A PRODUCER NEVER QUEUES — the ring the
    // STREAM arm used to append here moved to the RECEIVING vertex, below.
    //
    // The retained width is measured ONLY for a STREAM. `total_length()` walks the value's
    // links, and a producer must not pay a walk for a queue it does not have:
    // measured, hoisting it out of this branch cost the 4-writer plain-write point ~30%.
    //
    // A vertex that retains NOTHING (`retention_t::NONE`, RFC-0028 §5.4) keeps neither slot nor
    // ring: the write still moves the sequence and wakes awaiters, and the caller delivers the
    // value it gets back and then lets it go. `write_impl` never reaches here for such a vertex
    // (it relays from the stack); this arm serves the stores that do — a target delivery, which
    // adopts the source's block, and a branch landing site.
    if (v->retains_none()) {
        if (!sp) return std::unexpected(status_t::BACKPRESSURE);  // the block was refused
        v->note_write();
        return sp;
    }
    const bool receives = v->role() == role_t::STREAM;
    const std::size_t retained = receives && sp ? sp->total_length() + kRingEntryOverhead : 0;
    if (sp && !v->store(sp)) sp.reset();  // the slot declined: nothing published (#477)
    // vertex_t::store soft-fails its LKV allocation nothrow (#477): null here (a
    // non-handler role always publishes a pointer) is exactly OOM — report it as the
    // injected-resource status (BACKPRESSURE, ADR-0060 §3), never abort. Distinct from
    // `store_value`'s handler leg, whose empty reference is the "consumed, nothing stored"
    // SUCCESS sentinel.
    if (!sp) return std::unexpected(status_t::BACKPRESSURE);
    // THE RECEIVER'S QUEUE. A STREAM vertex is a consumer-owned ring: whoever wants depth
    // makes its OWN vertex a STREAM, and the entries it retains are charged, in bytes, to the
    // source that vertex injected (or, having declared none, to this graph's default). The
    // charge is reservation ADMISSION — the payload never moves, and `sp` is the same refcount
    // share the publish handed out.
    //
    // The admission runs AFTER the publish, so an awaiter woken by the sequence bump can reach
    // a drain before the entry is queued. That window defers a delivery; it never loses one —
    // the drain cursor only advances over entries that are IN the ring, so the late arrival is
    // taken by the next covering flush. Charging BEFORE the publish would instead have to
    // un-charge on an LKV soft-fail, which is a leak waiting for its first early return.
    //
    // A write hands `take` in, and the admission takes the unflushed window into it under the
    // SAME stripe section (#1713): admit-then-drain used to be two sections and a heap vector.
    //
    // The source is resolved INSIDE that section too. The graph hands only its DEFAULT; a vertex
    // that declared its own (or has already charged) has it bound in its ring state, and
    // `vertex_t::ring_admit` charges the bound source first — "per-injection-point, never a
    // shared pool" (RFC-0025 §4.6.1 clause 3), spelled once, under the lock. Reading the bound
    // source here, unlocked, raced the first admission that creates the ring state.
    if (receives && !v->ring_admit(sp, retained, *values_, drops, take))
        return std::unexpected(status_t::BACKPRESSURE);  // the RELIABLE arm of §4.4
    return sp;
}

void graph_t::deliver_unstored(vertex_t* v, const view::rope_t& value,
                               void (graph_t::*fn)(vertex_t*, const value_t&), std::size_t width) {
    if (value.link_count() <= kUnstoredInline) {
        const value_storage_t<kUnstoredInline> sv{value};
        (this->*fn)(v, sv.get());
        return;
    }
    const value_ref_t heap = value_ref_t::adopt(value_t::make(value.links(), *values_));
    if (!heap) {
        count_drop(drop_reason_t::OUT_OF_MEMORY, width);
        return;
    }
    (this->*fn)(v, *heap);
}

void graph_t::bubble_up(vertex_t* v, const value_t& value) {
    // Entered only when v->listeners_above() says an ancestor subscriber exists —
    // the idle write path never walks (RFC-0005 §near-free-when-idle; the counter
    // below is what tests/benches assert on via ancestor_walks(); compiled in only when
    // config_t::kInstrumentCounters is bound, #1664).
    instrument_.tick_walk();
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
[[nodiscard]] bool is_branch_point(const view::rope_t& value, role_t role) {
    if (role == role_t::HANDLER || value.link_count() < 1 || !value.links()[0].is_host())
        return false;
    const std::span<const std::byte> head = value.links()[0].bytes();
    return head.size() >= 4 &&
           std::to_integer<std::uint8_t>(head[0]) == std::to_underlying(type_t::POINT) &&
           (std::to_integer<std::uint8_t>(head[1]) & 0x40) != 0;
}

/**
 * @brief The written value's LEADING TLV type, or nullopt when it has none.
 *
 * A value with no links, or whose head is DEVICE memory (never dereferenced), has no type
 * byte the gate may read — so it is not a declared payload and takes the default right.
 */
[[nodiscard]] std::optional<type_t> leading_type(const view::rope_t& value) {
    if (value.link_count() < 1 || !value.links()[0].is_host()) return std::nullopt;
    const std::span<const std::byte> head = value.links()[0].bytes();
    if (head.empty()) return std::nullopt;
    return static_cast<type_t>(std::to_integer<std::uint8_t>(head[0]));
}
}  // namespace

result_t<void> graph_t::write_impl(vertex_t* v, view::rope_t value, std::string_view caller,
                                   const net::link_kind_t* link) {
    // The ONE WRITE gate of the value-write path, so counting the refusal here counts it for
    // every plane that enters through it: an API write, a FWD{WRITE} terminus, and both the
    // warm and cold COMPACT terminus arms (#1068). The router discards this status — it has
    // no caller to hand it to — so if the denial were not counted at the gate that produces
    // it, a revoked peer streaming into a protected vertex would look exactly like a quiet
    // link. Counting an API caller's own denial as well is deliberate (see delivery_drops_t
    // ::denied): `denied` means refusals, not refusals-nobody-heard-about, and a counter
    // whose value depended on WHICH door a refusal came through could not be summed.
    //
    // WHICH right is demanded is the vertex's own declaration (RFC-0014 Amendment 2): a
    // handler vertex may map a written TLV type to a right other than `WRITE` — the creator
    // endpoint demands `CREATE` for a `SPEC` — and everything else demands `WRITE`, as it
    // always has. The gate itself does not move, and neither does the counter: one
    // declaration decides the right, one site makes the demand and counts the refusal.
    //
    // The cost of the feature to a vertex that declares nothing is the `has_payload_rights()`
    // bit test below and NOTHING else — no byte on the vertex, no dereference, and no branch
    // taken. The rows live on the graph (@ref graph_t::payload_rights_) precisely so that
    // stays true.
    acl_right_t right = acl_right_t::WRITE;
    if (v->has_payload_rights()) {
        if (const std::optional<type_t> t = leading_type(value))
            right = declared_write_right(v, *t);
    }
    if (!acl_allows(v, caller, right)) {
        count_drop(drop_reason_t::DENIED, 1);
        return std::unexpected(status_t::PERMISSION_DENIED);
    }
    // `write` is the RFC-0008 §D composition — assign the vertex, then deliver exactly
    // what it stored (a leaf VALUE, or each landed descendant of a branch POINT). This is
    // the FWD{WRITE}-terminus behavior: a TARGETED delivery of the written vertex(es), not
    // a subtree sweep. propagate(v) is the separate accumulate-then-flush primitive.
    //
    // ONE role load for the whole frame (#1477). The three forks below used to re-read
    // `role()`; now that the member is atomic the compiler may not fold those reads, and —
    // more to the point — three reads of a member a concurrent retire can store to could
    // DISAGREE with each other, letting one write take the branch-POINT decision of the
    // retiring occupant and the storage decision of the placeholder. A single snapshot makes
    // the frame internally consistent whichever of the two it caught.
    const role_t role = v->role();
    if (is_branch_point(value, role)) return write_branch(v, value, caller, link, /*notify=*/true);
    if (role == role_t::HANDLER) return handler_write_deliver(v, std::move(value), caller, link);
    // RETENTION NONE (RFC-0028 §5.4): the pure relay. Nothing is kept, so nothing needs a
    // block of its own — the value is delivered from the stack exactly as the HANDLER arm above
    // delivers it, and a vertex whose subscribers are all callbacks draws ZERO blocks per
    // write. A target subscriber still gets a block of its own (it retains; the source did
    // not mint one to share). One relaxed test of the flag byte the admission check reads.
    if (v->retains_none()) return relay_write(v, std::move(value), caller, link);
    vertex_t::store_drops_t store_drops;
    // The STREAM arm's drain buffer, filled by the ring admission itself (#1713): stack-first,
    // so the common write — whose window is its own entry — allocates nothing for it.
    vertex_t::ring_take_t taken(*values_);
    const result_t<value_ref_t> stored = store_value(v, std::move(value), store_drops, caller, link,
                                                     role == role_t::STREAM ? &taken : nullptr);
    if (!stored) return std::unexpected(stored.error());
    if (role == role_t::STREAM) {
        // Drain this RECEIVER's ring and advance its cursor, so a later propagate over the
        // same stream does not re-deliver what went out here (RFC-0008 §E). The entries are
        // the ones its own admission just queued — a queue, in order, not a coalesce.
        //
        // If the admission was SHED, this drain finds less than was written — or nothing at
        // all — and the whole fan-out is abandoned without ever reaching the dispatch plane's
        // counting sites (#1003). The write still succeeds; the tally is what makes the loss
        // something an operator can see, and the drain's gap out-param is what makes it
        // something the CONSUMER can see, in order, at the shed point.
        //
        // The drain already HAPPENED, inside the admission's own lock section (#1713). Only a
        // store that never reached the ring (its role moved under a racing retire) left the
        // take disengaged, and that one falls back to the separate drain it always had.
        count_store_drops(v, store_drops);
        if (!taken.engaged()) {
            deliver_current(v);
        } else {
            for (const value_ref_t& sp : taken.entries()) deliver_vertex(v, *sp);
        }
    } else {
        // Deliver exactly what was stored (RFC-0008 §D): the published LKV pointer —
        // no notify reclone of the rope on the hot write path.
        deliver_vertex(v, **stored);
    }
    // Eager delivery flushes any pending mark a prior assign left — but only while what
    // this write published is still v's current LKV (#1185).
    clear_pending(v, stored->get());
    return {};
}

/**
 * @brief The `retention_t::NONE` write: admit, move the sequence, deliver from the stack, keep
 *        nothing (RFC-0028 §5.4).
 *
 * The HANDLER arm's delivery shape without the handler: `deliver_unstored` wraps the rope in a
 * `value_storage_t` on this frame, so callback subscribers see the value and nothing is
 * allocated for it. The admission filter still runs — it is a property of the vertex, not of
 * whether the vertex keeps what it admits — and a normalised value is the one delivered.
 */
result_t<void> graph_t::relay_write(vertex_t* v, view::rope_t value, std::string_view caller,
                                    const net::link_kind_t* link) {
    if (v->has_admission()) {
        // The filter reads a `value_t` (RFC-0028 D10): show it the writer's links on this frame
        // (a refcount clone per link, as the relay's own delivery below takes), or — past the
        // inline bound — in one block, which a relay that retains nothing frees on return.
        admission_t decided = [&]() -> admission_t {
            if (value.link_count() <= kUnstoredInline) {
                const value_storage_t<kUnstoredInline> sv{value};
                return admit(v, sv.get(), caller, link);
            }
            const value_ref_t block = value_ref_t::adopt(value_t::make(value.links(), *values_));
            if (!block) return std::unexpected(status_t::BACKPRESSURE);
            return admit(v, *block, caller, link);
        }();
        if (!decided) return std::unexpected(decided.error());
        if (*decided) value = std::move(**decided);
    }
    v->note_write();
    deliver_unstored(v, value, &graph_t::deliver_vertex, v->own_subs() + v->listeners_above());
    return {};
}

namespace {
/**
 * @brief Does @p v RETAIN a last-known-value — the one hard dependency RFC-0008's
 *        sweep plane has on the state plane?
 *
 * `STORED_VALUE` and `STREAM` publish an LKV every store unless declared `retention_t::NONE`
 * (RFC-0028 §5.4); a `HANDLER` hands the value to `on_write` and keeps nothing
 * (`graph_t::store_value`'s null success sentinel). Since `propagate` takes no value argument
 * — "the last-known-value is the single source of truth", RFC-0008 §C — the
 * accumulate-then-flush pair has nothing to flush at a vertex that retains nothing, and
 * RFC-0008 Amendment 2 refuses it at the verb rather than sweeping silence. Two loads on two
 * cold verbs.
 */
[[nodiscard]] bool retains(const vertex_t* v, role_t r) noexcept {
    return r != role_t::HANDLER && !v->retains_none();
}
}  // namespace

result_t<void> graph_t::assign(vertex_handle_t vh, view::rope_t value, std::string_view caller) {
    vertex_t* v = vh.get();
    if (!acl_allows(v, caller, acl_right_t::WRITE))
        return std::unexpected(status_t::PERMISSION_DENIED);
    // A non-retaining vertex has no state plane for the STATE half to land in (RFC-0008
    // Amendment 2). AFTER the ACL gate, so the refusal discloses the role only to a caller
    // already admitted to write. `SCHEMA_NOT_FOUND` is the taxonomy's contract-mismatch
    // status — the same answer `history` / `drain_unflushed` give a non-STREAM vertex —
    // and deliberately NOT `BACKPRESSURE`: nothing is under pressure and a retry will
    // never succeed. Use `write`, which dispatches the handler seam and delivers eagerly.
    // One snapshot for both forks, for the reason `write_impl` states (#1477).
    const role_t role = v->role();
    if (!retains(v, role)) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    // The STATE half only (RFC-0008 §A): swap the last-known-value / append the stream
    // ring / bump the write sequence (waking await), then mark v for the next covering
    // sweep. A branch POINT assigns each descendant the same way. Sends nothing.
    // An `assign` is an API call — no link carried it, so its admission sees a null link.
    if (is_branch_point(value, role))
        return write_branch(v, value, caller, nullptr, /*notify=*/false);
    vertex_t::store_drops_t store_drops;
    const result_t<value_ref_t> stored =
        store_value(v, std::move(value), store_drops, caller, nullptr);
    if (!stored) return std::unexpected(stored.error());
    // A shed ring append here loses the delivery the NEXT covering sweep would have drained
    // — deferred, not eager, but lost all the same, and the sweep has no way to know an
    // entry was ever meant to be there.
    count_store_drops(v, store_drops);
    mark_pending(v);
    return {};
}

result_t<void> graph_t::write_branch(vertex_t* v, const view::rope_t& value,
                                     std::string_view caller, const net::link_kind_t* link,
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
    const std::expected<view::view_t, tr::view::flatten_err_t> head =
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
    mem::bump_source_t src(stack, *tables_);
    const std::expected<wire::tlv_arena_t, wire::err_t> arena =
        wire::decode_into(head->bytes(), src);
    if (!arena) return std::unexpected(status_t::TYPE_MISMATCH);
    const wire::tlv_arena_t& a = *arena;

    // The written tree is rooted AT `v`: render its full key once (ADR-0057
    // render-on-demand) — the node-key prefix of the whole decomposition plan. The key
    // render and its parse copy are NOTHROW (#477): OOM soft-fails the branch write as
    // BACKPRESSURE, the injected-resource status, never an abort on the writer thread.
    mem::bytes_t root_key_bytes(src);  // the plan's scratch shares the decode's frame
    if (!try_build_key(v, root_key_bytes)) return std::unexpected(status_t::BACKPRESSURE);
    const std::span<const std::byte> root_key = mem::as_span(root_key_bytes);
    // post-order; plan.back() is the root. Its blocks come from the injected source (#873
    // phase 1) — the node count is PEER-CHOSEN (the peer picks how deep and how wide the
    // branch it writes is), so this is exactly the growth a bounded node must be able to cap.
    mem::block_array_t<branch_node_t> plan(src);
    const result_t<bool> parsed = parse_branch_node(a, 0, *head, root_key, plan, src);
    if (!parsed) return std::unexpected(parsed.error());
    // The root POINT's leading NAME — which the parse has just shape-checked — must name this
    // vertex (the written tree is rooted AT `v`); a mismatch is an addressing error, not a
    // shape error, and it outranks the value-free no-op below.
    if (!std::ranges::equal(a[wire::tlv_arena_t::first_child(0)].body,
                            key_view_t{v->name().bytes()}.last_segment()))
        return std::unexpected(status_t::INVALID_PATH);
    if (!*parsed) return {};  // a value-free branch is a no-op write
    // Tag the root ONCE: it lands at `v` itself (already WRITE-gated by write_impl), and its
    // subscription point is notified with the whole written TLV as-is. The notify is
    // move-assigned from a fresh copy: a copy-assignment straight from `*head` trips GCC's
    // -Wmaybe-uninitialized (a false positive on the SRA'd view at -Os, IDF v6.0).
    plan.back().vx = v;
    plan.back().notify = view::view_t(*head);

    // Admission: resolve every landing vertex and gate WRITE on each BEFORE any store, so a
    // denial or a miss rejects the whole branch with nothing landed. A missing landing site is
    // NOT_FOUND unless its parent's creation hook creates it (RFC-0030 §7.1); a vertex a hook
    // created stays if a later site refuses, as any registration would. A landing site
    // is a plan node with a VALUE of its own; `sites` lists them so the passes below walk
    // only those, and each site's outcome lives on its node. Failable, from the table source
    // (#477, #1778): a refusal => BACKPRESSURE.
    mem::block_array_t<branch_node_t*> sites(src);
    if (!sites.reserve(plan.size())) return std::unexpected(status_t::BACKPRESSURE);
    for (branch_node_t& node : plan) {
        if (node.store.empty()) continue;
        if (node.vx == nullptr) {               // every site but the root, tagged above
            std::optional<view::rope_t> slice;  // built only if a creation hook is asked
            const result_t<vertex_t*> ensured = find_or_create_ptr(
                mem::as_span(node.key), caller,
                [&]() -> const view::rope_t& { return slice.emplace(node.store); });
            if (!ensured) return std::unexpected(ensured.error());
            node.vx = *ensured;
            if (!acl_allows(node.vx, caller, acl_right_t::WRITE))
                return std::unexpected(status_t::PERMISSION_DENIED);
        }
        (void)sites.push_back(&node);  // reserved
    }

    // Apply: land every slice. Admission was atomic; application is per-vertex and
    // best-effort (a handler-role landing site may refuse its slice without
    // un-landing the others) — the branch is NOT a transaction (RFC-0005
    // §atomicity non-promise; each leaf is its own consistent refcounted snapshot).
    for (branch_node_t* const site : sites) {
        vertex_t::store_drops_t store_drops;
        result_t<value_ref_t> r = store_value(site->vx, site->store, store_drops, caller, link);
        // A landing site's own admission filter may refuse its slice, and per the
        // non-transaction rule above that un-lands nothing else. Record it so the notify
        // half skips THIS site: the branch's per-site refusal is worth exactly as much as
        // the plain path's if a subscriber can still see the slice that was refused.
        // BACKPRESSURE keeps its old behaviour (delivered, unretained) — that is a
        // resource event, not a verdict on the value — so it reads as "not refused".
        site->refused = !r && r.error() != status_t::BACKPRESSURE;
        site->stored = std::move(r).value_or(value_ref_t{});
        // Counted ONLY on the assign half. The notify half below delivers each covered site's
        // slice through fan_out and then mark_flushed()es the cursor, so on that path the ring
        // was never the delivery vehicle: a shed append costs a HISTORY entry, not a delivery,
        // and counting it would be the overcount that makes delivery_drops() lie the other way.
        if (!notify) count_store_drops(site->vx, store_drops);
    }

    if (!notify) {
        // The assign half (RFC-0008 §B branch-assign): mark each landed vertex for the
        // next covering propagate sweep; deliver nothing, bubble nothing. A pass of its own,
        // after every store, rather than folded into the store loop: interleaving the two
        // measured ~6% slower on a wide branch assign.
        for (const branch_node_t* site : sites) mark_pending(site->vx);
        return {};
    }

    // Notify: one delivery per covered subscription point, with its slice — the
    // VALUE for a leaf landing site, the node's POINT subtree for an interior
    // node, and the whole written TLV as-is at the root and (via bubbling) above.
    //
    // TWO LIMITS of admission on THIS path, stated rather than papered over. (1) The slices
    // fanned out here are cut from the WRITTEN tree, not read back from what each site stored,
    // so a filter that NORMALISES a branch slice changes what is retained and read, not what
    // this eager notify delivers — the pre-existing property of the branch path, unchanged.
    // (2) The bubbled delivery below carries the whole written tree to ancestor subscribers,
    // which no per-site skip can carve a refused leaf out of without re-encoding the tree.
    // REFUSAL at the site's own subscription point is what is enforced here, and it is the half
    // that matters: the vertex whose invariant the filter defends never delivers the value it
    // rejected.
    //
    // The refusal is the node's own flag, so a wide branch pays one test per node here, not a
    // scan of every site per node.
    for (const branch_node_t& node : plan) {
        if (node.refused || node.notify.empty()) continue;
        vertex_t* vx = node.vx != nullptr ? node.vx : find_ptr(node.key);
        if (vx != nullptr) fan_out_slice(vx, node.notify);
    }
    if (v->listeners_above() > 0)
        deliver_unstored(v, value, &graph_t::bubble_up, v->listeners_above());
    // Eager branch delivered these landing sites — clear any pending mark (a prior assign)
    // and advance stream drain cursors so a later sweep does not re-deliver (RFC-0008 §E).
    for (const branch_node_t* site : sites) {
        clear_pending(site->vx, site->stored.get());
        if (site->vx->role() == role_t::STREAM) site->vx->mark_flushed();
    }
    return {};
}

void graph_t::deliver_vertex(vertex_t* v, const value_t& value) {
    fan_out(v, value);
    // Vertical bubbling (RFC-0005): every subscription observes its vertex AND all
    // descendants, so a delivery also fans out to each ancestor's subscribers. Gated on
    // one relaxed load when nobody listens above.
    if (v->listeners_above() > 0) bubble_up(v, value);
}

void graph_t::deliver_current(vertex_t* v) {
    if (v->role() == role_t::STREAM) {
        // A stream is a queue (RFC-0008 §E): drain the RECEIVER's ring entries appended since
        // the last flush, in order — NOT a coalesce. Snapshot under the lock
        // (vertex_t::take_unflushed), deliver outside — into a stack-first buffer, so a sweep
        // over a short window allocates nothing for it (#1713).
        vertex_t::ring_take_t batch(*values_);
        if (v->take_unflushed(batch) == 0) return;  // nothing appended since the last flush
        for (const value_ref_t& sp : batch.entries()) deliver_vertex(v, *sp);
        return;
    }
    // STORED_VALUE: the last-known-value, once. HANDLER / never-assigned: null LKV, nothing.
    const value_ref_t sp = v->read_stored();
    if (!sp) return;
    deliver_vertex(v, *sp);
}

result_t<void> graph_t::propagate(vertex_handle_t v) {
    // The sweep root's OWN delivery is unconditional (below), and it reads the LKV — so a root
    // that retains nothing is a sweep that was always going to deliver its own value nowhere.
    // Refused at the verb, with `assign`'s status and for `assign`'s reason (RFC-0008
    // Amendment 2). Descendants are untouched by this: a sweep rooted at a RETAINING ancestor
    // still walks past non-retaining vertices exactly as before — they simply carry no mark,
    // because `assign` no longer admits one.
    if (!retains(v.get(), v.get()->role())) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    propagate_impl(v.get());
    return {};
}

result_t<void> graph_t::propagate(vertex_handle_t v, emission_mode_t mode) {
    // PER_VERTEX is the one-arg door verbatim — not a re-implementation of it, so the shipped
    // default cannot drift from the mode that names it (RFC-0008 §D stays the default;
    // RFC-0025 §4.1.2 clause 5 adds the alternative, it does not move the floor). The FOLD
    // body lives beside the other folds, below `read_subtree_folded`, because it shares their
    // header framing.
    if (mode != emission_mode_t::FOLD) return propagate(v);
    // The non-retaining refusal is the VERB's, not the emission mode's: a fold rooted at a
    // vertex that retains nothing has the same nothing to fold (RFC-0008 Amendment 2), and
    // refusing it here keeps the two modes answering alike for the same root.
    if (!retains(v.get(), v.get()->role())) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    return propagate_folded_impl(v.get());
}

void graph_t::propagate_impl(vertex_t* v) {
    // The argument is always delivered — a direct propagate is never gated by the vertex's
    // own delivery_mode (RFC-0008 §C, the EXPLICIT escape hatch, and "notify its own subs"
    // is policy-independent).
    deliver_current(v);
    // Sweep the strict descendants: DRAIN the IF_NEWER pending set over v's prefix range,
    // and ITERATE the UNCONDITIONAL set over it. A subtree is a contiguous prefix range of
    // the key order (RFC-0008 §B). Snapshot the keys under sweep_mutex_, then deliver
    // outside it — delivery re-enters the graph (fan_out/re-dispatch), like fan_out itself.
    // Every allocation in the snapshot is failable and drawn from the table source (#477,
    // #1778): a refused key render skips the sweep, and a refusal mid-collection stops it
    // BEFORE draining the affected mark — the undelivered entries stay in their sets, so the
    // sweep defers instead of aborting. The scratch is a stack frame first (#1778): a sweep
    // over a short subtree takes nothing from the table source's class locks, and a longer
    // one spills to the table source.
    std::array<std::byte, 512> scratch;
    mem::bump_source_t frame(scratch, *tables_);
    mem::bytes_t lo_key(frame);
    if (!try_build_key(v, lo_key)) return;  // refused: marks retained — the next sweep retries
    const std::span<const std::byte> lo = mem::as_span(lo_key);
    key_list_t to_deliver(frame);
    bool own_drained = false;  // v's own mark went with the drain (v was delivered above)
    {
        const std::lock_guard lock(sweep_mutex_);
        // Collect BEFORE the drain: a refusal leaves this and later marks for the next
        // covering sweep instead of silently losing them. The drained run goes in one erase.
        const auto [first, end] = subtree_run(pending_, lo);
        key_set_t::pos_t last = first;
        for (; last != end; last = pending_.next(last)) {
            const std::span<const std::byte> k = mem::as_span(pending_.at(last).key);
            if (k.size() != lo.size() && !to_deliver.push(k)) break;  // strict descendant
            own_drained = own_drained || k.size() == lo.size();
        }
        // v itself, if present, was delivered above. An empty run takes no atomic RMW: the
        // plain propagate of an unmarked vertex is this path's common case.
        if (last != first)
            pending_count_.fetch_sub(
                pending_.erase_if(first, last, [](const key_set_t::entry_t&) { return true; }),
                std::memory_order_relaxed);
        // Iterate, do not drain; a refusal defers the rest to the next sweep.
        const auto [ufirst, uend] = subtree_run(unconditional_, lo);
        for (key_set_t::pos_t i = ufirst; i != uend; i = unconditional_.next(i)) {
            const std::span<const std::byte> k = mem::as_span(unconditional_.at(i).key);
            if (k.size() != lo.size() && !to_deliver.push(k)) break;
        }
    }
    // Drop the pending-mark hint (#1712) of every vertex this sweep drained — but only where
    // the key is STILL absent, checked under the sweep lock, so the hint invariant holds
    // exactly: a key in `pending_` always has its vertex's hint up. Without the re-check, a
    // mark landing between the drain and the drop (another thread's assign, or a callback
    // of an EARLIER vertex in this very sweep) would keep its key and lose its hint; the
    // eager write that followed would skip retiring it, and the next covering sweep would
    // re-deliver what that write already delivered (RFC-0008 §B, 2026-09-30 erratum: an
    // eager write's clear means "a later covering sweep does not re-deliver").
    //
    // Two phases because resolving a key takes the map lock, which never nests under the
    // sweep lock: resolve outside it, then ONE more sweep-lock section for the whole batch —
    // a sweep cost, never an eager-write one. The resolved pointers are the delivery list
    // too, so no key is resolved twice. On a refusal for that list the hints stay up (a
    // stale-up hint costs one slow-path probe, never a delivery) and delivery resolves per key.
    mem::block_array_t<vertex_t*> targets(frame);
    if (!targets.reserve(to_deliver.size())) {
        for (std::size_t i = 0; i < to_deliver.size(); ++i) {
            if (vertex_t* u = find_ptr(to_deliver[i])) deliver_current(u);
        }
        return;
    }
    bool any_hint = own_drained && v->has_pending_mark();
    for (std::size_t i = 0; i < to_deliver.size(); ++i) {
        vertex_t* const u = find_ptr(to_deliver[i]);
        (void)targets.push_back(u);  // reserved: cannot fail; nullptr = vanished mid-sweep
        any_hint = any_hint || (u != nullptr && u->has_pending_mark());
    }
    if (any_hint) {  // an UNCONDITIONAL-only sweep is never hinted and takes no second lock
        const std::lock_guard lock(sweep_mutex_);
        if (own_drained && v->has_pending_mark() && !pending_.contains(lo))
            v->set_pending_mark(false);
        for (std::size_t i = 0; i < targets.size(); ++i) {
            vertex_t* const u = targets[i];
            if (u != nullptr && u->has_pending_mark() && !pending_.contains(to_deliver[i]))
                u->set_pending_mark(false);
        }
    }
    for (vertex_t* const u : targets) {
        if (u != nullptr) deliver_current(u);
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
    // The key render and the set insert both allocate on the writer thread, from the table
    // source and failably (#477, #1778): on a refusal the pending mark is dropped (that
    // deferred delivery is shed, exactly like an eager delivery leg under the same pressure),
    // never an abort.
    //
    // "Exactly like an eager delivery leg" is now true of the COUNTING too (#1003). It was
    // not: the eager legs have counted since the counting door landed while these two shed in
    // silence, and a comment asserting a symmetry the code did not have is what hid this. An
    // unmarked vertex is delivered by no sweep at all, so with no later write to re-mark it
    // the assigned value is never delivered — a lost delivery, not a deferred one. Counted at
    // the same one-per-subscriber width; the rare overcount when a later write DOES re-mark is
    // accepted, because undercounting a real loss is the worse failure.
    mem::bytes_t key(*tables_);  // outside the lock (a lock-free parent walk)
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
    // The shed is ATTRIBUTED from the insert's own answer: only a refused insert is a dropped
    // delivery. A mode that flipped to EXPLICIT / UNCONDITIONAL under the lock sheds nothing
    // (neither wants a mark), and an insert that finds the key already present sheds nothing
    // either — the mark is there and the next covering sweep will deliver. Still single-exit,
    // so `key`'s cleanup keeps the shape the paragraph above paid for.
    // The pending-mark hint (#1712) is raised here, under the lock, on every mark that leaves
    // a key in the set — a fresh insert or one already present. Every drop is also taken
    // under this lock and only over an absent key, so a set member's hint is always up.
    const bool if_newer = v->delivery_mode() == delivery_mode_t::IF_NEWER;
    const key_set_t::emplace_result_t ins = if_newer ? pending_.try_emplace(std::move(key), v)
                                                     : key_set_t::emplace_result_t{nullptr, false};
    if (ins.value != nullptr) v->set_pending_mark(true);
    if (ins.inserted)
        pending_count_.fetch_add(1, std::memory_order_relaxed);
    else if (if_newer && ins.value == nullptr)
        count_drop(drop_reason_t::OUT_OF_MEMORY, v->own_subs());
}

void graph_t::clear_pending(vertex_t* v, const value_t* delivered) {
    // Same idle fast path as mark_pending: an unobserved vertex was never marked.
    if (v->own_subs() == 0 && v->listeners_above() == 0) return;
    // Unmarked-vertex fast path (#1712): this vertex holds no mark, so there is nothing to
    // retire — no key render, no allocation, no graph-wide sweep lock, however many OTHER
    // vertices are marked. Racing a concurrent mark_pending is the same safe direction as the
    // count gate below: the mark stays for the next covering sweep.
    if (!v->has_pending_mark()) return;
    // Empty-set fast path (the per-eager-write case when nobody uses assign+propagate):
    // no key render, no sweep lock. Racing a concurrent mark_pending here leaves the mark
    // for the next covering sweep — the always-safe direction (one duplicate delivery of
    // the current LKV at worst, never a lost one).
    if (pending_count_.load(std::memory_order_relaxed) == 0) return;
    // Failable key render (#477): on a refusal keep the stale mark — the same safe direction.
    // Never an abort on the writer thread.
    std::array<std::byte, 256> scratch;  // the key is a stack frame first (#1778)
    mem::bump_source_t frame(scratch, *tables_);
    mem::bytes_t key(frame);  // outside the lock
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
    if (v->read_stored().get() != delivered) return;
    // The hint drops with the mark — or alone, when it was stale over an absent key. Under
    // the lock, so no racing mark can raise it between the erase and the drop.
    v->set_pending_mark(false);
    if (pending_.erase(mem::as_span(key))) pending_count_.fetch_sub(1, std::memory_order_relaxed);
}

bool graph_t::apply_delivery_mode(vertex_t* v, delivery_mode_t mode,
                                  std::span<const std::byte> key) {
    // The one failable step, taken before anything changes (#1778): the set entry, drawn only
    // when the key is not in the set yet. It goes in UNDER the same lock as the mode store and
    // the pending erase, so no sweep ever sees the key in both sets (#895). An entry already
    // there names @p v: a key has one node for the graph's life (ADR-0057), and a retiree's
    // entry that `retire` has not dropped yet is kept from here on, because @p v is now
    // UNCONDITIONAL (#1884).
    const std::lock_guard lock(sweep_mutex_);
    mem::bytes_t k(*tables_);
    if (mode == delivery_mode_t::UNCONDITIONAL && !unconditional_.contains(key) &&
        (!mem::assign_bytes(k, key) ||
         unconditional_.try_emplace(std::move(k), v).value == nullptr))
        return false;
    v->set_delivery_mode(mode);
    // Leaving IF_NEWER retires any mark below, and the pending-mark hint (#1712) with it:
    // UNCONDITIONAL is swept via unconditional_ now (no double membership) and EXPLICIT is
    // never ancestor-swept.
    if (mode != delivery_mode_t::IF_NEWER) {
        v->set_pending_mark(false);
        if (pending_.erase(key)) pending_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    if (mode != delivery_mode_t::UNCONDITIONAL) (void)unconditional_.erase(key);
    return true;
}

/**
 * @brief Write a value into an already-resolved vertex (RFC-0008 §D).
 *
 * DOCTRINE — write-vs-retire (#1477, the ruling). `write(vertex_handle_t)` takes NO map lock,
 * while `retire` mutates the vertex under the unique one. Serialising a write against a
 * retire of the SAME vertex is therefore the CALLING PLANE's responsibility, not the graph's,
 * and that is a decision rather than an omission:
 *
 * - A map lock here would sit on the delivery hot path, which ADR-0019/#1431 spent real work
 *   making lock- and branch-free (delivery is byte- and instruction-identical per `link_id_t`
 *   since then). Paying a graph-wide lock per write to fence a control-plane event is the
 *   wrong trade at every point on the NARROW/MID/WIDE spectrum. RULED OUT.
 * - A stateful debug assert ("the caller holds an appropriate guard") would need the graph to
 *   model a guard it does not own, in a plane it cannot name. RULED OUT.
 *
 * What the graph DOES owe is that the race is well-defined rather than UB. Every member a
 * lock-free reader touches on this path is atomic: the LKV slot, `edges_`, `ext_` and its
 * value seam, the subscriber counters, `flags_`, `retire_gen_`, `delivery_mode_` (#895) and,
 * since #1477, `role_`. A write that races a retire may see the retiring occupant's role or
 * the placeholder default, and either answer is a legal outcome of an unordered pair of
 * operations — the caller decides which it wanted by ordering them.
 *
 * The transport plane shows the shape a plane's own fence takes: `ctl_txn_t`'s `ops_m_`
 * (#492 S6 / PR #1473) is held across BOTH phases of the decide-then-act seam, which is what
 * makes its write-vs-retire window unreachable. Any future two-phase scheme un-masks the same
 * window and owes itself the same fence.
 */
result_t<void> graph_t::write(vertex_handle_t v, view::rope_t value, std::string_view caller,
                              const net::link_kind_t* link) {
    return write_impl(v.get(), std::move(value), caller, link);
}

result_t<void> graph_t::write(vertex_handle_t vh, const field_path_t& field, view::rope_t value,
                              std::string_view caller, const net::link_kind_t* link) {
    vertex_t* v = vh.get();
    if (field.empty()) return write_impl(v, std::move(value), caller, link);
    // A field write targets a contiguous control TLV (settings / acl / subscribers);
    // materialize it (single-link: zero copy) before the field surface parses it. A
    // multi-link value's flatten draws from the ADR-0060 value_backend_. The refusal keeps
    // its cause (#917): an exhausted pool surfaces the injected-resource BACKPRESSURE
    // (§3 — transient) rather than letting field_write read an empty head back as a
    // malformed value, while a DEVICE-link value — permanently un-parsable on the CPU —
    // is TYPE_MISMATCH.
    const std::expected<view::view_t, tr::view::flatten_err_t> head =
        value.try_materialize(*value_backend_);
    if (!head) {
        return std::unexpected(head.error() == tr::view::flatten_err_t::NO_MEMORY
                                   ? status_t::BACKPRESSURE
                                   : status_t::TYPE_MISMATCH);
    }
    return field_write(v, field, *head, write_ctx_t{.subject = caller, .link = link});
}

result_t<value_ref_t> graph_t::await(vertex_handle_t vh, std::chrono::nanoseconds timeout,
                                     std::string_view caller) {
    vertex_t* v = vh.get();
    // await is the readiness form of a data READ — same gate, checked up front so a
    // denied caller cannot camp on the condvar.
    if (!acl_allows(v, caller, acl_right_t::READ))
        return std::unexpected(status_t::PERMISSION_DENIED);
    const write_seq_t seq0 = v->current_seq();
    if (!v->wait_for_change(seq0, timeout)) return std::unexpected(status_t::TIMEOUT);
    return await_value(vh);
}

result_t<value_ref_t> graph_t::await_value(vertex_handle_t vh) const {
    vertex_t* v = vh.get();
    // Serve the woken value through the SAME ROLE DISPATCH `read` runs (RFC-0008 Amendment 2).
    // A HANDLER vertex answers `read` from its `on_read` seam and stores nothing, so the old
    // `read_stored()` here answered NOT_FOUND *after* the awaited write landed — await
    // contradicting the read contract it names as its own. The degradation that remains is the
    // read contract's: a handler with no `on_read` still answers NOT_FOUND, exactly as `read`
    // does. ACL was checked up front, so the arm is entered already gated.
    //
    // A RETAINING vertex (STORED_VALUE / STREAM) keeps the published fast path below,
    // untouched — the branch/leaf fork `read` takes is deliberately NOT mirrored here: await
    // observes assigns at ITS OWN vertex (RFC-0008 §A), so a branch vertex's await hands back
    // that vertex's own last-known-value, as it always has, not the composed subtree fold.
    if (v->role() == role_t::HANDLER) return read_handler_gated(v);
    value_ref_t sp = v->read_stored();
    if (!sp) return std::unexpected(status_t::NOT_FOUND);  // never assigned
    return sp;
}

result_t<void> graph_t::arm_await(vertex_handle_t vh, await_waiter_t& w, std::string_view caller) {
    vertex_t* v = vh.get();
    // The same up-front gate `await` runs: a denied caller cannot park a waiter either.
    if (!acl_allows(v, caller, acl_right_t::READ))
        return std::unexpected(status_t::PERMISSION_DENIED);
    v->arm_waiter(w);
    return {};
}

bool graph_t::disarm_await(await_waiter_t& w) noexcept { return vertex_t::disarm_waiter(w); }

result_t<std::size_t> graph_t::history(vertex_handle_t vh, std::span<value_ref_t> out) const {
    vertex_t* v = vh.get();
    if (v->role() != role_t::STREAM) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    if (!acl_allows(v, {}, acl_right_t::READ))  // local-only helper => local (empty) context
        return std::unexpected(status_t::PERMISSION_DENIED);
    return v->history_into(out);  // one refcount share per entry, no allocation
}

result_t<std::size_t> graph_t::drain_unflushed(vertex_handle_t vh, std::vector<value_ref_t>& out,
                                               std::uint64_t* gap_before) {
    vertex_t* v = vh.get();
    if (v->role() != role_t::STREAM) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    // Same gate as `history`, for the same reason: a drain hands back the same ring entries a
    // history read serves, so an ungated drain would be a READ-gate bypass (local-only helper
    // => local (empty) context).
    if (!acl_allows(v, {}, acl_right_t::READ)) return std::unexpected(status_t::PERMISSION_DENIED);
    return v->drain_unflushed(out, gap_before);
}

result_t<std::size_t> graph_t::drain_unflushed(vertex_handle_t vh,
                                               mem::block_array_t<value_ref_t>& out,
                                               std::uint64_t* gap_before) {
    vertex_t* v = vh.get();
    if (v->role() != role_t::STREAM) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    if (!acl_allows(v, {}, acl_right_t::READ)) return std::unexpected(status_t::PERMISSION_DENIED);
    return v->drain_unflushed(out, gap_before);
}

result_t<void> graph_t::mark_flushed(vertex_handle_t vh) {
    vertex_t* v = vh.get();
    if (v->role() != role_t::STREAM) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    // No ACL gate: nothing is disclosed and nothing is delivered — the cursor just moves. The
    // owner-side shape `propagate` and `set_retention` already carry.
    v->mark_flushed();
    return {};
}

result_t<subscription_t> graph_t::admit_subscriber(vertex_t* v, subscriber_t s,
                                                   std::string_view caller,
                                                   std::optional<std::size_t> slot,
                                                   link_id_t link_token) {
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
    // `vertex_slot` answers nothing for an absent (null) target, so that case needs no test.
    if (s.target_key) {
        if (const auto slot = vertex_slot(vertex_handle_t{find_ptr(*s.target_key)}))
            s.binding = target_binding_t{.index = slot->index, .generation = slot->generation};
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
    const view::view_t admitted_tlv = observe ? s.source_view : view::view_t{};
    const view::view_t displaced_tlv =
        (observe && slot) ? v->edge_source(*slot).value_or(view::view_t{}) : view::view_t{};
    // The link hold (#1816): a clone of the admitted edge's cold half, so its delivery link
    // can be named after `s` is moved into the slot, and the cold half a replace displaces.
    const remote_ptr_t admitted = s.remote;
    remote_ptr_t displaced;

    edge_latch_t latch;
    std::size_t idx = 0;
    // The listener count goes up BEFORE the slot exists, not after (#635). fan_out now SKIPS
    // the entire snapshot when this count reads zero, so a bump that TRAILED the append would
    // leave a window where the edge is live and invisible: a publish landing in it delivers to
    // nobody, while the latch taken inside the edge verb below already holds the PREVIOUS
    // value — the new subscriber would miss that write outright. Bumping first inverts the
    // window into the harmless direction (a count with no slot yet ⇒ one snapshot that finds
    // nothing). vertex_t::own_subs_ordered carries the ordering argument.
    // Index this vertex under the link the edge is admitted OVER, before `s` is moved into
    // the slot below and while its cold half is still readable (#1071). The spelling is
    // `vertex_t::evict_link_edges`'s, not a paraphrase of it: `link` first, `caller` when
    // that is empty, because a field_write admission stores the inbound link only as the
    // gate context (#943) and keying on the delivery link alone un-indexes it forever.
    //
    // Deliberately BEFORE the append and not conditional on it succeeding. The index is a
    // superset (see link_index_t): an admission that then fails BACKPRESSURE or OUT_OF_RANGE
    // leaves a stale entry, which costs one no-op eviction, whereas indexing only on success
    // would open a window where the edge is live and unindexed — a departure could then miss
    // it, which is a leaked subscriber edge rather than a wasted comparison.
    //
    // The carried token rides along (#1417) and is CHECKED against that spelling rather than
    // trusted: the two cases where it is the wrong key — a mount-routed target, whose
    // delivery link is the mount's and not the arrival's, and the `caller` fallback — are
    // exactly the ones the name compare inside rejects, so neither can silently un-index the
    // edges #943 and #1071 fixed.
    //
    // A refused entry (#1778: the table source is exhausted) refuses the ADMISSION, for the
    // same reason: an edge that no departure can find is a leak, not a degraded delivery.
    if (s.remote && !link_index_.index_vertex(
                        s.remote->link.empty() ? s.remote->caller.view() : s.remote->link.view(),
                        link_token, v))
        return std::unexpected(status_t::BACKPRESSURE);
    note_subscriber_added(v);  // RFC-0005: descendants' writes now bubble here
    // The hold is taken BEFORE the edge can be seen, for the reason the index entry above is:
    // a departure that evicts the edge the instant it lands gives the hold back, and must
    // find one to give. The failure returns below hand it back themselves (#1816).
    hold_link(delivery_link(admitted), true);
    if (slot) {
        // RFC-0009 §D.1 replace: the SAME door, so the SUBSCRIBE gate above and the latch
        // below apply identically to a replace and to an append (ADR-0049). An index no
        // slot answers to is a malformed address, not a silent no-op — and refusing it is
        // what stops a wire-supplied `:subscribers[65535]` from growing the slot vector.
        const vertex_t::edge_replace_t r = v->replace_edge(*slot, std::move(s), &latch, &displaced);
        // Only filling a CLEARED slot is genuinely a new listener; swapping a live one leaves
        // the count be, and a refused index adds nothing — both give the speculative bump
        // back. The unwind costs a second subtree walk on a control-plane-COLD path, which is
        // the right side to pay on: over-counting only ever buys a snapshot that finds
        // nothing, while under-counting drops a delivery.
        // A displaced SUSPENDED edge was never counted (#1533), so it is an add here too.
        if (r == vertex_t::edge_replace_t::OUT_OF_RANGE) {
            note_subscriber_removed(v);
            hold_link(delivery_link(admitted), false);
            return std::unexpected(status_t::INVALID_PATH);
        }
        if (r == vertex_t::edge_replace_t::REPLACED_ACTIVE) note_subscriber_removed(v);
        // A replace that displaced an edge — delivering or suspended — is two events, in
        // causal order: the old subscription ended and a new one began. Reporting only the
        // ADDED would leave an observer's inventory holding an edge that no longer exists.
        if (r != vertex_t::edge_replace_t::FILLED_EMPTY)
            notify_subscription(sub_event_t::kind_t::REMOVED, v, caller, displaced_tlv, *slot);
        idx = *slot;
    } else {
        idx = v->add_edge(std::move(s), &latch, *tables_);
        // The injected resource could not carry the edge (#477 / #635: publishing the new edge
        // array is the one allocation an append now makes). Nothing was admitted, so give the
        // speculative listener bump back and report it — an admitted-but-unpublished edge
        // would be a subscription that never receives. BACKPRESSURE is the injected-resource
        // status (ADR-0060 §3), the same one the store leg answers on exhaustion.
        if (idx == vertex_t::kNoSlot) {
            note_subscriber_removed(v);
            hold_link(delivery_link(admitted), false);
            return std::unexpected(status_t::BACKPRESSURE);
        }
    }

    // The ADR-0049 durability latch is the one dispatch that does NOT come through `fan_out`,
    // so it brackets itself: a latched replay invokes the same user callback a publish does,
    // and an `unsubscribe()` issued from inside it is just as re-entrant (ADR-0080). Scoped to
    // the `if`, so an admission that latched nothing constructs nothing.
    if (latch.value) {
        const dispatch_scope_t dispatch_scope;
        dispatch_edge(latch.edge, *latch.value);
    }
    // Last, and only on the success path: an edge the caller was told about is an edge that
    // landed. No graph lock is held here — the slot verb released the vertex stripe lock and
    // note_subscriber_added released the map lock — and the durability latch has already been
    // dispatched, so the observer never runs interleaved with this subscription's own replay.
    notify_subscription(sub_event_t::kind_t::ADDED, v, caller, admitted_tlv, idx);
    // The admitted edge's hold was taken before the slot verb, so a replace over the same
    // link never lets the count touch zero between the two (#1816).
    hold_link(delivery_link(displaced), false);
    return subscription_t{v, idx};
}

void graph_t::notify_subscription(sub_event_t::kind_t kind, const vertex_t* v,
                                  std::string_view caller, const view::view_t& sub_tlv,
                                  std::size_t slot) const {
    // The ONE external/local discrimination in the feature: a non-empty caller context is by
    // construction the inbound link NAME the FWD resolver drives the op under, and the local
    // doors (both subscribe() sugars, a field-write from host code) pass the empty context.
    if (!observing_subscriptions(caller)) return;
    // The COHERENT read (#1049) — `observing_subscriptions` above is only the cheap hint
    // that guards the decode work below. Taken before that work so the event is built at
    // all only for a pair this call will actually dispatch to.
    const auto observer = subscription_observer_.get();
    if (observer.fn == nullptr) return;
    // Decode the target from the record ITSELF, not from the parsed slot: subscribe_wire
    // deliberately drops target_key for a remote binding, but the PATH the peer sent is still
    // what an observer wants to see (sub_event_t::target carries the caveat). A slot with no
    // stored TLV, or one whose PATH is malformed, reports an EMPTY target rather than
    // suppressing the event — the mutation happened either way.
    std::span<const std::byte> target;  // borrowed from `sub_tlv` (#1885: no copy)
    if (!sub_tlv.empty()) {
        if (const auto tlv = wire::tlv_node_t::over(sub_tlv);
            tlv && tlv->type() == type_t::SUBSCRIBER) {
            for (const wire::tlv_node_t child : tlv->children()) {
                if (child.type() != type_t::PATH) continue;
                if (const auto k = wire::path_key(child)) target = *k;
                break;
            }
        }
    }
    // A refused producer render reports an EMPTY producer, on the same terms as the target
    // above: the mutation happened either way.
    std::array<std::byte, 256> scratch;  // the render is a stack frame first (#1778)
    mem::bump_source_t frame(scratch, *tables_);
    mem::bytes_t producer(frame);
    (void)try_build_key(v, producer);
    observer.fn(observer.ctx, sub_event_t{.kind = kind,
                                          .producer = wire::key_view_t{mem::as_span(producer)},
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
    // (the packed segment records, RFC-0018 / docs/reference/03), embedded verbatim. Runs under
    // the empty (local) caller context, so a resolver that assigns local callers a
    // subject sees these too (#81, ADR-0026).
    const std::span<const std::byte> key = target.key();
    // The optional SETTINGS child, built first so its size is known when the SUBSCRIBER
    // header is emitted. An all-zero policy emits NOTHING — the absent case of RFC-0022
    // §3.A — so a caller that states no policy produces the exact bytes it did before, and
    // the existing `subscriber-path` conformance vector still describes this encoder.
    //
    // Staged on a stack frame first and the table source past it (#1885), and every refusal
    // is BACKPRESSURE: nothing has been admitted yet.
    std::array<std::byte, 256> scratch;
    mem::bump_source_t frame(scratch, *tables_);
    mem::bytes_t qos(frame);
    if (policy.bits != 0) {
        mem::bytes_t members(frame);
        if (!wire::emit_name(members, "delivery_policy") ||
            !wire::emit_value_le(members, policy.bits, 2) ||
            !wire::emit_tlv(qos, type_t::SETTINGS, opt_t{.pl = true}, mem::as_span(members)))
            return std::unexpected(status_t::BACKPRESSURE);
    }
    mem::bytes_t sub(frame);
    if (!sub.reserve(8 + key.size() + qos.size()) ||
        !wire::emit_header(sub, type_t::SUBSCRIBER, opt_t{.pl = true},
                           4 + key.size() + qos.size()) ||
        !wire::emit_header(sub, type_t::PATH, opt_t{}, key.size()) ||
        !sub.append(key.data(), key.size()) || !sub.append(qos.data(), qos.size()))
        return std::unexpected(status_t::BACKPRESSURE);
    const std::optional<view::view_t> value = view::over_bytes(mem::as_span(sub), *value_backend_);
    if (!value) return std::unexpected(status_t::BACKPRESSURE);
    field_path_t field;
    field.steps.push_back(field_step_t{.name = "subscribers", .indexed = true, .append = true});
    return field_write(v, field, *value, write_ctx_t{});
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

result_t<void> graph_t::set_suspended(const subscription_t& sub, bool suspended) {
    vertex_t* const v = sub.vertex_;
    if (v == nullptr) return std::unexpected(status_t::NOT_FOUND);
    // No SUBSCRIBE gate and no re-admission: the edge's admission decision stands (#1533).
    // The RFC-0005 counts are of DELIVERING edges, so the toggle moves them, under ONE shared
    // map hold with the flip, as evict_link_edges does: a concurrent retire (unique lock) reads
    // own_subs() before zeroing it and must see the pair whole. A resume counts FIRST, the
    // order admit_subscriber uses (seq_cst ahead of the entry going live, against the
    // fan-out's own_subs_ordered skip), and gives the count back if nothing changed; a suspend
    // uncounts after the flip. Over-counting in between only buys a snapshot that skips it.
    const std::shared_lock lock(map_mutex_);
    const std::int32_t up = suspended ? 0 : 1;
    if (up != 0) {
        v->bump_own_subs(+1);
        bump_subtree_listeners(v, +1);
    }
    const vertex_t::edge_suspend_t r = v->set_edge_suspended(sub.slot_, suspended);
    const std::int32_t down = r != vertex_t::edge_suspend_t::CHANGED ? up : 1 - up;
    if (down != 0) {
        v->bump_own_subs(-down);
        bump_subtree_listeners(v, -down);
    }
    if (r == vertex_t::edge_suspend_t::NOT_FOUND) return std::unexpected(status_t::NOT_FOUND);
    if (r == vertex_t::edge_suspend_t::BACKPRESSURE) return std::unexpected(status_t::BACKPRESSURE);
    return {};  // no replay: a resume delivers from the next propagated value
}

result_t<bool> graph_t::is_suspended(const subscription_t& sub) const {
    if (sub.vertex_ == nullptr) return std::unexpected(status_t::NOT_FOUND);
    if (const std::optional<bool> s = sub.vertex_->edge_suspended(sub.slot_)) return *s;
    return std::unexpected(status_t::NOT_FOUND);
}

result_t<void> graph_t::unsubscribe(const subscription_t& sub) { return unsubscribe(sub, nullptr); }

result_t<void> graph_t::unsubscribe(const subscription_t& sub, subscriber_release_fn_t release) {
    if (sub.vertex_ == nullptr) return std::unexpected(status_t::NOT_FOUND);

    // ADR-0080's forbidden op, policed where it is committed rather than described in prose.
    // Under `reclaim_strict` the grace point IS this return, which is only sound if no dispatch
    // is walking a snapshot that names the edge — i.e. if this thread is not inside one. A
    // release build cannot see the violation; that is the trade `reclaim_strict` is FOR, and
    // the reason `reclaim_local` is the default.
    if constexpr (!reclaim_policy_t::kReentrantUnsubscribe)
        assert(!inside_dispatch() &&
               "reclaim_strict forbids unsubscribing from inside a delivery: the running "
               "fan-out still names this edge's {fn, ctx} pair in its snapshot, and this "
               "policy's grace point is THIS return. Bind reclaim_local_t (the default) if "
               "this node re-entrantly unsubscribes.");

    // The in-process counterpart of the wire ":subscribers[N] clear" (graph_fields.cpp):
    // deactivate the slot, then unwind the RFC-0005 listener bookkeeping — the SAME order and
    // the SAME helper the wire path uses, so both doors leave identical counters. clear_edge
    // RECLAIMS the slot's retained state (target key, segment pin, cold remote half) and
    // leaves an inert, index-stable shell that add_edge reuses; an in-flight delivery already
    // snapshotted the edge (ADR-0041 §2) and completes untouched — which is exactly why the
    // `{fn, ctx}` leg, the one part of that snapshot the library does NOT own a copy of, needs
    // the grace point below.
    //
    // Nothing was retired ⇒ nothing is owed. A NOT_FOUND return must not run the hook: the
    // caller may hand the same hook to several handles, and a signal for an edge that was
    // never cleared would free a context another live subscription is still delivering to.
    void* retired_ctx = nullptr;
    if (!clear_subscriber_slot(sub.vertex_, sub.slot_, {}, &retired_ctx))
        return std::unexpected(status_t::NOT_FOUND);

    // The grace point (ADR-0080). Reached HERE — synchronously, before returning — whenever
    // nothing can still be holding the pair: this thread holds no dispatch under the two
    // per-thread policies, and NO participant is mid-dispatch under `reclaim_qsbr`. That is
    // every ordinary unsubscribe, and every unsubscribe at all under `reclaim_strict`. Only the
    // cases that genuinely overlap a live snapshot defer; either way the library decided by
    // itself, and the caller was told rather than asked.
    if (release != nullptr) retire_pair({.ctx = retired_ctx, .release = release});
    return {};
}

std::uint64_t graph_t::deferred_release_drops() noexcept {
    // Two tallies, one reading: the per-thread park's and — under `reclaim_qsbr` — the shared
    // retired table's. The second term folds to a literal 0 under the other two policies, so
    // this stays a single relaxed load in the build that is not paying for a grace period.
    return g_deferred_release_drops.load(std::memory_order_relaxed) + qsbr_drops();
}

std::expected<void, wire::err_t> graph_t::lookup_via_resolver(void* ctx, std::string_view caller,
                                                              mem::bytes_t& out) {
    // The adapter reads a SETTLED hook slot, as the gate does: a republish in flight is waited
    // out by re-reading the slot's generation (bounded, no clock, no sleep), and the resolver it
    // settles on is the one that runs. A slot that does not settle, or settles cleared because
    // the hooks were being removed, refuses the caller.
    const auto resolver =
        static_cast<const graph_t*>(ctx)->subject_resolver_.get_settled(kHookSettleReads);
    if (!resolver || resolver->fn == nullptr) return std::unexpected(wire::err_t::ACCESS_DENIED);
    const std::expected<subject_token_t, wire::err_t> token = resolver->fn(resolver->ctx, caller);
    if (!token) return std::unexpected(token.error());
    if (!out.append(token->data(), token->size()))
        return std::unexpected(wire::err_t::ACCESS_DENIED);
    return {};
}

void graph_t::set_hooks(const graph_hooks_t& hooks) noexcept {
    // The returning-form resolver is kept for `lookup_via_resolver`, which stands in for it in
    // the gate's one slot; a caller-storage `subject_lookup` takes precedence (#1781).
    subject_resolver_.set(hooks.subject_resolver.fn, hooks.subject_resolver.ctx);
    if (hooks.subject_lookup.fn != nullptr) {
        subject_lookup_.set(hooks.subject_lookup.fn, hooks.subject_lookup.ctx);
    } else if (hooks.subject_resolver.fn != nullptr) {
        subject_lookup_.set(&graph_t::lookup_via_resolver, this);
    } else {
        subject_lookup_.set(nullptr, nullptr);
    }
    subscription_observer_.set(hooks.subscription_observer.fn, hooks.subscription_observer.ctx);
    remote_sink_.set(hooks.remote_delivery.fn, hooks.remote_delivery.ctx);
    wire_target_.set(hooks.wire_target.fn, hooks.wire_target.ctx);
    stats_sampler_.set(hooks.stats_sampler.fn, hooks.stats_sampler.ctx);
    link_hold_.set(hooks.link_hold.fn, hooks.link_hold.ctx);
}

graph_hooks_t graph_t::hooks() const noexcept {
    const auto sr = subject_resolver_.get();
    const auto so = subscription_observer_.get();
    const auto rd = remote_sink_.get();
    const auto wt = wire_target_.get();
    const auto ss = stats_sampler_.get();
    const auto lh = link_hold_.get();
    auto sl = subject_lookup_.get();
    if (sl.fn == &graph_t::lookup_via_resolver) sl = {};  // the adapter, not a caller's hook
    return graph_hooks_t{.subject_resolver = {sr.fn, sr.ctx},
                         .subscription_observer = {so.fn, so.ctx},
                         .remote_delivery = {rd.fn, rd.ctx},
                         .wire_target = {wt.fn, wt.ctx},
                         .stats_sampler = {ss.fn, ss.ctx},
                         .link_hold = {lh.fn, lh.ctx},
                         .subject_lookup = {sl.fn, sl.ctx}};
}

void graph_t::hold_link(std::string_view link, bool held, std::size_t n) const {
    // One call per edge, never a batched delta: the receiver's reference count is the whole
    // of the hold (#1816), so it is told exactly what an edge-by-edge walk would tell it.
    if (link.empty()) return;
    const auto hook = link_hold_.get();
    if (hook.fn == nullptr) return;
    for (std::size_t i = 0; i < n; ++i) hook.fn(hook.ctx, link, held);
}

bool graph_t::sample_stats(std::string_view seam_class, std::string_view seam_name,
                           stats_block_t* out) const noexcept {
    // One coherent {fn, ctx} read, then dispatch from the SNAPSHOT — never from the members
    // (`sink_slot.hpp`'s contract). An unset slot is the no-transport-plane node, and its
    // answer is the same "not published here" a router that does not know the seam gives.
    const auto sink = stats_sampler_.get();
    if (sink.fn == nullptr) return false;
    return sink.fn(sink.ctx, seam_class, seam_name, out);
}

result_t<void> graph_t::subscribe_wire(vertex_handle_t vh, view::view_t source_view,
                                       view::view_t return_route, std::string_view link,
                                       view::view_t reverse_route, std::string_view caller,
                                       link_id_t link_token) {
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
    // Parse the owned SUBSCRIBER copy ONCE (ADR-0049) through the door parse every subscriber
    // door shares (#869): decode, type check, parse, and the zero-copy retain the slot keeps.
    // delivery_compact comes from this parse (the resolver's parallel subscriber_compact() is
    // retired).
    subscriber_t s;
    if (const auto parsed = parse_wire_subscriber(source_view, s, *tables_); !parsed)
        return std::unexpected(parsed.error());
    // RFC-0021 §4.A/§4.B.1: the `PATH` child, when it routes through a MOUNT, is the delivery
    // target spelled in THIS (the producer's) frame — the same frame a `FWD`'s `dst` is
    // resolved in, because a delivery IS a write (RFC-0004 §D). Binding it here is what makes
    // third-party origination expressible (#491): the edge takes the MOUNT's link and the
    // residual below it, so `evict_link_edges(<the writer's session>)` no longer matches and
    // the subscription outlives the orchestrator that wrote it (§4.C — the whole point).
    //
    // Only the mount-routed arm is bound. A `PATH` that matches NO mount keeps today's
    // arrival-session binding: §4.B.2's purely-local arm is §7's unruled open question 3, and
    // in-tree wire senders (the TypeScript client's `subscribe`, most of `acl_test`) still
    // spell the target in the CONSUMER's own frame, which §B.3's rejection would refuse
    // outright. What is NOT tolerated is the mount-involving failure — §F: a target that
    // names a mount it cannot deliver through must be an error, never a silent degrade to the
    // arrival session, because that silence is exactly what made #491 look like it worked.
    mem::bytes_t mount_route_tlv(*tables_);  // outlives `route_view` below
    // The link this edge DELIVERS over — the arrival link until a mount-routed target moves
    // it. `caller` is left alone: it is the ACL subject context the SUBSCRIBE gate and
    // every delivery run under (#81, ADR-0026, RFC-0021 §E — the gate is the WRITER's, even
    // when the data goes somewhere else), so the two must not be the same variable. Since
    // #375 Part 2 they are not even the same STRING: at a FLAT listener every peer shares
    // one delivery link and each has its own subject.
    std::string_view delivery_link = link;  // copied into the cold half below
    // The parse never holds an empty key (`try_make_target_key` answers null for one), so an
    // engaged key is a non-empty PATH.
    if (s.target_key) {
        if (const auto slot = wire_target_.get(); slot.fn != nullptr) {
            const wire_target_split_t split = slot.fn(slot.ctx, *s.target_key);
            if (split.unroutable) return std::unexpected(status_t::INVALID_PATH);
            if (!split.link.empty()) {
                // The residual as ONE owned PATH TLV — the single copy of the route every
                // later delivery clones by refcount (ADR-0041 §2), the shape the accumulated
                // `src` arrives in. An empty residual never reaches here: the descent reports
                // a mount named exactly as `unroutable`.
                if (!wire::emit_tlv(mount_route_tlv, type_t::PATH, opt_t{}, split.residual))
                    return std::unexpected(status_t::BACKPRESSURE);
                std::optional<view::view_t> route_view =
                    view::over_bytes(mem::as_span(mount_route_tlv), *value_backend_);
                if (!route_view) return std::unexpected(status_t::BACKPRESSURE);
                return_route = *std::move(route_view);
                delivery_link = split.link;
                // The carried token moves with the key it names (#1437). The arrival's token
                // spells the arrival link, and the index is about to be keyed on the MOUNT —
                // so from here it is the wrong token, correctly rejected by the name compare
                // at the index door and correctly replaced by the resolver's own, which the
                // transport plane holds for the mount it just resolved. A resolver that gave
                // none hands back a default, which is this door before this line existed: the
                // scan. Assigned unconditionally for that reason — keeping the arrival's
                // token here could only ever buy a comparison that must fail.
                link_token = split.token;
                // The reverse bound route is the ARRIVAL link's (RFC-0024 §7.1): it spells the
                // way back to the writer, which is not where this edge delivers. Drop it —
                // the mount route is canonical-only.
                reverse_route = view::view_t{};
            }
        }
    }
    // A PATH child that named no mount stays what it always was here: the consumer at ITS
    // origin, never a local re-dispatch target — remote delivery rides the return route over
    // the link (RFC-0004 §D). This door is the one that CLEARS the key the two field-write
    // arms REQUIRE, so it stays out of the shared helper.
    s.target_key.reset();
    // The fan-in gate context this edge's deliveries run under (#81) — the WRITER's subject
    // since #375 Part 2, and the link's own name for every caller that supplied none, which
    // is byte for byte what this door stored before the two claims were separated (ADR-0082).
    // It is also the context the SUBSCRIBE gate runs under (#81/ADR-0026), not the delivery
    // link's.
    const std::string_view gate_ctx = caller.empty() ? std::string_view(link) : caller;
    // A wire subscriber always carries the cold half; it and both names draw from the table
    // source (#1885), and a refusal there is BACKPRESSURE before anything was admitted.
    subscriber_remote_t* const r = s.ensure_remote(*tables_);
    if (r == nullptr || !r->caller.assign(gate_ctx) || !r->link.assign(delivery_link))
        return std::unexpected(status_t::BACKPRESSURE);
    r->return_route = std::move(return_route);
    // The completed reverse bound route (RFC-0024 §7.1 amendment 1) — empty for every
    // canonical-only subscribe, and stored WITHOUT validation beyond what the resolver
    // already did: element 0 is this node's own mint, re-validated on every delivery.
    r->reverse_route = std::move(reverse_route);
    // A wire subscribe carries no host handle back — discard the slot (unsubscribe is the
    // wire :subscribers[N] clear, not this door's return).
    if (const auto r2 = admit_subscriber(v, std::move(s), gate_ctx, std::nullopt, link_token); !r2)
        return std::unexpected(r2.error());
    return {};
}

result_t<view::view_t> graph_t::read_children(vertex_t* v) const {
    // The synthesized listing wins (ADR-0044): a transport/connection vertex serves
    // its live bus peers here — a snapshot of traffic, never stored graph structure.
    // Load once — a concurrent retire may swap the seam out between check and call.
    if (const value_handlers_t& h = v->handlers(); h.on_children) return h.on_children();
    // Generic member enumeration (reference 05 §SPEC read-members): the DIRECT
    // children of v in the vertex map — keys of the form <v.key><one packed record>.
    // Each member is a minimal POINT{NAME} descriptor; order is unspecified.
    // Staged on the table source (#1885); a refusal anywhere is BACKPRESSURE.
    mem::bytes_t members(*tables_);
    bool staged = true;
    {
        const std::shared_lock lock(map_mutex_);
        // A direct child contributes ONE `POINT{NAME <segment>}` member (ADR-0057 — one
        // child-list walk, no whole-map prefix scan). Placeholders (unregistered
        // intermediate levels) are not members, matching the flat map where they did
        // not exist — and neither is an enumeration-hidden child
        // (@ref vertex_t::enumerable_member): the RFC-0014 §3 creator endpoint is registered
        // and addressable but is not one of its module's connections. One predicate, shared
        // with the folded door below, so the two listings cannot disagree about membership
        // (`folded_children_test` gates them byte-for-byte).
        //
        // The child's key RECORD used to be the POINT body verbatim, because a vertex-map
        // key record and a `NAME` TLV were the same four-byte-headed bytes. RFC-0018 packs
        // the key record to `[u8 len][bytes]` and `:children[]` is a POINT composite, NOT a
        // PATH — the RFC removes `NAME` from `PATH` bodies only and leaves the type and its
        // decoder standing everywhere else — so the member is RE-FRAMED here rather than
        // borrowed: same wire shape as before this RFC, one header write per child.
        v->for_each_child([&members, &staged](const vertex_t& c) {
            if (!staged || !c.enumerable_member()) return;
            const std::span<const std::byte> seg = child_segment(c);
            const std::size_t body = kNameHeaderBytes + seg.size();
            staged = wire::emit_header(members, type_t::POINT,
                                       opt_t{.pl = true, .ll = body > 0xFFFFu}, body) &&
                     wire::emit_name(members, seg);
        });
    }
    mem::bytes_t out(*tables_);
    if (!staged || !wire::emit_tlv(out, type_t::POINT, opt_t{.pl = true}, mem::as_span(members)))
        return std::unexpected(status_t::BACKPRESSURE);
    // `out` is non-empty by construction; `nullopt` is exactly an alloc failure
    // → BACKPRESSURE (the audited alloc/copy/over locus).
    const auto res = view::over_bytes(mem::as_span(out), *value_backend_);
    if (!res) return std::unexpected(status_t::BACKPRESSURE);
    return *res;
}

result_t<value_ref_t> graph_t::read_children_materialized(vertex_handle_t vh) const {
    const result_t<view::view_t> mv = read_children(vh.get());
    if (!mv) return std::unexpected(mv.error());
    return composed_or_backpressure(view::rope_t{*mv});
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
 * @brief What one folded node contributes ON ITS OWN, before its sub-branches: its POINT body
 *        bytes and its link count.
 *
 * The node's `NAME` header and segment text when it is @p named (null at a composed-read root
 * or a `:children` listing's outer POINT, whose identity is the addressed vertex — RFC-0016
 * §A), plus @p lkv's TLV verbatim when it carries one. The links are the owned header segment,
 * the borrowed name text and the stored TLV's links, which is exactly what
 * @ref append_folded_node appends — so a caller that reserves from this sum never reallocates.
 */
struct folded_own_t {
    std::size_t len = 0;   /**< @brief The node's own POINT body bytes. */
    std::size_t links = 1; /**< @brief Its own links; the owned header segment is always one. */
};

/** @brief The @ref folded_own_t of a node named by @p named carrying @p lkv (either may be null).
 */
[[nodiscard]] folded_own_t folded_own(const vertex_t* named, const value_t* lkv) noexcept {
    folded_own_t o;
    if (named != nullptr) {
        o.len += kNameHeaderBytes + child_segment(*named).size();
        o.links += 1;
    }
    if (lkv != nullptr) {
        o.len += lkv->total_length();
        o.links += lkv->link_count();
    }
    return o;
}

/**
 * @brief Append one folded node's own links to @p out: its POINT header for a @p body_len body
 *        — IMMEDIATELY FOLLOWED, when it is @p named, by the `NAME` header of its segment text,
 *        in ONE exactly-sized OWNED segment — then that segment text BORROWED in place, then
 *        @p lkv's links refcount-cloned. False ⇒ a seam refused (the caller answers
 *        `BACKPRESSURE` by value; nothing here throws).
 *
 * The ONE member emitter of the three folds — `:children` (@ref
 * graph_t::read_children_folded), the composed read (@ref graph_t::read_subtree_folded) and the
 * FOLD sweep (`propagate_folded_impl`) — so the `emit_tlv` auto-widen boundary of the header
 * framing cannot drift between them (#831): byte-identical to `wire::emit_header(out,
 * type_t::POINT, {.pl = true, .ll}, body_len)` plus `wire::emit_name` into a
 * `std::vector<std::byte>`, emitted by cursor with no throwing vector transient on the reply
 * path (the op_resolve_walk assemble pattern). The node's sub-branches are the caller's.
 *
 * Two headers in one segment, deliberately. Before RFC-0018 the folds emitted a POINT header
 * and then BORROWED the node's key record, because that record already WAS a `NAME` TLV. A
 * packed key record is `[u8 len][bytes]`, so the `NAME` framing has to be written — and writing
 * it into the same segment as the POINT header keeps every fold at exactly the link count it
 * had before, which is what their link-table reservations are sized against and what keeps a
 * wide `:children` listing off the transports' iovec spill. The named vertex is pinned and
 * insert-only and its `name_` is immutable once linked, so the borrowed text outlives the rope.
 *
 * @p backend is the graph's ADR-0060 `value_backend_` at every call site (#831). These are
 * PAYLOAD framing bytes — the length field wraps the stored TLV and the name records below it —
 * so they are that seam's byte class, NOT the ADR-0074 `egress` seam, which is documented and
 * sized against ROUTE bytes. Every count is PEER-influenced (a peer picks the composed root, and
 * thus how many subtree nodes fold; or whose ":children" to list, and thus how many members
 * frame), so an ADR-0067-class node with every backend at one slab would otherwise still leak
 * this framing to `malloc`. The segments escape inside the returned reply rope and are freed on
 * whichever thread drops the last reference — exactly the cross-thread self-routed reclaim
 * ADR-0060 §2 already requires of this backend. The default is `&mem::heap_backend()`, so a
 * shipped shape allocates byte-identically.
 */
[[nodiscard]] bool append_folded_node(view::rope_t& out, mem::mem_backend_t& backend,
                                      std::size_t body_len, const vertex_t* named,
                                      const value_t* lkv) {
    const bool ll = body_len > 0xFFFFu;  // mirror emit_tlv's auto-widen exactly
    const std::size_t len_bytes = ll ? 4u : 2u;
    const std::span<const std::byte> seg =
        named != nullptr ? child_segment(*named) : std::span<const std::byte>{};
    view::segment_ptr_t hseg = view::segment_alloc(
        backend, folded_hdr_len(body_len) + (named != nullptr ? kNameHeaderBytes : 0u));
    if (!hseg) return false;  // the seam refused — a null segment_ptr_t IS the refusal
    std::byte* p = hseg->bytes.data();
    *p++ = static_cast<std::byte>(std::to_underlying(type_t::POINT));
    *p++ = static_cast<std::byte>(opt_t{.pl = true, .ll = ll}.encode());
    detail::store_le(std::span<std::byte>(p, len_bytes), static_cast<std::uint32_t>(body_len),
                     len_bytes);
    p += len_bytes;
    view::segment_ptr_t nseg;  // the borrowed segment text — named nodes only
    if (named != nullptr) {
        *p++ = static_cast<std::byte>(std::to_underlying(type_t::NAME));
        *p++ = std::byte{0};
        detail::store_le(std::span<std::byte>(p, 2), static_cast<std::uint32_t>(seg.size()), 2);
        nseg = view::borrow_const(seg);
        if (!nseg) return false;
    }
    out.append(view::view_t::over(std::move(hseg)));            // owned POINT (+ NAME) headers
    if (nseg) out.append(view::view_t::over(std::move(nseg)));  // borrowed name (zero copy)
    if (lkv != nullptr) {  // the stored TLV verbatim — links cloned, refcount bump
        for (const view::view_t& l : lkv->links()) out.append(l);
    }
    return true;
}

}  // namespace

result_t<value_ref_t> graph_t::read_children_folded(vertex_handle_t vh) const {
    vertex_t* v = vh.get();
    // Synthesized listing (ADR-0044): a live bus-peer snapshot, already one contiguous
    // view — a fold has nothing to gather, so it crosses as a single-link value,
    // byte-identical to the read_children path.
    if (const value_handlers_t& h = v->handlers(); h.on_children) {
        const result_t<view::view_t> sv = h.on_children();
        if (!sv) return std::unexpected(sv.error());
        return composed_or_backpressure(view::rope_t{*sv});
    }
    // The folded projection of read_children: instead of concatenating every member into
    // one buffer and copying the whole listing (twice — into `out`, then into a segment),
    // gather each POINT{NAME} member as TWO scatter-gather links — the emitted
    // POINT-plus-NAME header, then the child's own segment TEXT borrowed IN PLACE (zero
    // copy). Under RFC-0018 the borrowed run is the key record minus its length byte, and
    // the `NAME` framing rides the owned header segment, so the member is still two links.
    // The child vertex is pinned and insert-only and its `name_` is immutable once linked,
    // so the borrowed bytes outlive this rope. flatten() is byte-identical to
    // read_children: same headers (opt.ll auto-widened at the same 0xFFFF boundary)
    // followed by the same name bytes, in the same child order.
    //
    // Every header here — one per registered child, plus the outer one — is framed by
    // append_folded_node from the ADR-0060 value_backend_, NOT from view::over_bytes' global
    // heap (#831). The count is PEER-influenced (a peer picks which vertex's ":children" to
    // READ, and thus how many members frame), and this is the site the wire ":children" field
    // READ routes to — see append_folded_node for the full seam argument, which the
    // composed-root fold below shares verbatim.
    mem::mem_backend_t& hdr_backend = *value_backend_;
    view::rope_t members;
    std::size_t members_len = 0;
    bool oom = false;
    {
        const std::shared_lock lock(map_mutex_);
        v->for_each_child([&members, &members_len, &oom, &hdr_backend](const vertex_t& c) {
            // THE membership predicate, shared verbatim with `read_children` — placeholders
            // and enumeration-hidden children (the RFC-0014 §3 creator endpoint) are not
            // members on either door.
            if (oom || !c.enumerable_member()) return;
            const std::size_t body = folded_own(&c, nullptr).len;
            oom = !append_folded_node(members, hdr_backend, body, &c, nullptr);
            members_len += folded_hdr_len(body) + body;
        });
    }
    view::rope_t out;
    if (oom || !append_folded_node(out, hdr_backend, members_len, nullptr, nullptr))
        return std::unexpected(status_t::BACKPRESSURE);
    // The member count is already in hand, so take the join as ONE sized growth instead of
    // the geometric push_back ladder (a wide listing is 2 links per child). Best effort:
    // on soft-fail the concat below still produces the right chain, it just pays the
    // ordinary growth path. `concat` no longer reserves for us — that guard belongs to the
    // caller that knows the count, not to every 1-link delivery clone (#1022).
    //
    // #981 residual: a listing wide enough to spill grows the rope's `std::vector<view_t>`
    // chain, and under `-fno-exceptions` that growth is probe-then-commit — a task switch
    // between the probe's free and the `reserve` abort()s the node (#850). `view_t` is
    // refcounted, so the chain cannot take `block_array_t`'s memcpy relocation (#873).
    static_cast<void>(out.try_reserve(members.link_count()));
    out.concat(members);  // empty members (no children) => header-only rope, len 0
    return composed_or_backpressure(std::move(out));
}

result_t<value_ref_t> graph_t::read_subtree_folded(vertex_handle_t vh,
                                                   std::string_view caller) const {
    vertex_t* root = vh.get();
    if (!acl_allows(root, caller, acl_right_t::READ))
        return std::unexpected(status_t::PERMISSION_DENIED);

    /**
     * @brief One included composed-read node, collected in PRE-ORDER (so the array order IS the
     *        wire order: a node's POINT header precedes its NAME/value/children bytes).
     */
    struct snap_node_t {
        const vertex_t* named = nullptr; /**< @brief The pinned vertex whose NAME leads the node
                                              (name bytes immutable); null at the root. */
        value_ref_t lkv;                 /**< @brief Its landed LKV — loaded ONCE, atomically. */
        std::size_t parent = 0;          /**< @brief Parent's index in the array (unused at the
                                              root, index 0). */
        std::size_t body_len = 0;        /**< @brief POINT body length, completed bottom-up. */
    };
    // The POINT header width and the member emission itself both come from the file-local
    // folded_hdr_len / folded_own / append_folded_node, shared with read_children_folded and
    // the FOLD sweep — one home, so the emit_tlv auto-widen boundary cannot drift between the
    // three folds (#831).
    mem::mem_backend_t& hdr_backend = *value_backend_;

    // Pass 1 — collect, under ONE shared map lock: an ITERATIVE pre-order stack machine
    // (house style, parse_branch_node). The stack is `ctl_`-backed, so its bound is the
    // allocator and it needs no synthetic cap. This comment used to attribute that to graph
    // depth being "kMaxSegments-bounded structurally"; it is not — kMaxSegments is enforced
    // only in path_t::parse, and a hook-created vertex never passes through it. Per node:
    // the ACL gate (a denied
    // vertex PRUNES its whole subtree, siblings unaffected), the placeholder skip
    // (unregistered levels are not members, exactly as read_children), one read_stored()
    // load, and the node's OWN body contribution (its NAME record below the root; its
    // stored TLV's total length verbatim). Descendant HANDLER on_read seams are NOT
    // invoked — the composed read serves landed LKVs only.
    // The node table is a core array over the table source (#873 phase 1, #1778), which
    // relocates `snap_node_t`'s reference by move. The node COUNT is peer-chosen, like the
    // collect stack below it.
    mem::block_array_t<snap_node_t> nodes(*tables_);
    // The reply's exact final link count, summed as each node is collected, so the reply rope
    // reserves its heap chain ONCE (nothrow) and every append below is guaranteed
    // non-reallocating. A composed-root reply is thousands of links on a fragmented heap — the
    // un-reserved spill is exactly what aborted the node.
    std::size_t total_links = 0;
    {
        /** @brief One unvisited subtree root: the vertex and its parent's array index. */
        struct work_t {
            vertex_t* v = nullptr;  /**< @brief The subtree root to collect. */
            std::size_t parent = 0; /**< @brief Its parent's index in `nodes`. */
        };
        const std::shared_lock lock(map_mutex_);
        // MIGRATED to the ADR-0065 failable seam (#981): `work_t` is a pointer plus an index
        // — trivially copyable and trivially destructible — so the collect stack takes
        // `block_array_t` over the injected `ctl_` rather than a `std::vector` +
        // `detail::try_push_back`. On the `-fno-exceptions` profile that helper can only
        // PROBE the global heap and then run the throwing `reserve`, and a context switch in
        // between lets another task take the just-freed block: the `reserve` then aborts()
        // the node (#850). One refusable `try_alloc` per growth has no such window, and it
        // draws from the node's own resource instead of the global heap. The node COUNT is
        // peer-chosen (it picks which composed root to READ), so this is exactly the growth
        // a peer can drive to exhaustion.
        mem::block_array_t<work_t> stack(*tables_);
        // `nodes` draws from the same store. A lambda cannot return the error, so the child
        // push latches `oom` and the loop propagates it after each visit.
        bool oom = false;
        if (!stack.push_back(work_t{.v = root, .parent = 0}))
            return std::unexpected(status_t::BACKPRESSURE);
        while (!stack.empty()) {
            const work_t w = stack.back();
            stack.pop_back();
            const std::size_t idx = nodes.size();
            snap_node_t n;
            // The root is tagged once, as the first node collected: its identity is the
            // addressed vertex, so its node carries no NAME (RFC-0016 §A).
            n.named = idx == 0 ? nullptr : w.v;
            n.lkv = w.v->read_stored();  // ONE atomic load per node
            n.parent = w.parent;
            const folded_own_t own = folded_own(n.named, n.lkv.get());
            n.body_len = own.len;
            total_links += own.links;
            // One refusable growth, no probe window (#850, #981 closed here by #1778).
            if (!nodes.push_back(std::move(n))) return std::unexpected(status_t::BACKPRESSURE);
            // Push the children, then reverse the just-pushed run: the LIFO pop then
            // visits siblings in for_each_child's sorted order, keeping the array's
            // pre-order equal to the emitted wire order.
            const auto first = static_cast<std::ptrdiff_t>(stack.size());
            w.v->for_each_child([this, caller, idx, &stack, &oom](vertex_t& c) {
                if (oom) return;  // a prior sibling push failed — stop growing
                // THE membership predicate, the same one both `:children[]` doors take
                // (@ref vertex_t::enumerable_member): placeholders are not members, and
                // neither is an enumeration-hidden child. The composed read is an
                // ENUMERATION-SHAPED surface — it answers "what is under here?" — so hiding
                // that held on one such surface and not the other was a disagreement about
                // membership, not a difference in kind (RFC-0016 Amendment 1 / RFC-0014 §3).
                // Direct addressing is untouched: the hidden vertex still resolves, which is
                // exactly what makes the §6 `conn:schema` probe the discovery route.
                if (!c.enumerable_member()) return;
                if (!acl_allows(&c, caller, acl_right_t::READ)) return;  // PRUNE the subtree
                if (!stack.push_back(work_t{.v = &c, .parent = idx})) oom = true;
            });
            if (oom) return std::unexpected(status_t::BACKPRESSURE);
            std::reverse(stack.data() + first, stack.data() + stack.size());
        }
    }

    // Pass 2 — body lengths bottom-up: reverse pre-order visits every child before its
    // parent, so each node's completed wire size (header + body) folds into the parent.
    for (std::size_t i = nodes.size(); i-- > 1;)
        nodes[nodes[i].parent].body_len += folded_hdr_len(nodes[i].body_len) + nodes[i].body_len;

    // #981 residual: the reservation itself is the `std::vector<view_t>` growth this
    // paragraph exists to make ONE growth — but under `-fno-exceptions` that one growth is
    // still probe-then-commit and abort()s the node if a racer takes the freed probe block
    // (#850). Refcounted `view_t` links cannot ride `block_array_t`'s memcpy relocation, so
    // the seam migration waits on a move-relocating failable array (#873).
    view::rope_t out;
    if (!out.try_reserve(total_links)) return std::unexpected(status_t::BACKPRESSURE);

    // Pass 3 — emit, in array (= pre-order = wire) order, through the one member emitter the
    // three folds share (append_folded_node, which carries the seam and lifetime arguments).
    // Per node: an OWNED header link (the POINT header, plus the NAME header below the root —
    // the root's identity is the addressed vertex, RFC-0016 §A; its own stored TLV leads the
    // root body), the BORROWED name text below the root, then the stored TLV's links
    // refcount-CLONED (no byte copy). Zero flatten anywhere; a view allocation failure is the
    // audited BACKPRESSURE pattern.
    for (const snap_node_t& n : nodes) {
        if (!append_folded_node(out, hdr_backend, n.body_len, n.named, n.lkv.get()))
            return std::unexpected(status_t::BACKPRESSURE);
    }
    return composed_or_backpressure(std::move(out));
}

namespace {

/**
 * @brief The leading TLV's type and options byte of @p r, or `nullopt` when @p r is shorter
 *        than a header or holds a link this host cannot read.
 *
 * The FOLD emission has to know whether a stored value is a plain trailer-less `VALUE` before
 * it may frame that value as an RFC-0005 §B node, and it must know it WITHOUT decoding: the
 * body is the embedder's schema and the graph never parses it (RFC-0025 claim 5). Two bytes
 * answer the question, so two bytes are what this reads. A device-resident link (`all_host()`
 * false) is not dereferenceable here at all, which is a refusal rather than a guess.
 */
[[nodiscard]] std::optional<std::pair<type_t, opt_t>> peek_tlv_head(const value_t& r) noexcept {
    if (r.total_length() < 2 || !r.all_host()) return std::nullopt;
    std::array<std::byte, 2> head{};
    std::size_t got = 0;
    r.walk([&head, &got](std::span<const std::byte> s) {
        for (const std::byte b : s) {
            if (got < head.size()) head[got++] = b;
        }
    });
    if (got < head.size()) return std::nullopt;
    return std::pair{static_cast<type_t>(std::to_integer<std::uint8_t>(head[0])),
                     opt_t::decode(std::to_integer<std::uint8_t>(head[1]))};
}

/**
 * @brief One node of the folded branch-write frame: the vertex it names, the stored `VALUE`
 *        it contributes (null on a skeleton node), and the sub-frame built for it bottom-up.
 *
 * A SKELETON node is an interior vertex the sweep did not select but a selected descendant
 * sits under. RFC-0005 §B admits it verbatim — "a node without a `VALUE` stores nothing (its
 * vertex's stored value, if any, is untouched)" — so the tree stays connected without the fold
 * inventing a value or resurrecting a stale one.
 */
struct fold_node_t {
    vertex_t* vx = nullptr;   /**< @brief The named vertex; its segment text is
                                          pinned and immutable, so it is borrowable. */
    value_ref_t lkv;          /**< @brief Its contributed VALUE, or null (skeleton). */
    bool selected = false;    /**< @brief True iff the sweep selected this vertex. */
    std::size_t body_len = 0; /**< @brief This node's POINT body length. */
    std::size_t kids_len = 0; /**< @brief Bytes its sub-branches contribute. */
    view::rope_t kids;        /**< @brief Those sub-branches' frames, in key order. */
    view::rope_t frame;       /**< @brief This node's WHOLE POINT TLV — the §B notify
                                    slice for an interior node. */
};

}  // namespace

bool graph_t::select_sweep(vertex_t* v, mem::bytes_t& lo, key_list_t& out) {
    if (!try_build_key(v, lo)) return false;  // refused: marks retained — the next sweep retries
    const std::span<const std::byte> los = mem::as_span(lo);
    // The same two sets, the same prefix range and the same strict-descendant test
    // propagate_impl walks (RFC-0008 §B). The ONE difference is that neither loop erases: a
    // fold that turns out to be unencodable must leave the sweep exactly as it found it, and
    // the marks it does deliver are retired afterwards by clear_pending — the #1185 compare,
    // which is strictly safer than an unconditional erase here would be.
    const std::lock_guard lock(sweep_mutex_);
    for (const key_set_t* set : {&pending_, &unconditional_}) {
        const auto [first, last] = subtree_run(*set, los);
        for (key_set_t::pos_t i = first; i != last; i = set->next(i)) {
            const std::span<const std::byte> k = mem::as_span(set->at(i).key);
            if (k.size() != los.size() && !out.push(k)) return false;
        }
    }
    return true;
}

result_t<void> graph_t::propagate_folded_impl(vertex_t* v) {
    // Every table below is per-call scratch: a stack frame first, spilling to the table
    // source (#1778), so a small fold takes nothing from its class locks.
    std::array<std::byte, 2048> scratch;
    mem::bump_source_t frame(scratch, *tables_);
    mem::bytes_t lo_key(frame);
    key_list_t keys(frame);
    if (!select_sweep(v, lo_key, keys)) return std::unexpected(status_t::BACKPRESSURE);
    const std::span<const std::byte> lo = mem::as_span(lo_key);

    // The node table, keyed by canonical vertex key. A child key is its parent key plus one
    // packed NAME record, so the parent is a strict PREFIX and therefore sorts FIRST: ascending
    // map order is pre-order and reverse order visits every child before its parent. That is
    // the whole reason this is an ordered map and not a hash — the two passes below need
    // exactly those two orders and nothing else. A sorted table from the table source
    // (#1778): entries MOVE on insert, so a node is addressed by index or re-found, never
    // held across an insert. Its keys are VIEWS, never copies: every key is `lo`, a selected
    // key, or a parent of one, and a parent is a prefix of its child's bytes, so all of them
    // point into `lo_key` and `keys`, which outlive the table.
    mem::sorted_map_t<std::span<const std::byte>, fold_node_t, mem::bytes_less_t> tree(frame);
    // Admit one node, resolving the vertex and validating what it may contribute. `selected`
    // false admits a skeleton: the vertex is named so the tree stays connected, and no value
    // rides it. Returns the error a §B decomposer would raise on the frame this would build.
    //
    // The root is `lo`, a strict prefix of every other key, so it is index 0 for the life of the
    // table. It is tagged once, here, as `v` itself; every other node resolves by key.
    const auto [root, root_fresh] = tree.try_emplace(lo);
    if (root == nullptr) return std::unexpected(status_t::BACKPRESSURE);
    root->vx = v;
    const auto admit = [this, &tree](std::span<const std::byte> key,
                                     bool selected) -> result_t<fold_node_t*> {
        const auto [found, fresh] = tree.try_emplace(key);
        if (found == nullptr) return std::unexpected(status_t::BACKPRESSURE);
        fold_node_t& n = *found;
        if (fresh) n.vx = find_ptr(key);
        if (n.vx == nullptr) return &n;  // vanished mid-sweep — propagate_impl skips it too
        if (!selected || n.selected) return &n;
        n.selected = true;
        // A STREAM's flush is its bounded since-last-flush LIST (RFC-0008 §E), and an
        // RFC-0005 §B node admits AT MOST ONE `VALUE`. A list therefore has no §B-legal seat
        // on a folded node, and the BATCH record that would seat it (RFC-0025 §4.1.2 clause 6)
        // is not a `VALUE` either, so §B strictness would reject it as "any other child type".
        // The fold refuses rather than conflating the list down to a snapshot — silently
        // dropping buffered entries would break the stream no-conflate contract §4.1.2
        // clause 2 exists to protect. Selecting PER_VERTEX is the working answer.
        //
        // This refusal is PERMANENT, and it is not what stands between a fold and a batch
        // (RFC-0025 §4.1.3, Amendment 4, clause 2). What it refuses is a PER-SAMPLE ring: N
        // separately stored entries, no single foldable value. Batching FOR a fold means
        // composing onto the vertex the sweep visits — the app ropes its samples into ONE
        // value (`wire::compose_batch`, batch.hpp) and swaps that in, after which the sweep
        // below sees an ordinary single `VALUE` and carries it with no change here at all.
        // A vertex that must keep a HISTORY of batches stays a STREAM, one ring entry per
        // batch, and delivers per-vertex; that is the app's role choice, not a workaround.
        if (n.vx->role() == role_t::STREAM) return std::unexpected(status_t::TYPE_MISMATCH);
        n.lkv = n.vx->read_stored();
        if (!n.lkv) return &n;  // never assigned: deliver_current sends nothing, so does this
        const std::optional<std::pair<type_t, opt_t>> head = peek_tlv_head(*n.lkv);
        if (!head) return std::unexpected(status_t::TYPE_MISMATCH);
        // §B strictness, enforced at the EMITTER so a folded frame is one no terminus can
        // refuse: the node's own value must be a `VALUE`, and it must be trailer-less. The
        // trailer half is REJECTION, never a silent strip — a stored slice is a subview and a
        // trailer cannot be sliced off without a copy (ADR-0041 §4). That this rule does not
        // simply outlaw folding a timed stream is RFC-0025 Amendment 1's doing: sample time
        // moved out of the trailer into payload `TIME` (`0x0C`) children INSIDE the value,
        // where §B never looks.
        const auto& [t, o] = *head;
        if (t != type_t::VALUE || !trailer_less(o)) return std::unexpected(status_t::TYPE_MISMATCH);
        return &n;
    };

    if (const result_t<fold_node_t*> r = admit(lo, true); !r) return std::unexpected(r.error());
    for (std::size_t ki = 0; ki < keys.size(); ++ki) {
        // The selected key itself, then every level between it and the root as a skeleton:
        // each must exist as a node or the tree is not a tree. Walking parents off the key
        // itself costs O(depth) per selected vertex; walking the whole subtree structurally
        // (the composed read's shape) would cost the SUBTREE, which is the wrong order for a
        // sweep whose selection is usually sparse. A selected key is a STRICT descendant, so
        // the walk always admits it.
        bool selected = true;
        for (key_view_t p{keys[ki]}; p.bytes().size() > lo.size(); p = p.parent()) {
            if (const result_t<fold_node_t*> r = admit(p.bytes(), selected); !r)
                return std::unexpected(r.error());
            selected = false;
        }
    }

    // Frame it, deepest first. Every node — the root INCLUDED — emits `POINT{ NAME, [VALUE],
    // POINT… }`: a branch-write root carries its leading NAME echoing the target's leaf
    // segment (RFC-0005 §B), which is the one root asymmetry RFC-0016 §A names against a
    // composed-READ root, and the asymmetry Amendment 3 clause 5 resolves in §B's favour — so
    // every node here is `named`, where the composed read's root is not. The member emission
    // is `append_folded_node`'s, shared verbatim with both folded reads, so the emit_tlv
    // auto-widen boundary cannot drift between the three (#831); its bytes come from the
    // ADR-0060 value_backend_ for the reason stated there.
    mem::mem_backend_t& hdr_backend = *value_backend_;
    for (std::size_t ti = tree.size(); ti-- > 0;) {
        fold_node_t& n = tree.at(ti).value;
        if (n.vx == nullptr) continue;
        const folded_own_t own = folded_own(n.vx, n.lkv.get());
        n.body_len = own.len + n.kids_len;
        if (!n.frame.try_reserve(own.links + n.kids.link_count()) ||
            !append_folded_node(n.frame, hdr_backend, n.body_len, n.vx, n.lkv.get()))
            return std::unexpected(status_t::BACKPRESSURE);
        n.frame.concat(n.kids);  // the sub-branches, in key order
        if (ti == 0) break;      // the root folds into nobody
        // admit() inserted every level, so the parent is always there.
        fold_node_t& parent = *tree.find(key_view_t{tree.at(ti).key}.parent().bytes());
        parent.kids.concat(n.frame);
        parent.kids_len += folded_hdr_len(n.body_len) + n.body_len;
    }

    // Deliver, and NOT one delivery per selected vertex — this is where the fold pays. Each
    // covered subscription point is notified ONCE with the smallest subview covering every
    // value at-or-below it (RFC-0005 §B): the VALUE for a leaf landing site, the node's whole
    // POINT subtree for an interior node, and the WHOLE frame at the root and (via §A
    // bubbling) above it. A remote subtree subscriber at or above the root therefore sees ONE
    // `FWD{WRITE}` where RFC-0008 §D's default emits one per selected vertex, and its terminus
    // slices that frame with the code it already has — no receive-path branch was added here,
    // and none is wanted (Amendment 3 clause 5, "terminus side: ZERO change").
    //
    // Descendant fan-outs are NOT bubbled: the root's own bubble already carries the whole
    // frame to every ancestor subscriber, exactly as the eager branch write's notify half does.
    for (std::size_t ti = tree.size(); ti-- > 1;) {  // index 0 is the root, delivered below
        const fold_node_t& n = tree.at(ti).value;
        if (n.vx == nullptr) continue;
        if (n.kids_len == 0) {
            if (n.lkv) fan_out(n.vx, *n.lkv);  // leaf landing site: its VALUE slice
        } else {  // interior node: its whole POINT subtree — a frame no vertex stored
            deliver_unstored(n.vx, n.frame, &graph_t::fan_out, n.vx->own_subs());
        }
    }
    const fold_node_t& root_node = tree.begin()->value;
    deliver_unstored(v, root_node.frame, &graph_t::fan_out, v->own_subs());
    if (v->listeners_above() > 0)
        deliver_unstored(v, root_node.frame, &graph_t::bubble_up, v->listeners_above());

    // Retire the marks this sweep just discharged — the SAME door the eager branch write uses
    // for its landing sites, and for the same reason: it erases only while the value this call
    // delivered is still the vertex's CURRENT LKV (#1185), so an assign that raced the
    // validation above keeps its mark and its delivery instead of losing both to the peek/drain
    // window an unconditional erase here would have opened.
    // `selected` is only ever set on a node whose vertex resolved.
    for (const auto& e : tree) {
        if (e.value.selected) clear_pending(e.value.vx, e.value.lkv.get());
    }
    return {};
}

result_t<value_ref_t> graph_t::read(vertex_handle_t vh, const field_path_t& field,
                                    std::string_view caller) const {
    // One read type (RFC-0028 D11): every arm answers a `value_ref_t`. The empty field IS the
    // value read, so it hands back the published reference itself; every other arm composes a
    // value nothing published and wraps it once, at the bottom.
    if (field.empty()) return read(vh, caller);
    return read_field_composed(vh, field, caller);
}

result_t<std::vector<view::view_t>> graph_t::read_subscribers(vertex_handle_t vh,
                                                              std::string_view caller) const {
    vertex_t* v = vh.get();
    if (!acl_allows(v, caller, acl_right_t::READ))  // control-surface read, like ":schema"
        return std::unexpected(status_t::PERMISSION_DENIED);
    return v->edge_sources();  // each a clone (refcount bump, no byte copy)
}

result_t<std::size_t> graph_t::read_subscribers(vertex_handle_t vh,
                                                mem::block_array_t<view::view_t>& out,
                                                std::string_view caller) const {
    vertex_t* v = vh.get();
    if (!acl_allows(v, caller, acl_right_t::READ))  // control-surface read, like ":schema"
        return std::unexpected(status_t::PERMISSION_DENIED);
    if (!v->edge_sources(out)) return std::unexpected(status_t::BACKPRESSURE);
    return out.size();
}

result_t<value_ref_t> graph_t::read(const path_t& path) const {
    vertex_t* v = find_ptr(path.key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    // A plain value read SHARES the published value; a `:field` read composes one — the field
    // overload makes that split itself (RFC-0028 D11).
    return read(vertex_handle_t{v}, path.field());
}

result_t<void> graph_t::write(const path_t& path, view::rope_t value) {
    // A miss is NOT_FOUND unless the parent's creation hook creates the target (RFC-0030 §7).
    // A `:field` write never creates (§7.1: there is no vertex whose control surface it could
    // address), so it never reaches the hook or the CREATE gate. The hit stays one lookup.
    vertex_t* v = find_ptr(path.key());
    if (v == nullptr) {
        if (!path.field().empty()) return std::unexpected(status_t::NOT_FOUND);
        const result_t<vertex_t*> made =
            find_or_create_ptr(path.key(), {}, [&]() -> const view::rope_t& { return value; });
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
