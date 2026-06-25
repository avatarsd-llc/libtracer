// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/path.hpp"

#include <charconv>

#include "libtracer/tlv_emit.hpp"

namespace tracer::graph {
namespace {

// Parse one field step: "name", "name[3]", or "name[]".
[[nodiscard]] Result<FieldStep> parse_step(std::string_view step) {
    FieldStep fs;
    const std::size_t br = step.find('[');
    if (br == std::string_view::npos) {
        fs.name = std::string(step);
    } else {
        if (step.back() != ']') return std::unexpected(Status::InvalidPath);
        fs.name = std::string(step.substr(0, br));
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
                return std::unexpected(Status::InvalidPath);
            fs.index = static_cast<std::uint16_t>(value);
        }
    }
    if (fs.name.empty()) return std::unexpected(Status::InvalidPath);
    return fs;
}

}  // namespace

Result<Path> Path::parse(std::string_view text) {
    // Split off the field tail at the first ':'.
    std::string_view addr = text;
    std::string_view field_text;
    if (const std::size_t colon = text.find(':'); colon != std::string_view::npos) {
        addr = text.substr(0, colon);
        field_text = text.substr(colon + 1);
    }

    // The address must be rooted at '/'.
    if (addr.empty() || addr.front() != '/') return std::unexpected(Status::InvalidPath);
    // Strip trailing slashes (but keep the root "/").
    while (addr.size() > 1 && addr.back() == '/') addr.remove_suffix(1);

    Path p;
    // Root "/" is zero segments (the graph root); otherwise split on '/'.
    if (addr != "/") {
        std::size_t pos = 1;  // skip the leading '/'
        for (;;) {
            const std::size_t slash = addr.find('/', pos);
            const std::size_t end = (slash == std::string_view::npos) ? addr.size() : slash;
            const std::string_view seg = addr.substr(pos, end - pos);
            if (seg.empty()) return std::unexpected(Status::InvalidPath);  // "//"
            if (seg.size() > kMaxSegmentBytes) return std::unexpected(Status::InvalidPath);
            if (++p.segments_ > kMaxSegments) return std::unexpected(Status::InvalidPath);
            detail::emit_name(p.payload_, seg);
            if (p.payload_.size() > kMaxPathBytes) return std::unexpected(Status::InvalidPath);
            if (slash == std::string_view::npos) break;
            pos = slash + 1;
        }
    }

    // Parse the field tail: dot-separated steps, each optionally "[N]" or "[]".
    if (!field_text.empty()) {
        std::size_t pos = 0;
        for (;;) {
            const std::size_t dot = field_text.find('.', pos);
            const std::size_t end = (dot == std::string_view::npos) ? field_text.size() : dot;
            const std::string_view step = field_text.substr(pos, end - pos);
            if (step.empty()) return std::unexpected(Status::InvalidPath);
            auto fs = parse_step(step);
            if (!fs) return std::unexpected(fs.error());
            p.field_.steps.push_back(std::move(*fs));
            if (p.field_.steps.size() > kMaxFieldDepth) return std::unexpected(Status::InvalidPath);
            if (dot == std::string_view::npos) break;
            pos = dot + 1;
        }
    }

    return p;
}

std::size_t PathKeyHash::operator()(const PathKey& k) const noexcept {
    // FNV-1a over the canonical payload bytes.
    std::size_t h = 1469598103934665603ull;
    for (std::byte b : k.bytes) {
        h ^= std::to_integer<std::uint8_t>(b);
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace tracer::graph
