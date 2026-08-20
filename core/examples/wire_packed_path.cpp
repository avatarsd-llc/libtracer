/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a `PATH` body is packed segment records, and IS the vertex-map key.
 *
 * Since RFC-0018 a `PATH` (`0x06`) body is `opt.PL = 0` and holds a self-delimiting run of
 * **segment records**, each `[u8 len][len bytes of UTF-8]` — no per-segment type byte, no
 * per-segment option byte, hence exactly ONE spelling per address. That is what lets the
 * vertex map be keyed on the body bytes directly: this example builds `/sensor/temp`'s body
 * by hand and checks it is byte-identical to what `tr::graph::path_t` parsed from the
 * string (`docs/reference/03-addressing.md`, `CONTEXT.md` §Segment / view).
 *
 * The decoded `PATH` has ZERO children, which is the RFC-0018 change in one assertion.
 *
 * Runs under ctest as `example_wire_packed_path`; returns non-zero on any failed check.
 */

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "libtracer/packed_path.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

}  // namespace

int main() {
    bool ok = true;

    // Two segment records, appended in order: 06 "sensor", 04 "temp" — 12 bytes.
    std::vector<std::byte> body;
    check(ok, tr::wire::emit_path_segment(body, "sensor"), "the first segment record appends");
    check(ok, tr::wire::emit_path_segment(body, "temp"), "the second appends after it");
    std::printf("/sensor/temp packs into %zu body bytes\n", body.size());
    check(ok, body.size() == (1 + 6) + (1 + 4), "each record costs one length byte plus its text");
    check(ok, tr::wire::packed_path_valid_key(body), "the body tiles exactly into literal records");

    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::PATH, opt_t{}, body);
    const auto decoded = tr::wire::decode(frame);
    check(ok, decoded.has_value(), "the PATH TLV decodes");
    check(ok, decoded && decoded->children.empty(),
          "a PATH has no children — the records are the body (RFC-0018)");

    // The key the graph would look this up by, from both directions.
    const auto key = decoded ? tr::wire::path_key(*decoded) : std::nullopt;
    // The path OBJECT owns the key bytes, so it has to outlive the span it hands out.
    const tr::graph::path_t path("/sensor/temp");
    const std::span<const std::byte> parsed = path.key();
    check(ok, key.has_value(), "path_key answers for a well-framed literal body");
    check(ok, key && std::equal(key->begin(), key->end(), parsed.begin(), parsed.end()),
          "and the bytes match what path_t parsed from the string");
    return ok ? 0 : 1;
}
