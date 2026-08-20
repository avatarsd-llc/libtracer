/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the escape record: admissible in a frame path, rejected as a key.
 *
 * `len == 0` cannot spell a segment record (path syntax has no empty segment), so RFC-0018
 * §5.4 reserves it for the **escape record** `00 <u8 kind> <u8 len> <len bytes>`. Its whole
 * point is asymmetry (`CONTEXT.md` §Segment / view): a forwarder steps over a `kind` it does
 * not implement **by the record's declared length** rather than dropping a frame it is only
 * relaying, while canonical / key context REFUSES it — a label is not canonical bytes, and
 * the vertex-map key must stay pure-string.
 *
 * Both halves are checked here, including the skip over a kind this build has never heard
 * of. `kPackedEscapeKindLabel` (`0x16`) is the kind RFC-0027's path-label element uses.
 * **This layer never mints one**: `packed_path.hpp` is kind-agnostic and emitting an escape
 * is not minting a label — that is the forwarder's business, and only on a node given a
 * mint table (RFC-0027 §8.3, off by default). Here the escape is simply bytes that arrived.
 *
 * Runs under ctest as `example_wire_path_escape`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
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

/** @brief A `kind` byte this build assigns no meaning to — the non-implementing hop's case. */
constexpr std::uint8_t kUnknownKind = 0x7F;

}  // namespace

int main() {
    bool ok = true;
    const std::vector<std::byte> payload(4, std::byte{0xEE});

    // "sensor" then an escape of an UNKNOWN kind then "temp".
    std::vector<std::byte> body;
    check(ok, tr::wire::emit_path_segment(body, "sensor"), "a literal record leads");
    const std::size_t at = body.size();
    check(ok, tr::wire::emit_path_escape(body, kUnknownKind, payload), "the escape record appends");
    check(ok, tr::wire::emit_path_segment(body, "temp"), "and a literal record follows it");

    const std::span<const std::byte> view(body);
    check(ok, tr::wire::packed_record_is_escape(view, at),
          "the record at that offset is an escape");
    check(ok, tr::wire::packed_escape_kind(view, at) == kUnknownKind, "its kind reads back");
    const auto esc = tr::wire::packed_escape_payload(view, at);
    check(ok, esc && esc->size() == payload.size(), "and its declared payload is delimited");

    // The forwarding property: step over it knowing nothing but its length.
    const std::size_t span = tr::wire::packed_record_span(view, at);
    std::printf("stepped over kind 0x%02X in %zu bytes without implementing it\n", kUnknownKind,
                span);
    check(ok, span == tr::wire::kPackedEscapeOverhead + payload.size(),
          "packed_record_span steps over an unknown kind by its declared length");
    check(ok, tr::wire::packed_record_is_escape(view, at + span) == false,
          "landing exactly on the next literal record");

    // The other half of the asymmetry: this body is not a key and never becomes one.
    check(ok, !tr::wire::packed_path_valid_key(view), "an escape-bearing body is not a valid key");
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::PATH, opt_t{}, body);
    const auto decoded = tr::wire::decode(frame);
    check(ok, decoded.has_value(), "the frame itself is well-formed and decodes");
    check(ok, decoded && tr::wire::path_key(*decoded) == std::nullopt,
          "yet path_key refuses it — canonical context rejects the escape");
    return ok ? 0 : 1;
}
