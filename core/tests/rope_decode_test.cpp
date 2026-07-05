/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Differential test for the rope-aware grammar (ADR-0048 §1): wire::validate_rope
 * over a scatter-gather rope MUST reach the exact same verdict as wire::decode over
 * the equivalent flat bytes — for every adversarial split, including splits that
 * fall mid-header, mid-trailer, and mid-payload. This is the proof obligation
 * ADR-0048 §consequences names: "same bytes split at adversarial link boundaries
 * MUST decode identically to the contiguous case." Valid frames validate; every
 * corruption is rejected with the identical err_t no matter where the rope is cut.
 */

#include "libtracer/rope_decode.hpp"

#include <cstddef>
#include <cstdio>
#include <string_view>
#include <vector>

#include "libtracer/frame.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/view.hpp"

namespace {

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// Owns the per-link byte copies and the rope viewing them (borrowed, not copied
// again) — the rope's links point into `parts`, which must outlive it.
struct split_rope_t {
    std::vector<std::vector<std::byte>> parts;
    tr::view::rope_t rope;
};

// Split `flat` into contiguous links at `cuts` (strictly increasing, interior
// offsets) and view each as a borrowed rope link. Builds all parts first (reserved,
// no realloc) so the borrowed pointers stay valid.
split_rope_t split_into(const std::vector<std::byte>& flat, const std::vector<std::size_t>& cuts) {
    split_rope_t out;
    out.parts.reserve(cuts.size() + 1);
    std::size_t prev = 0;
    for (const std::size_t c : cuts) {
        out.parts.emplace_back(flat.begin() + static_cast<std::ptrdiff_t>(prev),
                               flat.begin() + static_cast<std::ptrdiff_t>(c));
        prev = c;
    }
    out.parts.emplace_back(flat.begin() + static_cast<std::ptrdiff_t>(prev), flat.end());
    for (auto& p : out.parts) {
        out.rope.append(tr::view::view_t::over(tr::view::borrow(std::span<std::byte>(p))));
    }
    return out;
}

// The core obligation: for EVERY single cut 1..n-1 (2-link ropes) and a few 3-link
// ropes, validate_rope's ok/err verdict — and its err_t on failure — matches
// decode(flat). `name` labels the frame; `expect_ok` is decode's own verdict.
void assert_matches_decode(std::string_view name, const std::vector<std::byte>& flat) {
    const auto flat_decoded = tr::wire::decode(flat);
    const bool expect_ok = flat_decoded.has_value();
    const tr::wire::err_t expect_err =
        expect_ok ? tr::wire::err_t{} : flat_decoded.error();  // value unused when ok

    bool all_ok = true;
    tr::wire::err_t first_bad_err{};
    std::size_t first_bad_cut = 0;

    // Whole frame as a single-link rope, then every 2-link split.
    for (std::size_t cut = 0; cut < flat.size(); ++cut) {
        const std::vector<std::size_t> cuts =
            cut == 0 ? std::vector<std::size_t>{} : std::vector<std::size_t>{cut};
        split_rope_t sr = split_into(flat, cuts);
        const auto v = tr::wire::validate_rope(sr.rope);
        const bool ok = v.has_value();
        const bool verdict_matches = ok == expect_ok;
        const bool err_matches = ok || expect_ok || v.error() == expect_err;
        if (!verdict_matches || !err_matches) {
            if (all_ok) {  // record the first divergence for the message
                first_bad_err = ok ? tr::wire::err_t{} : v.error();
                first_bad_cut = cut;
            }
            all_ok = false;
        }
    }

    // A few 3-link ropes (two interior cuts) to exercise a payload straddling
    // three segments and back-to-back mid-header/mid-trailer splits.
    if (flat.size() >= 4) {
        const std::size_t a = 1;
        const std::size_t b = flat.size() - 1;
        split_rope_t sr = split_into(flat, {a, b});
        const auto v = tr::wire::validate_rope(sr.rope);
        if (v.has_value() != expect_ok) all_ok = false;
    }

    if (all_ok) {
        check(true, name);
    } else {
        std::printf("    (diverged at cut=%zu, expect_ok=%d, got_err=%d)\n", first_bad_cut,
                    static_cast<int>(expect_ok), static_cast<int>(first_bad_err));
        check(false, name);
    }
}

// ---- Frame builders (via the owning codec, so the flat bytes are canonical) ----

std::vector<std::byte> encode_opaque_value(std::size_t payload_len, bool crc32, bool crc16) {
    tr::wire::tlv_t t;
    t.type = tr::wire::type_t::VALUE;
    std::vector<std::byte> pay(payload_len);
    for (std::size_t i = 0; i < payload_len; ++i) pay[i] = static_cast<std::byte>(0xA0 + i);
    t.payload = pay;
    if (crc32) {
        t.opt.cr = true;
    } else if (crc16) {
        t.opt.cr = true;
        t.opt.cw = true;
    }
    return tr::wire::encode(t);
}

std::vector<std::byte> encode_path_two_names() {
    // tlv_t::payload BORROWS, so the NAME payload buffers must outlive encode() —
    // hold them in `storage` (reserved so no realloc invalidates a borrow).
    std::vector<std::vector<std::byte>> storage;
    storage.reserve(2);
    tr::wire::tlv_t root;
    root.type = tr::wire::type_t::PATH;
    root.opt.pl = true;
    for (const char* nm : {"sensor", "temperature-reading-long-enough-to-straddle"}) {
        std::vector<std::byte> bytes;
        for (const char* p = nm; *p; ++p) bytes.push_back(static_cast<std::byte>(*p));
        storage.push_back(std::move(bytes));
        tr::wire::tlv_t name;
        name.type = tr::wire::type_t::NAME;
        name.payload = storage.back();  // borrows the kept-alive buffer
        root.children.push_back(name);
    }
    return tr::wire::encode(root);
}

}  // namespace

