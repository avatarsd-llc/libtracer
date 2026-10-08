/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief The `:`-field surface (#1711): `:subscribers`, `:acl`, `:children`, `:settings`,
 *        `:schema`, `:identity` and `:stats`, each defined by one row of a constant table.
 *
 * Both field doors — `graph_t::read_field_composed` and `graph_t::field_write` — look the head
 * step's NAME up in that table and call the row's handler; neither carries a per-field
 * branch. The shape classifiers below (`whole_field`, `field_selector`, `app_field_sel`,
 * `stats_seam`) are the addressing grammar the handlers share (#869), and the readers further
 * down build each field's bytes. The colon fields are control-plane-cold: nothing here is on
 * the value read/write path, which never names a field.
 */

#include "graph_fields.hpp"

#include <array>
#include <cstring>
#include <iterator>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/config_reader.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/key_view.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/view.hpp"

namespace tr::graph {

using wire::key_view_t;
using wire::opt_t;
using wire::tlv_node_t;
using wire::type_t;

namespace {

/** @brief True iff @p s is a plain NAME step (no `[N]` / `[]` / `[*]` selector). */
[[nodiscard]] bool plain_step(const field_step_t& s) noexcept {
    return !s.indexed && !s.append && !s.wildcard;
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
        if (!plain_step(s)) return {};
        if (i > 2) key += '.';
        key += s.name;
    }
    return key;
}

/**
 * @brief True iff @p field is the "served whole" shape — exactly one plain NAME step.
 *
 * The single shape rule behind every field the protocol serves as ONE record with no member
 * and no slot addressing. Four handlers consult it (#869): the `:acl` write, and the `:acl`,
 * `:identity` and `:schema` reads. Before it the read door spelled it
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
 * that leaves the device — see the `:settings[0].app.<name>` note on
 * `field_surface_t::write_settings`.
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

/**
 * @brief One seam of the node-scoped `:stats` census (RFC-0010 Amendment 1, #1503 step 5).
 *
 * The spelling is `<any-vertex>:stats.<seam-class>.<name>` — node-scoped in the `:identity`
 * mould (RFC-0011 §C.1): the field takes no vertex, every vertex answers identically, and
 * the content describes the NODE. The GRAPH classes are the census this node reaches from
 * its own `graph_t` (Amendment 1 §D.4): the two injected `block_source_t` seams and the
 * graph's own delivery-drop door. The `router` / `labels` / `link` classes are the net
 * plane's (Amendment 2), and L4 still learns nothing about that plane to serve them: the
 * router registers a sampler UP through `graph_hooks_t::stats_sampler`, and @ref NET is
 * "a spelling only that sampler can recognise or refuse".
 */
enum class stats_seam_t : std::uint8_t {
    NONE,           /**< @brief Not a `:stats` seam spelling — SCHEMA_NOT_FOUND. */
    MEM_CONTROL,    /**< @brief `:stats.mem.control` — @ref graph_t::control_source. */
    MEM_RING,       /**< @brief `:stats.mem.ring` — @ref graph_t::default_ring_source. */
    MEM_VALUES,     /**< @brief `:stats.mem.values` — @ref graph_t::value_source (Am. 3). */
    MEM_TABLES,     /**< @brief `:stats.mem.tables` — @ref graph_t::table_source (Am. 3). */
    MEM_NET,        /**< @brief `:stats.mem.net` — @ref graph_t::net_source (Am. 3). */
    GRAPH_DELIVERY, /**< @brief `:stats.graph.delivery` — @ref graph_t::delivery_drops. */
    NET,            /**< @brief A net-plane class — the installed sampler decides (Am. 2). */
};

/** @brief One `:stats.mem.<name>` seam: its name and the `graph_t` accessor it samples. */
struct mem_seam_t {
    std::string_view name;                                    /**< @brief The seam NAME step. */
    mem::block_source_t& (graph_t::*source)() const noexcept; /**< @brief What it samples. */
};

/** @brief The mem seams, in @ref stats_seam_t order from `MEM_CONTROL`: one row each, so the
 *         classifier and the census read one table rather than a branch per seam. */
constexpr mem_seam_t kMemSeams[] = {
    {"control", &graph_t::control_source}, {"ring", &graph_t::default_ring_source},
    {"values", &graph_t::value_source},    {"tables", &graph_t::table_source},
    {"net", &graph_t::net_source},
};
static_assert(std::to_underlying(stats_seam_t::MEM_CONTROL) + std::size(kMemSeams) ==
                  std::to_underlying(stats_seam_t::GRAPH_DELIVERY),
              "the mem seams are one contiguous run of stats_seam_t, in table order");

/**
 * @brief Classify a `:stats…` field path per @ref stats_seam_t.
 *
 * Exactly three plain NAME steps: the head, the seam CLASS and the seam NAME. A census
 * block is served WHOLE — one READ is one seam is one TLV, which is the only way the
 * `core/STYLE.md` §Introspection snapshot-coherence clause can hold — so no member and no
 * indexed spelling names anything, and neither does a bare `:stats` or a bare
 * `:stats.<class>`: aggregating seams is exactly what ADR-0079 forbids.
 *
 * Out of line and `cold` for the same measured reason as @ref read_stats below, and reached
 * only when the head already spelled `stats` — a field read of any other name pays one
 * string compare and never this call.
 */
[[nodiscard, gnu::noinline, gnu::cold]] stats_seam_t stats_seam(
    const field_path_t& field) noexcept {
    if (field.steps.size() != 3) return stats_seam_t::NONE;
    for (const field_step_t& s : field.steps)
        if (!plain_step(s)) return stats_seam_t::NONE;
    const std::string_view cls = field.steps[1].name;
    const std::string_view name = field.steps[2].name;
    if (cls == "mem") {
        for (std::size_t i = 0; i < std::size(kMemSeams); ++i)
            if (name == kMemSeams[i].name)
                return static_cast<stats_seam_t>(std::to_underlying(stats_seam_t::MEM_CONTROL) + i);
    } else if (cls == "graph") {
        if (name == "delivery") return stats_seam_t::GRAPH_DELIVERY;
    } else if (cls == "router" || cls == "labels" || cls == "link") {
        // RESERVED to the net plane (Amendment 2 §D.4). The CLASS is spec text and is
        // recognised here unconditionally; whether the NAME within it is served is the
        // sampler's answer, and with no sampler installed every one of them is
        // SCHEMA_NOT_FOUND — the same answer these spellings gave before the amendment.
        return stats_seam_t::NET;
    }
    return stats_seam_t::NONE;
}

/** @brief Emit one census member — `NAME <noun> VALUE u64` (fixed 8-byte little-endian,
 *         the reference/05 integer convention), the shape every `:stats` block repeats.
 *  @retval false The staging source refused (#1885). */
[[nodiscard]] bool emit_counter(mem::bytes_t& out, std::string_view noun, std::uint64_t value) {
    return wire::emit_name(out, noun) && wire::emit_value_le(out, value, 8);
}

/** @brief The cold introspection encoders' scratch (#1778, #1781): a stack frame first,
 *         spilling to the graph's table source, so a short record allocates nothing. */
constexpr std::size_t kScratchBytes = 256;

/**
 * @brief Serve one `:stats` seam as ONE `SETTINGS` TLV (RFC-0010 Amendment 1 §D.3).
 *
 * Sampled in a single call, in the vocabulary `core/STYLE.md` §Introspection fixes, in the
 * declaration order of the underlying POD. The member set is a function of the SEAM CLASS,
 * so `mem.control` and `mem.ring` are byte-shaped alike and a reader parses one block per
 * class. Nothing here bumps `write_seq_` — counters are not writes, which is precisely why
 * the field is readable and never awaitable.
 *
 * A free function rather than a `graph_t` member on purpose: every accessor it reads is
 * already public, so the census needs no new public symbol and no friendship.
 *
 * `noinline` + `cold`, and the honest note is that this is PROPHYLACTIC, not a diagnosed
 * fix. The first CI run of this change read a reproduced -16% on `compact-forward` — a
 * point this code is not on — and the local interleaved A/B could not reproduce it in
 * EITHER direction (x1.01 with these attributes, x1.02 without, against the same baseline
 * on the same host). The attributes stay because they are right on their own terms: a
 * census is polled at a supervisor's cadence, so its block-building code belongs in
 * `.text.unlikely` behind one call rather than partitioned through this TU's hot text —
 * exactly the shape #1503 step 3 had to adopt when seven `fetch_add`s inlined at their drop
 * sites cost `bench_forward_demux` +7.3% until one `[[gnu::noinline, gnu::cold]]
 * count_drop` took them out of line.
 */
[[nodiscard, gnu::noinline, gnu::cold]] result_t<view::view_t> read_stats(const graph_t& g,
                                                                          const field_path_t& field,
                                                                          stats_seam_t seam) {
    // Every seam fills the same fixed-capacity carrier the net sampler fills, so ONE loop below
    // shapes every seam's bytes (Amendment 2).
    stats_block_t sampled;
    switch (seam) {
        case stats_seam_t::MEM_VALUES:
        case stats_seam_t::MEM_TABLES:
        case stats_seam_t::MEM_NET:
        case stats_seam_t::MEM_CONTROL:
        case stats_seam_t::MEM_RING: {
            const mem_seam_t& row =
                kMemSeams[std::to_underlying(seam) - std::to_underlying(stats_seam_t::MEM_CONTROL)];
            const mem::source_stats_t s = (g.*row.source)().stats();
            sampled.add("capacity", s.capacity);
            sampled.add("in_use", s.in_use);
            sampled.add("peak", s.peak);
            sampled.add("refused", s.refused);
            sampled.add("largest_refused", s.largest_refused);
            break;
        }
        case stats_seam_t::GRAPH_DELIVERY: {
            const graph_t::delivery_drops_t d = g.delivery_drops();
            sampled.add("no_target", d.no_target);
            sampled.add("denied", d.denied);
            sampled.add("out_of_memory", d.out_of_memory);
            sampled.add("fan_out_truncated", d.fan_out_truncated);
            break;
        }
        case stats_seam_t::NET:
            // Amendment 2: the block comes from the net plane's registered sampler, and the
            // sampling half never touches an allocator.
            if (!g.sample_stats(field.steps[1].name, field.steps[2].name, &sampled))
                return std::unexpected(status_t::SCHEMA_NOT_FOUND);
            break;
        case stats_seam_t::NONE:
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    }
    // Staged on this call's stack frame, spilling to the table source (#1885): a census
    // block is small, so sampling a source does not normally draw from the one it samples.
    std::array<std::byte, 512> scratch;
    mem::bump_source_t frame(scratch, g.table_source());
    mem::bytes_t members(frame);
    bool staged = true;
    for (std::size_t i = 0; staged && i < sampled.count; ++i)
        staged = emit_counter(members, sampled.members[i].noun, sampled.members[i].value);
    mem::bytes_t block(frame);
    if (!staged ||
        !wire::emit_tlv(block, type_t::SETTINGS, opt_t{.pl = true}, mem::as_span(members)))
        return std::unexpected(status_t::BACKPRESSURE);
    // `block` is non-empty by construction; `nullopt` is exactly an alloc failure
    // → BACKPRESSURE (the audited alloc/copy/over locus).
    const auto out = view::over_bytes(mem::as_span(block), g.value_backend());
    if (!out) return std::unexpected(status_t::BACKPRESSURE);
    return *out;
}

// The flat protocol-knob name table is GONE (RFC-0022 §3.B): `settings_t` is deleted, so
// the vertex `:settings` core namespace has no writable member left. All seven historical
// names — `reliability`, `priority`, `durability`, `deadline_ns`, `queue_max_bytes`,
// `history_keep_last`, `store_ref_min_bytes` — answer `SCHEMA_NOT_FOUND`, which is the
// honest answer an unsupported field already gives. The two survivors did not move to
// another name; they stopped being remotely writable at all and became owner-side
// declarations (the ring depth, which RFC-0028 D4 later folded into `graph_t::set_retention`,
// and the pin ratio that D3 replaced with `graph_t::set_share_threshold_bytes`).

/** @brief Emit the RFC-0010 §A.4 app-container members into @p out: each declared,
 *         non-`wo` field HOLDING a value, in table order — `NAME <name>` then its TLV bytes
 *         (`wo` has no read surface; unset fields are omitted). A field @p live answers
 *         (`handlers_t::on_app_field_read`, #1878) lists the owner's bytes instead of the
 *         stored ones, so the container never disagrees with the named read.
 *  @return False when @p live answered a refused allocation, or @p out's source refused
 *          (BACKPRESSURE). */
[[nodiscard]] bool emit_app_container(mem::bytes_t& out, const std::vector<app_field_t>& table,
                                      const app_field_read_hook_t& live) {
    for (const app_field_t& f : table) {
        if (f.access == app_access_t::WO) continue;
        const std::optional<value_ref_t> owner = live ? live(f.name) : std::nullopt;
        if (owner && !*owner) return false;
        if (!owner && f.value.empty()) continue;
        if (!wire::emit_name(out, f.name)) return false;
        if (!owner) {
            if (!out.append(f.value.data(), f.value.size())) return false;
        } else {
            for (const view::view_t& l : (**owner).links())
                if (!out.append(l.bytes().data(), l.bytes().size())) return false;
        }
    }
    return true;
}

}  // namespace

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
 *
 * @retval false The target key could not be held, or @p src refused the cold half (#1885).
 */
[[nodiscard]] bool parse_subscriber_tlv(const tlv_node_t& sub, subscriber_t& s,
                                        mem::block_source_t& src) {
    for (const tlv_node_t child : sub.children()) {
        if (child.type() == type_t::PATH && !s.target_key) {
            // An illegally-spelled target leaves target_key unset, which falls back to the
            // full-route delivery path exactly as an older parser would (#681). A legal one
            // that cannot be held refuses the admission as BACKPRESSURE (#1885): it was
            // admitted without its target and then refused as a TYPE_MISMATCH.
            const auto k = wire::path_key(child);
            if (k && !(s.target_key = try_make_target_key(src, *k)) && !k->empty()) return false;
        } else if (child.type() == type_t::SETTINGS) {
            const wire::config_reader_t qos(&child);
            if (qos.flag("delivery_compact").value_or(false)) {
                subscriber_remote_t* const r = s.ensure_remote(src);  // only when opted in
                if (r == nullptr) return false;
                r->delivery_compact = true;
            }
            if (const std::optional<std::uint16_t> word = qos.u16("delivery_policy"))
                s.policy.bits = *word;
        }
    }
    return true;
}

}  // namespace

/**
 * @brief The wire→`subscriber_t` admission parse, hand-rolled at three doors before #869:
 *        decode the record, type-check it, parse it ONCE (ADR-0049), and retain it.
 *
 * The three doors are `graph_t::subscribe_wire` and the `:subscribers[]` append and
 * `:subscribers[N]` replace of the field surface (graph_fields.cpp). What is deliberately NOT
 * in here is everything the doors disagree about: the `[N]` arm's `acl_allows(WRITE)` gate and
 * its empty-STATUS eviction sentinel (both of which run before this), the field-write
 * door's `require a PATH child` rule, and `subscribe_wire`'s inverse — it CLEARS `target_key`,
 * because a PATH child there names the consumer at ITS origin and delivery rides the return route.
 *
 * The record is taken by reference and retained by a refcount clone. Taking it BY VALUE so
 * the wire door could move it in measured **+1980 bytes** of graph.cpp `.text` at -O3 — a
 * `view_t` move plus its destructor and landing pad at each inline site — for one refcount
 * saved on a control-plane path.
 *
 * @param record The record as written: one TLV, validated here.
 * @param s      Filled on success, its `source_view` the zero-copy retain a later
 *               `:subscribers[]` read ropes into the REPLY (ADR-0035); untouched on a type
 *               refusal. On a source refusal it may hold part of the record, and the door
 *               drops it: the slot was never admitted.
 * @param src    The graph's table source, which the cold half draws from.
 * @return TYPE_MISMATCH iff @p record is not one valid SUBSCRIBER TLV — the doors' one shared
 *         refusal of the record — and BACKPRESSURE when the key or the cold half could not be
 *         held (#1885).
 */
[[nodiscard]] result_t<void> parse_wire_subscriber(const view::view_t& record, subscriber_t& s,
                                                   mem::block_source_t& src) {
    const auto tlv = wire::tlv_node_t::over(record);
    if (!tlv || tlv->type() != type_t::SUBSCRIBER) return std::unexpected(status_t::TYPE_MISMATCH);
    if (!parse_subscriber_tlv(*tlv, s, src)) return std::unexpected(status_t::BACKPRESSURE);
    s.source_view = record;
    return {};
}

/**
 * @brief The `:`-field surface: one handler pair per colon field, and the ONE table both doors
 *        dispatch on (#1711).
 *
 * A private nested type of `graph_t`, so the handlers reach the gates and stores the doors
 * always used without one `graph_t` declaration per field, and without a friend any class of
 * the same name could claim. Each READ handler owns its field's whole
 * order — what resolves above the gate, which right the gate checks, what serves the value —
 * because those orders are protocol text and differ per field (RFC-0010 §A erratum, #435;
 * the #869 divergences `field_shape_matrix` pins). What no handler owns is NAME validity: a
 * head the table does not carry is SCHEMA_NOT_FOUND on both doors, above every gate.
 */
struct graph_t::field_surface_t {
    /** @brief A field's read: the composed value, or why there is none. */
    using read_fn_t = result_t<value_ref_t> (*)(const graph_t& g, vertex_t* v,
                                                const field_path_t& field, std::string_view caller);
    /** @brief A field's write; a row without one has no write surface (SCHEMA_NOT_FOUND). */
    using write_fn_t = result_t<void> (*)(graph_t& g, vertex_t* v, const field_path_t& field,
                                          const view::view_t& value, const write_ctx_t& ctx);

    /**
     * @brief A contiguous control TLV as the one read type (RFC-0028 D11): a single-link
     *        value, or the view's own failure. Every field but the folded `:children`
     *        listing crosses back this way.
     */
    static result_t<value_ref_t> single_link(const result_t<view::view_t>& fv) noexcept {
        if (!fv) return std::unexpected(fv.error());
        return composed_or_backpressure(view::rope_t{*fv});
    }

    /** @brief One colon field: its NAME, its read, and its write (or none). */
    struct row_t {
        std::string_view name; /**< @brief The head step's NAME — published spec text. */
        read_fn_t read;        /**< @brief The read door's handler. */
        write_fn_t write;      /**< @brief The write door's handler; nullptr ⇒ read-only. */
    };

    /**
     * @brief `:subscribers[N]` read — the stored SUBSCRIBER view (clone).
     *
     * Bare `:subscribers` and any `:subscribers.<tail>` spelling name nothing on either door —
     * the record is addressed whole, per slot — and the write door answers both ungated (its
     * TAIL/WHOLE arms), so the read resolves the same two spellings at the same pre-gate
     * narrowness. Selector shapes (`[N]`, `[]`, `[*]`) resolve below the READ gate. Every shape
     * but `[N]` is then SCHEMA_NOT_FOUND — including `[*]`, which the WRITE door answers
     * INVALID_PATH (the pinned #869 divergence; see @ref write_subscribers), and including
     * `[]`, whose whole-array read the wire door serves through `graph_t::read_subscribers`
     * before ever reaching here.
     */
    static result_t<value_ref_t> read_subscribers(const graph_t& g, vertex_t* v,
                                                  const field_path_t& field,
                                                  std::string_view caller) {
        const field_sel_t sel = field_selector(field);
        if (sel == field_sel_t::WHOLE || sel == field_sel_t::TAIL)
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (!g.acl_allows(v, caller, acl_right_t::READ))
            return std::unexpected(status_t::PERMISSION_DENIED);
        if (sel != field_sel_t::SLOT) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (std::optional<view::view_t> sv = v->edge_source(field.steps[0].index))
            return composed_or_backpressure(view::rope_t{*sv});  // clone, no byte copy
        return std::unexpected(status_t::NOT_FOUND);
    }

    /**
     * @brief `:subscribers[]` / `[N]` write — append, clear or replace an edge (RFC-0009 §D.1).
     *
     * `:subscribers` is addressed WHOLE — `[]` appends an edge, `[N]` clears one, and there is
     * nothing INSIDE a slot to address: a SUBSCRIBER record is stored and served as one TLV,
     * never member-wise. So any further step names nothing.
     *
     * This gate is not cosmetic (#580). The `[N]` arm below is an unconditional `clear_edge`,
     * so before it, `:subscribers[0].liveness.last_seen_ns` — or any typo'd tail at all —
     * DESTROYED the slot and answered `kind=RESULT`, byte-identical to a legitimate `[0]`
     * clear. A caller aiming at a member wrote nothing, unbound a live subscriber, and was told
     * it succeeded. The read half already required `steps.size() == 1` (see
     * @ref read_subscribers), so this makes the two halves agree rather than inventing a rule.
     *
     * Resolved BEFORE the ACL gate, exactly as `:acl`: a shape that names nothing is not an
     * access question. Note that leaves the `[]` / `[N]` shapes untouched, so `plain_step` —
     * the `:acl` guard's second half — must NOT be used here: an append is `append == true`
     * and a clear is `indexed == true`, and both are legal.
     *
     * The selector decode itself is `field_selector` (#869) — the SAME classification the read
     * door switches on, so the two halves can no longer drift on WHICH shape they are looking
     * at. They still disagree on the ANSWER per shape, deliberately: that asymmetry (a `[]`
     * write subscribes, a `[]` read enumerates; a `[*]` write is INVALID_PATH, a `[*]` read is
     * SCHEMA_NOT_FOUND) is what each arm below states.
     */
    static result_t<void> write_subscribers(graph_t& g, vertex_t* v, const field_path_t& field,
                                            const view::view_t& value, const write_ctx_t& ctx) {
        // `[]` appends and `[N]` clears or replaces (RFC-0009 §D.1); the two share ONE parse
        // and admission tail below, and `slot` is the only thing that tells them apart.
        const field_sel_t sel = field_selector(field);
        std::optional<std::size_t> slot;
        if (sel == field_sel_t::SLOT) {
            // `[N]` is gated on WRITE before its payload is looked at. The append is gated by
            // the SUBSCRIBE gate inside `admit_subscriber` alone, as it always was.
            if (!g.acl_allows(v, ctx.subject, acl_right_t::WRITE))
                return std::unexpected(status_t::PERMISSION_DENIED);
            slot = field.steps[0].index;
            // §D.1 is payload-DISCRIMINATING. Before it, every indexed write cleared the slot
            // payload-blind, so a peer writing a SUBSCRIBER to slot N — plainly meaning to
            // replace that edge — silently destroyed it and was told RESULT. The eviction
            // sentinel is an empty STATUS — no payload bytes and no children (`09 00 00 00`,
            // the smallest valid TLV); on an append it is no SUBSCRIBER, so the parse below
            // refuses it, as it refuses a record that does not decode at all.
            if (const auto tlv = wire::tlv_node_t::over(value);
                tlv && tlv->type() == type_t::STATUS && tlv->body().empty()) {
                // Clear-and-report through the ONE slot-clear door `unsubscribe` also runs: the
                // observer's view is taken before the clear, the RFC-0005 counters unwind, and
                // only a slot that WAS active is reported as a removal.
                (void)g.clear_subscriber_slot(v, *slot, ctx.subject);
                return {};
            }
        } else if (sel != field_sel_t::APPEND) {
            // A tail and the bare name name nothing: SCHEMA_NOT_FOUND, ungated.
            //
            // `[*]` sets indexed=true AND wildcard=true and never assigns `index` — so it
            // would arrive at the `[N]` arm with `index == 0`, OUTSIDE the validity window
            // path.hpp declares ("valid when indexed && !append && !wildcard"). Testing
            // `indexed` alone therefore cleared SLOT 0 and answered RESULT: silent data loss
            // reported as success (#579). `field_selector` is what keeps that from ever being
            // retried: WILDCARD is decided before APPEND and SLOT, so neither arm can see a
            // wildcard step at all. The WRITE grammar has no wildcard axis, so a write bearing
            // one is a malformed address, not a missing schema entry — `:subscribers` plainly
            // exists: INVALID_PATH.
            //
            // DIVERGENCE (#869), pinned NOT fixed: the READ door answers `:subscribers[*]`
            // SCHEMA_NOT_FOUND, and it answers it BELOW the READ gate, so a denied caller is
            // told PERMISSION_DENIED. Which of the two codes is right is a wire question —
            // docs/reference/03-addressing.md §`[*]` treated as `[0]` calls `[*]` "legal only
            // where the field chain's first step is subscribers" and reads it as "every slot",
            // which points at an enumerating READ rather than at either error — so unifying it
            // needs an RFC, not this refactor. `field_wildcard_divergence` pins both answers.
            return std::unexpected(sel == field_sel_t::WILDCARD ? status_t::INVALID_PATH
                                                                : status_t::SCHEMA_NOT_FOUND);
        }
        subscriber_t s;
        // The shared door parse (ADR-0049, #869): decode, type check, parse and retain — the
        // same steps `subscribe_wire` runs. On `[N]` it sits AFTER the WRITE gate and AFTER the
        // sentinel discrimination above, which are that arm's alone. A record with no PATH
        // child names no local target, and this door has nowhere else to deliver.
        if (const auto parsed = parse_wire_subscriber(value, s, g.table_source()); !parsed)
            return std::unexpected(parsed.error());
        if (!s.target_key) return std::unexpected(status_t::TYPE_MISMATCH);
        // The fan-in gate context for this edge's deliveries (#81); the empty (local)
        // context needs no cold half. It is ALSO what makes the edge reclaimable: this
        // door leaves `subscriber_remote_t::link` empty (there is no return route to
        // deliver over — the edge fans out to a LOCAL target), so
        // `vertex_t::evict_link_edges` falls back to this context to find the link the
        // edge was admitted over (#943). Do NOT "fix" that by assigning `link` here:
        // `graph_t::dispatch_edge` gates its remote leg on `edge_view_t::has_remote_leg`
        // — i.e. on a non-empty cold-half link — so a
        // non-empty link would add a phantom `FWD{WRITE}` per publish carrying an EMPTY
        // return route. That is the invariant `subscribe_wire` now enforces at its own
        // door (#1055, INVALID_PATH on an empty route) — this door holds up the other half
        // of it, by binding no link when it binds no route.
        //
        // Reachability, as of this commit: the ONE in-tree wire door
        // (`op_resolve_walk.hpp`'s WRITE case) routes a remote `:subscribers[]` append
        // bearing a SUBSCRIBER to `subscribe_wire` instead (its `remote_sub` test), and
        // any other payload fails the TYPE_MISMATCH above — so a non-empty caller on an
        // append is reached only through the public `graph_t::write(v, field, value, caller)`,
        // which an embedder may drive with an inbound link name. `[N]` has no such diversion
        // and IS reached from the wire.
        if (!ctx.subject.empty()) {
            subscriber_remote_t* const r = s.ensure_remote(g.table_source());
            if (r == nullptr || !r->caller.assign(ctx.subject))
                return std::unexpected(status_t::BACKPRESSURE);
        }
        // The single admission step (ADR-0049): SUBSCRIBE gate → append (or replace at
        // `slot` — §D.1's "admitted through the same admission door") → latch. A field-write
        // subscribe returns no host handle — discard it.
        if (const auto r = g.admit_subscriber(v, std::move(s), ctx.subject, slot); !r)
            return std::unexpected(r.error());
        return {};
    }

    /**
     * @brief `:acl` read — the stored ACEs re-encoded, gated on READ_ACL.
     *
     * Served whole — no member or slot addressing, so `:acl[N]` names nothing (an ACE is not
     * separately addressable). Resolving the shape keeps `:acl[7]` from being served the
     * entire ACE collection under an OK status. `whole_field` is the shared shape rule (#869),
     * the same predicate @ref write_acl uses — note only the SHAPE is shared: this door gates
     * on READ_ACL and that one on WRITE_ACL, and an UNACCEPTED shape resolves below the gate
     * here (under the plain READ right) and above it there (the divergence pinned by
     * `field_shape_matrix`).
     */
    static result_t<value_ref_t> read_acl(const graph_t& g, vertex_t* v, const field_path_t& field,
                                          std::string_view caller) {
        const bool whole = whole_field(field);
        if (!g.acl_allows(v, caller, whole ? acl_right_t::READ_ACL : acl_right_t::READ))
            return std::unexpected(status_t::PERMISSION_DENIED);
        if (!whole) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        return single_link(g.read_acl(v));
    }

    /**
     * @brief `:acl` write — gate on WRITE_ACL, parse the typed ACEs, store that list alone.
     *
     * Store the :acl (#81, ADR-0018/0020): gate on WRITE_ACL — the `admin` right — then
     * validate + parse the typed ACEs (ADR-0050 parse_acl) and store THAT LIST ALONE, no
     * verbatim byte copy beside it (#907): read_acl re-encodes, so read-back cannot describe a
     * policy other than the one acl_allows walks. The outer SHAPE is checked too, not just the
     * type code — only `opt.pl` populates children, so a PRIMITIVE ACL parses as ZERO ACEs
     * and CLEARS enforcement on a write that looks like it installs one; an EMPTY CONTAINER is
     * the sanctioned clear. `set_acl` REPLACES, so an unresolved shape is no harmless no-op:
     * before this bound `:acl.bogus` / `:acl[0]` / `:acl[]` all reached it and silently
     * replaced the whole list. There is no member or slot addressing (an ACE is not separately
     * writable), so any other shape names nothing: SCHEMA_NOT_FOUND, resolved BEFORE the gate
     * like any field. `whole_field` is that rule, shared with the read door (#869).
     *
     * DIVERGENCE (#869), pinned NOT fixed: the READ door resolves an unaccepted `:acl` shape
     * BELOW its READ gate, so `:acl[0]` is SCHEMA_NOT_FOUND here (caller-independent) but
     * PERMISSION_DENIED there for a denied caller. Moving the read's resolution above its gate
     * changes a code that leaves the device; `field_shape_matrix` pins both answers.
     */
    static result_t<void> write_acl(graph_t& g, vertex_t* v, const field_path_t& field,
                                    const view::view_t& value, const write_ctx_t& ctx) {
        if (!whole_field(field)) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (!g.acl_allows(v, ctx.subject, acl_right_t::WRITE_ACL))
            return std::unexpected(status_t::PERMISSION_DENIED);
        const auto acl = wire::tlv_node_t::over(value);
        if (!acl || acl->type() != type_t::ACL || !acl->opt().pl)
            return std::unexpected(status_t::TYPE_MISMATCH);
        result_t<std::vector<ace_t>> aces = parse_acl(*acl);
        if (!aces) return std::unexpected(aces.error());
        // Storing replaces; empty => no restrictions. A refused extension block stores nothing.
        if (!v->set_acl(std::move(*aces), g.table_source()))
            return std::unexpected(status_t::BACKPRESSURE);
        {
            // Subtree-precise cache invalidation (ADR-0050 via the ADR-0057 child
            // links): every descendant's effective merge embeds this vertex's
            // INHERIT ACEs, so mark the whole subtree dirty (v itself was marked by
            // set_acl; re-marking is idempotent). Wiring-frequency — :acl writes
            // are control-plane-rare. Shared map lock: the walk only excludes
            // concurrent vertex creation; the marks are release stores.
            const std::shared_lock lock(g.map_mutex_);
            graph_t::mark_subtree_acl_dirty(v);
        }
        return {};
    }

    /**
     * @brief `:children` / `:children[]` read — member enumeration, served FOLDED.
     *
     * The read dual of the SPEC-creating append is served FOLDED (L4 fold, Slice 0): a
     * scatter-gather rope (outer POINT header + per-child borrowed NAME), byte-identical on
     * flatten() to the materialized listing (`folded_children_test` gates the differential
     * against `read_children_materialized`). A single `[N]` slot has no meaning here (members
     * are named, not indexed) and is SCHEMA_NOT_FOUND below the READ gate, as are `[*]` and a
     * tail — the same `field_selector` classification the write door switches on (#869), so
     * the two halves cannot drift on which shape they are looking at. The ANSWER stays per
     * side: `[]` enumerates here and CREATES there.
     */
    static result_t<value_ref_t> read_children(const graph_t& g, vertex_t* v,
                                               const field_path_t& field, std::string_view caller) {
        if (!g.acl_allows(v, caller, acl_right_t::READ))
            return std::unexpected(status_t::PERMISSION_DENIED);
        const field_sel_t sel = field_selector(field);
        if (sel != field_sel_t::WHOLE && sel != field_sel_t::APPEND)
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        return g.read_children_folded(vertex_handle_t{v});
    }

    /**
     * @brief `:children[]` write — in-band vertex creation from a SPEC (#82, ADR-0017).
     *
     * A `:children[]` APPEND of a SPEC instantiates a child of a device-catalog type, gated by
     * the parent's CREATE right (#81, ADR-0020). A `[N]` clear (child removal) is deferred
     * (#66). Read-back (members, not SPECs) is the field-read surface.
     *
     * `:children` is addressed WHOLE, like `:subscribers` (#581). `append` was the sole
     * predicate here, so `:children[].bogus` — or `[].a.b.c` — created the child exactly as
     * the sanctioned `:children[]` does and answered `kind=RESULT`, byte-identical, with the
     * tail provably inert (two different tails produced the same reply and the same graph). On
     * `/net` that spelling built a LIVE connection vertex and wired it into the router. The
     * READ of the byte-identical selector already answered SCHEMA_NOT_FOUND, so the two halves
     * disagreed; this makes them agree. Before the CREATE gate, like `:acl`.
     *
     * Same `field_selector` classification the read door uses for `:children` (#869) — the
     * shapes are one rule, the answers are per side: `[]` CREATES here and ENUMERATES there,
     * and `:children` bare has no write surface at all.
     */
    static result_t<void> write_children(graph_t& g, vertex_t* v, const field_path_t& field,
                                         const view::view_t& value, const write_ctx_t& ctx) {
        if (field_selector(field) != field_sel_t::APPEND)
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (!g.acl_allows(v, ctx.subject, acl_right_t::CREATE))
            return std::unexpected(status_t::PERMISSION_DENIED);
        return g.create_child(v, value);
    }

    /**
     * @brief `:settings`, `:settings.app` and `:settings.app.<name…>` read (RFC-0010 §A.4).
     *
     * An UNKNOWN core-namespace `:settings` NAME resolves above the READ gate — the exact
     * mirror of the write door (see @ref write_settings: only `settings.app.…` reaches a gate;
     * every other spelling under `settings` is an ungated SCHEMA_NOT_FOUND). RFC-0022 §3.B
     * deleted `settings_t` outright, so the core namespace is EMPTY and every name in it is an
     * unknown name — which docs/reference/05 §`0x0B` answers with
     * `ERROR{tr::schema::not_found}`, a rule stated with NO caller qualifier.
     *
     * Below that sits the READ gate, and a denied caller reaching it would be told
     * PERMISSION_DENIED on the READ of a name whose WRITE already answers SCHEMA_NOT_FOUND —
     * one name, two answers, split by who is asking. That is exactly the caller-DEPENDENT
     * disclosure §3.B forbids, so the read must resolve the name first, at the same narrowness
     * the write door does.
     *
     * Nothing leaks: "the core knob namespace is empty" is published spec text, so the
     * narrower answer discloses only what docs/reference/05 already states. Bare `:settings`
     * (the container) and the whole `settings.app.` subtree are untouched — both are KNOWN
     * names and keep their gates; the owner's name set is a secret, so `settings.app.<name>`
     * resolves only below the gate.
     *
     * The `settings.…` sub-shape is `app_field_sel` (#869), the SAME classification the write
     * door switches on. A CORE_KNOB is the RFC-0022 §3.B empty namespace on both doors.
     *
     * Past the gate: bare `:settings` is the container (RFC-0022 §4: the nested app record, and
     * nothing else); `:settings.app` is the app container alone; `:settings.app.<name…>` is
     * one declared field's stored TLV verbatim, or the owner's live value when its
     * `on_app_field_read` seam answers (#1878).
     *
     * DIVERGENCE (#869), pinned NOT fixed: `plain_step(steps[0])` is tested HERE and not on
     * the write door, so `:settings[0].app.<name>` is SCHEMA_NOT_FOUND on a read and a
     * successful WRITE. See @ref write_settings; `field_shape_matrix` pins both answers.
     */
    static result_t<value_ref_t> read_settings(const graph_t& g, vertex_t* v,
                                               const field_path_t& field, std::string_view caller) {
        std::string key;
        const app_sel_t app = app_field_sel(field, key);
        if (app == app_sel_t::CORE_KNOB) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (!g.acl_allows(v, caller, acl_right_t::READ))
            return std::unexpected(status_t::PERMISSION_DENIED);
        if (!plain_step(field.steps[0])) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (app == app_sel_t::NONE) return single_link(g.read_settings(v));
        if (app == app_sel_t::CONTAINER) return single_link(g.read_settings_app(v));
        if (app != app_sel_t::NAMED) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // ONE locked lookup classifies the field and copies its stored bytes, as before: a
        // vertex without the owner's read seam pays nothing more for it.
        std::vector<std::byte> bytes;
        const vertex_t::app_read_t got = v->app_field_get(key, bytes);
        // ENOTTY (undeclared), and `wo` has no read surface either (the secret never mirrors
        // back) — the same caller-independent identity, deliberately indistinguishable. Both
        // answer before the owner's read seam is asked, so it never runs on a field without a
        // read surface and is never an existence oracle.
        if (got == vertex_t::app_read_t::UNDECLARED || got == vertex_t::app_read_t::WRITE_ONLY)
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // The owner's LIVE value (#1878, RFC-0010 Amendment 4) wins over the stored bytes, so
        // a write the owner did not adopt never reads back as state. A decline reads the
        // stored bytes; an empty answer is the owner's refused allocation.
        if (const app_field_read_hook_t live = g.app_field_reader(v); live)
            if (std::optional<value_ref_t> owner = live(key))
                return *owner ? result_t<value_ref_t>{std::move(*owner)}
                              : std::unexpected(status_t::BACKPRESSURE);
        if (got == vertex_t::app_read_t::UNSET)  // declared but empty — distinct
            return std::unexpected(status_t::NOT_FOUND);
        // `bytes` is a non-empty stored TLV; `nullopt` is exactly an alloc
        // failure → BACKPRESSURE (the audited alloc/copy/over locus).
        const auto out = view::over_bytes(bytes, *g.value_backend_);
        if (!out) return std::unexpected(status_t::BACKPRESSURE);
        return composed_or_backpressure(view::rope_t{*out});
    }

    /**
     * @brief `:settings.app.<name…>` write — one owner-declared application field (RFC-0010 §A).
     *
     * This handler owns the whole `settings.app.` subtree — the protocol never minted (and per
     * the RFC must never mint) a knob named `app`. A bare `:settings.app` container write and
     * any `[...]`-selector step have no write surface, and neither has any other spelling under
     * `settings`: every `:settings.<knob>` name the protocol ever minted answers
     * SCHEMA_NOT_FOUND, because RFC-0022 §3.B withdrew the flat core-namespace write surface
     * whole. There is no `settings_t` to write into, so that answer is caller-INDEPENDENT
     * (never PERMISSION_DENIED) and there is no gate to get the order wrong.
     *
     * DIVERGENCE (#869), pinned NOT fixed: `step0`'s OWN shape is not tested here, so
     * `:settings[0].app.<name>` WRITES the field, while the read door — which does test
     * `plain_step(steps[0])` — answers it SCHEMA_NOT_FOUND. Tightening the write is a change
     * to what leaves the device for that spelling; `field_shape_matrix` pins both answers.
     */
    static result_t<void> write_settings(graph_t& g, vertex_t* v, const field_path_t& field,
                                         const view::view_t& value, const write_ctx_t& ctx) {
        std::string key;
        if (app_field_sel(field, key) != app_sel_t::NAMED)
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // GATE-BEFORE-RESOLVE (#435, RFC-0010 §A erratum 2026-08-12). Owner-defined names
        // are a per-node secret — unlike the protocol's published constants, whose
        // pre-gate resolution #430 justified — so a caller-attributed write evaluates
        // the vertex WRITE right BEFORE any name under `settings.app.` is resolved: a
        // denied caller is told PERMISSION_DENIED whether the name is declared,
        // undeclared, `ro` or `wo`, and the error channel discloses neither the owner's
        // name set nor which spellings exist. The read door has the same order (its READ
        // gate sits above `settings.app.` resolution); #430's write-side hoist left this
        // branch answering SCHEMA_NOT_FOUND pre-gate, which leaked field existence.
        if (!ctx.is_local_owner() && !g.acl_allows(v, ctx.subject, acl_right_t::WRITE))
            return std::unexpected(status_t::PERMISSION_DENIED);
        const std::optional<app_access_t> access = v->app_field_access(key);
        if (!access)  // undeclared stays ENOTTY — the table opens only its own names
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // A field not declared remotely writable has NO write surface (RFC-0010 §A.3
        // gate 1, the ENOTTY of writing a read-only ioctl) — the answer every ADMITTED
        // caller gets, identically; per the erratum it sits BELOW the ACL gate so it is
        // never an existence oracle for a denied one. The owner (empty caller) skips
        // both checks — it is updating its own projection, not a caller.
        if (!ctx.is_local_owner() && *access == app_access_t::RO)
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // ADMISSION (§A.3), the field plane's twin of the value plane's `on_admit`. The
        // descriptor table is still consumer self-description and still never a runtime
        // validation schema — this is the OWNER's own filter, not a schema the protocol
        // enforces, and a vertex that installs none stores verbatim exactly as before.
        //
        // Placement: BELOW the ACL gate and BELOW the RFC-0010 §A.3 writability check, so a
        // denied or read-only caller is answered by the gates and the owner's filter never runs
        // on a write that was never going to land (and cannot become an existence oracle for
        // one). ABOVE the store, which is the whole point — a refusal leaves the field's prior
        // bytes untouched and `on_app_field_write` unfired, because there is nothing to apply.
        //
        // The filter is read LOCK-FREE off the graph's immortal declaration list (where it
        // lives for the cost reason `graph_t::admissions_` states) and called with no vertex
        // lock held, so it may re-enter the graph like the apply seam below. `admitted` is the
        // view it returned, READ by the store on the next line and never retained past it.
        // `ctx` is the very context the gates above ran on (#1832): the filter and the gate
        // cannot disagree about who wrote, and it sees the arrival link the value plane's
        // `on_admit` sees.
        view::view_t admitted = value;
        const admission_node_t* adm = g.admission_for(v);
        if (adm != nullptr && adm->on_app_field_admit) {
            result_t<view::view_t> decided = adm->on_app_field_admit(key, value, ctx);
            if (!decided) return std::unexpected(decided.error());
            admitted = std::move(*decided);
        }
        // Store (§D — bytes in, bytes out). SCHEMA_NOT_FOUND means a concurrent table
        // replacement un-declared the name between gate and store; BACKPRESSURE means the
        // value's bytes did not fit the table source (#1778).
        if (const result_t<void> stored = v->app_field_store(key, admitted.bytes()); !stored)
            return std::unexpected(stored.error());
        // The owner apply seam (§A.3), OUTSIDE the vertex lock — it may re-enter the
        // graph (apply the config, restructure children, then ANNOUNCE per §C). The
        // field write itself deliberately neither wakes `await` nor propagates:
        // the property plane is silent (ADR-0021 / RFC-0010 §C).
        // Snapshot the seam under the vertex lock (ADR-0058 Step 2 moved it to the lazy
        // app-field group), then fire the copy here, unlocked.
        // Fired with the ADMITTED bytes — what actually landed, which is what an apply seam
        // must act on. Absent a filter that is the written value, byte-for-byte as before.
        if (auto seam = v->on_app_field_write(); seam) seam(key, admitted);
        return {};
    }

    /** @brief `:schema` read — one synthesized POINT, served whole (no `[N]` surface), below
     *         the READ gate. The shared `whole_field` shape rule (#869). */
    static result_t<value_ref_t> read_schema(const graph_t& g, vertex_t* v,
                                             const field_path_t& field, std::string_view caller) {
        if (!g.acl_allows(v, caller, acl_right_t::READ))
            return std::unexpected(status_t::PERMISSION_DENIED);
        if (!whole_field(field)) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        return single_link(g.read_schema(v));
    }

    /**
     * @brief `:identity` read — PRE-AUTH BY DESIGN (#406, RFC-0011 §C.2), so no gate at all.
     *
     * A narrow, named exemption that applies to this field alone. The public key is precisely
     * what an unauthenticated peer must obtain in order to TOFU-pin and to verify the ADR-0045
     * challenge, so gating it behind READ would deadlock first contact (the default ACL ships
     * closed). It discloses nothing the Noise handshake would not present as its static key
     * anyway. Node-scoped: it takes no vertex, and every vertex answers identically (§C.1).
     * The WHOLE `identity` namespace resolves here, not just the bare spelling: the record is
     * served whole and has no member or indexed addressing (§C.4), so any other shape names
     * nothing and is SCHEMA_NOT_FOUND — caller-independent, like any unknown field. Nothing is
     * disclosed by the narrower answer — the record itself is world-readable by design.
     */
    static result_t<value_ref_t> read_identity(const graph_t& g, vertex_t* /*v*/,
                                               const field_path_t& field,
                                               std::string_view /*caller*/) {
        if (!whole_field(field)) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        return single_link(g.read_identity());
    }

    /**
     * @brief `:stats.<class>.<name>` read — one census block (#1503 step 5, RFC-0010 Am. 1).
     *
     * Node-scoped in the `:identity` mould — it takes no vertex, every vertex answers
     * identically — with the GATING INVERTED: a memory census is not first-contact material,
     * so the VALUE resolves BELOW the READ gate and there is no pre-auth exemption. Only NAME
     * VALIDITY resolves above it, exactly as the erratum's protocol-owned row requires: the
     * seam namespace is published spec text, identical on every node, so an unrecognised seam
     * spelling answers SCHEMA_NOT_FOUND caller-INDEPENDENTLY and a denied caller reading a
     * RECOGNISED seam gets PERMISSION_DENIED. That does disclose that the seam exists —
     * intended, and no more than `settings` or `children` already disclose.
     *
     * A NET-plane class (Amendment 2) is recognised by SPELLING but SERVED by the registered
     * sampler, so validity must be settled above the gate too — with a probe that samples
     * nothing. Otherwise an unserved `router` / `labels` / `link` NAME would answer
     * SCHEMA_NOT_FOUND to an admitted caller and PERMISSION_DENIED to a denied one: one
     * spelling, two answers split by who asked, which is exactly what §D.2 forbids. The probe
     * is caller-independent by construction — it is not handed the caller — and it discloses
     * no value.
     */
    static result_t<value_ref_t> read_stats_field(const graph_t& g, vertex_t* v,
                                                  const field_path_t& field,
                                                  std::string_view caller) {
        const stats_seam_t seam = stats_seam(field);
        if (seam == stats_seam_t::NONE) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (seam == stats_seam_t::NET &&
            !g.sample_stats(field.steps[1].name, field.steps[2].name, nullptr))
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // Amendment 3: a node that derives no sub-pools (an injected root, or a build without
        // the host slab pool) does not publish `mem.values`, `.tables` or `.net` — a NODE
        // property, so it is settled above the gate like every other validity answer.
        if (seam >= stats_seam_t::MEM_VALUES && seam <= stats_seam_t::MEM_NET &&
            !g.derives_sub_pools())
            return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (!g.acl_allows(v, caller, acl_right_t::READ))
            return std::unexpected(status_t::PERMISSION_DENIED);
        return single_link(read_stats(g, field, seam));
    }

    /**
     * @brief THE colon-field table (#1711): one row per field, and the whole recognised
     *        namespace.
     *
     * PROTOCOL-OWNED NAME VALIDITY RESOLVES ABOVE EVERY GATE (#435, RFC-0010 §A erratum
     * 2026-08-12). These names are published spec text (docs/reference/05 §0x09 STATUS /
     * §Field namespace), identical on every node, so answering an unknown NAME before any gate
     * discloses nothing; answering it below the READ gate would split one spelling's answer by
     * who asked — the caller-dependent disclosure reference/05 §Gating-:identity names as the
     * failure mode. Both doors therefore answer a head missing from this table
     * SCHEMA_NOT_FOUND, ungated. Adding a field is adding a row.
     */
    static constexpr row_t kRows[] = {
        {"subscribers", &read_subscribers, &write_subscribers},
        {"acl", &read_acl, &write_acl},
        {"children", &read_children, &write_children},
        {"settings", &read_settings, &write_settings},
        {"schema", &read_schema, nullptr},
        {"identity", &read_identity, nullptr},
        {"stats", &read_stats_field, nullptr},
    };

    /** @brief The row whose NAME is @p head, or nullptr — no such field on either door. */
    [[nodiscard]] static const row_t* find(std::string_view head) noexcept {
        for (const row_t& row : kRows)
            if (row.name == head) return &row;
        return nullptr;
    }
};

result_t<void> graph_t::field_write(vertex_t* v, const field_path_t& field,
                                    const view::view_t& value, const write_ctx_t& ctx) {
    const field_surface_t::row_t* row = field_surface_t::find(field.steps[0].name);
    if (row == nullptr || row->write == nullptr) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    return row->write(*this, v, field, value, ctx);
}

result_t<value_ref_t> graph_t::read_field_composed(vertex_handle_t vh, const field_path_t& field,
                                                   std::string_view caller) const {
    // A field read serves a control TLV; it crosses back as one value (RFC-0028 D11 — every
    // value read answers `value_ref_t`), single-link for every field but the folded
    // `:children` listing.
    // Field reads are gated like data reads (#81) — each row states its own gate.
    const field_surface_t::row_t* row = field_surface_t::find(field.steps[0].name);
    if (row == nullptr) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    return row->read(*this, vh.get(), field, caller);
}

result_t<void> graph_t::create_child(vertex_t* parent, const view::view_t& spec_value) {
    // Parse SPEC{ NAME "type" <sel>, NAME "name" <seg>, SETTINGS "config"? } — the
    // creation spec of docs/reference/05 §0x0E. The two NAMEs are positional pairs
    // (NAME key, NAME/SETTINGS value), read through the ONE pair-consuming walk,
    // wire::config_reader_t (#927 — hoisted to L2/L3 by #985 so this file no longer
    // carries a hand-written copy of the rule).
    const auto spec = wire::tlv_node_t::over(spec_value);
    if (!spec || spec->type() != type_t::SPEC) return std::unexpected(status_t::TYPE_MISMATCH);

    const wire::config_reader_t spec_pairs(&*spec);
    const std::string_view type_sel = spec_pairs.name("type").value_or(std::string_view{});
    const std::span<const std::byte> child_name =
        spec_pairs.name_bytes("name").value_or(std::span<const std::byte>{});
    const std::optional<tlv_node_t> config = spec_pairs.settings("config");
    // The wire boundary runs THE segment predicate (ADR-0073 §1, #688): a peer-supplied
    // name must be expressible in the addressing grammar, or the vertex it creates is
    // enumerable but unaddressable — and a `/` inside one NAME breaks the injectivity of
    // the address→vertex map (reference/02). Same predicate, same INVALID_PATH answer as
    // the local parser, so the tiers cannot drift.
    if (type_sel.empty() || !valid_segment(detail::as_string_view(child_name)))
        return std::unexpected(status_t::INVALID_PATH);

    // Look up the catalog type (ADR-0017): unknown => SCHEMA_NOT_FOUND (ENOTTY). Setup-only
    // by doctrine, but this walk runs from a PEER's bytes, so it is taken under the shared
    // lock (#1049) and the factory is COPIED out of the map before the lock drops. The copy
    // is what makes the lock re-entrancy-safe: factories call back into `graph_t` (they
    // register vertices), and one that registered a child type while we still held a shared
    // lock would self-deadlock on the exclusive side. One std::function copy on a path that
    // is about to allocate a vertex is not a cost worth avoiding.
    child_factory_t factory;
    {
        const std::shared_lock lock(child_types_mutex_);
        const child_factory_t* const found = child_types_.find(type_sel);
        if (found == nullptr) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        factory = *found;
    }

    // Compose the child key = parent's canonical PATH-payload + one packed record for
    // `child_name` (RFC-0018). The graph owns this addressing; the factory only sees the
    // finished key.
    // The factory's key is an owning `std::vector` by its public signature (the public-API
    // half of the seam migration); the render itself draws from the table source (#1778).
    mem::bytes_t parent_key(*tables_);
    if (!try_build_key(parent, parent_key)) return std::unexpected(status_t::BACKPRESSURE);
    std::vector<std::byte> child_key(parent_key.begin(), parent_key.end());
    if (!wire::emit_path_segment(child_key, child_name))
        return std::unexpected(status_t::INVALID_PATH);

    result_t<vertex_handle_t> made =
        factory(*this, std::move(child_key), config ? &*config : nullptr);
    if (!made) return std::unexpected(made.error());  // PATH_IN_USE on a duplicate name
    return {};
}

result_t<view::view_t> graph_t::read_schema(vertex_t* v) const {
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
    //
    // The one exception is a CONTROL vertex that declared a catalog at registration
    // (RFC-0014 Amendment 3 — a transport module's creator endpoint): its catalog IS the
    // content of this `SETTINGS`, served verbatim. The graph owns the frame, the declarer owns
    // what is inside it. Behind the same flag bit as the payload-right rows, so a vertex that
    // declared neither does not walk.
    const std::span<const std::byte> settings_children =
        v->has_payload_rights() ? declared_catalog(v) : std::span<const std::byte>{};

    // Staged on the table source (#1885); a refusal anywhere is BACKPRESSURE.
    mem::bytes_t point_body(*tables_);
    if (!wire::emit_name(point_body, key_view_t{v->name().bytes()}.last_segment()) ||
        !wire::emit_tlv(point_body, type_t::SETTINGS, opt_t{.pl = true},
                        settings_children))  // SETTINGS
        return std::unexpected(status_t::BACKPRESSURE);

    // The owner part (RFC-0010 §B.2), present iff a descriptor table is installed —
    // `NAME "app" SETTINGS{ NAME <field> SETTINGS{…} … }` appended AFTER the synthesized
    // protocol part (precedence by position, zero merge logic; the two parts describe
    // disjoint namespaces by the §A.1 reservation). Each field's record leads with the
    // runtime-projected `access` member — the one §B.1 datum the runtime holds natively,
    // so the schema can never contradict the write gate — then the owner's descriptor
    // bytes verbatim. A vertex without a table keeps today's POINT byte-for-byte.
    const std::vector<app_field_t> table = v->app_fields_snapshot();
    if (!table.empty()) {
        mem::bytes_t app_children(*tables_);
        mem::bytes_t desc(*tables_);
        for (const app_field_t& f : table) {
            desc.clear();
            const std::string_view a = to_string(f.access);
            if (!wire::emit_name(desc, "access") ||
                !wire::emit_tlv(desc, type_t::VALUE, opt_t{},
                                std::span<const std::byte>(
                                    reinterpret_cast<const std::byte*>(a.data()), a.size())) ||
                !desc.append(f.descriptor.data(), f.descriptor.size()) ||
                !wire::emit_name(app_children, f.name) ||
                !wire::emit_tlv(app_children, type_t::SETTINGS, opt_t{.pl = true},
                                mem::as_span(desc)))
                return std::unexpected(status_t::BACKPRESSURE);
        }
        if (!wire::emit_name(point_body, "app") ||
            !wire::emit_tlv(point_body, type_t::SETTINGS, opt_t{.pl = true},
                            mem::as_span(app_children)))
            return std::unexpected(status_t::BACKPRESSURE);
    }

    mem::bytes_t point(*tables_);
    if (!wire::emit_tlv(point, type_t::POINT, opt_t{.pl = true}, mem::as_span(point_body)))
        return std::unexpected(status_t::BACKPRESSURE);  // POINT

    // `point` is a POINT TLV (never empty); `nullopt` is exactly an alloc failure
    // → BACKPRESSURE. One audited locus for the alloc/copy/over triplet.
    const auto out = view::over_bytes(mem::as_span(point), *value_backend_);
    if (!out) return std::unexpected(status_t::BACKPRESSURE);
    return *out;
}

/**
 * @brief The largest `:identity` record the RFC-0011 §B kind registry can produce.
 *
 * Every kind fixes its key length, so the record's length is a function of the registry
 * rather than of anything a caller supplies — this is a consequence of §B, NOT a synthetic
 * cap on user data. ed25519, the only kind today, serializes to exactly 60 bytes
 * (`SETTINGS{ NAME "kind" VALUE u8, NAME "key" VALUE 32B }`); the headroom absorbs a
 * registry addition without a reader change.
 *
 * It exists so @ref graph_t::read_identity can copy the record out under its lock WITHOUT
 * allocating under that lock. @ref graph_t::set_identity is the single writer and checks
 * against it, which is what makes the reader's buffer provably sufficient.
 */
constexpr std::size_t kMaxIdentityRecordBytes = 96;

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
    // Staged on this call's stack frame (#1885): the record is bounded by the registry.
    std::array<std::byte, 2 * kMaxIdentityRecordBytes> scratch;
    mem::bump_source_t frame(scratch, *tables_);
    mem::bytes_t members(frame);
    mem::bytes_t record(frame);
    if (!wire::emit_name(members, "kind") || !wire::emit_value_le(members, kind, 1) ||
        !wire::emit_name(members, "key") || !wire::emit_tlv(members, type_t::VALUE, opt_t{}, key) ||
        !wire::emit_tlv(record, type_t::SETTINGS, opt_t{.pl = true}, mem::as_span(members)))
        return std::unexpected(status_t::BACKPRESSURE);
    // The single-writer half of @ref kMaxIdentityRecordBytes. Unreachable at today's
    // registry (ed25519 is 60 bytes and every other kind was refused above); it is here so
    // that a future §B addition whose record outgrows the reader's stack buffer fails
    // LOUDLY at the one install site rather than overflowing that buffer.
    if (record.size() > kMaxIdentityRecordBytes) return std::unexpected(status_t::TYPE_MISMATCH);

    // Publish under the lock (#1049). The record is BUILT and copied into the table source
    // above, outside it, so the only thing serialized is the swap — the previous buffer, the
    // one a concurrent `read_identity` may be memcpying from on behalf of a peer that has
    // authenticated nothing (RFC-0011 §C: the facet resolves above the READ gate on purpose),
    // is freed after the unlock, so no allocator call runs inside this leaf (#1778).
    mem::bytes_t fresh(*tables_);
    if (!mem::assign_bytes(fresh, mem::as_span(record)))
        return std::unexpected(status_t::BACKPRESSURE);
    {
        const std::unique_lock lock(identity_mutex_);
        std::swap(identity_record_, fresh);
    }
    return {};
}

void graph_t::clear_identity() {
    mem::bytes_t old(*tables_);
    const std::unique_lock lock(identity_mutex_);
    std::swap(identity_record_, old);
}

result_t<view::view_t> graph_t::read_identity() const {
    // No keypair => the facet is ABSENT, not empty (RFC-0011 §C.3): the ENOTTY of an
    // unsupported field, byte-for-byte the pre-RFC behaviour. An empty record was
    // rejected precisely because it would fabricate an "identity exists but is vacant"
    // state no consumer can act on.
    //
    // Shared-locked across the emptiness test AND the copy (#1049): those two straddled a
    // possible install/clear, either of which frees the buffer the copy reads. Concurrent
    // reads still run in parallel; the only exclusion is against a rotation.
    //
    // The lock deliberately does NOT span the ALLOCATION. The bytes land in a stack buffer
    // the §B registry bounds (@ref kMaxIdentityRecordBytes, enforced at the single install
    // site) and `over_bytes` runs after the unlock, because holding a lock across an
    // allocator call is what would stop `identity_mutex_` being a LEAF: the moment that
    // allocator becomes an injected `block_source_t` carrying its own `Sync` policy
    // (#873 / ADR-0079), a lock-ordering obligation appears that does not exist today.
    // Copying first costs one memcpy of at most 96 bytes on a cold path and removes it.
    std::array<std::byte, kMaxIdentityRecordBytes> scratch;
    std::size_t len = 0;
    {
        const std::shared_lock lock(identity_mutex_);
        if (identity_record_.empty()) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        len = identity_record_.size();
        std::memcpy(scratch.data(), identity_record_.data(), len);
    }
    // Pre-serialized at install, so every vertex of this node serves BYTE-IDENTICAL
    // bytes (§C.1) — the invariant that makes the record a valid cross-path key.
    const auto out =
        view::over_bytes(std::span<const std::byte>(scratch.data(), len), *value_backend_);
    if (!out) return std::unexpected(status_t::BACKPRESSURE);
    return *out;
}

result_t<view::view_t> graph_t::read_settings(vertex_t* v) const {
    // The settings container KEEPS ITS SHAPE and LOSES ITS KNOBS (RFC-0010 §A.4 as amended
    // by RFC-0022 §4): `SETTINGS{ [NAME "app" SETTINGS{…}] }`. The reserved `app` subkey and
    // the single-traversal renderer contract survive; the core knob namespace is empty
    // because `settings_t` is deleted, so the read still enumerates exactly what the WRITE
    // gate accepts — which is now nothing, honestly, rather than seven names of which four
    // were never honoured. A vertex with no declared app fields reads an EMPTY `SETTINGS{}`,
    // which is honest rather than absent.
    // Staged on the table source (#1885); a refusal anywhere is BACKPRESSURE.
    mem::bytes_t children(*tables_);
    const std::vector<app_field_t> table = v->app_fields_snapshot();
    if (!table.empty()) {
        mem::bytes_t app_children(*tables_);
        if (!emit_app_container(app_children, table, app_field_reader(v)) ||
            !wire::emit_name(children, "app") ||
            !wire::emit_tlv(children, type_t::SETTINGS, opt_t{.pl = true},
                            mem::as_span(app_children)))
            return std::unexpected(status_t::BACKPRESSURE);
    }
    mem::bytes_t out(*tables_);
    if (!wire::emit_tlv(out, type_t::SETTINGS, opt_t{.pl = true}, mem::as_span(children)))
        return std::unexpected(status_t::BACKPRESSURE);
    // `out` is non-empty by construction; `nullopt` is exactly an alloc failure
    // → BACKPRESSURE (the audited alloc/copy/over locus).
    const auto res = view::over_bytes(mem::as_span(out), *value_backend_);
    if (!res) return std::unexpected(status_t::BACKPRESSURE);
    return *res;
}

result_t<view::view_t> graph_t::read_settings_app(vertex_t* v) const {
    // The app container alone (RFC-0010 §A.4). No installed table ⇒ the surface stays
    // closed (SCHEMA_NOT_FOUND — byte-for-byte the pre-RFC vertex); an installed table
    // serves the declared, non-`wo`, value-holding fields verbatim (possibly an empty
    // SETTINGS when nothing has been written yet).
    const std::vector<app_field_t> table = v->app_fields_snapshot();
    if (table.empty()) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    mem::bytes_t children(*tables_);  // staged on the table source (#1885)
    mem::bytes_t out(*tables_);
    if (!emit_app_container(children, table, app_field_reader(v)) ||
        !wire::emit_tlv(out, type_t::SETTINGS, opt_t{.pl = true}, mem::as_span(children)))
        return std::unexpected(status_t::BACKPRESSURE);
    // `out` is non-empty by construction (the SETTINGS header at minimum); `nullopt` is
    // exactly an alloc failure → BACKPRESSURE (the audited alloc/copy/over locus).
    const auto res = view::over_bytes(mem::as_span(out), *value_backend_);
    if (!res) return std::unexpected(status_t::BACKPRESSURE);
    return *res;
}

app_field_read_hook_t graph_t::app_field_reader(const vertex_t* v) const noexcept {
    const admission_node_t* adm = admission_for(v);
    return adm != nullptr ? adm->on_app_field_read : app_field_read_hook_t{};
}

result_t<view::view_t> graph_t::read_acl(vertex_t* v) const {
    // RE-ENCODE the stored ACEs (#907): read-back is a projection of the list acl_allows
    // walks, never a copy that could disagree with it. An encoded ACL is never empty, so
    // empty ⇒ no :acl was ever written — NOT_FOUND, distinct from an EMPTY container.
    std::array<std::byte, kScratchBytes> scratch;  // stack first, then the table source (#1781)
    mem::bump_source_t frame(scratch, *tables_);
    mem::bytes_t acl(frame);
    bool ok = true;
    v->with_acl([&acl, &ok](bool set, const std::vector<ace_t>& aces) {
        if (set) ok = encode_acl(aces, acl);
        return set;
    });
    if (!ok) return std::unexpected(status_t::BACKPRESSURE);
    if (acl.empty()) return std::unexpected(status_t::NOT_FOUND);
    const auto out = view::over_bytes(mem::as_span(acl), *value_backend_);
    if (!out) return std::unexpected(status_t::BACKPRESSURE);
    return *out;
}

}  // namespace tr::graph
