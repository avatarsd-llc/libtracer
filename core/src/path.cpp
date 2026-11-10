/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/path.hpp"

#include <charconv>
#include <new>

#include "libtracer/mem_heap.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path_element.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::graph {

using wire::opt_t;
using wire::type_t;
namespace {
// The reserved-character / size / non-empty checks live in path.hpp's `valid_segment` —
// THE shared segment predicate (ADR-0073 §1), also enforced at the wire creation
// boundary (#688) so the two tiers cannot drift.

/** @brief Parse one field step: "name", "name[3]", or "name[]". The name views @p step. */
[[nodiscard]] result_t<field_step_t> parse_step(std::string_view step) {
    field_step_t fs;
    const std::size_t br = step.find('[');
    fs.name = step.substr(0, br);
    if (br != std::string_view::npos) {
        if (step.back() != ']') return std::unexpected(status_t::INVALID_PATH);
        fs.indexed = true;
        const std::string_view idx = step.substr(br + 1, step.size() - br - 2);
        if (idx.empty()) {
            fs.append = true;
        } else {
            unsigned value = 0;
            const auto* first = idx.data();
            const auto* last = idx.data() + idx.size();
            const auto [ptr, ec] = std::from_chars(first, last, value);
            if (ec != std::errc{} || ptr != last || value > 0xFFFFu)
                return std::unexpected(status_t::INVALID_PATH);
            fs.index = static_cast<std::uint16_t>(value);
        }
    }
    if (!valid_segment(fs.name)) return std::unexpected(status_t::INVALID_PATH);
    return fs;
}

/** @brief The field path of a path that has none: what @ref path_t::field answers then. */
const field_path_t kNoField{};

/** @brief Every block a path draws is aligned for the field path it may lead with. */
constexpr std::size_t kBlockAlign = alignof(field_path_t);

}  // namespace

result_t<path_t> path_t::parse(std::string_view text) {
    return parse(text, tr::mem::table_source());
}

result_t<path_t> path_t::parse(std::string_view text, tr::mem::block_source_t& src) {
    std::string_view addr = text;
    std::string_view field_text;
    if (const std::size_t colon = text.find(':'); colon != std::string_view::npos) {
        addr = text.substr(0, colon);
        field_text = text.substr(colon + 1);
    }

    // The address must be rooted at '/'.
    if (addr.empty() || addr.front() != '/') return std::unexpected(status_t::INVALID_PATH);
    // Strip trailing slashes; the root "/" is zero segments (the graph root).
    while (addr.size() > 1 && addr.back() == '/') addr.remove_suffix(1);
    if (addr.size() == 1) addr = {};
    // One packed record per `/seg` is `[u8 len][seg]`, the same byte count as its text, so the
    // key is exactly `addr.size()` bytes, known before a byte is written.
    if (addr.size() > kMaxPathBytes) return std::unexpected(status_t::INVALID_PATH);

    // The steps first, their names viewing `text`; they are re-pointed into the block below.
    field_path_t field;
    std::size_t name_bytes = 0;
    if (!field_text.empty()) {
        std::size_t pos = 0;
        for (;;) {
            const std::size_t dot = field_text.find('.', pos);
            const std::size_t end = (dot == std::string_view::npos) ? field_text.size() : dot;
            const std::string_view step = field_text.substr(pos, end - pos);
            if (step.empty()) return std::unexpected(status_t::INVALID_PATH);
            auto fs = parse_step(step);
            if (!fs) return std::unexpected(fs.error());
            if (!field.steps.push_back(*fs)) return std::unexpected(status_t::INVALID_PATH);
            name_bytes += fs->name.size();
            if (dot == std::string_view::npos) break;
            pos = dot + 1;
        }
    }

    path_t p;
    p.src_ = &src;
    p.has_field_ = !field.empty();
    const std::size_t len = p.payload_off() + addr.size() + name_bytes;
    if (len != 0) {
        p.block_ = static_cast<std::byte*>(src.try_alloc(len, kBlockAlign));
        if (p.block_ == nullptr) return std::unexpected(status_t::BACKPRESSURE);
        p.block_len_ = static_cast<std::uint32_t>(len);
    }
    std::byte* at = p.block_ + p.payload_off();
    std::size_t segments = 0;
    for (std::size_t pos = 1; pos < addr.size();) {
        const std::size_t slash = addr.find('/', pos);
        const std::size_t end = (slash == std::string_view::npos) ? addr.size() : slash;
        const std::string_view seg = addr.substr(pos, end - pos);
        if (!valid_segment(seg) || ++segments > kMaxSegments)
            return std::unexpected(status_t::INVALID_PATH);
        at += wire::store_path_segment(std::span<std::byte>(at, 1 + seg.size()), seg);
        pos = end + 1;
    }
    p.segments_ = static_cast<std::uint8_t>(segments);
    p.key_len_ = static_cast<std::uint16_t>(addr.size());
    if (p.has_field_) {
        for (field_step_t& st : field.steps) {
            std::memcpy(at, st.name.data(), st.name.size());
            st.name = {reinterpret_cast<const char*>(at), st.name.size()};
            at += st.name.size();
        }
        new (p.block_) field_path_t(field);
    }
    return p;
}

const field_path_t& path_t::field() const noexcept {
    return has_field_ ? *std::launder(reinterpret_cast<const field_path_t*>(block_)) : kNoField;
}

tr::mem::block_source_t& path_t::source() const noexcept {
    return src_ != nullptr ? *src_ : tr::mem::table_source();
}

path_t::path_t(const path_t& o) { copy_from(o); }

path_t& path_t::operator=(const path_t& o) {
    if (this != &o) {
        release();
        copy_from(o);
    }
    return *this;
}

void path_t::copy_from(const path_t& o) {
    tr::mem::block_source_t& src = o.source();
    src_ = o.src_;
    key_len_ = o.key_len_;
    segments_ = o.segments_;
    has_field_ = o.has_field_;
    if (o.block_len_ != 0) {
        block_ = static_cast<std::byte*>(src.try_alloc(o.block_len_, kBlockAlign));
        if (block_ == nullptr) tr::mem::exhausted_at_init(src, "path_t");
        block_len_ = o.block_len_;
        std::memcpy(block_ + payload_off(), o.block_ + payload_off(), block_len_ - payload_off());
    }
    if (has_field_) {
        // The steps' names view the block they were parsed into; re-point them at this one.
        field_path_t f = o.field();
        for (field_step_t& st : f.steps)
            st.name = {reinterpret_cast<const char*>(block_) +
                           (reinterpret_cast<const std::byte*>(st.name.data()) - o.block_),
                       st.name.size()};
        new (block_) field_path_t(f);
    }
}

void path_t::release() noexcept {
    if (block_ != nullptr) source().release(block_, block_len_, kBlockAlign);
    block_ = nullptr;
    block_len_ = 0;
}

void path_t::take(path_t& o) noexcept {
    src_ = o.src_;
    block_ = std::exchange(o.block_, nullptr);
    block_len_ = std::exchange(o.block_len_, 0);
    key_len_ = std::exchange(o.key_len_, 0);
    segments_ = std::exchange(o.segments_, 0);
    has_field_ = std::exchange(o.has_field_, false);
}

}  // namespace tr::graph
