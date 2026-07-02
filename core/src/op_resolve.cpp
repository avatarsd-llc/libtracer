/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/op_resolve.hpp"

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::graph {

using view::rope_t;
using view::segment_ptr_t;
using view::view_t;
using wire::arena_tlv_t;
using wire::opt_t;
using wire::tlv_arena_t;
using wire::type_t;

namespace {

// The opt-byte mask an ADR-0041 §4 trailer-sliced copy applies: the structural
// bits (PL, LL) survive; the trailer bits (TS, CR, CW, TF) are cleared so the
// copied TLV — whose bytes exclude the trailer by construction (`node.wire`) —
// stays self-consistent and trailer-less at rest.
constexpr std::byte kStructOptMask{0x48};

// Map an L4 status_t to its wire ERROR code byte (docs/reference/05 §error code
// registry). Provisional shape pending the ERROR registry (#8 / RFC-0002) — the
// kind=ERROR reply payload is STATUS{ ERROR u8 } per RFC-0004 §D.
[[nodiscard]] std::uint8_t error_code(status_t s) noexcept {
    switch (s) {
        case status_t::NOT_FOUND:
            return 0x01;
        case status_t::PERMISSION_DENIED:
            return 0x02;
        case status_t::INVALID_PATH:
            return 0x03;
        case status_t::TYPE_MISMATCH:
            return 0x04;
        case status_t::BACKPRESSURE:
            return 0x07;
        case status_t::TIMEOUT:
            return 0x08;
        case status_t::SCHEMA_NOT_FOUND:
            return 0x0A;
        case status_t::PATH_IN_USE:
            return 0x0E;
    }
    return 0x01;
}

// A parsed request FWD — arena node INDICES, no bytes owned (ADR-0041 §2: the
// arena and its spans are read-only until an ownership copy is taken).
struct parsed_fwd_t {
    fwd_op_t op{};
    std::uint32_t dst = 0;            // forward route (PATH node index)
    std::uint32_t selector = 0;       // optional :field (FIELD node index; 0 = absent)
    std::uint32_t src = 0;            // accumulated return route (PATH node index)
    std::uint32_t payload = 0;        // WRITE only (node index; 0 = absent)
    std::uint64_t await_timeout = 0;  // AWAIT only
    bool has_await_timeout = false;
};

// Parse the FWD child sequence positionally (RFC-0004 §B order: op, dst, FIELD?,
// src, [payload | await_timeout]) over the arena's pre-order indices. Returns
// INVALID_PATH for a structurally malformed frame (the resolver turns that into
// the error side, not a reply). Index 0 is the root, so 0 doubles as "absent".
[[nodiscard]] result_t<parsed_fwd_t> parse_fwd(const tlv_arena_t& a) {
    const arena_tlv_t& root = a.root();
    if (root.type != type_t::FWD || !root.opt.pl) return std::unexpected(status_t::INVALID_PATH);
    const std::uint32_t end = root.end;
    parsed_fwd_t p;
    std::uint32_t i = tlv_arena_t::first_child(0);
    if (i >= end || a[i].type != type_t::VALUE) return std::unexpected(status_t::INVALID_PATH);
    p.op = static_cast<fwd_op_t>(detail::load_le<std::uint8_t>(a[i].body));
    i = a.next_sibling(i);
    if (i >= end || a[i].type != type_t::PATH) return std::unexpected(status_t::INVALID_PATH);
    p.dst = i;
    i = a.next_sibling(i);
    if (i < end && a[i].type == type_t::FIELD) {
        p.selector = i;
        i = a.next_sibling(i);
    }
    if (i >= end || a[i].type != type_t::PATH) return std::unexpected(status_t::INVALID_PATH);
    p.src = i;
    i = a.next_sibling(i);
    if (p.op == fwd_op_t::WRITE) {
        if (i < end) p.payload = i;
    } else if (p.op == fwd_op_t::AWAIT) {
        if (i < end && a[i].type == type_t::VALUE) {
            p.await_timeout = detail::load_le<std::uint64_t>(a[i].body);
            p.has_await_timeout = true;
        }
    }
    return p;
}

// FIELD index_mode (RFC-0004 §C, the optional u8 index_mode VALUE).
enum class index_mode_t : std::uint8_t { SCALAR = 0, ELEMENT = 1, WILDCARD = 2 };

// Decode a FIELD selector node into the graph's field_path_t. Each level is a
// NAME followed by 0/1/2 VALUE children: 0 => SCALAR; 1 => index_mode only
// (ELEMENT append "[]" or WILDCARD "[*]"); 2 => [index u32, index_mode u8]
// ("[N]"). `wildcard_seen` is set if any level carries index_mode=WILDCARD.
[[nodiscard]] result_t<field_path_t> selector_to_field(const tlv_arena_t& a, std::uint32_t field,
                                                       bool& wildcard_seen) {
    field_path_t fp;
    const std::uint32_t end = a[field].end;
    std::uint32_t i = tlv_arena_t::first_child(field);
    while (i < end) {
        if (a[i].type != type_t::NAME) return std::unexpected(status_t::INVALID_PATH);
        field_step_t step;
        step.name.assign(detail::as_string_view(a[i].body));
        i = a.next_sibling(i);
        std::uint32_t v0 = 0;
        std::uint32_t v1 = 0;
        if (i < end && a[i].type == type_t::VALUE) {
            v0 = i;
            i = a.next_sibling(i);
        }
        if (i < end && a[i].type == type_t::VALUE) {
            v1 = i;
            i = a.next_sibling(i);
        }
        index_mode_t mode = index_mode_t::SCALAR;
        bool has_index = false;
        std::uint32_t index = 0;
        if (v0 != 0 && v1 != 0) {
            has_index = true;
            index = detail::load_le<std::uint32_t>(a[v0].body);
            mode = static_cast<index_mode_t>(detail::load_le<std::uint8_t>(a[v1].body));
        } else if (v0 != 0) {
            mode = static_cast<index_mode_t>(detail::load_le<std::uint8_t>(a[v0].body));
        }
        switch (mode) {
            case index_mode_t::ELEMENT:
                step.indexed = true;
                if (has_index)
                    step.index = static_cast<std::uint16_t>(index);
                else
                    step.append = true;
                break;
            case index_mode_t::WILDCARD:
                step.indexed = true;
                step.wildcard = true;
                wildcard_seen = true;
                break;
            case index_mode_t::SCALAR:
                step.indexed = has_index;
                if (has_index) step.index = static_cast<std::uint16_t>(index);
                break;
        }
        fp.steps.push_back(std::move(step));
        if (fp.steps.size() > kMaxFieldDepth) return std::unexpected(status_t::INVALID_PATH);
    }
    return fp;
}

// True for a whole-array ":subscribers[]" read (vs. a single "[N]" slot).
[[nodiscard]] bool is_subscribers_array(const field_path_t& fp) noexcept {
    return fp.steps.size() == 1 && fp.steps[0].name == "subscribers" &&
           (fp.steps[0].append || (!fp.steps[0].indexed && !fp.steps[0].wildcard));
}

// True for the subscribe form specifically — a ":subscribers[]" APPEND (a new edge),
// distinct from a ":subscribers[N]" clear (unsubscribe) or the whole-array read.
[[nodiscard]] bool is_subscribe_append(const field_path_t& fp) noexcept {
    return fp.steps.size() == 1 && fp.steps[0].name == "subscribers" && fp.steps[0].append;
}

// Extract the SUBSCRIBER.qos_settings `delivery_compact` opt-in (RFC-0004 §E.1)
// from the arena-decoded SUBSCRIBER node; false when absent. Mirrors graph.cpp
// field_write's parse (one locus per layer — graph stores the local slot's flag,
// the resolver reads it for the remote-subscriber binding) so the two never drift.
[[nodiscard]] bool subscriber_compact(const tlv_arena_t& a, std::uint32_t sub) noexcept {
    const std::uint32_t sub_end = a[sub].end;
    for (std::uint32_t c = tlv_arena_t::first_child(sub); c < sub_end; c = a.next_sibling(c)) {
        if (a[c].type != type_t::SETTINGS) continue;
        const std::uint32_t q_end = a[c].end;
        for (std::uint32_t n = tlv_arena_t::first_child(c); n < q_end; n = a.next_sibling(n)) {
            const std::uint32_t v = a.next_sibling(n);
            if (v >= q_end) break;
            if (a[n].type != type_t::NAME || a[v].type != type_t::VALUE) continue;
            if (detail::as_string_view(a[n].body) == "delivery_compact")
                return detail::load_le<std::uint8_t>(a[v].body) != 0;
        }
    }
    return false;
}

// The one ADR-0041 §2 ownership copy of a whole TLV into a fresh owned segment:
// `node.wire` (trailer already excluded) with the copied opt byte's trailer bits
// cleared (§4) — the stored TLV is trailer-less at rest and self-consistent.
[[nodiscard]] view_t own_tlv(const arena_tlv_t& node) {
    view_t v = view::over_bytes(node.wire);
    if (!v.empty()) v.owner->bytes[1] &= kStructOptMask;
    return v;
}

// A tiny cursor writer over a preallocated, exactly-sized head segment — the
// direct-emit that replaces the old 4-stage (encode → children → head → segment)
// staging: every length is known from the arena spans up front, so the reply
// head is ONE allocation and each route byte is copied exactly once.
struct emit_cursor_t {
    std::byte* p;

    void struct_header(type_t type, bool ll, std::size_t body_len) {
        *p++ = static_cast<std::byte>(std::to_underlying(type));
        *p++ = static_cast<std::byte>(opt_t{.pl = true, .ll = ll}.encode());
        detail::store_le(std::span<std::byte>(p, ll ? 4u : 2u),
                         static_cast<std::uint32_t>(body_len), ll ? 4u : 2u);
        p += ll ? 4u : 2u;
    }
    void u8_value(std::uint8_t v) {  // a 1-byte VALUE TLV (op / kind discriminants)
        *p++ = static_cast<std::byte>(std::to_underlying(type_t::VALUE));
        *p++ = std::byte{0};
        *p++ = std::byte{1};
        *p++ = std::byte{0};
        *p++ = std::byte{v};
    }
    void tlv_sliced(const arena_tlv_t& node) {  // trailer-sliced whole-TLV copy (§4)
        std::memcpy(p, node.wire.data(), node.wire.size());
        p[1] &= kStructOptMask;
        p += node.wire.size();
    }
    void raw(std::span<const std::byte> bytes) {
        if (!bytes.empty()) std::memcpy(p, bytes.data(), bytes.size());
        p += bytes.size();
    }
};

constexpr std::size_t kU8ValueLen = 5;  // 4-byte VALUE header + 1 payload byte

// Assemble the FWD{REPLY} rope: one exactly-sized head segment (FWD header +
// op=REPLY + dst=req.src + src=req.dst + kind + `inline_tail`) prepended to
// `shared` (refcount clones of the stored payload view(s)). The head is the only
// allocation; the route bytes are copied ONCE (trailer-sliced); `shared` is never
// copied — RFC-0004 §D / ADR-0035 zero-copy reply rule, ADR-0041 §5 direct emit.
[[nodiscard]] rope_t assemble(const tlv_arena_t& a, const parsed_fwd_t& req, reply_kind_t kind,
                              std::span<const std::byte> inline_tail,
                              const std::vector<view_t>& shared, std::size_t shared_len) {
    const arena_tlv_t& reply_dst = a[req.src];  // reply dst = the request's src
    const arena_tlv_t& reply_src = a[req.dst];  // reply src = the responder endpoint
    const std::size_t children_len = kU8ValueLen + reply_dst.wire.size() + reply_src.wire.size() +
                                     kU8ValueLen + inline_tail.size();
    const std::size_t body_len = children_len + shared_len;
    const bool ll = body_len > 0xFFFFu;
    const std::size_t head_len = (ll ? 6u : 4u) + children_len;

    rope_t rope;
    segment_ptr_t seg = view::heap_alloc(head_len);
    if (seg) {
        // A head-alloc failure degrades the reply (payload views only), never crashes.
        emit_cursor_t out{seg->bytes.data()};
        out.struct_header(type_t::FWD, ll, body_len);
        out.u8_value(std::to_underlying(fwd_op_t::REPLY));
        out.tlv_sliced(reply_dst);
        out.tlv_sliced(reply_src);
        out.u8_value(std::to_underlying(kind));
        out.raw(inline_tail);
        rope.append(view_t::over(std::move(seg)));
    }
    for (const view_t& v : shared) rope.append(v);  // refcount clone — no byte copy
    return rope;
}

// A kind=ERROR reply carrying STATUS{ ERROR u8=<code> } (RFC-0004 §D, provisional
// #8) — a fixed 9-byte tail, built on the stack.
[[nodiscard]] rope_t assemble_error(const tlv_arena_t& a, const parsed_fwd_t& req,
                                    status_t status) {
    const std::array<std::byte, 9> tail{
        static_cast<std::byte>(std::to_underlying(type_t::STATUS)),
        static_cast<std::byte>(opt_t{.pl = true}.encode()),
        std::byte{5},
        std::byte{0},  // STATUS length = one 5-byte ERROR child
        static_cast<std::byte>(std::to_underlying(type_t::ERROR)),
        std::byte{0},
        std::byte{1},
        std::byte{0},  // ERROR length = 1
        std::byte{error_code(status)},
    };
    return assemble(a, req, reply_kind_t::ERROR, tail, {}, 0);
}

// A kind=RESULT reply whose single payload child is a stored view (data / slot read).
[[nodiscard]] rope_t assemble_result_view(const tlv_arena_t& a, const parsed_fwd_t& req,
                                          const view_t& payload) {
    const std::vector<view_t> shared{payload};
    return assemble(a, req, reply_kind_t::RESULT, {}, shared, payload.length);
}

// The vertex-map key for an arena-decoded PATH: span-aliased when canonical
// (ADR-0041 §3 — the PATH body IS the key, zero materialization), re-emitted
// into `fallback` otherwise (a foreign encoder's LL-widened / trailer-carrying
// NAMEs). Our own encoders always produce the canonical form.
[[nodiscard]] std::span<const std::byte> path_lookup_key(const tlv_arena_t& a, std::uint32_t path,
                                                         std::vector<std::byte>& fallback) {
    const arena_tlv_t& node = a[path];
    if (node.canonical_path) return node.body;
    const std::uint32_t end = node.end;
    for (std::uint32_t i = tlv_arena_t::first_child(path); i < end; i = a.next_sibling(i))
        detail::emit_name(fallback, a[i].body);
    return fallback;
}

}  // namespace

result_t<rope_t> op_resolver_t::resolve(const tlv_arena_t& fwd, std::string_view inbound_link) {
    result_t<parsed_fwd_t> parsed = parse_fwd(fwd);
    if (!parsed) return std::unexpected(parsed.error());
    const parsed_fwd_t& req = *parsed;
    if (req.op == fwd_op_t::REPLY) return std::unexpected(status_t::INVALID_PATH);

    // Decode the optional :field selector and the wildcard deferral: a [*] level
    // on a non-subscriber-path target is rejected with INVALID_PATH.
    field_path_t field;
    const bool has_field = req.selector != 0;
    if (has_field) {
        bool wildcard = false;
        result_t<field_path_t> f = selector_to_field(fwd, req.selector, wildcard);
        if (!f) return assemble_error(fwd, req, status_t::INVALID_PATH);
        field = std::move(*f);
        if (wildcard && (field.steps.empty() || field.steps[0].name != "subscribers"))
            return assemble_error(fwd, req, status_t::INVALID_PATH);
    }

    // dst resolution is the router's PATH-keyed dispatch — span-aliased for a
    // canonical PATH (ADR-0041 §3: the frame IS the key). Local-only: a dst
    // naming a transport child / unknown path is not local => ERROR(NOT_FOUND).
    std::vector<std::byte> key_fallback;
    vertex_t* v = graph_.find(path_lookup_key(fwd, req.dst, key_fallback));
    if (!v) return assemble_error(fwd, req, status_t::NOT_FOUND);

    switch (req.op) {
        case fwd_op_t::READ: {
            if (has_field && is_subscribers_array(field)) {
                result_t<std::vector<view_t>> subs = graph_.read_subscribers(v);
                if (!subs) return assemble_error(fwd, req, subs.error());
                std::size_t sub_len = 0;
                for (const view_t& s : *subs) sub_len += s.length;
                // PL=1 wrapper (POINT) whose children are the slot SUBSCRIBER views,
                // roped on zero-copy. POINT is the structured introspection-result
                // container already used for :schema and vertex enumeration.
                std::array<std::byte, 6> wrapper;
                const bool wll = sub_len > 0xFFFFu;
                emit_cursor_t wout{wrapper.data()};
                wout.struct_header(type_t::POINT, wll, sub_len);
                return assemble(fwd, req, reply_kind_t::RESULT,
                                std::span<const std::byte>(wrapper.data(), wll ? 6u : 4u), *subs,
                                sub_len);
            }
            result_t<view_t> r = has_field ? graph_.read(v, field) : graph_.read(v);
            if (!r) return assemble_error(fwd, req, r.error());
            return assemble_result_view(fwd, req, *r);
        }
        case fwd_op_t::WRITE: {
            if (req.payload == 0) return assemble_error(fwd, req, status_t::TYPE_MISMATCH);
            // The one ownership copy of the written value (ADR-0041 §2):
            // trailer-sliced by construction (§4 — an arriving CRC/TS trailer is
            // NOT stored; stored TLVs are trailer-less at rest, ADR-0035).
            // A wire TLV is never empty, so an empty result is exactly an
            // allocation failure → BACKPRESSURE.
            const arena_tlv_t& payload_node = fwd[req.payload];
            const view_t value = own_tlv(payload_node);
            if (value.empty()) return assemble_error(fwd, req, status_t::BACKPRESSURE);

            // A remote subscribe — a `:subscribers[]` APPEND that arrived over a transport
            // (inbound_link set) carrying a SUBSCRIBER — binds a REMOTE subscriber instead
            // of a local fan-out edge (#136). The slot retains this request's accumulated
            // return route (`src`, copied once, trailer-sliced) + the inbound link, so the
            // producer fan-out delivers FWD{WRITE}/COMPACT home. A bare local resolve
            // (no inbound_link) keeps the local field-write path unchanged.
            if (!inbound_link.empty() && has_field && is_subscribe_append(field) &&
                payload_node.type == type_t::SUBSCRIBER) {
                const arena_tlv_t& src_node = fwd[req.src];
                std::vector<std::byte> return_route(src_node.wire.begin(), src_node.wire.end());
                return_route[1] &= kStructOptMask;  // trailer-sliced copy (§4)
                result_t<void> w = graph_.add_remote_subscriber(
                    v, value, std::move(return_route), std::string(inbound_link),
                    subscriber_compact(fwd, req.payload));
                if (!w) return assemble_error(fwd, req, w.error());
                return assemble(fwd, req, reply_kind_t::RESULT, {}, {}, 0);  // OK, empty payload
            }

            result_t<void> w = graph_.write(v, has_field ? field : field_path_t{}, value);
            if (!w) return assemble_error(fwd, req, w.error());
            return assemble(fwd, req, reply_kind_t::RESULT, {}, {}, 0);  // OK, empty payload
        }
        case fwd_op_t::AWAIT: {
            const std::chrono::nanoseconds timeout =
                req.has_await_timeout ? std::chrono::nanoseconds(req.await_timeout)
                                      : kDefaultAwaitTimeout;
            result_t<view_t> r = graph_.await(v, timeout);
            if (!r) return assemble_error(fwd, req, r.error());  // TIMEOUT => ERROR(0x08)
            return assemble_result_view(fwd, req, *r);
        }
        case fwd_op_t::REPLY:
            break;  // unreachable — handled above
    }
    return std::unexpected(status_t::INVALID_PATH);
}

}  // namespace tr::graph
