/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/op_resolve.hpp"

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
using wire::opt_t;
using wire::tlv_t;
using wire::type_t;

namespace {

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

// The canonical PATH-payload key (concatenated NAME children) of a decoded PATH TLV
// — the graph vertex-map key. Mirrors graph.cpp's path_child_key.
[[nodiscard]] std::vector<std::byte> path_tlv_key(const tlv_t& path) {
    std::vector<std::byte> key;
    for (const tlv_t& name : path.children) {
        const std::vector<std::byte> enc = wire::encode(name);  // 02 00 <len> <bytes>
        key.insert(key.end(), enc.begin(), enc.end());
    }
    return key;
}

// Append a structured-TLV header (type, opt.PL [+LL], little-endian length) for a
// body of `body_len` bytes — the body itself is appended by the caller or roped on.
void emit_struct_header(std::vector<std::byte>& out, type_t type, std::size_t body_len) {
    opt_t opt{.pl = true};
    if (body_len > 0xFFFFu) opt.ll = true;
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(type)));
    out.push_back(static_cast<std::byte>(opt.encode()));
    detail::append_le(out, static_cast<std::uint32_t>(body_len), opt.ll ? 4u : 2u);
}

// Append a 1-byte VALUE TLV (the op / kind discriminants).
void emit_u8_value(std::vector<std::byte>& out, std::uint8_t v) {
    const std::byte b{v};
    detail::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
}

// A decoded request FWD, its children borrowed from the input tlv_t.
struct parsed_fwd_t {
    fwd_op_t op{};
    const tlv_t* dst = nullptr;       // forward route (PATH)
    const tlv_t* selector = nullptr;  // optional :field (FIELD)
    const tlv_t* src = nullptr;       // accumulated return route (PATH)
    const tlv_t* payload = nullptr;   // WRITE only
    std::uint64_t await_timeout = 0;  // AWAIT only
    bool has_await_timeout = false;
};

// Parse the FWD child sequence positionally (RFC-0004 §B order: op, dst, FIELD?,
// src, [payload | await_timeout]). Returns INVALID_PATH for a structurally
// malformed frame (the resolver turns that into the error side, not a reply).
[[nodiscard]] result_t<parsed_fwd_t> parse_fwd(const tlv_t& fwd) {
    if (fwd.type != type_t::FWD || !fwd.opt.pl) return std::unexpected(status_t::INVALID_PATH);
    const std::vector<tlv_t>& ch = fwd.children;
    parsed_fwd_t p;
    std::size_t i = 0;
    if (i >= ch.size() || ch[i].type != type_t::VALUE)
        return std::unexpected(status_t::INVALID_PATH);
    p.op = static_cast<fwd_op_t>(detail::load_le<std::uint8_t>(ch[i].payload));
    ++i;
    if (i >= ch.size() || ch[i].type != type_t::PATH)
        return std::unexpected(status_t::INVALID_PATH);
    p.dst = &ch[i++];
    if (i < ch.size() && ch[i].type == type_t::FIELD) p.selector = &ch[i++];
    if (i >= ch.size() || ch[i].type != type_t::PATH)
        return std::unexpected(status_t::INVALID_PATH);
    p.src = &ch[i++];
    if (p.op == fwd_op_t::WRITE) {
        if (i < ch.size()) p.payload = &ch[i++];
    } else if (p.op == fwd_op_t::AWAIT) {
        if (i < ch.size() && ch[i].type == type_t::VALUE) {
            p.await_timeout = detail::load_le<std::uint64_t>(ch[i].payload);
            p.has_await_timeout = true;
            ++i;
        }
    }
    return p;
}

// FIELD index_mode (RFC-0004 §C, the optional u8 index_mode VALUE).
enum class index_mode_t : std::uint8_t { SCALAR = 0, ELEMENT = 1, WILDCARD = 2 };

