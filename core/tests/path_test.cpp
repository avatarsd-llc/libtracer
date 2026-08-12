/**
 * @file
 * @brief Path parsing / validation (docs/reference/03-addressing.md).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Focus: reserved-
 * character rejection in NAME segments (§Reserved characters), the structural
 * limits, and that the field tail (dot-separated, with [N]/[]) still parses.
 */

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/frame.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::status_t;

using tr::testing::check;

/** @brief The raw bytes of a conformance vector's `input.bin`. */
std::vector<std::byte> vector_bytes(std::string_view case_dir) {
    const std::filesystem::path p =
        std::filesystem::path{LIBTRACER_VECTORS_DIR} / case_dir / "input.bin";
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    return out;
}

/** @brief A NAME child's payload as text (the segment bytes a PATH child carries). */
std::string_view name_text(const tr::wire::tlv_t& name) {
    return {reinterpret_cast<const char*>(name.payload.data()), name.payload.size()};
}

void ok_parse(std::string_view text) {
    const auto r = path_t::parse(text);
    check(r.has_value(), text);
}

void rejected(std::string_view text) {
    const auto r = path_t::parse(text);
    check(!r.has_value() && r.error() == status_t::INVALID_PATH, text);
}

/** @brief `"/a/a/…"` with @p n single-byte segments — a depth probe, printed by count. */
std::string repeat_segments(std::size_t n) {
    std::string s;
    s.reserve(2 * n);
    for (std::size_t i = 0; i < n; ++i) s += "/a";
    return s;
}

}  // namespace

int main() {
    std::printf("Valid paths parse:\n");
    ok_parse("/");
    ok_parse("/sensor/temp");
    ok_parse("/a/b/c");
    ok_parse("/i2c-bus/0x68/accel");
    ok_parse("/x:settings.anything");  // dot is the field-chain separator (not a NAME char)
    ok_parse("/x:subscribers[]");      // append field
    ok_parse("/x:subscribers[3]");     // indexed field

    std::printf("Reserved characters in a NAME segment are rejected (reference/03 §Reserved):\n");
    rejected("/a.b");  // '.' is the field separator, illegal in a NAME
    rejected("/a*b");  // '*' wildcard
    rejected("/a?b");  // '?' reserved
    rejected("/sensor/te.mp");
    rejected("/a/b*/c");
    // '[' / ']' — the two the pre-#996 predicate admitted (Rust/TS always rejected
    // them, per the reference/03 MUST). Index addressing, if it lands, lives OUTSIDE
    // the NAME bytes, so a bracketed ADDRESS segment is invalid, full stop.
    rejected("/camera/frame[7]");
    rejected("/camera/frame[]");
    rejected("/a[b");
    rejected("/a]b");

    std::printf("The full reserved set is exactly the seven of reference/03 (#996):\n");
    {
        for (const char c : std::string_view{"/:.[]*?"}) {
            const std::string seg = std::string("a") + c + "b";
            check(!tr::graph::valid_segment(seg), seg);
        }
        check(tr::graph::valid_segment("frame"), "control: `frame` is a valid segment");
        check(!tr::graph::valid_segment("frame[7]"), "`frame[7]` is NOT a valid segment");
    }

    std::printf("The shared vector path/path-reserved-brackets pins the verdict cross-tier:\n");
    {
        // The vector is CODEC-legal (every harness round-trips it); what each tier's
        // host suite pins is its OWN segment predicate's verdict over the vector's
        // NAME payloads. Rust: bindings/rust/tests/conformance_vectors.rs. TS:
        // bindings/typescript/packages/client/test/vectors.test.mjs.
        const auto bytes = vector_bytes("path/path-reserved-brackets");
        check(!bytes.empty(), "vector input.bin found");
        const auto dec = tr::wire::decode(bytes);
        check(dec.has_value(), "the vector decodes (codec-tier: NAME bytes are free)");
        if (dec) {
            check(tr::wire::encode(*dec) == bytes, "round-trip is byte-exact");
            check(dec->children.size() == 2, "two NAME children");
            check(tr::graph::valid_segment(name_text(dec->children[0])),
                  "child 0 `camera` passes the predicate (control)");
            check(!tr::graph::valid_segment(name_text(dec->children[1])),
                  "child 1 `frame[7]` FAILS the predicate (the #996 verdict)");
        }
    }

    std::printf("Structural limits still hold:\n");
    rejected("relative/no/root");  // must be rooted at '/'
    rejected("/a//b");             // empty segment

    std::printf("The segment cap is 255, and the byte cap binds first (RFC-0023):\n");
    {
        const auto accepts = [](std::size_t n) {
            const auto r = path_t::parse(repeat_segments(n));
            return r.has_value() && r->segment_count() == n;
        };
        const auto refuses = [](std::size_t n) {
            const auto r = path_t::parse(repeat_segments(n));
            return !r.has_value() && r.error() == status_t::INVALID_PATH;
        };
        // A depth the inherited cap of 32 rejected. 33 single-byte segments encode to
        // 33 * (4 + 1) = 165 bytes, far under kMaxPathBytes — legal at cap 255.
        check(accepts(33), "33 segments parse (the inherited 32-segment cap rejected this)");
        // 204 segments = 1020 bytes: the byte-derived ceiling under this body encoding, and
        // the deepest path today's NAME-TLV grammar can express (RFC-0023 §4.2).
        check(accepts(204), "204 segments parse (1020 B — the byte-derived ceiling)");
        // 205 segments = 1025 bytes. Rejected by kMaxPathBytes, NOT by kMaxSegments: under
        // this encoding the count clause can never fire, which RFC-0023 §4.2 states rather
        // than smuggles. The count becomes binding only under RFC-0018's packed body.
        check(refuses(205), "205 segments reject (1025 B — the BYTE cap, not the count)");
        check(refuses(256), "256 segments reject (over both caps)");
    }

    std::printf("The parse-once constructor yields the same key as parse():\n");
    {
        // path_t("literal") parses once; the key must be byte-identical to parse()'s,
        // so a held path is a drop-in for `*path_t::parse(...)` at every call site.
        const path_t ctor("/sensor/temp");
        const auto viaparse = path_t::parse("/sensor/temp");
        check(viaparse.has_value(), "control path parses");
        check(viaparse && std::ranges::equal(ctor.key(), viaparse->key()),
              "path_t(\"/sensor/temp\").key() == parse(\"/sensor/temp\").key()");
        // Held once, reused — the point of the constructor (no re-parse per use).
        const path_t held("/x:subscribers[3]");
        check(std::ranges::equal(held.key(), held.key()) && held.field() == held.field(),
              "a held path_t is stable across reuse (field tail preserved)");
    }

    return tr::testing::summary("path");
}