int main() {
    std::printf("Rope-aware grammar differential (validate_rope == decode, ADR-0048 §1):\n");

    // Valid frames of assorted shapes — each must validate for every split.
    assert_matches_decode("opaque VALUE, no trailer, 8B payload",
                          encode_opaque_value(8, false, false));
    assert_matches_decode("opaque VALUE + CRC-32C trailer", encode_opaque_value(20, true, false));
    assert_matches_decode("opaque VALUE + CRC-16 trailer", encode_opaque_value(20, false, true));
    assert_matches_decode("empty opaque VALUE", encode_opaque_value(0, false, false));
    assert_matches_decode("structured PATH, 2 NAME children", encode_path_two_names());

    // Corruptions — validate_rope must reject with the SAME err_t as decode, at
    // every cut (so a mid-header/mid-trailer split can't mask or change the error).
    {
        std::vector<std::byte> f = encode_opaque_value(20, true, false);  // has CRC-32C
        f.back() ^= std::byte{0xFF};                                      // flip a CRC byte
        assert_matches_decode("corrupt CRC-32C trailer -> FRAME_CRC_FAIL (any cut)", f);
    }
    {
        std::vector<std::byte> f = encode_opaque_value(8, false, false);
        f.pop_back();  // drop the last payload byte: length now overruns
        assert_matches_decode("truncated body -> FRAME_TRUNCATED (any cut)", f);
    }
    {
        std::vector<std::byte> f = encode_opaque_value(8, false, false);
        f.push_back(std::byte{0x99});  // one trailing byte past a complete frame
        assert_matches_decode("trailing byte -> FRAME_INVALID (any cut)", f);
    }
    {
        std::vector<std::byte> f = encode_opaque_value(8, false, false);
        f[0] = std::byte{0x00};  // type 0x00 is reserved/illegal
        assert_matches_decode("type 0x00 -> FRAME_INVALID (any cut)", f);
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