// Decode a FIELD selector TLV into the graph's field_path_t. Each level is a NAME
// followed by 0/1/2 VALUE children: 0 => SCALAR; 1 => index_mode only (ELEMENT
// append "[]" or WILDCARD "[*]"); 2 => [index u32, index_mode u8] ("[N]").
// `wildcard_seen` is set if any level carries index_mode=WILDCARD.
[[nodiscard]] result_t<field_path_t> selector_to_field(const tlv_t& field, bool& wildcard_seen) {
    field_path_t fp;
    const std::vector<tlv_t>& ch = field.children;
    std::size_t i = 0;
    while (i < ch.size()) {
        if (ch[i].type != type_t::NAME) return std::unexpected(status_t::INVALID_PATH);
        field_step_t step;
        const std::span<const std::byte> nb = ch[i].payload;
        step.name.assign(reinterpret_cast<const char*>(nb.data()), nb.size());
        ++i;
        const tlv_t* v0 = nullptr;
        const tlv_t* v1 = nullptr;
        if (i < ch.size() && ch[i].type == type_t::VALUE) v0 = &ch[i++];
        if (i < ch.size() && ch[i].type == type_t::VALUE) v1 = &ch[i++];
        index_mode_t mode = index_mode_t::SCALAR;
        bool has_index = false;
        std::uint32_t index = 0;
        if (v0 && v1) {
            has_index = true;
            index = detail::load_le<std::uint32_t>(v0->payload);
            mode = static_cast<index_mode_t>(detail::load_le<std::uint8_t>(v1->payload));
        } else if (v0) {
            mode = static_cast<index_mode_t>(detail::load_le<std::uint8_t>(v0->payload));
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

// Assemble the FWD{REPLY} rope: a fresh head (FWD header + op=REPLY + dst=req.src +
// src=req.dst + kind + `inline_tail`) prepended to `shared` (refcount clones of the
// stored payload view(s)). The head is the only allocation; `shared` is never copied
// — RFC-0004 §D / ADR-0035 zero-copy reply rule.
[[nodiscard]] rope_t assemble(const parsed_fwd_t& req, reply_kind_t kind,
                              std::span<const std::byte> inline_tail,
                              const std::vector<view_t>& shared, std::size_t shared_len) {
    std::vector<std::byte> head_children;
    emit_u8_value(head_children, static_cast<std::uint8_t>(fwd_op_t::REPLY));
    {
        const std::vector<std::byte> dst = wire::encode(*req.src);  // reply dst = request src
        head_children.insert(head_children.end(), dst.begin(), dst.end());
    }
    {
        const std::vector<std::byte> src = wire::encode(*req.dst);  // reply src = responder ep
        head_children.insert(head_children.end(), src.begin(), src.end());
    }
    emit_u8_value(head_children, static_cast<std::uint8_t>(kind));
    head_children.insert(head_children.end(), inline_tail.begin(), inline_tail.end());

    const std::size_t body_len = head_children.size() + shared_len;
    std::vector<std::byte> head;
    emit_struct_header(head, type_t::FWD, body_len);
    head.insert(head.end(), head_children.begin(), head_children.end());

    segment_ptr_t seg = view::heap_alloc(head.size());
    rope_t rope;
    if (seg) {
        std::memcpy(seg->bytes.data(), head.data(), head.size());
        rope.append(view_t::over(std::move(seg)));
    }
    for (const view_t& v : shared) rope.append(v);  // refcount clone — no byte copy
    return rope;
}

// A kind=ERROR reply carrying STATUS{ ERROR u8=<code> } (RFC-0004 §D, provisional #8).
[[nodiscard]] rope_t assemble_error(const parsed_fwd_t& req, status_t status) {
    std::vector<std::byte> err;
    const std::byte code{error_code(status)};
    detail::emit_tlv(err, type_t::ERROR, opt_t{}, std::span<const std::byte>(&code, 1));
    std::vector<std::byte> statust;
    detail::emit_tlv(statust, type_t::STATUS, opt_t{.pl = true}, err);
    return assemble(req, reply_kind_t::ERROR, statust, {}, 0);
}

// A kind=RESULT reply whose single payload child is a stored view (data / slot read).
[[nodiscard]] rope_t assemble_result_view(const parsed_fwd_t& req, const view_t& payload) {
    const std::vector<view_t> shared{payload};
    return assemble(req, reply_kind_t::RESULT, {}, shared, payload.length);
}

}  // namespace

result_t<rope_t> op_resolver_t::resolve(const tlv_t& fwd) {
    result_t<parsed_fwd_t> parsed = parse_fwd(fwd);
    if (!parsed) return std::unexpected(parsed.error());
    const parsed_fwd_t& req = *parsed;
    if (req.op == fwd_op_t::REPLY) return std::unexpected(status_t::INVALID_PATH);

    // Decode the optional :field selector and apply the slice-2 wildcard deferral:
    // a [*] level on a non-subscriber-path target is rejected with INVALID_PATH.
    field_path_t field;
    const bool has_field = req.selector != nullptr;
    if (has_field) {
        bool wildcard = false;
        result_t<field_path_t> f = selector_to_field(*req.selector, wildcard);
        if (!f) return assemble_error(req, status_t::INVALID_PATH);
        field = std::move(*f);
        if (wildcard && (field.steps.empty() || field.steps[0].name != "subscribers"))
            return assemble_error(req, status_t::INVALID_PATH);
    }

    // dst resolution is the router's PATH-keyed dispatch. Slice 2 is local-only: a
    // dst naming a transport child / unknown path is not local => ERROR(NOT_FOUND).
    vertex_t* v = graph_.find(path_tlv_key(*req.dst));
    if (!v) return assemble_error(req, status_t::NOT_FOUND);

    switch (req.op) {
        case fwd_op_t::READ: {
            if (has_field && is_subscribers_array(field)) {
                result_t<std::vector<view_t>> subs = graph_.read_subscribers(v);
                if (!subs) return assemble_error(req, subs.error());
                std::size_t sub_len = 0;
                for (const view_t& s : *subs) sub_len += s.length;
                // PL=1 wrapper (POINT) whose children are the slot SUBSCRIBER views,
                // roped on zero-copy. POINT is the structured introspection-result
                // container already used for :schema and vertex enumeration.
                std::vector<std::byte> wrapper;
                emit_struct_header(wrapper, type_t::POINT, sub_len);
                return assemble(req, reply_kind_t::RESULT, wrapper, *subs, sub_len);
            }
            result_t<view_t> r = has_field ? graph_.read(v, field) : graph_.read(v);
            if (!r) return assemble_error(req, r.error());
            return assemble_result_view(req, *r);
        }
        case fwd_op_t::WRITE: {
            if (!req.payload) return assemble_error(req, status_t::TYPE_MISMATCH);
            // The reply for a WRITE carries no shared payload, so re-encoding the
            // value into a fresh segment to write it is fine (the zero-copy rule
            // governs the REPLY payload, which here is an empty OK).
            const std::vector<std::byte> enc = wire::encode(*req.payload);
            segment_ptr_t seg = view::heap_alloc(enc.size());
            if (!seg) return assemble_error(req, status_t::BACKPRESSURE);
            std::memcpy(seg->bytes.data(), enc.data(), enc.size());
            const view_t value = view_t::over(std::move(seg));
            result_t<void> w = graph_.write(v, has_field ? field : field_path_t{}, value);
            if (!w) return assemble_error(req, w.error());
            return assemble(req, reply_kind_t::RESULT, {}, {}, 0);  // OK, empty payload
        }
        case fwd_op_t::AWAIT: {
            const std::chrono::nanoseconds timeout =
                req.has_await_timeout ? std::chrono::nanoseconds(req.await_timeout)
                                      : kDefaultAwaitTimeout;
            result_t<view_t> r = graph_.await(v, timeout);
            if (!r) return assemble_error(req, r.error());  // TIMEOUT => ERROR(0x08)
            return assemble_result_view(req, *r);
        }
        case fwd_op_t::REPLY:
            break;  // unreachable — handled above
    }
    return std::unexpected(status_t::INVALID_PATH);
}

}  // namespace tr::graph
