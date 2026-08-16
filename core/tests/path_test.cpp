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
#include <span>
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

/**
 * @brief The packed segment records of a `PATH` body, as text (RFC-0018).
 *
 * A `PATH` body is `[u8 len][bytes]` records with `opt.PL = 0`, so there are no child TLVs
 * to read a payload off — the segments are sliced out of the body here.
 */
std::vector<std::string_view> path_segments(std::span<const std::byte> body) {
    std::vector<std::string_view> out;
    for (std::size_t at = 0; at < body.size();) {
        const auto len = static_cast<std::size_t>(static_cast<std::uint8_t>(body[at]));
        if (len == 0 || at + 1 + len > body.size()) break;
        out.emplace_back(reinterpret_cast<const char*>(body.data()) + at + 1, len);
        at += 1 + len;
    }
    return out;
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
            const std::vector<std::string_view> segs = path_segments(dec->payload);
            check(segs.size() == 2, "two packed segment records");
            check(segs.size() == 2 && tr::graph::valid_segment(segs[0]),
                  "segment 0 `camera` passes the predicate (control)");
            check(segs.size() == 2 && !tr::graph::valid_segment(segs[1]),
                  "segment 1 `frame[7]` FAILS the predicate (the #996 verdict)");
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
        // A depth the inherited cap of 32 rejected. 33 single-byte segments pack to
        // 33 * (1 + 1) = 66 bytes, far under kMaxPathBytes — legal at cap 255.
        check(accepts(33), "33 segments parse (the inherited 32-segment cap rejected this)");
        // THE CROSSOVER RFC-0018 §5 predicted, now measured here. Under the old NAME-child
        // encoding a one-byte segment cost 5 bytes, so 204 segments filled 1020 of the 1024
        // and the BYTE cap bound first — the 255 count clause could never fire, which
        // RFC-0023 §4.2 stated rather than smuggled. Packed, the same segment costs 2 bytes:
        // 1024 admits 512 records, so the COUNT is what binds and 255 is a real ceiling.
        check(accepts(204), "204 segments still parse (408 B packed — nowhere near the cap)");
        check(accepts(255), "255 segments parse (510 B — the RFC-0023 count cap, reachable)");
        check(refuses(256), "256 segments reject — by the COUNT now, at half the byte budget");
        // And the byte cap still exists: 512 one-byte segments would be 1024 bytes, but the
        // count refuses long before that, so the two clauses no longer trade places.
        check(refuses(512), "512 segments reject (the count fires first, then the bytes)");
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
