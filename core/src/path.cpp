/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/path.hpp"

#include <charconv>

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

/** @brief Parse one field step: "name", "name[3]", or "name[]". */
[[nodiscard]] result_t<field_step_t> parse_step(std::string_view step) {
    field_step_t fs;
    const std::size_t br = step.find('[');
    if (br == std::string_view::npos) {
        fs.name = std::string(step);
    } else {
        if (step.back() != ']') return std::unexpected(status_t::INVALID_PATH);
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
                return std::unexpected(status_t::INVALID_PATH);
            fs.index = static_cast<std::uint16_t>(value);
        }
    }
    if (fs.name.empty()) return std::unexpected(status_t::INVALID_PATH);
    return fs;
}

}  // namespace

result_t<path_t> path_t::parse(std::string_view text) {
    // Split off the field tail at the first ':'.
    std::string_view addr = text;
    std::string_view field_text;
    if (const std::size_t colon = text.find(':'); colon != std::string_view::npos) {
        addr = text.substr(0, colon);
        field_text = text.substr(colon + 1);
    }

    // The address must be rooted at '/'.
    if (addr.empty() || addr.front() != '/') return std::unexpected(status_t::INVALID_PATH);
    // Strip trailing slashes (but keep the root "/").
    while (addr.size() > 1 && addr.back() == '/') addr.remove_suffix(1);

    path_t p;
    // Root "/" is zero segments (the graph root); otherwise split on '/'.
    if (addr != "/") {
        // Pre-size the payload EXACTLY, before a single byte is appended. The emitter grows
        // `payload_` by geometric doubling, so a two-segment path used to walk a 1→2→4→8→16
        // realloc chain — four throwaway blocks and four frees to build fifteen bytes. That
        // is not a registration cost: EVERY path-keyed read, write and subscribe parses a
        // path first, so the whole codebase paid it per operation, and on an MCU allocator a
        // malloc/free round-trip is hundreds of nanoseconds rather than the tens glibc's
        // tcache serves it in.
        //
        // The size is exact, not an estimate. `addr` is rooted and has no trailing or
        // doubled slashes by this point, so each segment is preceded by exactly one '/':
        // the separator count IS the segment count, the segment bytes total
        // `addr.size() - nsegs`, and each PACKED record adds exactly ONE length byte
        // (RFC-0018) — giving `nsegs + (addr.size() - nsegs)` = `addr.size()`. The whole
        // canonical key is now the address string's own length, one byte per '/' included.
        std::size_t nsegs = 0;
        for (const char c : addr) {
            if (c == '/') ++nsegs;
        }
        // Reserve only within the bound the loop below enforces anyway, so a garbage address
        // cannot make this allocate more than a well-formed one of the same length would.
        if (const std::size_t want = addr.size(); want <= kMaxPathBytes) p.payload_.reserve(want);

        std::size_t pos = 1;  // skip the leading '/'
        for (;;) {
            const std::size_t slash = addr.find('/', pos);
            const std::size_t end = (slash == std::string_view::npos) ? addr.size() : slash;
            const std::string_view seg = addr.substr(pos, end - pos);
            // empty ("//"), reserved character, or over-long — one predicate, shared
            // with the wire creation boundary (ADR-0073 §1).
            if (!valid_segment(seg)) return std::unexpected(status_t::INVALID_PATH);
            if (++p.segments_ > kMaxSegments) return std::unexpected(status_t::INVALID_PATH);
            // `valid_segment` already bounded the segment at `kMaxSegmentBytes` (64) and
            // refused an empty one, so the packed emitter cannot refuse here — but its
            // answer is checked rather than discarded, because THAT bound is a build-time
            // constant and the record's `u8` length field is the wire's (RFC-0018 §5).
            if (!wire::emit_path_segment(p.payload_, seg))
                return std::unexpected(status_t::INVALID_PATH);
            if (p.payload_.size() > kMaxPathBytes) return std::unexpected(status_t::INVALID_PATH);
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
            if (step.empty()) return std::unexpected(status_t::INVALID_PATH);
            auto fs = parse_step(step);
            if (!fs) return std::unexpected(fs.error());
            p.field_.steps.push_back(std::move(*fs));
            if (p.field_.steps.size() > kMaxFieldDepth)
                return std::unexpected(status_t::INVALID_PATH);
            if (dot == std::string_view::npos) break;
            pos = dot + 1;
        }
    }

    return p;
}

namespace {
/**
 * @brief FNV-1a 64-bit over the canonical payload bytes.
 *
 * The 64-bit offset basis and prime
 * can't live in a std::size_t on a 32-bit target (std::size_t is unsigned int on
 * RISC-V MCUs), so accumulate in std::uint64_t and narrow to std::size_t for the
 * bucket index via an explicit cast — the 64-bit state keeps the avalanche, and the
 * cast avoids -Woverflow (GCC 15, newly strict) on 32-bit. The owned-key and by-span
 * hashers share this so a heterogeneous lookup hashes identically to the stored key.
 */
[[nodiscard]] std::size_t fnv1a_key(std::span<const std::byte> bytes) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (std::byte b : bytes) {
        h ^= std::to_integer<std::uint8_t>(b);
        h *= 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
}
}  // namespace

/**
 * @brief RFC-0027 §6.1's origin-side cache — validate a minted spelling, then keep it.
 *
 * The walk is `wire::path_element_census`, which is the same reader a forwarder uses, so the
 * origin cannot come to a different conclusion about a body than the hop that wrote it. This
 * validator lives here rather than in the header because it is the only place `path.hpp` would
 * otherwise have to pull the element codec into every translation unit that merely holds a
 * `path_t` — a core that never sees a path label neither includes nor instantiates it.
 */
bool path_t::cache_path_label(std::span<const std::byte> body) {
    if (binding_.bound) return false;  // RFC-0027 §11.2 — one compression per address
    if (body.empty() || body.size() > kMaxPathBytes) return false;
    const wire::path_element_census_t census = wire::path_element_census(body);
    if (!census.well_formed || census.labels == 0) return false;
    labels_.body.assign(body.begin(), body.end());
    labels_.cached = true;
    return true;
}

std::size_t path_key_hash_t::operator()(const path_key_t& k) const noexcept {
    return fnv1a_key(k.bytes());
}

std::size_t path_key_hash_t::operator()(std::span<const std::byte> k) const noexcept {
    return fnv1a_key(k);
}

}  // namespace tr::graph
